// =============================================================
// Dosierplan-Berechnung
// =============================================================
// Implementierung der Plan-Logik (portiert + erweitert aus dem
// Original-Sketch 088_sketch_sep3a_01.ino).
//
// - Plan-Frequenz wählbar via settings.dosingsPerDay (2,3,4,6,8,12)
//   → Intervall = 24 / dosingsPerDay Stunden
// - Plan beginnt bei der nächsten "Dosier-Stunde" nach jetzt
// - Maintenance-Dose = dailyConsumption / dosingsPerDay
// - Verbrauchs-Schätzung dynamisch aus Mess- + Dosierungs-Historie
//   (estimateDailyConsumption) mit Fallback auf initialKH/CaConsumption
// =============================================================

const KH_ML_PER_DKH_100L = 20.0;  // 20 ml KH-Lösung erhöhen 1°dKH in 100 L
const CA_ML_PER_MGL_100L = 1.0;   // 1 ml Ca-Lösung erhöht 1 mg/L in 100 L

// ---------- Umrechnungs-Faktoren ----------
export function khPerML(aquariumVolume) {
  // Wieviel °dKH bringt 1 ml KH-Lösung in dem Aquarium
  return (1.0 / KH_ML_PER_DKH_100L) * (100.0 / aquariumVolume);
}

export function caPerML(aquariumVolume) {
  // Wieviel mg/L bringt 1 ml Ca-Lösung in dem Aquarium
  return (1.0 / CA_ML_PER_MGL_100L) * (100.0 / aquariumVolume);
}

// ---------- Hilfsfunktionen ----------
// Liefert das Intervall (Stunden) zwischen Dosierungen aus den Settings.
// Default 2 h (= 12×/Tag). Erlaubte Werte: alle Teiler von 24 → 1,2,3,4,6,8,12,24.
function intervalHours(settings) {
  const n = parseInt(settings?.dosingsPerDay, 10);
  if (!n || n <= 0 || n > 24) return 2;
  return Math.floor(24 / n);
}

function getNextDosageHour(now, settings) {
  const intH = intervalHours(settings);
  // Nächste Stunde die durch intH teilbar ist, Minute 10 als Anker (wie im ESP)
  const d = new Date(now * 1000);
  let h = d.getHours();
  let plus = 0;
  const onMark = (h % intH === 0);
  if (onMark) {
    if (d.getMinutes() >= 10) plus = intH;
  } else {
    plus = intH - (h % intH);
  }
  d.setHours(h + plus, 10, 0, 0);
  return Math.floor(d.getTime() / 1000);
}

