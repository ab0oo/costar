/**
 * screens.js — Screen library page logic.
 *
 * Displays all screens as thumbnail cards with live DSL previews.
 * Supports create, edit (name/description/tab_label + DSL JSON), and delete.
 */

// ── State ────────────────────────────────────────────────────────────────────

let screens      = [];       // [{id, name, description, tab_label, filename}, ...]
let dslEditor    = null;     // JSONEditor instance for the modal
let editingId    = null;     // screen id being edited, or null for new
let previewTick  = null;     // requestAnimationFrame handle for clock animation

// ── Boot ─────────────────────────────────────────────────────────────────────

document.addEventListener("DOMContentLoaded", async () => {
  initModal();
  await loadScreens();
});

// ── Data ─────────────────────────────────────────────────────────────────────

async function loadScreens() {
  setStatus("Loading screens…");
  try {
    screens = await api.screens.list();
    renderGrid();
    setStatus("");
  } catch (e) {
    setStatus("Error loading screens: " + e.message, "err");
  }
}

// ── Grid rendering ───────────────────────────────────────────────────────────

function renderGrid() {
  const grid = document.getElementById("screenGrid");
  grid.innerHTML = "";

  if (screens.length === 0) {
    grid.innerHTML = '<p style="color:var(--muted);grid-column:1/-1">No screens yet. Click New Screen to create one.</p>';
    return;
  }

  for (const s of screens) {
    const card = buildCard(s);
    grid.appendChild(card);
  }
}

function buildCard(s) {
  const card = document.createElement("div");
  card.className = "screen-card";
  card.dataset.id = s.id;

  const preview = document.createElement("div");
  preview.className = "screen-card-preview";
  const canvas = document.createElement("canvas");
  canvas.width  = 320;
  canvas.height = 240;
  preview.appendChild(canvas);

  const meta = document.createElement("div");
  meta.className = "screen-card-meta";
  meta.innerHTML = `
    <div class="screen-card-name">${escHtml(s.name)}</div>
    <div class="screen-card-label">Tab: <span class="badge">${escHtml(s.tab_label || "—")}</span>
      ${s.description ? ` · ${escHtml(s.description)}` : ""}</div>
    <div class="screen-card-actions">
      <button type="button" onclick="openEdit(${s.id})">Edit</button>
      <button type="button" class="danger" onclick="deleteScreen(${s.id})">Delete</button>
    </div>`;

  card.appendChild(preview);
  card.appendChild(meta);

  // Render DSL preview asynchronously so grid appears immediately
  loadAndRenderPreview(s.id, canvas);
  return card;
}

async function loadAndRenderPreview(screenId, canvas) {
  try {
    const s = await api.screens.get(screenId);
    if (!s.content) return;
    // Each screen JSON is a screen_layout — find the first widget DSL path
    // and render using an empty payload (static preview)
    renderDslToCanvas(canvas, s.content, {}, {}, {});
  } catch {
    // Silently skip — canvas stays black
  }
}

// ── Modal ─────────────────────────────────────────────────────────────────────

function initModal() {
  const overlay = document.getElementById("modalOverlay");
  overlay.addEventListener("click", (e) => {
    if (e.target === overlay) closeModal();
  });
  document.getElementById("cancelBtn").addEventListener("click", closeModal);
  document.getElementById("saveBtn").addEventListener("click",   saveScreen);
  document.getElementById("newScreenBtn").addEventListener("click", openNew);

  dslEditor = new JSONEditor(document.getElementById("dslEditorArea"), {
    mode: "code",
    mainMenuBar: false,
    statusBar: false,
  });
}

function openNew() {
  editingId = null;
  document.getElementById("modalTitle").textContent  = "New Screen";
  document.getElementById("screenName").value        = "";
  document.getElementById("screenDesc").value        = "";
  document.getElementById("screenTabLabel").value    = "";
  dslEditor.set({
    "version": 1,
    "screen": { "id": "new-screen", "orientation": "landscape", "width": 320, "height": 220, "regions": [] },
    "widget_defs": {}
  });
  setModalStatus("");
  showModal();
}

async function openEdit(id) {
  editingId = id;
  setModalStatus("Loading…");
  showModal();
  try {
    const s = await api.screens.get(id);
    document.getElementById("modalTitle").textContent  = "Edit Screen";
    document.getElementById("screenName").value        = s.name;
    document.getElementById("screenDesc").value        = s.description || "";
    document.getElementById("screenTabLabel").value    = s.tab_label   || "";
    dslEditor.set(s.content || {});
    setModalStatus("");
  } catch (e) {
    setModalStatus("Load failed: " + e.message, "err");
  }
}

async function saveScreen() {
  const name      = document.getElementById("screenName").value.trim();
  const desc      = document.getElementById("screenDesc").value.trim();
  const tab_label = document.getElementById("screenTabLabel").value.trim().slice(0, 6);
  let content;
  try {
    content = dslEditor.get();
  } catch (e) {
    setModalStatus("Invalid JSON: " + e.message, "err");
    return;
  }
  if (!name) {
    setModalStatus("Name is required.", "err");
    return;
  }

  setModalStatus("Saving…");
  try {
    if (editingId === null) {
      await api.screens.create(name, desc, tab_label, content);
    } else {
      await api.screens.update(editingId, { name, description: desc, tab_label, content });
    }
    closeModal();
    await loadScreens();
  } catch (e) {
    setModalStatus("Save failed: " + e.message, "err");
  }
}

async function deleteScreen(id) {
  const s = screens.find(x => x.id === id);
  if (!s) return;
  if (!confirm(`Delete screen "${s.name}"? This cannot be undone.`)) return;
  setStatus("Deleting…");
  try {
    await api.screens.delete(id);
    await loadScreens();
    setStatus("Deleted.");
  } catch (e) {
    setStatus("Delete failed: " + e.message, "err");
  }
}

function showModal()  { document.getElementById("modalOverlay").classList.remove("hidden"); }
function closeModal() { document.getElementById("modalOverlay").classList.add("hidden"); editingId = null; }

// ── Helpers ───────────────────────────────────────────────────────────────────

function setStatus(msg, type) {
  const el = document.getElementById("pageStatus");
  el.textContent = msg;
  el.className   = "status" + (type ? " " + type : "");
}

function setModalStatus(msg, type) {
  const el = document.getElementById("modalStatus");
  el.textContent = msg;
  el.className   = "status" + (type ? " " + type : "");
}

function escHtml(s) {
  return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;");
}
