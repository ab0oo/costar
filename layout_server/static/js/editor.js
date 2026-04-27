/**
 * editor.js — DSL editor page logic.
 *
 * Rewired from the original tools/dsl_editor/index.html to use the layout
 * server API instead of showDirectoryPicker / static file fetches.
 *
 * Screen list populates from GET /api/screens.
 * "Save" does PUT /api/screens/<id> (existing) or POST /api/screens (new).
 * All DSL rendering delegates to renderDslToCanvas() from preview.js.
 */

// ── State ─────────────────────────────────────────────────────────────────────

let parsedDsl     = null;
let parsedPayload = {};
let sharedSettings = {};
let widgetSettings = {};
let mergedSettings = {};
let lastError      = "";

const iconCache = new Map();   // used by preview.js renderer

const runtimeContext = {
  "geo.lat":             "37.4220",
  "geo.lon":             "-122.0841",
  "geo.tz":              "America/Los_Angeles",
  "geo.offset_min":      "-480",
  "geo.label":           "Mountain View, CA, USA",
  "pref.clock_24h":      "true",
  "pref.temp_unit":      "F",
  "pref.distance_unit":  "mi",
};

let screenList     = [];   // [{id, name, tab_label, description}, ...]
let activeScreenId = null; // id of the screen currently loaded, or null for unsaved

// ── Boot ──────────────────────────────────────���───────────────────────────────

document.addEventListener("DOMContentLoaded", () => {
  initEditors();
  initTabBar();
  initToolbar();
  initPrefsGeo();
  initScreenPicker();
  applyRuntimePreferenceOverrides();
  applyRuntimeGeoOverrides();
  setDefaultDsl();
  requestAnimationFrame(tick);
});

// ── JSONEditor instances ─────���────────────────────────────────────────────────

let dslEditor, payloadEditor, sharedSettingsEditor, widgetSettingsEditor, layoutSettingsEditor;

function initEditors() {
  const onDslChange = () => {
    clearTimeout(dslEditor._debounce);
    dslEditor._debounce = setTimeout(() => { compileDsl(); renderPreview(); }, 300);
  };
  const onPayloadChange = () => {
    clearTimeout(payloadEditor._debounce);
    payloadEditor._debounce = setTimeout(() => { compilePayload(); renderPreview(); }, 300);
  };

  dslEditor = new JSONEditor(document.getElementById("dslEditorEl"), {
    mode: "code", mainMenuBar: false, statusBar: false,
    onChange: onDslChange,
  });
  payloadEditor = new JSONEditor(document.getElementById("payloadEditorEl"), {
    mode: "code", mainMenuBar: false, statusBar: false,
    onChange: onPayloadChange,
  });
  sharedSettingsEditor = new JSONEditor(document.getElementById("sharedSettingsEditorEl"), {
    mode: "code", mainMenuBar: false, statusBar: false,
    onChange: () => renderPreview(),
  });
  widgetSettingsEditor = new JSONEditor(document.getElementById("widgetSettingsEditorEl"), {
    mode: "code", mainMenuBar: false, statusBar: false,
    onChange: () => renderPreview(),
  });
  layoutSettingsEditor = new JSONEditor(document.getElementById("layoutSettingsEditorEl"), {
    mode: "code", mainMenuBar: false, statusBar: false,
  });

  // Keep editors filling their containers
  const ro = new ResizeObserver(() => {
    [dslEditor, payloadEditor, sharedSettingsEditor, widgetSettingsEditor, layoutSettingsEditor]
      .forEach(e => { try { e.refresh?.(); e.resize?.(); } catch {} });
  });
  document.querySelectorAll(".editor-area").forEach(el => ro.observe(el));
}

// ── Screen picker (replaces old file select + showDirectoryPicker) ─────────────

function initScreenPicker() {
  const sel     = document.getElementById("screenSelect");
  const newBtn  = document.getElementById("newScreenBtn");
  const saveBtn = document.getElementById("saveScreenBtn");
  const delBtn  = document.getElementById("deleteScreenBtn");

  loadScreenList().then(() => {
    // If ?id= in URL, open that screen
    const params = new URLSearchParams(location.search);
    const idParam = Number(params.get("id"));
    if (idParam && screenList.find(s => s.id === idParam)) {
      sel.value = String(idParam);
      loadScreen(idParam);
    }
  });

  sel.addEventListener("change", () => {
    const id = Number(sel.value);
    if (id) loadScreen(id);
    else { activeScreenId = null; setFileStatus(""); }
  });

  newBtn.addEventListener("click",  () => newScreen());
  saveBtn.addEventListener("click", () => saveScreen());
  delBtn.addEventListener("click",  () => deleteScreen());
}