// ---------- KH-Plan ----------
export function calculateKHPlan({
  settings,        // SystemSettings-Objekt aus PocketBase
  currentKH,       // letzter gemessener KH-Wert
  dailyConsumption,// ml/Tag (aus Historie oder initialer Wert)
  anchorTime       // Unix-Timestamp Sek: Beginn der Rampe = Zeitpunkt der Messung
                   // (NICHT "jetzt" — sonst verschiebt sich der Plan bei jedem Neuberechnen)
} = {}) {
  if (!settings || !settings.aquariumVolume) {
    return { entries: [], maintenance: 0, warning: "Aquariumvolumen fehlt." };
  }
  const V = settings.aquariumVolume;
  const targetKH = settings.targetKH || 7.5;
  const maxDailyChange = settings.maxDailyChangeKH || 2.0;
  const dailyCons = dailyConsumption || settings.initialKHConsumption || 160;
  const startKH = (currentKH != null && !isNaN(currentKH)) ? currentKH : targetKH;
  const KpM = khPerML(V);

  // Anzahl Dosierungen pro Tag aus Settings (default 12)
  const dosesPerDay = parseInt(settings.dosingsPerDay, 10) || 12;
  const intH = intervalHours(settings);

  // Maintenance pro Intervall — Tages-Verbrauch / Anzahl Dosen
  const maintenanceML = dailyCons / dosesPerDay;

  // Bereits am Ziel?
  const diff = Math.abs(targetKH - startKH);
  if (diff < 0.05) {
    return {
      entries: [{ date: 0, dosageML: maintenanceML, projectedValue: startKH, isNightDosage: false, isMaintenanceDose: true }],
      maintenance: maintenanceML,
      target: targetKH,
      info: "Bereits am Ziel — nur Erhaltungsdosierung."
    };
  }

  const isIncrease = targetKH > startKH;
  const naturalConsumptionDKH = maintenanceML * KpM; // °dKH pro Intervall, der "von selbst" verloren geht
  const settingsMaxPerInterval = maxDailyChange / dosesPerDay;

  let maxChangePerInterval, warning = null;
  if (isIncrease) {
    maxChangePerInterval = settingsMaxPerInterval;
  } else {
    // Senkung: durch natürlichen Verbrauch begrenzt
    maxChangePerInterval = Math.min(settingsMaxPerInterval, naturalConsumptionDKH);
    if (naturalConsumptionDKH < settingsMaxPerInterval) {
      warning = `KH-Senkung durch natürlichen Verbrauch begrenzt: max ${(naturalConsumptionDKH * dosesPerDay).toFixed(2)} °dKH/Tag (statt eingestellte ${maxDailyChange})`;
    }
  }

  let numDosages, changePerDosage;
  if (isIncrease) {
    numDosages = Math.ceil(diff / maxChangePerInterval);
    changePerDosage = diff / numDosages;
  } else {
    const fullUnits = Math.floor(diff / maxChangePerInterval);
    const remainder = diff - fullUnits * maxChangePerInterval;
    numDosages = fullUnits + (remainder > 0.001 ? 1 : 0);
    changePerDosage = maxChangePerInterval;
  }

  // Plan-Einträge erstellen — Rampe startet am Messzeitpunkt (anchorTime)
  const entries = [];
  let currentTime = getNextDosageHour(anchorTime, settings);
  const intSec = intH * 3600;

  if (isIncrease) {
    const adjustmentML = changePerDosage / KpM;
    for (let i = 0; i < numDosages; i++) {
      const totalML = adjustmentML + maintenanceML;
      const projectedKH = startKH + (i + 1) * changePerDosage;
      entries.push({
        date: currentTime,
        dosageML: totalML,
        projectedValue: projectedKH,
        isNightDosage: false,    // Tag/Nacht wird zur Laufzeit vom ESP entschieden
        isMaintenanceDose: false
      });
      currentTime += intSec;
    }
  } else {
    let remaining = diff;
    for (let i = 0; i < numDosages; i++) {
      const required = Math.min(remaining, maxChangePerInterval);
      let finalDosage = 0;
      if (required <= naturalConsumptionDKH) {
        const dosageReduction = required / KpM;
        finalDosage = Math.max(0, maintenanceML - dosageReduction);
        remaining -= required;
      } else {
        remaining -= Math.min(remaining, naturalConsumptionDKH);
      }
      const totalReductionSoFar = diff - remaining;
      const projectedKH = Math.max(targetKH, startKH - totalReductionSoFar);
      entries.push({
        date: currentTime,
        dosageML: finalDosage,
        projectedValue: projectedKH,
        isNightDosage: false,
        isMaintenanceDose: false
      });
      currentTime += intSec;
      if (remaining <= 0.001) break;
    }
  }

  // Erhaltungsdosis am Ende
  entries.push({
    date: 0,
    dosageML: maintenanceML,
    projectedValue: targetKH,
    isNightDosage: false,
    isMaintenanceDose: true
  });

  return { entries, maintenance: maintenanceML, target: targetKH, warning, isIncrease, numDosages, durationDays: numDosages * intH / 24 };
}

