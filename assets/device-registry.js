// Gemeinsame Geräte-Registry-UI für Kalk- und Nährstoff-App.
// Liest/schreibt die `devices`-Collection (chipId -> role).
// Der ESP registriert sich selbst per chipId und liest seine Rolle hier aus.
import { pb } from "/assets/pb-client.js";

export const DEVICE_ROLES = [
  { value: "",         label: "— nicht zugewiesen —" },
  { value: "kalk",     label: "Kalkmanagement (Ca/KH)" },
  { value: "nutrient", label: "Nährstoffmanagement (NO₃/PO₄)" },
];

function roleLabel(v) {
  const r = DEVICE_ROLES.find((x) => x.value === (v || ""));
  return r ? r.label : v;
}

export async function listDevices() {
  const uid = pb.authStore?.model?.id;
  if (!uid) return [];
  return pb.collection("devices").getFullList({
    filter: `user = "${uid}"`,
    sort: "-lastSeen",
  });
}

export async function setDeviceRole(id, role) {
  return pb.collection("devices").update(id, { role });
}

export async function setDeviceName(id, name) {
  return pb.collection("devices").update(id, { name });
}

export async function deleteDevice(id) {
  return pb.collection("devices").delete(id);
}

let _stylesInjected = false;
function injectStyles() {
  if (_stylesInjected) return;
  _stylesInjected = true;
  const css = `
  .devreg-row { display:flex; align-items:center; gap:0.7rem; padding:0.7rem 0; border-bottom:1px solid #eef2f6; flex-wrap:wrap; }
  .devreg-row:last-child { border-bottom:none; }
  .devreg-dot { width:10px; height:10px; border-radius:50%; flex:0 0 auto; }
  .devreg-main { flex:1 1 180px; min-width:0; }
  .devreg-name { font-weight:600; font-size:0.95rem; background:transparent; border:1px solid transparent; border-radius:6px; padding:0.2rem 0.35rem; width:100%; max-width:260px; font-family:inherit; }
  .devreg-name:hover, .devreg-name:focus { border-color:#d4dde6; background:#fff; outline:none; }
  .devreg-meta { font-size:0.76rem; color:#7a8a99; margin-top:0.15rem; }
  .devreg-role { flex:0 0 auto; font-family:inherit; font-size:0.88rem; padding:0.4rem 0.5rem; border:1px solid #d4dde6; border-radius:8px; background:#fff; cursor:pointer; }
  .devreg-del { flex:0 0 auto; background:transparent; border:none; color:#b8c2cc; cursor:pointer; font-size:1.05rem; padding:0.2rem 0.4rem; border-radius:6px; }
  .devreg-del:hover { color:#e05b5b; background:#fdf0f0; }
  .devreg-empty { color:#7a8a99; font-style:italic; padding:1rem 0; text-align:center; }
  .devreg-saved { font-size:0.74rem; color:#27ae60; margin-left:0.4rem; opacity:0; transition:opacity .2s; }
  .devreg-saved.show { opacity:1; }
  `;
  const el = document.createElement("style");
  el.textContent = css;
  document.head.appendChild(el);
}

function flashSaved(el) {
  if (!el) return;
  el.classList.add("show");
  setTimeout(() => el.classList.remove("show"), 1400);
}

// Rendert die Registry in `container`. highlightRole hebt die zu dieser App
// passende Rolle hervor (z.B. "kalk" in der Kalk-App).
export async function renderDeviceRegistry(container, opts = {}) {
  if (!container) return;
  injectStyles();
  const highlightRole = opts.highlightRole || null;
  container.innerHTML = `<div class="devreg-empty">Lade Geräte …</div>`;

  let devices = [];
  try {
    devices = await listDevices();
  } catch (e) {
    container.innerHTML = `<div class="devreg-empty">Geräte konnten nicht geladen werden: ${e.message}</div>`;
    return;
  }

  if (!devices.length) {
    container.innerHTML = `<div class="devreg-empty">
      Noch kein Gerät registriert.<br>
      Sobald ein ESP mit rollenfähiger Firmware online geht, erscheint er hier und du kannst ihm eine Rolle zuweisen.
    </div>`;
    return;
  }

  container.innerHTML = "";
  for (const d of devices) {
    const lastSeenMs = d.lastSeen ? (d.lastSeen < 1e12 ? d.lastSeen * 1000 : d.lastSeen) : 0;
    const online = lastSeenMs && Date.now() - lastSeenMs < 120000;
    const seenTxt = lastSeenMs ? new Date(lastSeenMs).toLocaleString("de-DE") : "nie";
    const chipShort = (d.chipId || "?").slice(-8);

    const row = document.createElement("div");
    row.className = "devreg-row";

    const opts2 = DEVICE_ROLES.map(
      (r) => `<option value="${r.value}" ${r.value === (d.role || "") ? "selected" : ""}>${r.label}</option>`
    ).join("");

    row.innerHTML = `
      <span class="devreg-dot" style="background:${online ? "#27ae60" : "#c2ccd6"}"></span>
      <div class="devreg-main">
        <input class="devreg-name" value="${(d.name || "").replace(/"/g, "&quot;")}" placeholder="Gerätename" data-id="${d.id}">
        <div class="devreg-meta">Chip …${chipShort}${d.fwVersion ? " · FW " + d.fwVersion : ""} · ${online ? "online" : "zuletzt " + seenTxt}</div>
      </div>
      <select class="devreg-role" data-id="${d.id}" title="Rolle / Dosierplan">${opts2}</select>
      <span class="devreg-saved" data-saved="${d.id}">✓</span>
      <button class="devreg-del" data-del="${d.id}" title="Gerät aus Liste entfernen">✕</button>
    `;
    container.appendChild(row);
  }

  // Handler verdrahten
  container.querySelectorAll(".devreg-role").forEach((sel) => {
    sel.addEventListener("change", async () => {
      const id = sel.dataset.id;
      try {
        await setDeviceRole(id, sel.value);
        flashSaved(container.querySelector(`[data-saved="${id}"]`));
      } catch (e) {
        alert("Rolle konnte nicht gespeichert werden: " + e.message);
      }
    });
  });
  container.querySelectorAll(".devreg-name").forEach((inp) => {
    inp.addEventListener("change", async () => {
      const id = inp.dataset.id;
      try {
        await setDeviceName(id, inp.value.trim());
        flashSaved(container.querySelector(`[data-saved="${id}"]`));
      } catch (e) {
        alert("Name konnte nicht gespeichert werden: " + e.message);
      }
    });
  });
  container.querySelectorAll(".devreg-del").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const id = btn.dataset.del;
      if (!confirm("Gerät wirklich aus der Liste entfernen? Wenn der ESP noch läuft, registriert er sich beim nächsten Start erneut.")) return;
      try {
        await deleteDevice(id);
        renderDeviceRegistry(container, opts);
      } catch (e) {
        alert("Gerät konnte nicht entfernt werden: " + e.message);
      }
    });
  });
}
