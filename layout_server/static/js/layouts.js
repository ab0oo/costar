/**
 * layouts.js — Layout assembly page logic.
 *
 * Left panel:  saved layout list (create / rename / delete)
 * Center:      5-slot drop target — drag screens from library into slots
 * Right:       device config (wifi SSIDs, locale/units)
 *
 * Active layout state lives in `active` and is auto-saved on every meaningful
 * change via saveActive().  The build button assembles a LittleFS image
 * server-side and offers a download link.
 */

// ── State ─────────────────────────────────────────────────────────────────────

let layouts    = [];     // [{id, name, device_nick, ...}, ...]
let screens    = [];     // [{id, name, tab_label, description}, ...]
let activeId   = null;   // layout id currently open, or null

// Mutable working copy of the active layout (not yet saved)
let active = emptyLayout();

function emptyLayout() {
  return {
    id:          null,
    name:        "",
    device_nick: "",
    screens:     [],   // [{position, id, name, tab_label}, ...]  position 1-5
    wifi:        [],   // [{ssid, password}, ...]
    locale: {
      temp_unit:     "F",
      distance_unit: "mi",
      clock_24h:     false,
      timezone:      "America/New_York",
    },
  };
}

// ── Boot ─────────────────────────────────────────────────────────────────────

document.addEventListener("DOMContentLoaded", async () => {
  await Promise.all([loadLayouts(), loadScreens()]);
  initDragDrop();
  initWifiControls();
  initLocaleControls();
  initBuildControls();
  document.getElementById("newLayoutBtn").addEventListener("click", createLayout);
  renderLibrary();
});

// ── Data loading ──────────────────────────────────────────────────────────────

async function loadLayouts() {
  try {
    layouts = await api.layouts.list();
    renderLayoutList();
  } catch (e) {
    setStatus("Error loading layouts: " + e.message, "err");
  }
}

async function loadScreens() {
  try {
    screens = await api.screens.list();
    renderLibrary();
  } catch (e) {
    setStatus("Error loading screens: " + e.message, "err");
  }
}

// ── Layout list (left panel) ──────────────────────────────────────────────────

function renderLayoutList() {
  const ul = document.getElementById("layoutList");
  ul.innerHTML = "";
  if (layouts.length === 0) {
    ul.innerHTML = '<li style="color:var(--muted);font-size:11px;padding:8px">No layouts yet.</li>';
    return;
  }
  for (const l of layouts) {
    const li  = document.createElement("li");
    li.className = "layout-list-item" + (l.id === activeId ? " active" : "");
    li.dataset.id = l.id;
    li.innerHTML = `
      <span class="layout-list-name" title="${escHtml(l.device_nick)}">${escHtml(l.name)}</span>
      <span class="layout-list-nick">${escHtml(l.device_nick || "—")}</span>
      <button type="button" class="icon-btn danger" title="Delete" onclick="deleteLayout(${l.id},event)">✕</button>`;
    li.addEventListener("click", (e) => {
      if (e.target.closest("button")) return;
      openLayout(l.id);
    });
    ul.appendChild(li);
  }
}

async function createLayout() {
  const name = prompt("Layout name:");
  if (!name || !name.trim()) return;
  try {
    const l = await api.layouts.create({
      name:        name.trim(),
      device_nick: "",
      screen_ids:  [],
      wifi:        [],
      locale:      emptyLayout().locale,
    });
    layouts.push(l);
    renderLayoutList();
    openLayout(l.id);
  } catch (e) {
    setStatus("Create failed: " + e.message, "err");
  }
}

async function deleteLayout(id, e) {
  e.stopPropagation();
  const l = layouts.find(x => x.id === id);
  if (!l || !confirm(`Delete layout "${l.name}"?`)) return;
  try {
    await api.layouts.delete(id);
    layouts = layouts.filter(x => x.id !== id);
    if (activeId === id) { activeId = null; active = emptyLayout(); renderAssembly(); renderConfig(); }
    renderLayoutList();
  } catch (e2) {
    setStatus("Delete failed: " + e2.message, "err");
  }
}

async function openLayout(id) {
  activeId = id;
  renderLayoutList();
  setStatus("Loading layout…");
  try {
    const l  = await api.layouts.get(id);
    active   = {
      id:          l.id,
      name:        l.name,
      device_nick: l.device_nick || "",
      screens:     (l.screens || []).map(s => ({
        position:  s.position,
        id:        s.id,
        name:      s.name,
        tab_label: s.tab_label || s.name.slice(0, 4).toUpperCase(),
      })),
      wifi:        l.wifi   || [],
      locale:      Object.assign(emptyLayout().locale, l.locale || {}),
    };
    renderAssembly();
    renderConfig();
    setStatus("");
  } catch (e) {
    setStatus("Load failed: " + e.message, "err");
  }
}

// ── Auto-save ─────────────────────────────────────────────────────────────────

let saveTimer = null;

function scheduleSave() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveActive, 800);
}