async function loadScreenList() {
  try {
    screenList = await api.screens.list();
    const sel = document.getElementById("screenSelect");
    const prev = sel.value;
    sel.innerHTML = '<option value="">— select screen —</option>';
    for (const s of screenList) {
      const opt = document.createElement("option");
      opt.value = s.id;
      opt.textContent = s.name + (s.tab_label ? ` [${s.tab_label}]` : "");
      sel.appendChild(opt);
    }
    if (prev && sel.querySelector(`option[value="${prev}"]`)) sel.value = prev;
  } catch (e) {
    setFileStatus("Could not load screen list: " + e.message, false);
  }
}

async function loadScreen(id) {
  setFileStatus("Loading…");
  try {
    const s = await api.screens.get(id);
    activeScreenId = s.id;
    document.getElementById("screenSelect").value   = String(id);
    document.getElementById("screenNameInput").value = s.name;
    document.getElementById("screenTabInput").value  = s.tab_label || "";
    document.getElementById("screenDescInput").value = s.description || "";
    dslEditor.set(s.content || {});
    compileDsl();
    renderPreview();
    setFileStatus("Loaded " + s.name, true);
  } catch (e) {
    setFileStatus("Load failed: " + e.message, false);
  }
}

function newScreen() {
  activeScreenId = null;
  document.getElementById("screenSelect").value    = "";
  document.getElementById("screenNameInput").value = "";
  document.getElementById("screenTabInput").value  = "";
  document.getElementById("screenDescInput").value = "";
  setDefaultDsl();
  setFileStatus("New screen — fill in name and save.", true);
}

async function saveScreen() {
  const name      = document.getElementById("screenNameInput").value.trim();
  const tab_label = document.getElementById("screenTabInput").value.trim().slice(0, 6);
  const description = document.getElementById("screenDescInput").value.trim();
  let content;
  try {
    content = dslEditor.get();
  } catch (e) {
    setFileStatus("Cannot save — invalid JSON: " + e.message, false);
    return;
  }
  if (!name) { setFileStatus("Screen name is required.", false); return; }

  setFileStatus("Saving…");
  try {
    let s;
    if (activeScreenId) {
      s = await api.screens.update(activeScreenId, { name, tab_label, description, content });
    } else {
      s = await api.screens.create(name, description, tab_label, content);
      activeScreenId = s.id;
    }
    await loadScreenList();
    document.getElementById("screenSelect").value = String(s.id);
    setFileStatus("Saved " + s.name, true);
  } catch (e) {
    setFileStatus("Save failed: " + e.message, false);
  }
}

async function deleteScreen() {
  if (!activeScreenId) return;
  const s = screenList.find(x => x.id === activeScreenId);
  if (!confirm(`Delete screen "${s?.name || activeScreenId}"?`)) return;
  try {
    await api.screens.delete(activeScreenId);
    activeScreenId = null;
    await loadScreenList();
    document.getElementById("screenSelect").value = "";
    newScreen();
    setFileStatus("Deleted.", true);
  } catch (e) {
    setFileStatus("Delete failed: " + e.message, false);
  }
}

// ── Toolbar buttons ────���──────────────────────────────────────────────────────

