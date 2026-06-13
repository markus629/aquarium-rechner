// =============================================================
// Daten-Store für Kalkmanagement (1 User = 1 Aquarium)
// Backend: PocketBase
// =============================================================
// Collections-Mapping (je 1 Doc pro Schlüssel unter aqua_docs):
//   - Einzeldokumente (settings, pump-0..3, plan-current,
//     ph-calibration, info)  -> Collection "aqua_docs" (user, key, data)
//   - Messungen              -> Collection "aqua_measurements"
//   - Dosierungen            -> Collection "aqua_dosings"
//   - ESP-Command            -> Collection "aqua_command" (1 Doc pro User)
// =============================================================

import { pb } from "/assets/pb-client.js";

function requireUid() {
  if (!pb.authStore.isValid) throw new Error("Nicht eingeloggt");
  return pb.authStore.record.id;
}

// ---------- Generische Einzeldokumente (aqua_docs) ----------
export async function getAquaDoc(key) {
  const uid = requireUid();
  try {
    const r = await pb.collection("aqua_docs").getFirstListItem(`user="${uid}" && key="${key}"`);
    return r;
  } catch (_) {
    return null; // 404 = nicht vorhanden
  }
}

export async function setAquaDoc(key, data, merge = false) {
  const uid = requireUid();
  const existing = await getAquaDoc(key);
  if (existing) {
    const newData = merge ? { ...(existing.data || {}), ...data } : data;
    await pb.collection("aqua_docs").update(existing.id, { data: newData });
  } else {
    await pb.collection("aqua_docs").create({ user: uid, key, data });
  }
}

// ---------- Einstellungen ----------
// Liefert Defaults + gespeicherte Settings darüber gemergt.
export async function getSettings() {
  const doc = await getAquaDoc("settings");
  const stored = (doc && doc.data) || {};
  return { ...defaultSettings(), ...stored };
}

export async function saveSettings(settings) {
  await setAquaDoc("settings", settings, true); // merge:true
}

// ---------- Pumpen ----------
export async function getPumps() {
  const pumps = [];
  for (let i = 0; i < 4; i++) {
    const doc = await getAquaDoc(`pump-${i}`);
    const stored = (doc && doc.data) || {};
    pumps.push({ index: i, ...defaultPump(i), ...stored });
  }
  return pumps;
}

export async function savePump(pumpIndex, pumpData) {
  await setAquaDoc(`pump-${pumpIndex}`, pumpData, true); // merge:true
}

// ---------- Messungen ----------
export async function addMeasurement({ type, value, timestamp, source }) {
  const uid = requireUid();
  await pb.collection("aqua_measurements").create({
    user: uid, type, value,
    timestamp: timestamp || Math.floor(Date.now() / 1000),
    source: source || "manual"
  });
}

export async function listMeasurements(type, maxItems = 200) {
  requireUid();
  const res = await pb.collection("aqua_measurements").getList(1, maxItems, {
    filter: `type="${type}"`,
    sort: "-timestamp"
  });
  return res.items;
}

export async function listAllMeasurements(maxItems = 1000) {
  requireUid();
  const res = await pb.collection("aqua_measurements").getList(1, maxItems, {
    sort: "-timestamp"
  });
  return res.items;
}

export async function deleteMeasurement(measurementId) {
  requireUid();
  await pb.collection("aqua_measurements").delete(measurementId);
}

// ---------- Dosierungen ----------
// HINWEIS: Web schreibt KEINE Dosen direkt — das macht ausschließlich der ESP.
// Diese Funktion bleibt nur für Import/Restore aus JSON-Backups vorhanden.
export async function addDosing({ pump, ml, timestamp, isAutomatic, factor, dosageType, success }) {
  const uid = requireUid();
  await pb.collection("aqua_dosings").create({
    user: uid, pump, ml,
    timestamp: timestamp || Math.floor(Date.now() / 1000),
    isAutomatic: !!isAutomatic, factor: factor || 1, dosageType, success: success !== false
  });
}

export async function listDosings(maxItems = 200) {
  requireUid();
  const res = await pb.collection("aqua_dosings").getList(1, maxItems, {
    sort: "-timestamp"
  });
  return res.items;
}

export async function deleteDosing(dosingId) {
  requireUid();
  await pb.collection("aqua_dosings").delete(dosingId);
}

// ---------- Commands (ESP-Steuerung) ----------
// EIN Record pro User in "aqua_command". Web abonniert per Realtime,
// der ESP pollt den Record. cmdId identifiziert das aktuelle Command.
function genCmdId() {
  return Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(-4);
}

