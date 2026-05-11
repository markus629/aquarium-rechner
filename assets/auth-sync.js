// =============================================================
// Gemeinsame Auth + Firestore Sync für alle Rechner
// =============================================================
// Verwendung in einer Rechner-HTML-Datei:
//
//   import { initAuthSync } from "../assets/auth-sync.js";
//   const sync = await initAuthSync({
//     calculatorId: "cr-rechner",          // oder "spurenrechner" etc.
//     calculatorName: "C&R Rechner",       // Anzeigename im Login-UI
//     getLocalState: () => collectState(), // Aktuellen State sammeln
//     applyState: (s) => applyState(s),    // State in UI/Storage anwenden
//     hostElementSelector: "header",       // wohin das Auth-UI gemountet wird
//   });
//
//   // Bei jeder Änderung (statt nur localStorage):
//   sync.save();   // Speichert lokal + Cloud (falls eingeloggt)
//
// =============================================================

import { initializeApp } from "https://www.gstatic.com/firebasejs/10.13.2/firebase-app.js";
import {
  getAuth, onAuthStateChanged,
  GoogleAuthProvider, signInWithPopup,
  createUserWithEmailAndPassword, signInWithEmailAndPassword,
  sendEmailVerification, sendPasswordResetEmail,
  signOut
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-auth.js";
import {
  getFirestore, doc, getDoc, setDoc, serverTimestamp
} from "https://www.gstatic.com/firebasejs/10.13.2/firebase-firestore.js";
import { firebaseConfig } from "./firebase-config.js";

// ---------- Firebase init (singleton über Module-Cache) ----------
const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getFirestore(app);

// ---------- Toast / Modal Helfer (selbst-gestylt, keine deps) ----------
function ensureStyles() {
  if (document.getElementById("auth-sync-styles")) return;
  const s = document.createElement("style");
  s.id = "auth-sync-styles";
  s.textContent = `
    .as-btn { position:absolute; top:1rem; right:1rem; min-width:36px; height:36px; padding:0 0.6rem; display:flex; align-items:center; gap:0.4rem; background:rgba(255,255,255,0.15); border:none; border-radius:8px; cursor:pointer; color:#fff; font-size:0.85rem; font-weight:600; font-family:inherit; transition:background 0.15s; z-index:2; }
    .as-btn:hover { background:rgba(255,255,255,0.28); }
    .as-btn svg { width:16px; height:16px; }
    .as-avatar { width:24px; height:24px; border-radius:50%; background:#fff; color:#2c3e50; display:inline-flex; align-items:center; justify-content:center; font-size:0.7rem; font-weight:700; }
    .as-overlay { position:fixed; inset:0; background:rgba(0,0,0,0.45); z-index:1000; display:flex; align-items:center; justify-content:center; padding:1rem; animation:as-fade 0.15s ease; }
    @keyframes as-fade { from{opacity:0} to{opacity:1} }
    .as-modal { background:#fff; border-radius:14px; padding:1.5rem; max-width:380px; width:100%; box-shadow:0 10px 40px rgba(0,0,0,0.2); animation:as-slide 0.2s ease; }
    @keyframes as-slide { from{transform:translateY(-10px);opacity:0} to{transform:translateY(0);opacity:1} }
    .as-modal h3 { margin:0 0 0.25rem; font-size:1.2rem; color:#2c3e50; }
    .as-modal p.sub { color:#7a8896; font-size:0.85rem; margin-bottom:1rem; }
    .as-modal label { display:block; font-size:0.78rem; font-weight:600; color:#5a6776; margin-bottom:0.3rem; margin-top:0.7rem; }
    .as-modal input[type=email], .as-modal input[type=password] { width:100%; padding:0.55rem 0.7rem; border:2px solid #e1e8ed; border-radius:8px; font-size:0.95rem; font-family:inherit; box-sizing:border-box; }
    .as-modal input:focus { outline:none; border-color:#3498db; }
    .as-modal .as-btn-primary { width:100%; padding:0.7rem; background:#3498db; color:#fff; border:none; border-radius:8px; font-weight:700; cursor:pointer; margin-top:1rem; font-size:0.95rem; font-family:inherit; }
    .as-modal .as-btn-primary:hover { background:#2980b9; }
    .as-modal .as-btn-primary:disabled { background:#aabbcc; cursor:not-allowed; }
    .as-modal .as-btn-google { width:100%; padding:0.65rem; background:#fff; color:#3c4043; border:1px solid #dadce0; border-radius:8px; font-weight:600; cursor:pointer; display:flex; align-items:center; justify-content:center; gap:0.6rem; font-size:0.9rem; font-family:inherit; }
    .as-modal .as-btn-google:hover { background:#f8f9fa; }
    .as-modal .as-divider { display:flex; align-items:center; gap:0.6rem; margin:1rem 0; color:#aabbcc; font-size:0.75rem; }
    .as-modal .as-divider::before, .as-modal .as-divider::after { content:""; flex:1; height:1px; background:#e1e8ed; }
    .as-modal .as-tabs { display:flex; gap:0.5rem; margin-bottom:0.5rem; }
    .as-modal .as-tab { flex:1; padding:0.4rem; border:none; background:none; cursor:pointer; font-weight:600; color:#7a8896; border-bottom:2px solid transparent; font-family:inherit; font-size:0.9rem; }
    .as-modal .as-tab.active { color:#3498db; border-bottom-color:#3498db; }
    .as-modal .as-msg { font-size:0.82rem; margin-top:0.6rem; min-height:1.2em; }
    .as-modal .as-msg.error { color:#c0392b; }
    .as-modal .as-msg.success { color:#27ae60; }
    .as-modal .as-msg.info { color:#3498db; }
    .as-modal .as-close { position:absolute; top:0.6rem; right:0.6rem; width:30px; height:30px; border:none; background:none; cursor:pointer; font-size:1.4rem; color:#7a8896; border-radius:50%; }
    .as-modal .as-close:hover { background:#f0f4f8; }
    .as-modal .as-footer-link { font-size:0.78rem; color:#7a8896; text-align:center; margin-top:0.8rem; }
    .as-modal .as-footer-link a { color:#3498db; text-decoration:none; cursor:pointer; }
    .as-toast { position:fixed; bottom:1rem; left:50%; transform:translateX(-50%); background:#2c3e50; color:#fff; padding:0.7rem 1.2rem; border-radius:8px; font-size:0.9rem; box-shadow:0 4px 16px rgba(0,0,0,0.2); z-index:1001; animation:as-toast-in 0.2s ease; }
    .as-toast.error { background:#c0392b; }
    .as-toast.success { background:#27ae60; }
    @keyframes as-toast-in { from{opacity:0;transform:translate(-50%,10px)} to{opacity:1;transform:translate(-50%,0)} }
    .as-menu { position:absolute; top:calc(100% + 6px); right:0; background:#fff; border-radius:10px; box-shadow:0 6px 20px rgba(0,0,0,0.15); min-width:200px; padding:0.4rem; z-index:5; color:#2c3e50; }
    .as-menu-item { display:flex; align-items:center; gap:0.5rem; padding:0.55rem 0.7rem; border-radius:6px; cursor:pointer; font-size:0.9rem; font-weight:500; }
    .as-menu-item:hover { background:#f0f4f8; }
    .as-menu-info { padding:0.6rem 0.7rem; border-bottom:1px solid #f0f4f8; margin-bottom:0.3rem; font-size:0.8rem; color:#7a8896; }
    .as-menu-info strong { display:block; color:#2c3e50; font-size:0.88rem; margin-bottom:0.1rem; word-break:break-all; }
    .as-sync-status { font-size:0.7rem; color:#7a8896; margin-top:0.2rem; }
    .as-sync-status.synced { color:#27ae60; }
  `;
  document.head.appendChild(s);
}

function toast(msg, type = "") {
  const t = document.createElement("div");
  t.className = "as-toast" + (type ? " " + type : "");
  t.textContent = msg;
  document.body.appendChild(t);
  setTimeout(() => { t.style.opacity = "0"; setTimeout(() => t.remove(), 300); }, 2800);
}

// ---------- Auth UI: Login-Modal ----------
function openLoginModal(calculatorName) {
  return new Promise((resolve) => {
    let mode = "login"; // "login" | "signup" | "reset"
    const overlay = document.createElement("div");
    overlay.className = "as-overlay";
    const closeModal = (result) => { overlay.remove(); resolve(result); };
    overlay.addEventListener("click", (e) => { if (e.target === overlay) closeModal(null); });

    const render = () => {
      const isLogin = mode === "login";
      const isSignup = mode === "signup";
      const isReset = mode === "reset";
      overlay.innerHTML = `
        <div class="as-modal" style="position:relative">
          <button class="as-close" title="Schließen">&times;</button>
          <h3>${isReset ? "Passwort zurücksetzen" : "Anmelden"}</h3>
          <p class="sub">${isReset ? "Wir schicken dir einen Link per E-Mail." : `${calculatorName} — Daten in der Cloud sichern und auf jedem Gerät verwenden.`}</p>

          ${!isReset ? `
            <button class="as-btn-google" id="as-google">
              <svg width="18" height="18" viewBox="0 0 24 24"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92c-.26 1.37-1.04 2.53-2.21 3.31v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.09z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.07H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.93l2.85-2.22.81-.62z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.07l3.66 2.84c.87-2.6 3.3-4.53 6.16-4.53z"/></svg>
              Mit Google fortfahren
            </button>
            <div class="as-divider">oder</div>
          ` : ""}

          ${isSignup ? `<div class="as-tabs"><button class="as-tab" data-mode="login">Anmelden</button><button class="as-tab active" data-mode="signup">Registrieren</button></div>` : ""}
          ${isLogin ? `<div class="as-tabs"><button class="as-tab active" data-mode="login">Anmelden</button><button class="as-tab" data-mode="signup">Registrieren</button></div>` : ""}

          <label>E-Mail</label>
          <input type="email" id="as-email" autocomplete="email" placeholder="du@beispiel.de">
          ${!isReset ? `
            <label>Passwort</label>
            <input type="password" id="as-pass" autocomplete="${isSignup ? "new-password" : "current-password"}" placeholder="${isSignup ? "mind. 6 Zeichen" : ""}">
          ` : ""}

          <button class="as-btn-primary" id="as-submit">${isReset ? "Reset-Link senden" : (isSignup ? "Konto erstellen" : "Anmelden")}</button>
          <div class="as-msg" id="as-msg"></div>

          ${isLogin ? `<div class="as-footer-link"><a id="as-reset">Passwort vergessen?</a></div>` : ""}
          ${isReset ? `<div class="as-footer-link"><a data-mode="login" class="as-mode-link">← Zurück zum Login</a></div>` : ""}
        </div>
      `;

      const msgEl = overlay.querySelector("#as-msg");
      const setMsg = (txt, cls = "") => { msgEl.textContent = txt; msgEl.className = "as-msg " + cls; };

      overlay.querySelector(".as-close").onclick = () => closeModal(null);
      overlay.querySelectorAll(".as-tab, .as-mode-link").forEach(b => b.onclick = () => { mode = b.dataset.mode; render(); });
      const resetLink = overlay.querySelector("#as-reset");
      if (resetLink) resetLink.onclick = () => { mode = "reset"; render(); };

      const googleBtn = overlay.querySelector("#as-google");
      if (googleBtn) googleBtn.onclick = async () => {
        try {
          setMsg("Öffne Google-Login …", "info");
          const provider = new GoogleAuthProvider();
          const res = await signInWithPopup(auth, provider);
          closeModal({ user: res.user, isNew: res._tokenResponse?.isNewUser });
        } catch (e) {
          if (e.code === "auth/popup-closed-by-user" || e.code === "auth/cancelled-popup-request") {
            setMsg("", "");
          } else {
            setMsg("Google-Login fehlgeschlagen: " + (e.message || e.code), "error");
          }
        }
      };

      const submit = overlay.querySelector("#as-submit");
      submit.onclick = async () => {
        const email = overlay.querySelector("#as-email").value.trim();
        const passEl = overlay.querySelector("#as-pass");
        const pass = passEl ? passEl.value : null;
        if (!email) { setMsg("Bitte E-Mail eingeben.", "error"); return; }
        submit.disabled = true;
        try {
          if (mode === "reset") {
            await sendPasswordResetEmail(auth, email);
            setMsg("Reset-Link wurde gesendet. Prüfe dein Postfach.", "success");
            submit.disabled = false;
            return;
          }
          if (!pass) { setMsg("Bitte Passwort eingeben.", "error"); submit.disabled = false; return; }
          if (mode === "signup") {
            const res = await createUserWithEmailAndPassword(auth, email, pass);
            try { await sendEmailVerification(res.user); } catch (_) {}
            closeModal({ user: res.user, isNew: true, sentVerification: true });
            return;
          }
          // login
          const res = await signInWithEmailAndPassword(auth, email, pass);
          closeModal({ user: res.user, isNew: false });
        } catch (e) {
          const code = e.code || "";
          let msg = e.message || "Fehler";
          if (code.includes("invalid-credential") || code.includes("wrong-password") || code.includes("user-not-found")) msg = "E-Mail oder Passwort falsch.";
          else if (code.includes("email-already-in-use")) msg = "Diese E-Mail ist bereits registriert.";
          else if (code.includes("weak-password")) msg = "Passwort zu schwach (mind. 6 Zeichen).";
          else if (code.includes("invalid-email")) msg = "E-Mail-Adresse ungültig.";
          else if (code.includes("network")) msg = "Keine Internetverbindung.";
          setMsg(msg, "error");
          submit.disabled = false;
        }
      };
    };

    render();
    document.body.appendChild(overlay);
    setTimeout(() => overlay.querySelector("#as-email")?.focus(), 50);
  });
}

// ---------- User-Menü (Dropdown nach Login) ----------
function buildUserMenu(btn, user, onSync, onLogout, isVerified) {
  // Bestehende schließen
  document.querySelectorAll(".as-menu").forEach(m => m.remove());
  const menu = document.createElement("div");
  menu.className = "as-menu";
  const initials = (user.displayName || user.email || "?").trim()[0].toUpperCase();
  menu.innerHTML = `
    <div class="as-menu-info">
      <strong>${user.displayName || user.email}</strong>
      ${user.displayName ? `<div>${user.email}</div>` : ""}
      ${!isVerified && user.providerData[0]?.providerId === "password" ? `<div class="as-sync-status" style="color:#d09020">⚠️ E-Mail nicht verifiziert</div>` : ""}
    </div>
    <div class="as-menu-item" data-act="sync">🔄 Jetzt synchronisieren</div>
    ${!isVerified && user.providerData[0]?.providerId === "password" ? `<div class="as-menu-item" data-act="verify">📧 Verifikations-Mail erneut senden</div>` : ""}
    <div class="as-menu-item" data-act="logout" style="color:#c0392b">🚪 Abmelden</div>
  `;
  btn.style.position = "relative";
  btn.appendChild(menu);
  menu.querySelectorAll(".as-menu-item").forEach(item => {
    item.onclick = async (e) => {
      e.stopPropagation();
      menu.remove();
      const act = item.dataset.act;
      if (act === "sync") onSync();
      if (act === "logout") onLogout();
      if (act === "verify") {
        try { await sendEmailVerification(user); toast("Verifikations-Mail gesendet.", "success"); }
        catch (e) { toast("Fehler: " + e.message, "error"); }
      }
    };
  });
  // Außerhalb-Klick schließt
  setTimeout(() => {
    const outside = (e) => { if (!menu.contains(e.target) && !btn.contains(e.target)) { menu.remove(); document.removeEventListener("click", outside); } };
    document.addEventListener("click", outside);
  }, 50);
}

// ---------- Haupt-API ----------
export async function initAuthSync(opts) {
  const {
    calculatorId,
    calculatorName,
    getLocalState,
    applyState,
    hostElementSelector = "header"
  } = opts;

  ensureStyles();
  const host = document.querySelector(hostElementSelector);
  if (!host) throw new Error("Host-Element nicht gefunden: " + hostElementSelector);

  // Login-Button im Header
  const btn = document.createElement("button");
  btn.className = "as-btn";
  btn.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 4-6 8-6s8 2 8 6"/></svg><span class="as-btn-label">Anmelden</span>`;
  btn.title = "Cloud-Sync";
  host.appendChild(btn);

  let currentUser = null;
  let saveInFlight = false;

  // ---------- Save / Load ----------
  async function saveCloud() {
    if (!currentUser) return;
    if (saveInFlight) return;
    saveInFlight = true;
    try {
      const state = getLocalState();
      const ref = doc(db, "users", currentUser.uid, "calculators", calculatorId);
      await setDoc(ref, { ...state, updatedAt: serverTimestamp() }, { merge: false });
    } catch (e) {
      console.warn("Cloud-Save fehlgeschlagen:", e);
    } finally {
      saveInFlight = false;
    }
  }

  async function loadCloud() {
    if (!currentUser) return null;
    try {
      const ref = doc(db, "users", currentUser.uid, "calculators", calculatorId);
      const snap = await getDoc(ref);
      if (!snap.exists()) return null;
      const data = snap.data();
      // updatedAt rauswerfen (kein State-Feld)
      delete data.updatedAt;
      return data;
    } catch (e) {
      console.warn("Cloud-Load fehlgeschlagen:", e);
      return null;
    }
  }

  async function handleLogin(user, isNew, sentVerification) {
    currentUser = user;
    updateBtn();
    if (sentVerification) {
      toast("Konto erstellt — Verifikations-Mail gesendet.", "success");
    }
    // Cloud-Daten holen
    const cloudState = await loadCloud();
    if (cloudState) {
      // Cloud hat Daten → übernehmen (Cloud ist die "Wahrheit")
      const useCloud = isNew ? false : confirm("Cloud-Daten gefunden. In dieses Gerät laden?\n\nOK = Cloud-Daten anwenden (lokale werden überschrieben)\nAbbrechen = Lokale Daten behalten (und in die Cloud hochladen)");
      if (useCloud || isNew) {
        applyState(cloudState);
        toast("Daten aus Cloud geladen.", "success");
      } else {
        await saveCloud();
        toast("Lokale Daten in Cloud hochgeladen.", "success");
      }
    } else {
      // Keine Cloud-Daten → lokale hochladen
      await saveCloud();
      toast(isNew ? "Konto erstellt — Daten gesichert." : "Daten in Cloud gesichert.", "success");
    }
  }

  function updateBtn() {
    if (currentUser) {
      const initials = (currentUser.displayName || currentUser.email || "?").trim()[0].toUpperCase();
      btn.innerHTML = `<span class="as-avatar">${initials}</span><span class="as-btn-label" style="max-width:120px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${currentUser.displayName || currentUser.email.split("@")[0]}</span>`;
      btn.title = currentUser.email;
    } else {
      btn.innerHTML = `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 4-6 8-6s8 2 8 6"/></svg><span class="as-btn-label">Anmelden</span>`;
      btn.title = "Cloud-Sync";
    }
  }

  btn.onclick = async (e) => {
    e.stopPropagation();
    if (currentUser) {
      buildUserMenu(btn, currentUser,
        async () => { await saveCloud(); toast("Synchronisiert.", "success"); },
        async () => { await signOut(auth); toast("Abgemeldet.", ""); }
      , currentUser.emailVerified);
      return;
    }
    const result = await openLoginModal(calculatorName);
    if (result?.user) {
      await handleLogin(result.user, !!result.isNew, !!result.sentVerification);
    }
  };

  // Auth-State beobachten (z.B. nach Reload)
  onAuthStateChanged(auth, async (user) => {
    if (user) {
      if (!currentUser) {
        currentUser = user;
        updateBtn();
        const cloudState = await loadCloud();
        if (cloudState) {
          applyState(cloudState);
        }
      }
    } else {
      currentUser = null;
      updateBtn();
    }
  });

  return {
    save: () => { saveCloud(); /* nicht awaiten — UI bleibt schnell */ },
    isLoggedIn: () => !!currentUser,
    getUser: () => currentUser,
    forceSync: saveCloud
  };
}