async function saveActive() {
  if (!active.id) return;
  try {
    await api.layouts.update(active.id, {
      name:        active.name,
      device_nick: active.device_nick,
      screen_ids:  active.screens.map(s => s.id),
      wifi:        active.wifi,
      locale:      active.locale,
    });
    setStatus("Saved.", "ok");
    setTimeout(() => setStatus(""), 1500);
    // Refresh layout list to pick up name changes
    const idx = layouts.findIndex(x => x.id === active.id);
    if (idx >= 0) {
      layouts[idx].name        = active.name;
      layouts[idx].device_nick = active.device_nick;
      renderLayoutList();
    }
  } catch (e) {
    setStatus("Save failed: " + e.message, "err");
  }
}

// ── Screen library (source for drag) ─────────────────────────────────────────

function renderLibrary() {
  const el = document.getElementById("screenLibrary");
  el.innerHTML = "";
  for (const s of screens) {
    const chip = document.createElement("div");
    chip.className       = "screen-chip";
    chip.draggable       = true;
    chip.dataset.screenId   = s.id;
    chip.dataset.screenName = s.name;
    chip.dataset.tabLabel   = s.tab_label || s.name.slice(0, 4).toUpperCase();
    chip.innerHTML = `<span class="badge">${escHtml(chip.dataset.tabLabel)}</span> ${escHtml(s.name)}`;
    chip.addEventListener("dragstart", onChipDragStart);
    el.appendChild(chip);
  }
}

function onChipDragStart(e) {
  e.dataTransfer.setData("application/x-screen-id",   e.currentTarget.dataset.screenId);
  e.dataTransfer.setData("application/x-screen-name", e.currentTarget.dataset.screenName);
  e.dataTransfer.setData("application/x-tab-label",   e.currentTarget.dataset.tabLabel);
  e.dataTransfer.effectAllowed = "copy";
}

// ── Assembly slots (center) ───────────────────────────────────────────────────

function renderAssembly() {
  // Header fields
  document.getElementById("layoutName").value      = active.name        || "";
  document.getElementById("layoutDeviceNick").value = active.device_nick || "";

  for (let pos = 1; pos <= 5; pos++) {
    const slot    = document.getElementById(`slot-${pos}`);
    const occupied = active.screens.find(s => s.position === pos);
    renderSlot(slot, pos, occupied || null);
  }
}

function renderSlot(slotEl, pos, screen) {
  slotEl.innerHTML = "";
  slotEl.classList.toggle("occupied", !!screen);

  if (screen) {
    slotEl.innerHTML = `
      <span class="slot-pos">${pos}</span>
      <span class="badge">${escHtml(screen.tab_label)}</span>
      <span class="slot-name">${escHtml(screen.name)}</span>
      <button type="button" class="icon-btn danger slot-remove" title="Remove" data-pos="${pos}">✕</button>`;
    slotEl.querySelector(".slot-remove").addEventListener("click", () => removeFromSlot(pos));
  } else {
    slotEl.innerHTML = `<span class="slot-pos">${pos}</span><span class="slot-empty">drop screen here</span>`;
  }
}

function removeFromSlot(pos) {
  active.screens = active.screens.filter(s => s.position !== pos);
  renderAssembly();
  scheduleSave();
}

function initDragDrop() {
  for (let pos = 1; pos <= 5; pos++) {
    const slot = document.getElementById(`slot-${pos}`);
    slot.addEventListener("dragover",  (e) => { e.preventDefault(); e.dataTransfer.dropEffect = "copy"; slot.classList.add("drag-over"); });
    slot.addEventListener("dragleave", ()  => slot.classList.remove("drag-over"));
    slot.addEventListener("drop",      (e) => onSlotDrop(e, pos));
  }

  // Name / device_nick inputs
  document.getElementById("layoutName").addEventListener("input", (e) => {
    active.name = e.target.value.trim();
    scheduleSave();
  });
  document.getElementById("layoutDeviceNick").addEventListener("input", (e) => {
    active.device_nick = e.target.value.trim();
    scheduleSave();
  });
}

function onSlotDrop(e, pos) {
  e.preventDefault();
  const slot = document.getElementById(`slot-${pos}`);
  slot.classList.remove("drag-over");
  if (!active.id) { setStatus("Open or create a layout first.", "err"); return; }

  const id       = Number(e.dataTransfer.getData("application/x-screen-id"));
  const name     = e.dataTransfer.getData("application/x-screen-name");
  const tabLabel = e.dataTransfer.getData("application/x-tab-label");
  if (!id) return;

  // Remove screen from any existing slot first (screens are unique within a layout)
  active.screens = active.screens.filter(s => s.id !== id);
  // Remove whatever was in target slot
  active.screens = active.screens.filter(s => s.position !== pos);
  active.screens.push({ position: pos, id, name, tab_label: tabLabel });
  active.screens.sort((a, b) => a.position - b.position);

  renderAssembly();
  scheduleSave();
}

// ── Device config (right panel) ───────────────────────────────────────────────

