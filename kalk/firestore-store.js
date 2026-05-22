// =============================================================
// Firestore-Wrapper für Kalkmanagement (1 User = 1 Aquarium)
// =============================================================

import { initializeApp, getApps } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-app.js";
import { getAuth } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-auth.js";
import {
  getFirestore, collection, doc,
  getDoc, getDocs, setDoc, addDoc, deleteDoc,
  query, orderBy, limit, where,
  serverTimestamp
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-firestore.js";
import { firebaseConfig } from "../assets/firebase-config.js";

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

// ---------- Aquarium-Setup ----------
export async function hasAquarium() {
  const snap = await getDoc(doc(db, aquaPath("info")));
  return snap.exists();
}

export async function createAquarium({ name, aquariumVolume }) {
  const uid = requireUid();
  await setDoc(doc(db, `users/${uid}/aquarium/info`), {
    name: name || "Aquarium",
    createdAt: serverTimestamp(),
    online: false,
    firmware: null,
    lastSeen: null
  });
  await setDoc(doc(db, `users/${uid}/aquarium/settings`), defaultSettings(aquariumVolume));
  for (let i = 0; i < 4; i++) {
    await setDoc(doc(db, `users/${uid}/aquarium/pump-${i}`), defaultPump(i));
  }
}

export async function getAquariumInfo() {
  const snap = await getDoc(doc(db, aquaPath("info")));
  return snap.exists() ? snap.data() : null;
}

export async function updateAquariumInfo(patch) {
  await setDoc(doc(db, aquaPath("info")), patch, { merge: true });
}

// ---------- Einstellungen ----------
export async function getSettings() {
  const snap = await getDoc(doc(db, aquaPath("settings")));
  return snap.exists() ? snap.data() : null;
}

export async function saveSettings(settings) {
  await setDoc(doc(db, aquaPath("settings")), settings, { merge: true });
}

// ---------- Pumpen ----------
export async function getPumps() {
  const pumps = [];
  for (let i = 0; i < 4; i++) {
    const snap = await getDoc(doc(db, aquaPath(`pump-${i}`)));
    pumps.push(snap.exists() ? { index: i, ...snap.data() } : { index: i, ...defaultPump(i) });
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

export function getAuthInstance() {
  return auth;
}
