// =============================================================
// Gemeinsame PocketBase-Instanz für alle Rechner
// =============================================================
// Eine einzige Instanz über den Modul-Cache — auth-sync.js und der Kalk-Store
// teilen sich denselben Login-Zustand (in localStorage persistiert).
//
// Die Web-App wird von PocketBase selbst ausgeliefert (pb_public/),
// daher ist die API gleich-origin: window.location.origin.
// =============================================================

import PocketBase from "https://cdn.jsdelivr.net/npm/pocketbase@0.27.0/dist/pocketbase.es.mjs";

export const pb = new PocketBase(window.location.origin);

// Parallele Reads mit gleichem Key nicht abbrechen lassen, sonst
// werden gleichzeitige Anfragen gecancelt.
pb.autoCancellation(false);
