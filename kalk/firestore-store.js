// =============================================================
// Firestore-Wrapper für Kalkmanagement (1 User = 1 Aquarium)
// =============================================================

import { initializeApp, getApps } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-app.js";
import { getAuth } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-auth.js";
import {
  getFirestore, collection, doc,
  getDoc, getDocs, setDoc, addDoc, deleteDoc, onSnapshot,
  query, orderBy, limit, where,
  serverTimestamp
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-firestore.js";
import { firebaseConfig } from "/assets/firebase-config.js";

const app = getApps().length ? getApps()[0] : initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getFirestore(app);

function requireUid() {
  const u = auth.currentUser;
  if (!u) throw new Error("Nicht eingeloggt");
  return u.uid;
}

function aquaPath(docId) {
  return `users/${requireUid()}/aquarium/${docId}`;
}

// ---------- Einstellungen ----------
// Liefert Defaults + gespeicherte Settings darüber gemergt.
// Wichtig: einzelne Felder werden mit merge:true gespeichert. Wenn nur
// einzelne Felder im Doc sind, fallen die anderen auf Defaults zurück.
export async function getSettings() {
  const snap = await getDoc(doc(db, aquaPath("settings")));
  const stored = snap.exists() ? snap.data() : {};
  return { ...defaultSettings(), ...stored };
}

export async function saveSettings(settings) {
  await setDoc(doc(db, aquaPath("settings")), settings, { merge: true });
}

// ---------- Pumpen ----------
// Auch hier: Defaults pro Pumpe + gespeicherte Felder darüber.
export async function getPumps() {
  const pumps = [];
  for (let i = 0; i < 4; i++) {
    const snap = await getDoc(doc(db, aquaPath(`pump-${i}`)));
    const stored = snap.exists() ? snap.data() : {};
    pumps.push({ index: i, ...defaultPump(i), ...stored });
  }
  return pumps;
}

export async function savePump(pumpIndex, pumpData) {
  await setDoc(doc(db, aquaPath(`pump-${pumpIndex}`)), pumpData, { merge: true });
}

// ---------- Messungen ----------
export async function addMeasurement({ type, value, timestamp, source }) {
  const uid = requireUid();
  await addDoc(collection(db, `users/${uid}/aquarium/measurements/items`), {
    type, value, timestamp: timestamp || Math.floor(Date.now() / 1000),
    source: source || "manual",
    createdAt: serverTimestamp()
  });
}

export async function listMeasurements(type, maxItems = 200) {
  const uid = requireUid();
  const q = query(
    collection(db, `users/${uid}/aquarium/measurements/items`),
    where("type", "==", type),
    orderBy("timestamp", "desc"),
    limit(maxItems)
  );
  const snap = await getDocs(q);
  const out = [];
  snap.forEach(d => out.push({ id: d.id, ...d.data() }));
  return out;
}

export async function deleteMeasurement(measurementId) {
  const uid = requireUid();
  await deleteDoc(doc(db, `users/${uid}/aquarium/measurements/items/${measurementId}`));
}

// ---------- Dosierungen ----------
// HINWEIS: Web schreibt KEINE Dosen direkt — das macht ausschließlich der ESP.
// Diese Funktion bleibt nur für Import/Restore aus JSON-Backups vorhanden.
// Bei Manuell-Dosierungen über den Web-UI Tab Manuell wird stattdessen
// sendCommand({action:"dose"}) verwendet, der ESP führt aus und schreibt
// dann selbst nach dosings/items. So landen nur tatsächlich gefahrene
// Doses in der DB — wichtig für korrekte Verbrauchs-Berechnung.
export async function addDosing({ pump, ml, timestamp, isAutomatic, factor, dosageType, success }) {
  const uid = requireUid();
  await addDoc(collection(db, `users/${uid}/aquarium/dosings/items`), {
    pump, ml, timestamp: timestamp || Math.floor(Date.now() / 1000),
    isAutomatic: !!isAutomatic, factor: factor || 1, dosageType, success: success !== false,
    createdAt: serverTimestamp()
  });
}

export async function listDosings(maxItems = 200) {
  const uid = requireUid();
  const q = query(
    collection(db, `users/${uid}/aquarium/dosings/items`),
    orderBy("timestamp", "desc"),
    limit(maxItems)
  );
  const snap = await getDocs(q);
  const out = [];
  snap.forEach(d => out.push({ id: d.id, ...d.data() }));
  return out;
}

// ---------- Commands (ESP-Steuerung) ----------
// EIN einzelnes Dokument auf bekannten Pfad — vermeidet runQuery (das in
// der Firebase-ESP-Lib zickig ist). Pfad: users/{uid}/aquarium/cmd
// Inhalt: { id, action, status, pump?, ml?, steps?, phValue?, createdAt,
//           startedAt?, completedAt?, result? }
//
// Race-Condition: Neue Commands überschreiben das alte. UI sorgt dafür
// dass nur ein Wizard zur Zeit läuft.
function commandPath() {
  return `users/${requireUid()}/aquarium/cmd`;
}

function genCmdId() {
  return Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(-4);
}

export async function sendCommand({ action, pump, ml, steps, phValue }) {
  const cmdId = genCmdId();
  const cmd = {
    id: cmdId,
    action,
    status: "pending",
    createdAt: serverTimestamp()
  };
  if (pump != null)    cmd.pump = pump;
  if (ml != null)      cmd.ml = ml;
  if (steps != null)   cmd.steps = steps;
  if (phValue != null) cmd.phValue = phValue;
  await setDoc(doc(db, commandPath()), cmd);
  return cmdId;
}

// onSnapshot auf das EINE Command-Dokument. Filtert auf unsere ID —
// wenn jemand anders ein neueres Command schreibt, kriegen wir's nicht.
// Liefert Unsubscribe-Funktion.
export function watchCommand(commandId, callback) {
  return onSnapshot(doc(db, commandPath()), snap => {
    if (!snap.exists()) { callback(null); return; }
    const data = snap.data();
    if (data.id !== commandId) return;  // nicht unser Command
    callback(data);
  }, err => {
    console.warn("watchCommand error:", err);
    callback(null);
  });
}

// Command-Doc löschen (z. B. bei Abbruch)
export async function deleteCommand(commandId) {
  // Nur löschen wenn ID matched (kein Race-Risiko)
  try {
    const snap = await getDoc(doc(db, commandPath()));
    if (snap.exists() && snap.data().id === commandId) {
      await deleteDoc(doc(db, commandPath()));
    }
  } catch (e) { console.warn("deleteCommand:", e); }
}

// ---------- Pläne ----------
export async function getCurrentPlan() {
  const snap = await getDoc(doc(db, aquaPath("plan-current")));
  return snap.exists() ? snap.data() : null;
}

export async function savePlan(plan) {
  await setDoc(doc(db, aquaPath("plan-current")), {
    ...plan,
    generatedAt: serverTimestamp()
  });
}

// ---------- Defaults ----------
function defaultSettings(volume) {
  return {
    aquariumVolume: volume || 450,
    targetKH: 7.5,
    targetCalcium: 420,
    historyCount: 5,
    autoDosing: false,
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
    antiDripStepsPerSec: 400,   // Rückzugsgeschwindigkeit in Hz (default = wie global)
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

export function getAuthInstance() {
  return auth;
}

// ---------- Firmware-Update (OTA via GitHub Releases) ----------
// Liest das neueste Release direkt von der GitHub-API (public, keine Auth).
// ESP liest die gleiche Quelle.
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
