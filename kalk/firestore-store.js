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
export async function addDosing({ pump, ml, timestamp, isAutomatic, factor, dosageType, success }) {
  const uid = requireUid();
  await addDoc(collection(db, `users/${uid}/aquarium/dosings/items`), {
    pump, ml, timestamp: timestamp || Math.floor(Date.now() / 1000),
    isAutomatic: !!isAutomatic, factor: factor || 1, dosageType, success: success !== false,
    createdAt: serverTimestamp()
  });
  // Container-Level dekrementieren (clamped bei 0). Nur Info-Tracking — wenn 0,
  // wird trotzdem weiter dosiert (User hat evtl. nachgefüllt aber nicht geklickt).
  if (pump != null && ml > 0 && success !== false) {
    try {
      const settings = await getSettings();
      const levels = [...(settings.containerLevel || [5000, 5000, 5000, 5000])];
      if (pump >= 0 && pump < levels.length) {
        levels[pump] = Math.max(0, levels[pump] - ml);
        await saveSettings({ containerLevel: levels });
      }
    } catch (e) { console.warn("Container-Decrement:", e); }
  }
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
// Pfad: users/{uid}/aquarium-commands/{auto-id}
// Wir verwenden eine separate Subcollection auf User-Ebene, damit
// die Pumpen-Config-Docs aquarium/pump-N und die Commands sich nicht
// auf gleichem "aquarium" Pfad mischen.
function commandsPath() {
  return `users/${requireUid()}/aquarium-commands`;
}

// Sendet einen Command an den ESP. Liefert command-ID zurück.
//   action: "calibrate" → braucht pump + steps
//   action: "dose"      → braucht pump + ml
//   action: "stop"      → stoppt aktuelle Pumpenaktion
export async function sendCommand({ action, pump, ml, steps }) {
  const uid = requireUid();
  const cmd = {
    action,
    status: "pending",
    createdAt: serverTimestamp()
  };
  if (pump != null) cmd.pump = pump;
  if (ml != null) cmd.ml = ml;
  if (steps != null) cmd.steps = steps;
  const ref = await addDoc(collection(db, commandsPath()), cmd);
  return ref.id;
}

// onSnapshot auf einen bestimmten Command-Doc. callback bekommt das aktuelle data oder null wenn gelöscht.
// Liefert die Unsubscribe-Funktion.
export function watchCommand(commandId, callback) {
  const uid = requireUid();
  const ref = doc(db, `${commandsPath()}/${commandId}`);
  return onSnapshot(ref, snap => {
    callback(snap.exists() ? { id: snap.id, ...snap.data() } : null);
  }, err => {
    console.warn("watchCommand error:", err);
    callback(null);
  });
}

export async function deleteCommand(commandId) {
  await deleteDoc(doc(db, `${commandsPath()}/${commandId}`));
}

// Letzte Commands (für Debug/Verlauf)
export async function listRecentCommands(maxItems = 20) {
  const q = query(
    collection(db, commandsPath()),
    orderBy("createdAt", "desc"),
    limit(maxItems)
  );
  const snap = await getDocs(q);
  const out = [];
  snap.forEach(d => out.push({ id: d.id, ...d.data() }));
  return out;
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
    mlPerStep: 0,
    speedML: 3.6,
    accelerationML: 1.8,
    lastCalibrationDate: null
  };
}

export function getAuthInstance() {
  return auth;
}
