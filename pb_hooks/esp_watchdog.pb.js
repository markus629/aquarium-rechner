/// <reference path="../pb_data/types.d.ts" />
// =============================================================
// ESP-Ausfall-Watchdog (Totmann-Schalter)
// =============================================================
// Prüft alle 2 Minuten den letzten Heartbeat JEDES Geräts in der
// `devices`-Collection (Feld lastSeen, Unix-Sekunden). Diese wird vom
// ESP in JEDER Rolle aktualisiert (Kalk, Nährstoff, Leerlauf) — daher
// die zuverlässige, rollen-unabhängige Quelle.
//
// Kommt seit > 5 Min kein Heartbeat → E-Mail-Alarm an den Besitzer.
// Kommt das Gerät zurück → Entwarnung. Der Alarm-Status liegt pro Gerät
// persistent in aqua_docs key="_watchdog" (data = { <deviceId>: true/false })
// → keine Mail-Flut, neustart-fest, mehrere Geräte unabhängig.
// =============================================================

cronAdd("esp_watchdog", "*/2 * * * *", () => {
  const DOWN_AFTER_SEC = 300; // 5 Minuten
  const now = Math.floor(Date.now() / 1000);

  // Robust gegen die verschiedenen Rückgabeformen von record.get(jsonField) in der
  // PB-JSVM (String, JsonRaw/[]byte, bereits dekodiertes Objekt).
  const asObj = (v) => {
    if (v == null) return {};
    if (typeof v === "string") { try { return JSON.parse(v); } catch (_) { return {}; } }
    try { const s = String(v); if (s && (s.charAt(0) === "{" || s.charAt(0) === "[")) return JSON.parse(s); } catch (_) {}
    return (typeof v === "object") ? v : {};
  };

  let devices = [];
  try {
    devices = $app.findRecordsByFilter("devices", "lastSeen > 0", "", 200, 0);
  } catch (e) {
    console.log("[watchdog] devices-Abfrage-Fehler:", e);
    return;
  }

  for (const dev of devices) {
    const userId = dev.get("user");
    const lastSeen = Number(dev.get("lastSeen")) || 0;
    if (!userId || !lastSeen) continue;
    const role = (dev.get("role") || "").toString();
    const name = (dev.get("name") || "").toString();
    const deviceId = dev.id;

    const age = now - lastSeen;
    const isDown = age > DOWN_AFTER_SEC;

    // Opt-out: Settings der jeweiligen Rolle (Nährstoff → nutri_docs, sonst aqua_docs).
    const settingsCol = role === "nutrient" ? "nutri_docs" : "aqua_docs";
    let alertEnabled = true;
    try {
      const s = $app.findFirstRecordByFilter(settingsCol, "user = {:u} && key = 'settings'", { u: userId });
      if (asObj(s.get("data")).espOfflineAlert === false) alertEnabled = false;
    } catch (_) { /* keine Settings → Default an */ }
    if (!alertEnabled) continue;

    // Persistenter Alarm-Status pro Gerät (in aqua_docs key="_watchdog")
    let wd = null;
    try {
      wd = $app.findFirstRecordByFilter("aqua_docs", "user = {:u} && key = '_watchdog'", { u: userId });
    } catch (_) { wd = null; }
    const wdData = wd ? asObj(wd.get("data")) : {};
    const wasDown = wdData[deviceId] === true;

    console.log(`[watchdog] device=${deviceId} role=${role} lastSeen vor ${age}s down=${isDown} wasDown=${wasDown}`);

    if (isDown === wasDown) continue; // keine Zustandsänderung → nichts tun

    // Status persistieren
    try {
      if (!wd) {
        const col = $app.findCollectionByNameOrId("aqua_docs");
        wd = new Record(col);
        wd.set("user", userId);
        wd.set("key", "_watchdog");
      }
      wdData[deviceId] = isDown;
      wd.set("data", JSON.stringify(wdData));   // als String → zuverlässig wieder lesbar
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
      const roleLabel = role === "nutrient" ? "Nährstoff" : (role === "kalk" ? "Kalk" : "");
      const devLabel = name ? `„${name}"` : (roleLabel ? `${roleLabel}-ESP` : "ESP");
      const msg = new MailerMessage({
        from: { address: "markus@strogg.de", name: "Aquarium Rechner" },
        to: [{ address: email }],
        subject: isDown ? `⚠️ ${devLabel} offline` : `✅ ${devLabel} wieder online`,
        text: isDown
          ? `Dein Aquarium-Rechner-Gerät ${devLabel}${roleLabel ? ` (Rolle: ${roleLabel})` : ""} hat seit ~${mins} Minuten keinen Heartbeat mehr geschickt.\n\nMögliche Ursachen: Stromausfall, WLAN-Problem oder Absturz. Bitte prüfen.\n\nHinweis: Dank lokalem Plan-Cache dosiert der ESP evtl. weiter, meldet sich aber nicht mehr am Server.`
          : `Entwarnung: Dein Gerät ${devLabel} meldet sich wieder am Server.`,
      });
      $app.newMailClient().send(msg);
      console.log(`[watchdog] Mail an ${email}: ${devLabel} ${isDown ? "OFFLINE" : "online"}`);
    } catch (e) {
      console.log("[watchdog] Mail-Fehler:", e);
    }
  }
});
