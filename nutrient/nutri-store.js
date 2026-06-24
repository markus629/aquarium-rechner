// =============================================================
// Daten-Store für Nährstoffmanagement (1 User = 1 Aquarium)
// Backend: PocketBase. Eigener ESP/Subsystem für NO3 & PO4.
// =============================================================
// Collections:
//   - Einzeldokumente (settings, plan-current, info, _state, pump-0..3)
//        -> "nutri_docs" (user, key, data)
//   - Messungen (type = "no3" | "po4")   -> "nutri_measurements"
//   - Dosierungen (Log, was wirklich dosiert wurde) -> "nutri_dosings"
//   - ESP-Command (1 Doc pro User)       -> "nutri_command"
// Der user wird im Code automatisch angehängt; die API-Regeln sorgen
// dafür, dass jeder nur seine eigenen Daten sieht.
// =============================================================

import { pb } from "/assets/pb-client.js";

// Pumpen-Zuordnung des Nährstoff-ESP (fix):
//   0 = N (Stickstoff, hebt NO3)   1 = P (hebt PO4)
//   2 = C (Kohlenstoff, senkt NO3 & PO4)   3 = Lanthan (senkt PO4 gezielt)
export const PUMP = { N: 0, P: 1, C: 2, LA: 3 };
export const PUMP_NAMES = ["Stickstoff (N)", "Phosphat (P)", "Kohlenstoff (C)", "Lanthan"];

function requireUid() {
  if (!pb.authStore.isValid) throw new Error("Nicht eingeloggt");
  return pb.authStore.record.id;
}

// ---------- Generische Einzeldokumente (nutri_docs) ----------
export async function getNutriDoc(key) {
  const uid = requireUid();
  try {
    return await pb.collection("nutri_docs").getFirstListItem(`user="${uid}" && key="${key}"`);
  } catch (_) {
    return null; // 404 = nicht vorhanden
  }
}

export async function setNutriDoc(key, data, merge = false) {
  const uid = requireUid();
  const existing = await getNutriDoc(key);
  if (existing) {
    const newData = merge ? { ...(existing.data || {}), ...data } : data;
    await pb.collection("nutri_docs").update(existing.id, { data: newData });
  } else {
    await pb.collection("nutri_docs").create({ user: uid, key, data });
  }
}

// ---------- Einstellungen ----------
export async function getSettings() {
  const doc = await getNutriDoc("settings");
  const stored = (doc && doc.data) || {};
  return { ...defaultSettings(), ...stored };
}

export async function saveSettings(settings) {
  await setNutriDoc("settings", settings, true); // merge:true
}

// ---------- Pumpen (Kalibrierung Schritte/ml) ----------
export async function getPumps() {
  const pumps = [];
  for (let i = 0; i < 4; i++) {
    const doc = await getNutriDoc(`pump-${i}`);
    const stored = (doc && doc.data) || {};
    pumps.push({ index: i, ...defaultPump(i), ...stored });
  }
  return pumps;
}

export async function savePump(pumpIndex, pumpData) {
  await setNutriDoc(`pump-${pumpIndex}`, pumpData, true);
}

// ---------- Messungen (type = "no3" | "po4") ----------
export async function addMeasurement({ type, value, timestamp, source }) {
  const uid = requireUid();
  await pb.collection("nutri_measurements").create({
    user: uid, type, value,
    timestamp: timestamp || Math.floor(Date.now() / 1000),
    source: source || "manual"
  });
}

export async function listMeasurements(type, maxItems = 200) {
  requireUid();
  const res = await pb.collection("nutri_measurements").getList(1, maxItems, {
    filter: `type="${type}"`,
    sort: "-timestamp"
  });
  return res.items;
}

export async function listAllMeasurements(maxItems = 1000) {
  requireUid();
  const res = await pb.collection("nutri_measurements").getList(1, maxItems, { sort: "-timestamp" });
  return res.items;
}

export async function deleteMeasurement(measurementId) {
  requireUid();
  await pb.collection("nutri_measurements").delete(measurementId);
}

