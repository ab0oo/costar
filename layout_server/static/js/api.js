/**
 * api.js — fetch wrappers for all /api/* and /manifest/* endpoints.
 * All functions return the parsed JSON body on success, or throw an Error
 * with a human-readable message on failure.
 */

// Derive the app's base path from the <base> tag so API calls work regardless
// of what prefix Apache proxies the app under (e.g. /costar/).
const _base = (document.getElementById("costarBase")?.href ?? "/").replace(/\/$/, "");

async function _fetch(method, path, body) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.headers["Content-Type"] = "application/json";
    opts.body = JSON.stringify(body);
  }
  const res = await fetch(_base + path, opts);
  const ct  = res.headers.get("content-type") || "";
  const data = ct.includes("application/json") ? await res.json() : await res.text();
  if (!res.ok) {
    const msg = (data && data.error) ? data.error : (typeof data === "string" ? data : res.statusText);
    throw new Error(`${method} ${path} → ${res.status}: ${msg}`);
  }
  return data;
}

// ── Screens ─────────────────────────────────────────────────────────────────

const api = {
  screens: {
    list:   ()                        => _fetch("GET",    "/api/screens"),
    get:    (id)                      => _fetch("GET",    `/api/screens/${id}`),
    create: (name, description, tab_label, content) =>
      _fetch("POST", "/api/screens", { name, description, tab_label, content }),
    update: (id, fields)              => _fetch("PUT",    `/api/screens/${id}`, fields),
    delete: (id)                      => _fetch("DELETE", `/api/screens/${id}`),
  },

  // ── Layouts ───────────────────────────────────────────────────────────────

  layouts: {
    list:   ()        => _fetch("GET",    "/api/layouts"),
    get:    (id)      => _fetch("GET",    `/api/layouts/${id}`),
    create: (payload) => _fetch("POST",   "/api/layouts",      payload),
    update: (id, payload) => _fetch("PUT", `/api/layouts/${id}`, payload),
    delete: (id)      => _fetch("DELETE", `/api/layouts/${id}`),
    build:  (id)      => _fetch("POST",   `/api/layouts/${id}/build`),
    imageUrl: (id)    => `${_base}/api/layouts/${id}/image`,
  },

  // ── Manifest (device) ─────────────────────────────────────────────────────

  manifest: {
    get:  (deviceNick) => _fetch("GET", `/manifest/${encodeURIComponent(deviceNick)}`),
  },
};
