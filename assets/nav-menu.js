// =============================================================
// Geteiltes Navigations-Menü am ☰-Button (Hover-Dropdown).
// =============================================================
// Klick auf den ☰-Button → zurück zur Übersicht (wie gehabt).
// Maus drüber → Menü mit allen Rechnern als Shortcut.
// Optik kommt komplett aus app.css (.nav-dropdown / .nav-menu).
// EINE Quelle für alle Seiten — einfach dieses Modul einbinden:
//   <script type="module" src="/assets/nav-menu.js"></script>
// =============================================================

const LINKS = [
  { href: "/",               label: "Übersicht", overview: true },
  { href: "/kalk/",          label: "Kalkmanagement" },
  { href: "/nutrient/",      label: "Nährstoffmanagement" },
  { href: "/cr-rechner/",    label: "C&R-Rechner" },
  { href: "/spurenrechner/", label: "Spurenelemente" },
];

function initNavMenu() {
  const backBtn = document.querySelector(".back-btn");
  if (!backBtn || backBtn.closest(".nav-dropdown")) return;  // kein Button / schon initialisiert

  // Wrapper um den Button (übernimmt dessen Position oben links)
  const wrap = document.createElement("div");
  wrap.className = "nav-dropdown";
  backBtn.parentNode.insertBefore(wrap, backBtn);
  wrap.appendChild(backBtn);

  // Menü-Panel
  const path = location.pathname;
  const menu = document.createElement("div");
  menu.className = "nav-menu";
  menu.setAttribute("role", "menu");
  menu.innerHTML = LINKS.map((l) => {
    const base = l.href.replace(/\/$/, "");                       // "/kalk" etc.
    const isCurrent = !l.overview && base && (path === base || path.indexOf(base + "/") === 0);
    const cls = ["nav-menu-item"];
    if (l.overview) cls.push("overview");
    if (isCurrent) cls.push("current");
    return `<a href="${l.href}" class="${cls.join(" ")}" role="menuitem">${l.label}</a>`;
  }).join("");
  wrap.appendChild(menu);
}

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", initNavMenu);
else initNavMenu();
