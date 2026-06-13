/// <reference path="../pb_data/types.d.ts" />
// =============================================================
// ESP-Ausfall-Watchdog (Totmann-Schalter)
// =============================================================
// Prüft alle 2 Minuten den letzten Heartbeat jedes ESP
// (aqua_docs key="info", Feld data.lastSeen, Unix-Sekunden).
// Kommt seit > 5 Min keiner mehr → E-Mail-Alarm an den Besitzer.
// Kommt er zurück → Entwarnung. Alarm-Status liegt persistent in
// aqua_docs key="_watchdog" (vom ESP nie überschrieben) → keine
// Mail-Flut, neustart-fest.
// =============================================================

cronAdd("esp_watchdog", "*/2 * * * *", () => {
  const DOWN_AFTER_SEC = 300; // 5 Minuten
  const now = Math.floor(Date.now() / 1000);

  const asObj = (v) => {
    if (typeof v === "string") { try { return JSON.parse(v); } catch (_) { return {}; } }
    return v || {};
  };

  let infos = [];
  try {
    infos = $app.findRecordsByFilter("aqua_docs", "key = 'info'", "", 200, 0);
  } catch (e) {
    console.log("[watchdog] Abfrage-Fehler:", e);
    return;
  }

  for (const rec of infos) {
    const userId = rec.get("user");
    const data = asObj(rec.get("data"));
    const lastSeen = data.lastSeen ? Number(data.lastSeen) : 0;
    if (!lastSeen) continue;

    const age = now - lastSeen;
    const isDown = age > DOWN_AFTER_SEC;

    // Opt-out: nur mailen, wenn der Nutzer espOfflineAlert nicht abgeschaltet hat.
    // (Kein ESP = kein info-Doc = wir landen hier gar nicht erst.)
    let alertEnabled = true;
    try {
      const s = $app.findFirstRecordByFilter("aqua_docs", "user = {:u} && key = 'settings'", { u: userId });
      if (asObj(s.get("data")).espOfflineAlert === false) alertEnabled = false;
    } catch (_) { /* keine Settings gespeichert → Default an */ }
    if (!alertEnabled) continue;

    // Persistenter Alarm-Status
    let wd = null;
    try {
      wd = $app.findFirstRecordByFilter("aqua_docs", "user = {:u} && key = '_watchdog'", { u: userId });
    } catch (_) { wd = null; }
    const wasDown = wd ? !!asObj(wd.get("data")).down : false;

    console.log(`[watchdog] user=${userId} lastSeen vor ${age}s down=${isDown} wasDown=${wasDown}`);

    if (isDown === wasDown) continue; // keine Zustandsänderung → nichts tun

    // Status persistieren
    try {
      if (!wd) {
        const col = $app.findCollectionByNameOrId("aqua_docs");
        wd = new Record(col);
        wd.set("user", userId);
        wd.set("key", "_watchdog");
      }
      wd.set("data", { down: isDown, changedAt: now });
      $app.save(wd);
    } catch (e) {
      console.log("[watchdog] Status-Speichern fehlgeschlagen:", e);
    }

    // Benachrichtigung per E-Mail
    try {
      const user = $app.findRecordById("users", userId);
      const email = user.get("email");
      if (!email) continue;
      const mins = Math.round(age / 60);
      const msg = new MailerMessage({
        from: { address: "markus@strogg.de", name: "Aquarium Rechner" },
        to: [{ address: email }],
        subject: isDown ? "⚠️ Aquarium-ESP offline" : "✅ Aquarium-ESP wieder online",
        text: isDown
          ? `Dein Aquarium-Rechner-ESP hat seit ~${mins} Minuten keinen Heartbeat mehr geschickt.\n\nMögliche Ursachen: Stromausfall, WLAN-Problem oder Absturz. Bitte prüfen.\n\nHinweis: Dank lokalem Plan-Cache dosiert der ESP evtl. weiter, meldet sich aber nicht mehr am Server.`
          : `Entwarnung: Dein Aquarium-Rechner-ESP meldet sich wieder am Server.`,
      });
      $app.newMailClient().send(msg);
      console.log(`[watchdog] Mail an ${email} gesendet (${isDown ? "OFFLINE" : "online"})`);
    } catch (e) {
      console.log("[watchdog] Mail-Fehler:", e);
    }
  }
});
