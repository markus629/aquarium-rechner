// =============================================================
// Firestore-Wrapper für die Dosierpumpe
// =============================================================
// Stellt eine schmale API für CRUD auf Devices + Subcollections bereit.
// Wird vom Dosier-UI verwendet, sobald ein User eingeloggt ist.
// =============================================================

import { initializeApp, getApps } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-app.js";
import { getAuth } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-auth.js";
import {
  getFirestore, collection, doc,
  getDoc, getDocs, setDoc, addDoc, updateDoc, deleteDoc,
  query, orderBy, limit, where,
  serverTimestamp
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-firestore.js";
import { firebaseConfig } from "../assets/firebase-config.js";

// Firebase Singleton (idempotent — auth-sync hat es ggf. schon initialisiert)
const app = getApps().length ? getApps()[0] : initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getFirestore(app);

function requireUid() {
  const u = auth.currentUser;
  if (!u) throw new Error("Nicht eingeloggt");
  return u.uid;
}

function devicePath(deviceId) {
  return `users/${requireUid()}/devices/${deviceId}`;
}

// ---------- Devices ----------
export async function listDevices() {
  const uid = requireUid();
  const snap = await getDocs(collection(db, `users/${uid}/devices`));
  const devices = [];
  snap.forEach(d => devices.push({ id: d.id, ...d.data() }));
  // info-Dokumente nach Name sortieren
  devices.sort((a, b) => (a.name || a.id).localeCompare(b.name || b.id));
  return devices;
}

export async function createDevice({ name, aquariumVolume }) {
  const uid = requireUid();
  const devRef = await addDoc(collection(db, `users/${uid}/devices`), {
    name: name || "Aquarium",
    aquariumVolume: aquariumVolume || 450,
    createdAt: serverTimestamp(),
    online: false,
    firmware: null,
    lastSeen: null
  });
  // Default-Settings als Sub-Doc anlegen
  await setDoc(doc(db, `users/${uid}/devices/${devRef.id}/config/settings`), defaultSettings(aquariumVolume));
  // Vier Default-Pumpen anlegen (Ca, Mg, KH-Tag, KH-Nacht)
  for (let i = 0; i < 4; i++) {
    await setDoc(doc(db, `users/${uid}/devices/${devRef.id}/config/pump-${i}`), defaultPump(i));
  }
  return devRef.id;
}

export async function deleteDevice(deviceId) {
  // Firestore hat keine rekursive Löschung — wir entfernen nur das Top-Doc.
  // Subcollections bleiben verwaist und können später per Cloud Function gepurged werden.
  // Für den Anfang reicht das.
  await deleteDoc(doc(db, devicePath(deviceId)));
}

export async function getDevice(deviceId) {
  const snap = await getDoc(doc(db, devicePath(deviceId)));
  if (!snap.exists()) return null;
  return { id: snap.id, ...snap.data() };
}

export async function updateDevice(deviceId, patch) {
  await updateDoc(doc(db, devicePath(deviceId)), patch);
}

// ---------- Settings / Pumpen ----------
export async function getSettings(deviceId) {
  const snap = await getDoc(doc(db, `${devicePath(deviceId)}/config/settings`));
  return snap.exists() ? snap.data() : null;
}

export async function saveSettings(deviceId, settings) {
  await setDoc(doc(db, `${devicePath(deviceId)}/config/settings`), settings, { merge: true });
}

export async function getPumps(deviceId) {
  const pumps = [];
  for (let i = 0; i < 4; i++) {
    const snap = await getDoc(doc(db, `${devicePath(deviceId)}/config/pump-${i}`));
    pumps.push(snap.exists() ? { index: i, ...snap.data() } : { index: i, ...defaultPump(i) });
  }
  return pumps;
}

export async function savePump(deviceId, pumpIndex, pumpData) {
  await setDoc(doc(db, `${devicePath(deviceId)}/config/pump-${pumpIndex}`), pumpData, { merge: true });
}

// ---------- Messungen ----------
// type: "kh" | "ca" | "ph" | "auto-kh"
export async function addMeasurement(deviceId, { type, value, timestamp, source }) {
  await addDoc(collection(db, `${devicePath(deviceId)}/measurements`), {
    type, value, timestamp: timestamp || Math.floor(Date.now() / 1000),
    source: source || "manual",
    createdAt: serverTimestamp()
  });
}

export async function listMeasurements(deviceId, type, maxItems = 200) {
  const q = query(
    collection(db, `${devicePath(deviceId)}/measurements`),
    where("type", "==", type),
    orderBy("timestamp", "desc"),
    limit(maxItems)
  );
  const snap = await getDocs(q);
  const out = [];
  snap.forEach(d => out.push({ id: d.id, ...d.data() }));
  return out;
}

export async function deleteMeasurement(deviceId, measurementId) {
  await deleteDoc(doc(db, `${devicePath(deviceId)}/measurements/${measurementId}`));
}

// ---------- Dosierungen (Historie der Ausführungen) ----------
export async function addDosing(deviceId, { pump, ml, timestamp, isAutomatic, factor, dosageType, success }) {
  await addDoc(collection(db, `${devicePath(deviceId)}/dosings`), {
    pump, ml, timestamp: timestamp || Math.floor(Date.now() / 1000),
    isAutomatic: !!isAutomatic, factor: factor || 1, dosageType, success: success !== false,
    createdAt: serverTimestamp()
  });
}

export async function listDosings(deviceId, maxItems = 200) {
  const q = query(
    collection(db, `${devicePath(deviceId)}/dosings`),
    orderBy("timestamp", "desc"),
    limit(maxItems)
  );
  const snap = await getDocs(q);
  const out = [];
  snap.forEach(d => out.push({ id: d.id, ...d.data() }));
  return out;
}

// ---------- Pläne ----------
export async function getCurrentPlan(deviceId) {
  const snap = await getDoc(doc(db, `${devicePath(deviceId)}/plans/current`));
  return snap.exists() ? snap.data() : null;
}

export async function savePlan(deviceId, plan) {
  await setDoc(doc(db, `${devicePath(deviceId)}/plans/current`), {
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
    antiDripSpeedML: 3.6,
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

// Auth-Status für UI-Helper
export function getAuthInstance() {
  return auth;
}