// ---------- Dosierungen (Log) ----------
// Schreibt normalerweise der ESP (was er real dosiert hat). Diese Funktion
// dient v.a. Import/Restore — und im reinen Web-Test, um Dosierungen zu
// simulieren, damit die Lern-Logik die "schon zugeführte Menge" kennt.
export async function addDosing({ pump, ml, timestamp, isAutomatic, factor, dosageType, success }) {
  const uid = requireUid();
  await pb.collection("nutri_dosings").create({
    user: uid, pump, ml,
    timestamp: timestamp || Math.floor(Date.now() / 1000),
    isAutomatic: !!isAutomatic, factor: factor || 1, dosageType, success: success !== false
  });
}

export async function listDosings(maxItems = 500) {
  requireUid();
  const res = await pb.collection("nutri_dosings").getList(1, maxItems, { sort: "-timestamp" });
  return res.items;
}

export async function deleteDosing(dosingId) {
  requireUid();
  await pb.collection("nutri_dosings").delete(dosingId);
}

// ---------- Commands (ESP-Steuerung) ----------
function genCmdId() {
  return Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(-4);
}

export async function sendCommand({ action, pump, ml, steps }) {
  const uid = requireUid();
  const cmdId = genCmdId();
  const body = { user: uid, cmdId, action, status: "pending", result: null };
  if (pump != null)  body.pump = pump;
  if (ml != null)    body.ml = ml;
  if (steps != null) body.steps = steps;

  let existing = null;
  try { existing = await pb.collection("nutri_command").getFirstListItem(`user="${uid}"`); } catch (_) {}
  if (existing) await pb.collection("nutri_command").update(existing.id, body);
  else          await pb.collection("nutri_command").create(body);
  return cmdId;
}

export function watchCommand(commandId, callback) {
  let unsub = null;
  let cancelled = false;
  pb.collection("nutri_command").subscribe("*", (e) => {
    if (e.action === "delete") { callback(null); return; }
    const r = e.record;
    if (!r || r.cmdId !== commandId) return;
    callback(r);
  }).then(fn => { if (cancelled) fn(); else unsub = fn; })
    .catch(err => console.warn("watchCommand error:", err));
  return () => { cancelled = true; if (unsub) unsub(); };
}

export async function deleteCommand(commandId) {
  const uid = requireUid();
  try {
    const r = await pb.collection("nutri_command").getFirstListItem(`user="${uid}"`);
    if (r && r.cmdId === commandId) await pb.collection("nutri_command").delete(r.id);
  } catch (e) { console.warn("deleteCommand:", e); }
}

// ---------- Plan / gelernter Zustand ----------
export async function getCurrentPlan() {
  const doc = await getNutriDoc("plan-current");
  return (doc && doc.data) || null;
}

export async function savePlan(plan) {
  await setNutriDoc("plan-current", { ...plan, generatedAt: Math.floor(Date.now() / 1000) });
}

// Gelernter Regel-Zustand (Erhaltungsraten, Überschuss-Bestätigung) getrennt
// von den Settings, da er automatisch fortgeschrieben wird.
export async function getState() {
  const doc = await getNutriDoc("_state");
  return (doc && doc.data) || {};
}
export async function saveState(state) {
  await setNutriDoc("_state", state, true);
}