// ---------- Ca-Plan (analog) ----------
export function calculateCaPlan({
  settings,
  currentCa,
  dailyConsumption,
  anchorTime       // Unix-Timestamp Sek: Rampen-Start = Messzeitpunkt (nicht "jetzt")
} = {}) {
  if (!settings || !settings.aquariumVolume) {
    return { entries: [], maintenance: 0, warning: "Aquariumvolumen fehlt." };
  }
  const V = settings.aquariumVolume;
  const targetCa = settings.targetCalcium || 420;
  const maxDailyChange = settings.maxDailyChangeCalcium || 20.0;
  const dailyCons = dailyConsumption || settings.initialCalciumConsumption || 60;
  const mgRatio = (settings.magnesiumRatio || 50) / 100;
  const startCa = (currentCa != null && !isNaN(currentCa)) ? currentCa : targetCa;
  const CpM = caPerML(V);
  const dosesPerDay = parseInt(settings.dosingsPerDay, 10) || 12;
  const intH = intervalHours(settings);
  const maintenanceCa = dailyCons / dosesPerDay;

  const diff = Math.abs(targetCa - startCa);
  if (diff < 0.5) {
    return {
      entries: [{ date: 0, caDosageML: maintenanceCa, mgDosageML: maintenanceCa * mgRatio, projectedValue: startCa, isMaintenanceDose: true }],
      maintenance: maintenanceCa,
      target: targetCa,
      info: "Bereits am Ziel — nur Erhaltungsdosierung."
    };
  }

  const isIncrease = targetCa > startCa;
  const naturalConsumptionMgL = maintenanceCa * CpM;
  const settingsMaxPerInterval = maxDailyChange / dosesPerDay;

  let maxChangePerInterval, warning = null;
  if (isIncrease) {
    maxChangePerInterval = settingsMaxPerInterval;
  } else {
    maxChangePerInterval = Math.min(settingsMaxPerInterval, naturalConsumptionMgL);
    if (naturalConsumptionMgL < settingsMaxPerInterval) {
      warning = `Ca-Senkung durch natürlichen Verbrauch begrenzt: max ${(naturalConsumptionMgL * dosesPerDay).toFixed(1)} mg/L/Tag (statt eingestellte ${maxDailyChange})`;
    }
  }

  let numDosages, changePerDosage;
  if (isIncrease) {
    numDosages = Math.ceil(diff / maxChangePerInterval);
    changePerDosage = diff / numDosages;
  } else {
    const fullUnits = Math.floor(diff / maxChangePerInterval);
    const remainder = diff - fullUnits * maxChangePerInterval;
    numDosages = fullUnits + (remainder > 0.001 ? 1 : 0);
    changePerDosage = maxChangePerInterval;
  }

  const entries = [];
  let currentTime = getNextDosageHour(anchorTime, settings);  // Rampe ab Messzeitpunkt
  const intSec = intH * 3600;

  if (isIncrease) {
    const adjustmentML = changePerDosage / CpM;
    for (let i = 0; i < numDosages; i++) {
      const totalCaML = adjustmentML + maintenanceCa;
      const projectedCa = startCa + (i + 1) * changePerDosage;
      entries.push({
        date: currentTime,
        caDosageML: totalCaML,
        mgDosageML: totalCaML * mgRatio,
        projectedValue: projectedCa,
        isMaintenanceDose: false
      });
      currentTime += intSec;
    }
  } else {
    let remaining = diff;
    for (let i = 0; i < numDosages; i++) {
      const required = Math.min(remaining, maxChangePerInterval);
      let finalCaDosage = 0;
      if (required <= naturalConsumptionMgL) {
        const dosageReduction = required / CpM;
        finalCaDosage = Math.max(0, maintenanceCa - dosageReduction);
        remaining -= required;
      } else {
        remaining -= Math.min(remaining, naturalConsumptionMgL);
      }
      const totalReductionSoFar = diff - remaining;
      const projectedCa = Math.max(targetCa, startCa - totalReductionSoFar);
      entries.push({
        date: currentTime,
        caDosageML: finalCaDosage,
        mgDosageML: finalCaDosage * mgRatio,
        projectedValue: projectedCa,
        isMaintenanceDose: false
      });
      currentTime += intSec;
      if (remaining <= 0.001) break;
    }
  }

  entries.push({
    date: 0,
    caDosageML: maintenanceCa,
    mgDosageML: maintenanceCa * mgRatio,
    projectedValue: targetCa,
    isMaintenanceDose: true
  });

  return { entries, maintenance: maintenanceCa, target: targetCa, warning, isIncrease, numDosages, durationDays: numDosages * intH / 24 };
}