function initToolbar() {
  document.getElementById("formatBtn").addEventListener("click", () => {
    try { dslEditor.set(dslEditor.get()); } catch {}
    compileDsl(); renderPreview();
  });

  document.getElementById("downloadBtn").addEventListener("click", () => {
    try {
      const name = (document.getElementById("screenNameInput").value.trim() || "dsl") + ".json";
      const blob = new Blob([dslEditor.getText()], { type: "application/json" });
      const a = Object.assign(document.createElement("a"), { href: URL.createObjectURL(blob), download: name });
      a.click(); URL.revokeObjectURL(a.href);
    } catch {}
  });

  document.getElementById("applyPayloadBtn").addEventListener("click", () => { compilePayload(); renderPreview(); });
  document.getElementById("applySettingsBtn").addEventListener("click", () => { compileSettings(); renderPreview(); });

  document.getElementById("importLayoutSettingsBtn").addEventListener("click", () => {
    try {
      const layoutObj  = layoutSettingsEditor.get();
      const shared     = layoutObj?.shared_settings || {};
      const widgetDefs = layoutObj?.widget_defs     || {};
      const widgetId   = document.getElementById("layoutWidgetId").value.trim();
      const widget     = widgetDefs[widgetId]?.settings || {};
      sharedSettingsEditor.set(shared);
      widgetSettingsEditor.set(widget);
      compileSettings(); renderPreview();
      setSettingsStatus(`Imported settings for widget '${widgetId || "(none)"}'`, true);
    } catch (e) {
      setSettingsStatus("Import error: " + e.message, false);
    }
  });

  document.getElementById("loadWeatherBtn").addEventListener("click", () => loadSample("weather"));
  document.getElementById("loadAdsbBtn").addEventListener("click",    () => loadSample("adsb"));
  document.getElementById("useBrowserGeoBtn").addEventListener("click", useBrowserGeo);
}

