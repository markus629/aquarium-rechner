// =============================================================
// Zentrale Vorhersage „Nächster Test" + einheitliche Texte.
// Eine Quelle für: Startseiten-Kacheln, Kalk-Rechner, Nährstoff-Rechner.
//
// Mathematik: Drift der letzten 2 Messungen gegen den rechnerischen Soll
// (vom vorletzten Wert in max-Schritten Richtung Ziel — gilt auch in der
// Rampe) → Zeit bis zur Min/Max-Grenze, gedeckelt aufs Max-Messintervall.
//
// Fünf Zustände (überall identisch):
//   1. zu wenig Daten  → null            (Platzhalter zeigt Beispielwert)
//   2. außerhalb Band  → out:true        → „jetzt"
//   3. sehr bald       → days < 1        → „heute"
//   4. normal          → days ≥ 1        → „in X Tagen"
//   5. sehr stabil     → capped:true     → „in {maxMeas} Tagen (Max-Intervall)"
// =============================================================
const DAY = 86400;

// items: Messungen ABSTEIGEND sortiert (items[0] = neueste), je {timestamp, value}.
// Liefert null (zu wenig Daten) oder { days, out, capped, drift }.
export function predictNextTest(items, { target, min, max, maxChangePerDay, maxMeasureDays } = {}) {
  const maxMeas = Math.max(1, parseFloat(maxMeasureDays) || 7);
  if (!items || items.length < 2) return null;
  const b = items[0], a = items[1];
  const dt = (b.timestamp - a.timestamp) / DAY;
  if (dt <= 0.05) return null;
  if (b.value < min || b.value > max) return { days: 0, out: true, capped: false, drift: null };
  const gap = target - a.value;
  const soll = a.value + Math.sign(gap) * Math.min(Math.abs(gap), (maxChangePerDay || 0) * dt);
  const drift = (b.value - soll) / dt;            // System-Ungenauigkeit pro Tag
  let edge = Infinity;
  if (drift < -1e-9) edge = (b.value - min) / (-drift);   // fällt → Richtung Min
  else if (drift > 1e-9) edge = (max - b.value) / drift;  // steigt → Richtung Max
  if (!(edge >= 0)) edge = Infinity;
  return { days: Math.min(maxMeas, edge), out: false, capped: edge > maxMeas, drift };
}

// Kurztext fürs Eingabefeld (Platzhalter). fallback = Beispielwert bei zu wenig Daten.
export function nextTestPlaceholder(pred, fallback) {
  if (!pred) return fallback;
  if (pred.out) return "jetzt messen!";
  if (pred.days < 1) return "heute messen";
  const n = Math.round(pred.days);
  return `in ${n} Tag${n === 1 ? "" : "en"} messen`;
}

// Lange „Nächster Test: …"-Zeile (inneres HTML, ohne Wrapper) für die Plan-Seiten.
// opts: { dp, unit, min, max }
export function nextTestText(pred, { dp = 1, unit = "", min, max } = {}) {
  const muted = "color:var(--text-light)";
  const f = (v) => Number(v).toFixed(dp);
  if (!pred) return `Nächster Test: <span style="${muted}">– (mind. 2 Messwerte nötig)</span>`;
  if (pred.out) return `Nächster Test: <strong style="color:var(--red)">jetzt</strong> — Wert außerhalb ${f(min)}–${f(max)} ${unit}`;
  const d = new Date(Date.now() + pred.days * DAY * 1000).toLocaleDateString("de-DE", { day: "2-digit", month: "2-digit" });
  const driftTxt = pred.drift == null ? "" :
    ` <span style="${muted}">· Ungenauigkeit ${pred.drift > 0 ? "+" : ""}${Number(pred.drift).toFixed(dp + 1)} ${unit}/Tag</span>`;
  if (pred.days < 1) return `Nächster Test: <strong>heute</strong> <span style="${muted}">(${d})</span>${driftTxt}`;
  if (pred.capped)   return `Nächster Test: <strong>in ${Math.round(pred.days)} Tagen</strong> <span style="${muted}">(${d}, Max-Intervall — sehr stabil)</span>${driftTxt}`;
  return `Nächster Test: <strong>in ${pred.days.toFixed(1)} Tagen</strong> <span style="${muted}">(${d})</span>${driftTxt}`;
}