// ---------- Verbrauchs-Schätzung aus Messhistorie ----------
// Portiert 1:1 aus 088_sketch_sep3a_01.ino — calculateConsumption() (Zeile 3992+):
//
//  - 0 oder 1 Messung: null → Caller nutzt settings.initialXxxConsumption
//  - 2 Messungen: direkte Berechnung über den gesamten Zeitraum
//  - 3+ Messungen: pro Intervall berechnen, dann gleichgewichtet mitteln
//
// KH-Nacht-Dosierungen werden mit Faktor 2 gewertet (Konzentrat ist 2×).
//
// Formel pro Intervall:
//   consumption = (dosagedML_im_intervall − valueDrift / perMlFactor) / days
// Bedeutet: ml die das Aquarium "natürlich" verbraucht (alles was dosiert wurde
// minus das, was als Wertänderung sichtbar wurde).
function dosagesInInterval(dosings, type, startTs, endTs) {
  let sum = 0;
  for (const d of dosings || []) {
    if (d.timestamp < startTs || d.timestamp >= endTs) continue;
    let multi = 0;
    if (type === "kh") {
      if (d.pump === 2) multi = 1;        // KH-Tag
      else if (d.pump === 3) multi = 2;   // KH-Nacht: 2× konzentriert
    } else if (type === "ca") {
      if (d.pump === 0) multi = 1;
    }
    sum += (d.ml || 0) * multi;
  }
  return sum;
}

export function estimateDailyConsumption({ measurements, dosings, type, perMlFactor, historyCount = 5 }) {
  if (!perMlFactor || perMlFactor <= 0) return null;
  const sorted = (measurements || [])
    .filter(m => m.type === type)
    .sort((a, b) => a.timestamp - b.timestamp);

  // 0 oder 1 Messung: Caller soll initialKHConsumption nutzen
  if (sorted.length < 2) return null;

  // 2 Messungen: direkter Verbrauch
  if (sorted.length === 2) {
    const a = sorted[0], b = sorted[1];
    const days = (b.timestamp - a.timestamp) / 86400;
    if (days <= 0) return null;
    const dosed = dosagesInInterval(dosings, type, a.timestamp, b.timestamp);
    const drift = b.value - a.value;
    const c = (dosed - (drift / perMlFactor)) / days;
    return (isFinite(c) && c > 0) ? c : null;
  }

  // 3+ Messungen: per-Intervall berechnen, gleichgewichtet mitteln
  const used = Math.min(sorted.length, historyCount);
  const startIdx = Math.max(0, sorted.length - used);
  const window = sorted.slice(startIdx);
  const intervalConsumptions = [];
  for (let i = 1; i < window.length; i++) {
    const a = window[i - 1], b = window[i];
    const days = (b.timestamp - a.timestamp) / 86400;
    if (days <= 0) continue;
    const dosed = dosagesInInterval(dosings, type, a.timestamp, b.timestamp);
    const drift = b.value - a.value;
    const c = (dosed - (drift / perMlFactor)) / days;
    if (isFinite(c)) intervalConsumptions.push(c);
  }
  if (intervalConsumptions.length === 0) return null;
  const avg = intervalConsumptions.reduce((s, x) => s + x, 0) / intervalConsumptions.length;
  return avg > 0 ? avg : null;
}
