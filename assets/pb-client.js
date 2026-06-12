// =============================================================
// Gemeinsame PocketBase-Instanz für alle Rechner
// =============================================================
// Ersetzt die frühere Firebase-Initialisierung. Eine einzige
// Instanz über den Modul-Cache — auth-sync.js und der Kalk-Store
// teilen sich denselben Login-Zustand (in localStorage persistiert).
//
// Die Web-App wird von PocketBase selbst ausgeliefert (pb_public/),
// daher ist die API gleich-origin: window.location.origin.
// =============================================================

import PocketBase from "https://cdn.jsdelivr.net/npm/pocketbase@0.27.0/dist/pocketbase.es.mjs";

export const pb = new PocketBase(window.location.origin);

// Firebase hat parallele Reads nicht abgebrochen — wir wollen dasselbe
// Verhalten, sonst werden gleichzeitige Anfragen mit gleichem Key gecancelt.
pb.autoCancellation(false);