async function loadSample(type) {
  const paths = {
    weather: "/data/dsl_available/weather_now.json",
    adsb:    "/data/dsl_available/adsb_nearest.json",
  };
  const payloads = {
    weather: { current: { temperature_2m: 18.2, relative_humidity_2m: 52, pressure_msl: 1015.6, wind_speed_10m: 6.3, wind_direction_10m: 247, weather_code: 2 }, daily: { sunrise: ["2026-02-21T14:47"], sunset: ["2026-02-22T01:49"] } },
    adsb:    { ac: [{ flight:"UAL2423", t:"B739", alt_baro:10025, dst:35.6, destination:"KSFO", lat:37.65, lon:-121.98 }, { flight:"VOI1772", t:"A20N", alt_baro:4475, dst:37.9, destination:"MMUN", lat:37.55, lon:-121.96 }] },
  };
  try {
    const res = await fetch(paths[type], { cache: "no-store" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    const text = await res.text();
    dslEditor.setText(text);
    payloadEditor.set(payloads[type]);
    compileDsl(); compilePayload(); renderPreview();
    setFileStatus("Loaded " + type + " sample.", true);
  } catch (e) {
    setFileStatus("Sample load failed: " + e.message, false);
  }
}

// ── Compile / render ─────��────────────────────────────────────────────────────

function compileDsl() {
  try {
    parsedDsl = JSON.parse(dslEditor.getText());
    lastError = "";
    setDslStatus("DSL parsed.", true);
  } catch (e) {
    parsedDsl = null; lastError = e.message;
    setDslStatus("Parse error: " + e.message, false);
  }
}

function compilePayload() {
  const raw = payloadEditor.getText().trim();
  if (!raw) { parsedPayload = {}; return; }
  try {
    parsedPayload = JSON.parse(raw);
    setPayloadStatus("Payload parsed.", true);
  } catch (e) {
    parsedPayload = {};
    setPayloadStatus("Parse error: " + e.message, false);
  }
}

function compileSettings() {
  try {
    const s = sanitizeSettingsMap(sharedSettingsEditor.get());
    const w = sanitizeSettingsMap(widgetSettingsEditor.get());
    sharedSettings = s; widgetSettings = w;
    mergedSettings = Object.assign({}, s, w);
    const keys = Object.keys(mergedSettings);
    document.getElementById("settingsHint").textContent =
      `Merged setting.* keys (${keys.length}): ${keys.slice(0, 8).join(", ") || "(none)"}`;
    setSettingsStatus(`shared=${Object.keys(s).length}, widget=${Object.keys(w).length}`, true);
  } catch (e) {
    sharedSettings = {}; widgetSettings = {}; mergedSettings = {};
    setSettingsStatus("Parse error: " + e.message, false);
  }
}

function renderPreview() {
  const canvas = document.getElementById("previewCanvas");
  if (!parsedDsl) {
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, 320, 240);
    ctx.fillStyle = "#000"; ctx.fillRect(0, 0, 320, 240);
    ctx.fillStyle = "#ff3f5b"; ctx.beginPath(); ctx.arc(314, 6, 3, 0, Math.PI * 2); ctx.fill();
    return;
  }
  renderDslToCanvas(canvas, parsedDsl, parsedPayload, mergedSettings, runtimeContext);
}

// ── Runtime prefs / geo ─────��─────────────────────────────────────────────────

function detectBrowserRuntimePrefs() {
  const locale = (navigator.languages?.[0]) || navigator.language || "en-US";
  let region = "";
  try { region = new Intl.Locale(locale).region || ""; } catch {}
  if (!region) { const m = locale.match(/[-_]([A-Za-z]{2})\b/); if (m) region = m[1].toUpperCase(); }
  const fahrenheitRegions = new Set(["US","BS","BZ","KY","PW","LR","FM","MH"]);
  const mileRegions       = new Set(["US","GB","LR","MM"]);
  const tempUnit     = fahrenheitRegions.has(region) ? "F" : "C";
  const distanceUnit = mileRegions.has(region) ? "mi" : "km";
  let clock24h = true;
  try {
    const ro = new Intl.DateTimeFormat(locale, { hour: "numeric" }).resolvedOptions();
    clock24h = typeof ro.hour12 === "boolean" ? !ro.hour12
             : typeof ro.hourCycle === "string" ? ro.hourCycle.startsWith("h2")
             : !/[AaPp]/.test(new Intl.DateTimeFormat(locale, { hour: "numeric" }).format(new Date(2020,0,1,13)));
  } catch {}
  return { locale, region, tempUnit, distanceUnit, clock24h };
}

const browserPrefs = detectBrowserRuntimePrefs();

function initPrefsGeo() {
  document.getElementById("prefTempUnit").value     = "auto";
  document.getElementById("prefDistanceUnit").value = "auto";
  document.getElementById("prefClockMode").value    = "auto";
  document.getElementById("geoLatInput").value      = runtimeContext["geo.lat"];
  document.getElementById("geoLonInput").value      = runtimeContext["geo.lon"];

  ["prefTempUnit","prefDistanceUnit","prefClockMode"].forEach(id => {
    document.getElementById(id).addEventListener("change", () => { applyRuntimePreferenceOverrides(); renderPreview(); });
  });
  ["geoLatInput","geoLonInput"].forEach(id => {
    document.getElementById(id).addEventListener("change", () => { applyRuntimeGeoOverrides(); renderPreview(); });
  });
}

function applyRuntimePreferenceOverrides() {
  const tempChoice  = document.getElementById("prefTempUnit").value     || "auto";
  const distChoice  = document.getElementById("prefDistanceUnit").value || "auto";
  const clockChoice = document.getElementById("prefClockMode").value    || "auto";
  const tempUnit     = tempChoice  === "auto" ? browserPrefs.tempUnit     : tempChoice;
  const distanceUnit = distChoice  === "auto" ? browserPrefs.distanceUnit : distChoice;
  const clock24h     = clockChoice === "auto" ? browserPrefs.clock24h     : (clockChoice === "24h");
  runtimeContext["pref.temp_unit"]     = tempUnit;
  runtimeContext["pref.distance_unit"] = distanceUnit;
  runtimeContext["pref.clock_24h"]     = clock24h ? "true" : "false";
  const regionText = browserPrefs.region ? `${browserPrefs.locale} (${browserPrefs.region})` : browserPrefs.locale;
  document.getElementById("runtimePrefHint").textContent =
    `Browser: ${regionText} temp=${browserPrefs.tempUnit} dist=${browserPrefs.distanceUnit} ` +
    `clock=${browserPrefs.clock24h?"24h":"12h"} · Active: temp=${tempUnit} dist=${distanceUnit} clock=${clock24h?"24h":"12h"}`;
}

function applyRuntimeGeoOverrides() {
  const lat = Number(document.getElementById("geoLatInput").value);
  const lon = Number(document.getElementById("geoLonInput").value);
  if (Number.isFinite(lat)) runtimeContext["geo.lat"] = lat.toFixed(4);
  if (Number.isFinite(lon)) runtimeContext["geo.lon"] = lon.toFixed(4);
  document.getElementById("runtimeGeoHint").textContent =
    `Geo context: lat=${runtimeContext["geo.lat"]} lon=${runtimeContext["geo.lon"]} tz=${runtimeContext["geo.tz"]}`;
}

function useBrowserGeo() {
  if (!("geolocation" in navigator)) { setPayloadStatus("Browser geolocation unavailable.", false); return; }
  navigator.geolocation.getCurrentPosition(
    (pos) => {
      document.getElementById("geoLatInput").value = pos.coords.latitude.toFixed(4);
      document.getElementById("geoLonInput").value = pos.coords.longitude.toFixed(4);
      applyRuntimeGeoOverrides(); renderPreview();
      setPayloadStatus("Applied browser geolocation.", true);
    },
    (err) => setPayloadStatus("Geolocation failed: " + err.message, false),
    { enableHighAccuracy: false, timeout: 5000, maximumAge: 300000 }
  );
}

// ── Tab bar ────────────────────────��──────────────────────────────────────────

function initTabBar() {
  document.querySelectorAll(".tab-btn").forEach(btn => {
    btn.addEventListener("click", () => {
      const target = btn.dataset.tab;
      document.querySelectorAll(".tab-btn").forEach(b => b.classList.toggle("active", b.dataset.tab === target));
      document.querySelectorAll(".tab-panel").forEach(p => p.classList.toggle("active", p.id === target));
    });
  });
}

// ── Clock animation ────────────────────────────���──────────────────────────────

function tick() {
  if (document.getElementById("animateClock").checked &&
      parsedDsl?.data?.source === "local_time") {
    renderPreview();
  }
  requestAnimationFrame(tick);
}

// ── Default DSL ───────────────────────────────────────────────────────────────

function setDefaultDsl() {
  dslEditor.set({
    version: 1,
    data: {
      source: "local_time",
      poll_ms: 1000,
      fields: {
        hour: "hour", minute: "minute", second: "second",
        date_long: { path: "iso_local", format: { tz: "local", time_format: "%B %d, %Y" } },
      },
    },
    ui: {
      title: "Clock",
      nodes: [
        { type: "circle", x: 160, y: 120, r: 100, color: "#5ec8ff", bg: "#000000", thickness: 1 },
        { type: "line", x: 160, y: 120, length: 50, width: 3, color: "#ffffff", angle_expr: "(hour%12)*30+minute*0.5" },
        { type: "line", x: 160, y: 120, length: 80, width: 2, color: "#9fd3ff", angle_expr: "minute*6" },
        { type: "line", x: 160, y: 120, length: 95, width: 1, color: "#f5e663", angle_expr: "second*6" },
        { type: "circle", x: 160, y: 120, r: 4, color: "#5ec8ff", bg: "#5ec8ff", thickness: 1 },
        { type: "label", x: 160, y: 6, font: 2, color: "#9fd3ff", text: "{{date_long}}", align: "center" },
      ],
    },
  });
  payloadEditor.set({});
  sharedSettingsEditor.set({});
  widgetSettingsEditor.set({});
  layoutSettingsEditor.set({ shared_settings: {}, widget_defs: { "widget-id": { settings: {} } } });
  compileDsl(); compilePayload(); compileSettings(); renderPreview();
}

// ── Status helpers ────────���───────────────────────────────────────────────────

function setFileStatus(msg, ok)     { _setStatus("fileStatus",     msg, ok); }
function setDslStatus(msg, ok)      { _setStatus("dslStatus",      msg, ok); }
function setPayloadStatus(msg, ok)  { _setStatus("payloadStatus",  msg, ok); }
function setSettingsStatus(msg, ok) { _setStatus("settingsStatus", msg, ok); }

function _setStatus(id, msg, ok) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = msg;
  el.className   = "status" + (ok === true ? " ok" : ok === false ? " err" : "");
}

// ── sanitizeSettingsMap — shared with preview.js renderer context ─────────────
// (defined here so editor.js doesn't depend on preview.js internals)

function sanitizeSettingsMap(obj) {
  const out = {};
  if (!obj || typeof obj !== "object" || Array.isArray(obj)) return out;
  for (const [k, v] of Object.entries(obj)) {
    const key = String(k).trim();
    if (!key) continue;
    if (v == null)                      { out[key] = ""; continue; }
    if (typeof v === "string")          { out[key] = v.trim(); continue; }
    if (typeof v === "boolean")         { out[key] = v ? "true" : "false"; continue; }
    if (typeof v === "number") {
      if (!Number.isFinite(v))          { out[key] = ""; continue; }
      out[key] = Math.abs(v - Math.round(v)) < 1e-6 ? String(Math.round(v)) : v.toFixed(3).replace(/\.?0+$/, "");
      continue;
    }
    try { out[key] = JSON.stringify(v); } catch { out[key] = String(v); }
  }
  return out;
}