// ---------- Defaults ----------
export function defaultSettings() {
  return {
    aquariumVolume: 450,            // Liter (Netto)
    // Zielwerte + Bereiche
    targetNO3: 5,   no3Min: 2,    no3Max: 15,    // mg/l
    targetPO4: 0.05, po4Min: 0.02, po4Max: 0.15, // mg/l
    npTargetRatio: 16,             // molares Ziel-Verhältnis N:P (Redfield ~16:1)
    maxMeasureDays: 7,             // spätestens nach so vielen Tagen messen (Cap der Vorhersage)
    // Lernen (Ansatz A: Netto-Verbrauch aus Massenbilanz = Dosierrate − Steigung)
    learnWindowDays: 28,            // 4-Wochen-Fenster für Dosierrate + Steigung
    learnAlpha: 0.5,               // Glättung der Erhaltungsdosis je neuer Messung (0..1)
    surplusConfirmDays: 5,          // so lange flach/steigend bei 0-Dosis -> echter Überschuss -> C/Lanthan
    // Sicherheits-Caps (max. Tagesänderung der Konzentration durch unsere Dosis)
    maxDailyChangeNO3: 2.0,         // mg/l/Tag
    maxDailyChangePO4: 0.03,        // mg/l/Tag
    // Wirkfaktoren (mg/l je 1 ml/100 L) — Herstellerangaben, N adaptiv
    coeffN_NO3: 1.1,                // N-Pumpe: +1,1 NO3 (theoretisch, real weniger -> wird gelernt)
    coeffP_PO4: 0.1,                // P-Pumpe: +0,1 PO4
    coeffLa_PO4: 0.1,               // Lanthan: -0,1 PO4 ("bis zu" -> konservativ)
    cMinML100: 0.2, cMaxML100: 2.0, // C-Pumpe: Dosier-Hüllkurve ml/100 L (unspezifisch)
    // Betrieb — Dosierungen/Tag je Pumpe (1 = auf einen Schlag zur Fenster-Startzeit).
    // N und P individuell (mit eigenem Zeitfenster); C + Lanthan gemeinsam (immer ganztägig).
    dosesPerDayN: 12, dosesPerDayP: 12, dosesPerDayCLa: 12,
    dosingsPerDay: 12,             // intern: nur für OTA-Schutzfenster-Heuristik
    // Dosier-Zeitfenster (Stunden 0–24). 0–24 = ganzer Tag. Nur N (no3*) und P (po4*);
    // C + Lanthan dosieren immer ganztägig.
    no3FromHour: 0, no3ToHour: 24,
    po4FromHour: 0, po4ToHour: 24,
    // Start-Dosis ml/Tag je Pumpe (Warmstart; 0 = von 0 lernen).
    // N/P seeden die gelernte Erhaltung; C/La sind die Start-Menge beim Abbau.
    startDoseN: 0, startDoseP: 0, startDoseC: 0, startDoseLa: 0,
    autoDosing: false,
    espOfflineAlert: true,
    // Pumpen-Hardware (rollen-spezifisch, gleiche Keys wie Kalk → ESP liest sie
    // im Nährstoff-Modus aus nutri_docs/settings)
    stepsPerSec: 400,                 // Hz
    accelStepsPerSec2: 200,           // Hz/sec (0 = quasi sofort)
    enableAntiDrip: true,
    antiDripML: 0.015,                // Rückzugsmenge ml
    antiDripStepsPerSec: 400,         // Rückzugsgeschwindigkeit Hz
    antiDripAccelStepsPerSec2: 1000,  // Rückzugs-Beschleunigung Hz/s
    containerCapacity: [5000, 5000, 5000, 5000],
    containerLevel: [5000, 5000, 5000, 5000]
  };
}

export function defaultPump(index) {
  return {
    name: PUMP_NAMES[index] || `Pumpe ${index}`,
    stepsPerML: 0,
    lastCalibrationDate: null
  };
}

// ---------- Auth-Shim (wie kalk) ----------
export function getAuthInstance() {
  return {
    get currentUser() {
      if (!pb.authStore.isValid) return null;
      const r = pb.authStore.record;
      return { uid: r.id, email: r.email, emailVerified: r.verified };
    },
    onAuthStateChanged(cb) {
      return pb.authStore.onChange((token, r) => {
        cb(r ? { uid: r.id, email: r.email, emailVerified: r.verified } : null);
      }, true);
    }
  };
}

export function getPB() { return pb; }

// ---------- Firmware-Update (OTA via GitHub Releases) ----------
export async function getLatestGithubRelease(owner, repo) {
  const url = `https://api.github.com/repos/${owner}/${repo}/releases/latest`;
  const r = await fetch(url, { headers: { "Accept": "application/vnd.github+json" } });
  if (!r.ok) {
    if (r.status === 404) return null;
    throw new Error(`GitHub API HTTP ${r.status}`);
  }
  const data = await r.json();
  const tag = (data.tag_name || "").replace(/^v/i, "");
  const asset = (data.assets || []).find(a => /\.bin$/i.test(a.name));
  return {
    version: tag,
    url: asset?.browser_download_url || null,
    name: asset?.name || null,
    size: asset?.size || 0,
    publishedAt: data.published_at,
    htmlUrl: data.html_url,
    body: data.body || ""
  };
}
