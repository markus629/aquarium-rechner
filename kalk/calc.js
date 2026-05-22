// =============================================================
// Dosierplan-Berechnung — portiert aus ESP-Code (.ino)
// =============================================================
// Vereinfachte aber korrekte Implementierung der Plan-Logik.
// Quelle: 088_sketch_sep3a_01.ino Zeilen 3774-5660
//
// Annahmen:
// - Plan beginnt bei der nächsten "geraden Stunde" nach jetzt
// - Pro Tag 12 Intervalle (alle 2 Stunden)
// - Verbrauchsrate kommt aus den Initialwerten in Settings
//   (dynamische Verbrauchs-Schätzung aus Historie folgt später)
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
function getNextEvenHour(now) {
  // Nächste gerade Stunde mit Minute 10 als Anker (wie im ESP)
  const d = new Date(now * 1000);
  let h = d.getHours();
  let plus = 0;
  if (h % 2 === 0) {
    if (d.getMinutes() >= 10) plus = 2;
  } else {
    plus = 1;
  }
  d.setHours(h + plus, 10, 0, 0);
  return Math.floor(d.getTime() / 1000);
}

// ---------- KH-Plan ----------
export function calculateKHPlan({
  settings,        // SystemSettings-Objekt aus Firestore
  currentKH,       // letzter gemessener KH-Wert
  dailyConsumption,// ml/Tag (aus Historie oder initialer Wert)
  now              // Unix-Timestamp Sekunden
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

  // Maintenance pro Intervall (12 Intervalle pro Tag)
  const maintenanceML = dailyCons / 12.0;

  // Bereits am Ziel?
  const diff = Math.abs(targetKH - startKH);
  if (diff < 0.05) {
    return {
      entries: [{ date: 0, dosageML: maintenanceML, projectedValue: startKH, isNightDosage: false, isMaintenanceDose: true }],
      maintenance: maintenanceML,
      info: "Bereits am Ziel — nur Erhaltungsdosierung."
    };
  }

  const isIncrease = targetKH > startKH;
  const naturalConsumptionDKH = maintenanceML * KpM; // °dKH pro Intervall, der "von selbst" verloren geht
  const settingsMaxPerInterval = maxDailyChange / 12.0;

  let maxChangePerInterval, warning = null;
  if (isIncrease) {
    maxChangePerInterval = settingsMaxPerInterval;
  } else {
    // Senkung: durch natürlichen Verbrauch begrenzt
    maxChangePerInterval = Math.min(settingsMaxPerInterval, naturalConsumptionDKH);
    if (naturalConsumptionDKH < settingsMaxPerInterval) {
      warning = `KH-Senkung durch natürlichen Verbrauch begrenzt: max ${(naturalConsumptionDKH * 12).toFixed(2)} °dKH/Tag (statt eingestellte ${maxDailyChange})`;
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

  // Plan-Einträge erstellen
  const entries = [];
  let currentTime = getNextEvenHour(now);

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
      currentTime += 2 * 3600;
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
      currentTime += 2 * 3600;
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

  return { entries, maintenance: maintenanceML, warning, isIncrease, numDosages, durationDays: numDosages * 2 / 24 };
}

// ---------- Ca-Plan (analog) ----------
export function calculateCaPlan({
  settings,
  currentCa,
  dailyConsumption,
  now
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
  const maintenanceCa = dailyCons / 12.0;

  const diff = Math.abs(targetCa - startCa);
  if (diff < 0.5) {
    return {
      entries: [{ date: 0, caDosageML: maintenanceCa, mgDosageML: maintenanceCa * mgRatio, projectedValue: startCa, isMaintenanceDose: true }],
      maintenance: maintenanceCa,
      info: "Bereits am Ziel — nur Erhaltungsdosierung."
    };
  }

  const isIncrease = targetCa > startCa;
  const naturalConsumptionMgL = maintenanceCa * CpM;
  const settingsMaxPerInterval = maxDailyChange / 12.0;

  let maxChangePerInterval, warning = null;
  if (isIncrease) {
    maxChangePerInterval = settingsMaxPerInterval;
  } else {
    maxChangePerInterval = Math.min(settingsMaxPerInterval, naturalConsumptionMgL);
    if (naturalConsumptionMgL < settingsMaxPerInterval) {
      warning = `Ca-Senkung durch natürlichen Verbrauch begrenzt: max ${(naturalConsumptionMgL * 12).toFixed(1)} mg/L/Tag (statt eingestellte ${maxDailyChange})`;
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
  let currentTime = getNextEvenHour(now);

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
      currentTime += 2 * 3600;
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
      currentTime += 2 * 3600;
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

  return { entries, maintenance: maintenanceCa, warning, isIncrease, numDosages, durationDays: numDosages * 2 / 24 };
}

// ---------- Verbrauchs-Schätzung aus Messhistorie ----------
// Liefert ml/Tag basierend auf den letzten N Messungen und dosierten Mengen.
// Wenn zu wenig Daten: null (Caller verwendet dann initialKHConsumption).
//
// Vereinfacht: nimmt erste und letzte Messung, errechnet linearen Drift,
// und addiert die in dem Intervall dosierte Menge zurück.
export function estimateDailyConsumption({ measurements, dosings, type, perMlFactor, historyCount = 5 }) {
  const sorted = (measurements || [])
    .filter(m => m.type === type)
    .sort((a, b) => a.timestamp - b.timestamp);
  if (sorted.length < 2) return null;
  const recent = sorted.slice(-historyCount);
  const first = recent[0], last = recent[recent.length - 1];
  const elapsedDays = (last.timestamp - first.timestamp) / 86400;
  if (elapsedDays < 0.5) return null;

  const drift = last.value - first.value;            // wert-änderung
  // Dosierungen in dem Zeitraum
  const ml = (dosings || [])
    .filter(d => d.timestamp >= first.timestamp && d.timestamp <= last.timestamp)
    .filter(d => {
      // KH: Pumpe 2 (Tag) + 3 (Nacht); Ca: Pumpe 0
      if (type === "kh") return d.pump === 2 || d.pump === 3;
      if (type === "ca") return d.pump === 0;
      return false;
    })
    .reduce((s, d) => s + (d.ml || 0), 0);

  // verbrauch_value = ml_dosiert × perMlFactor − drift
  // verbrauch_ml/Tag = verbrauch_value / (perMlFactor × elapsedDays)
  const consumedValue = (ml * perMlFactor) - drift;
  const dailyConsumption = consumedValue / (perMlFactor * elapsedDays);
  if (!isFinite(dailyConsumption) || dailyConsumption < 0) return null;
  return dailyConsumption;
}