function renderConfig() {
  renderWifi();
  renderLocale();
}

// --- Wifi ---

function renderWifi() {
  const tbody = document.getElementById("wifiRows");
  tbody.innerHTML = "";
  for (let i = 0; i < active.wifi.length; i++) {
    const row = document.createElement("tr");
    row.innerHTML = `
      <td><input type="text"     class="wifi-ssid"     value="${escHtml(active.wifi[i].ssid     || "")}" data-idx="${i}" placeholder="SSID"></td>
      <td><input type="password" class="wifi-password" value="${escHtml(active.wifi[i].password || "")}" data-idx="${i}" placeholder="Password"></td>
      <td><button type="button" class="icon-btn danger" data-idx="${i}">✕</button></td>`;
    row.querySelectorAll("input").forEach(inp => inp.addEventListener("input", onWifiInput));
    row.querySelector("button").addEventListener("click", (e) => removeWifi(Number(e.currentTarget.dataset.idx)));
    tbody.appendChild(row);
  }
}

function initWifiControls() {
  document.getElementById("addWifiBtn").addEventListener("click", () => {
    if (!active.id) return;
    active.wifi.push({ ssid: "", password: "" });
    renderWifi();
    scheduleSave();
  });
}

function onWifiInput(e) {
  const idx   = Number(e.target.dataset.idx);
  const field = e.target.classList.contains("wifi-ssid") ? "ssid" : "password";
  active.wifi[idx][field] = e.target.value;
  scheduleSave();
}

function removeWifi(idx) {
  active.wifi.splice(idx, 1);
  renderWifi();
  scheduleSave();
}

// --- Locale ---

const TIMEZONES = [
  "UTC",
  "America/New_York","America/Chicago","America/Denver","America/Los_Angeles",
  "America/Anchorage","America/Honolulu","America/Phoenix",
  "Europe/London","Europe/Paris","Europe/Berlin","Europe/Helsinki",
  "Asia/Tokyo","Asia/Shanghai","Asia/Kolkata","Asia/Dubai",
  "Australia/Sydney","Pacific/Auckland",
];

function renderLocale() {
  document.getElementById("localeTempUnit").value     = active.locale.temp_unit     || "F";
  document.getElementById("localeDistUnit").value     = active.locale.distance_unit || "mi";
  document.getElementById("localeClock24h").value      = active.locale.clock_24h ? "1" : "";
  const tzSel = document.getElementById("localeTimezone");
  if (!tzSel.options.length) {
    TIMEZONES.forEach(tz => {
      const opt = document.createElement("option");
      opt.value = opt.textContent = tz;
      tzSel.appendChild(opt);
    });
  }
  tzSel.value = active.locale.timezone || "UTC";
}

function initLocaleControls() {
  document.getElementById("localeTempUnit").addEventListener("change",  onLocaleChange);
  document.getElementById("localeDistUnit").addEventListener("change",  onLocaleChange);
  // clock_24h handled inline in layouts.html (value "1"/"" → bool)
  document.getElementById("localeTimezone").addEventListener("change",  onLocaleChange);
}

function onLocaleChange() {
  if (!active.id) return;
  active.locale.temp_unit     = document.getElementById("localeTempUnit").value;
  active.locale.distance_unit = document.getElementById("localeDistUnit").value;
  active.locale.clock_24h     = document.getElementById("localeClock24h").checked;
  active.locale.timezone      = document.getElementById("localeTimezone").value;
  scheduleSave();
}

// ── Build ─────────────────────────────────────────────────────────────────────

function initBuildControls() {
  document.getElementById("buildBtn").addEventListener("click", buildImage);
}

async function buildImage() {
  if (!active.id) { setStatus("No layout open.", "err"); return; }
  if (active.screens.length === 0) { setStatus("Add at least one screen before building.", "err"); return; }

  const btn = document.getElementById("buildBtn");
  btn.disabled = true;
  setBuildStatus("Building LittleFS image…");

  try {
    const result = await api.layouts.build(active.id);
    const dlLink = document.getElementById("downloadLink");
    dlLink.href        = api.layouts.imageUrl(active.id);
    dlLink.textContent = `Download costar_layout_${active.id}.bin (${(result.image_size / 1024).toFixed(0)} KB)`;
    dlLink.style.display = "inline";
    setBuildStatus(
      `Built ${result.cached ? "(cached)" : "fresh"} · ${result.files.length} files · hash ${result.content_hash.slice(0,10)}`,
      "ok"
    );
  } catch (e) {
    setBuildStatus("Build failed: " + e.message, "err");
  } finally {
    btn.disabled = false;
  }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

function setStatus(msg, type) {
  const el = document.getElementById("pageStatus");
  el.textContent = msg;
  el.className   = "status" + (type ? " " + type : "");
}

function setBuildStatus(msg, type) {
  const el = document.getElementById("buildStatus");
  el.textContent = msg;
  el.className   = "status" + (type ? " " + type : "");
}

function escHtml(s) {
  return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;");
}
