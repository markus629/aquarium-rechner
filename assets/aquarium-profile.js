// =============================================================
// Zentrales Aquarium-Profil (geteilt über alle Rechner)
// =============================================================
// Speichert gemeinsame Fakten (aktuell: Beckenvolumen in Liter) EINMAL pro
// Nutzer in der bereits geteilten Collection "calculators" unter key="aquarium".
// Jeder Rechner liest den Wert beim Laden und schreibt ihn bei Änderung zurück,
// damit das Volumen-Feld überall denselben Wert zeigt.
// =============================================================

import { pb } from "/assets/pb-client.js";

const KEY = "aquarium";

async function findDoc() {
  const uid = pb.authStore.record.id;
  return await pb.collection("calculators").getFirstListItem(`user="${uid}" && key="${KEY}"`);
}

// Liefert das zentrale Volumen (Liter) oder null, wenn keins gesetzt/nicht eingeloggt.
export async function getAquariumLiters() {
  if (!pb.authStore.isValid) return null;
  try {
    const rec = await findDoc();
    const v = rec && rec.data ? rec.data.liters : null;
    return (v != null && !isNaN(v)) ? Number(v) : null;
  } catch (_) {
    return null; // 404 = noch nicht gesetzt
  }
}

// Setzt das zentrale Volumen. No-op, wenn unverändert oder ungültig.
export async function setAquariumLiters(liters) {
  if (!pb.authStore.isValid) return;
  const v = Number(liters);
  if (!(v > 0)) return;
  const uid = pb.authStore.record.id;
  let rec = null;
  try { rec = await findDoc(); } catch (_) { rec = null; }
  if (rec) {
    if (rec.data && Number(rec.data.liters) === v) return; // keine Änderung
    await pb.collection("calculators").update(rec.id, { data: { ...(rec.data || {}), liters: v } });
  } else {
    await pb.collection("calculators").create({ user: uid, key: KEY, data: { liters: v } });
  }
}