export async function sendCommand({ action, pump, ml, steps, phValue }) {
  const uid = requireUid();
  const cmdId = genCmdId();
  const body = { user: uid, cmdId, action, status: "pending", result: null };
  if (pump != null)    body.pump = pump;
  if (ml != null)      body.ml = ml;
  if (steps != null)   body.steps = steps;
  if (phValue != null) body.phValue = phValue;

  let existing = null;
  try { existing = await pb.collection("aqua_command").getFirstListItem(`user="${uid}"`); } catch (_) {}
  if (existing) await pb.collection("aqua_command").update(existing.id, body);
  else          await pb.collection("aqua_command").create(body);
  return cmdId;
}

// Realtime-Abo auf den Command-Record. Liefert eine synchrone
// Unsubscribe-Funktion (wie früher onSnapshot).
export function watchCommand(commandId, callback) {
  let unsub = null;
  let cancelled = false;
  pb.collection("aqua_command").subscribe("*", (e) => {
    if (e.action === "delete") { callback(null); return; }
    const r = e.record;
    if (!r || r.cmdId !== commandId) return; // nicht unser Command
    callback(r);
  }).then(fn => { if (cancelled) fn(); else unsub = fn; })
    .catch(err => console.warn("watchCommand error:", err));
  return () => { cancelled = true; if (unsub) unsub(); };
}

export async function deleteCommand(commandId) {
  const uid = requireUid();
  try {
    const r = await pb.collection("aqua_command").getFirstListItem(`user="${uid}"`);
    if (r && r.cmdId === commandId) await pb.collection("aqua_command").delete(r.id);
  } catch (e) { console.warn("deleteCommand:", e); }
}

// ---------- Pläne ----------
export async function getCurrentPlan() {
  const doc = await getAquaDoc("plan-current");
  return (doc && doc.data) || null;
}

export async function savePlan(plan) {
  await setAquaDoc("plan-current", { ...plan, generatedAt: Math.floor(Date.now() / 1000) });
}

// ---------- Defaults ----------
function defaultSettings(volume) {
  return {
    aquariumVolume: volume || 450,
    targetKH: 7.5,
    targetCalcium: 420,
    historyCount: 5,
    autoDosing: false,
    dosingsPerDay: 12,    // wählbar: 2, 3, 4, 6, 8, 12 (alle teilen 24 sauber)
    espOfflineAlert: true,   // E-Mail bei ESP-Ausfall (Server-Watchdog)
    maxDailyChangeKH: 2.0,
    maxDailyChangeCalcium: 20.0,
    magnesiumRatio: 50,
    khNightStart: 19,
    khNightEnd: 7,
    initialKHConsumption: 160,
    initialCalciumConsumption: 60,
    autoUpdateInitialRates: true,
    usePhBasedKHDosing: false,
    phThresholdForKHNight: 8.0,
    enableAntiDrip: true,
    antiDripML: 0.015,
    antiDripStepsPerSec: 400,   // Rückzugsgeschwindigkeit in Hz
    antiDripAccelStepsPerSec2: 1000,   // Rückzugs-Beschleunigung in Hz/s
    // Globale Pumpen-Geschwindigkeit (Schritte/sec direkt — unabhängig von Kalibrierung)
    stepsPerSec: 400,           // Hz
    accelStepsPerSec2: 200,     // Hz/sec (0 = quasi sofort)
    containerCapacity: [5000, 5000, 5000, 5000],
    containerLevel: [5000, 5000, 5000, 5000]
  };
}

function defaultPump(index) {
  const names = ["Calcium", "Magnesium", "KH-Tag", "KH-Nacht"];
  return {
    name: names[index] || `Pumpe ${index}`,
    stepsPerML: 0,   // Kalibrierung: Schritte pro ml
    lastCalibrationDate: null
  };
}

// Schlanker Auth-Shim für den restlichen Kalk-Code.
// Bietet nur, was kalk/index.html und die Übersicht nutzen:
//   getAuthInstance().currentUser?.{uid,email,emailVerified}
//   getAuthInstance().onAuthStateChanged(cb)
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

// Token für authentifizierte Roh-Requests (z. B. ESP-Doku / Debug)
export function getPB() { return pb; }

// ---------- Firmware-Update (OTA via GitHub Releases) ----------
// Liest das neueste Release direkt von der GitHub-API (public, keine Auth).
export async function getLatestGithubRelease(owner, repo) {
  const url = `https://api.github.com/repos/${owner}/${repo}/releases/latest`;
  const r = await fetch(url, { headers: { "Accept": "application/vnd.github+json" } });
  if (!r.ok) {
    if (r.status === 404) return null;  // noch kein Release vorhanden
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
