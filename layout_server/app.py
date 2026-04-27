"""
app.py — CoStar layout server

Serves the screen library, layout assembly API, device manifest endpoints,
and the static web UI.

Environment variables:
  COSTAR_DB       path to SQLite database file (default: costar.db)
  COSTAR_SCREENS  path to screen JSON directory (default: screens/)
  COSTAR_BUILDS   path to LittleFS build output directory (default: builds/)
  COSTAR_DATA     path to firmware data/ directory, used for seeding on first run
  PORT            HTTP listen port (default: 8087)
"""

import json
import logging
import os
from pathlib import Path

from flask import Flask, abort, jsonify, request, send_from_directory
from werkzeug.middleware.proxy_fix import ProxyFix

import db

log = logging.getLogger("layout_server")

_prefix = os.environ.get("COSTAR_PREFIX", "").rstrip("/")  # e.g. "/costar"

app = Flask(__name__, static_folder="static", static_url_path=f"{_prefix}/static")
app.config["APPLICATION_ROOT"] = _prefix or "/"
app.wsgi_app = ProxyFix(app.wsgi_app, x_prefix=1)


# ---------------------------------------------------------------------------
# Startup
# ---------------------------------------------------------------------------

def _startup():
    db.init_db()
    data_dir = Path(os.environ.get("COSTAR_DATA", "../data"))
    if data_dir.is_dir():
        db.seed_from_data_dir(data_dir)
        log.info("seeded screens from %s", data_dir)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _ok(data, status=200):
    return jsonify(data), status


def _err(msg, status=400):
    return jsonify({"error": msg}), status


def _require_json(*fields):
    """Parse request JSON and return (data, error_response).  error_response
    is None on success."""
    if not request.is_json:
        return None, _err("Content-Type must be application/json")
    data = request.get_json(silent=True)
    if data is None:
        return None, _err("Invalid JSON body")
    missing = [f for f in fields if f not in data]
    if missing:
        return None, _err(f"Missing required fields: {', '.join(missing)}")
    return data, None


# ---------------------------------------------------------------------------
# Root — serve the UI shell
# ---------------------------------------------------------------------------

@app.route(f"{_prefix}/")
@app.route(f"{_prefix}")
def root():
    return send_from_directory("static", "screens.html")


@app.route(f"{_prefix}/<path:filename>")
def static_pages(filename):
    if filename.endswith(".html"):
        return send_from_directory("static", filename)
    abort(404)


# ---------------------------------------------------------------------------
# Screens API
# ---------------------------------------------------------------------------

@app.route(f"{_prefix}/api/screens", methods=["GET"])
def api_list_screens():
    return _ok(db.list_screens())


@app.route(f"{_prefix}/api/screens/<int:screen_id>", methods=["GET"])
def api_get_screen(screen_id):
    s = db.get_screen(screen_id)
    if s is None:
        return _err("Screen not found", 404)
    return _ok(s)


@app.route(f"{_prefix}/api/screens", methods=["POST"])
def api_create_screen():
    data, err = _require_json("name", "content")
    if err:
        return err
    name        = str(data["name"]).strip()
    description = str(data.get("description", "")).strip()
    tab_label   = str(data.get("tab_label",   "")).strip()[:6]
    content     = data["content"]
    if not name:
        return _err("name cannot be empty")
    if not isinstance(content, dict):
        return _err("content must be a JSON object")
    try:
        s = db.create_screen(name, description, tab_label, content)
    except Exception as e:
        return _err(str(e))
    return _ok(s, 201)


@app.route(f"{_prefix}/api/screens/<int:screen_id>", methods=["PUT"])
def api_update_screen(screen_id):
    data, err = _require_json()
    if err:
        return err
    name        = data.get("name")
    description = data.get("description")
    tab_label   = str(data["tab_label"]).strip()[:6] if "tab_label" in data else None
    content     = data.get("content")
    if content is not None and not isinstance(content, dict):
        return _err("content must be a JSON object")
    try:
        s = db.update_screen(screen_id, name, description, tab_label, content)
    except Exception as e:
        return _err(str(e))
    if s is None:
        return _err("Screen not found", 404)
    return _ok(s)


@app.route(f"{_prefix}/api/screens/<int:screen_id>", methods=["DELETE"])
def api_delete_screen(screen_id):
    try:
        deleted = db.delete_screen(screen_id)
    except ValueError as e:
        return _err(str(e), 409)
    if not deleted:
        return _err("Screen not found", 404)
    return _ok({"deleted": screen_id})


# ---------------------------------------------------------------------------
# Layouts API
# ---------------------------------------------------------------------------

@app.route(f"{_prefix}/api/layouts", methods=["GET"])
def api_list_layouts():
    return _ok(db.list_layouts())


@app.route(f"{_prefix}/api/layouts/<int:layout_id>", methods=["GET"])
def api_get_layout(layout_id):
    layout = db.get_layout(layout_id)
    if layout is None:
        return _err("Layout not found", 404)
    return _ok(layout)


@app.route(f"{_prefix}/api/layouts", methods=["POST"])
def api_create_layout():
    data, err = _require_json("name", "screen_ids")
    if err:
        return err
    name        = str(data["name"]).strip()
    device_nick = str(data.get("device_nick", "")).strip()
    screen_ids  = data["screen_ids"]
    wifi        = data.get("wifi",   [])
    locale      = data.get("locale", {})
    if not name:
        return _err("name cannot be empty")
    if not isinstance(screen_ids, list):
        return _err("screen_ids must be an array")
    try:
        layout = db.create_layout(name, device_nick, screen_ids, wifi, locale)
    except (ValueError, Exception) as e:
        return _err(str(e))
    return _ok(layout, 201)


@app.route(f"{_prefix}/api/layouts/<int:layout_id>", methods=["PUT"])
def api_update_layout(layout_id):
    data, err = _require_json()
    if err:
        return err
    screen_ids = data.get("screen_ids")
    if screen_ids is not None and not isinstance(screen_ids, list):
        return _err("screen_ids must be an array")
    try:
        layout = db.update_layout(
            layout_id,
            name=data.get("name"),
            device_nick=data.get("device_nick"),
            screen_ids=screen_ids,
            wifi=data.get("wifi"),
            locale=data.get("locale"),
        )
    except (ValueError, Exception) as e:
        return _err(str(e))
    if layout is None:
        return _err("Layout not found", 404)
    return _ok(layout)


@app.route(f"{_prefix}/api/layouts/<int:layout_id>", methods=["DELETE"])
def api_delete_layout(layout_id):
    deleted = db.delete_layout(layout_id)
    if not deleted:
        return _err("Layout not found", 404)
    return _ok({"deleted": layout_id})


# ---------------------------------------------------------------------------
# LittleFS image build
# ---------------------------------------------------------------------------

@app.route(f"{_prefix}/api/layouts/<int:layout_id>/build", methods=["POST"])
def api_build_layout(layout_id):
    layout = db.get_layout(layout_id)
    if layout is None:
        return _err("Layout not found", 404)
    try:
        import build as builder
        result = builder.build_layout_image(layout)
    except Exception as e:
        log.exception("build failed for layout %d", layout_id)
        return _err(f"Build failed: {e}", 500)
    return _ok(result)


@app.route(f"{_prefix}/api/layouts/<int:layout_id>/image", methods=["GET"])
def api_download_image(layout_id):
    layout = db.get_layout(layout_id)
    if layout is None:
        return _err("Layout not found", 404)
    import build as builder
    bin_path = builder.image_path(layout_id)
    if not bin_path.exists():
        return _err("No built image for this layout — POST /build first", 404)
    return send_from_directory(
        str(bin_path.parent),
        bin_path.name,
        as_attachment=True,
        download_name=f"costar_layout_{layout_id}.bin",
        mimetype="application/octet-stream",
    )


# ---------------------------------------------------------------------------
# Device manifest endpoints (polled by ESP32 at boot)
# ---------------------------------------------------------------------------

@app.route(f"{_prefix}/manifest/<device_nick>", methods=["GET"])
def api_manifest(device_nick):
    manifest = db.get_manifest(device_nick)
    if manifest is None:
        return _err(f"No layout assigned to device '{device_nick}'", 404)
    return _ok(manifest)


@app.route(f"{_prefix}/files/<device_nick>/<filename>", methods=["GET"])
def api_manifest_file(device_nick, filename):
    # Only allow safe filenames: screen_layout_N.json, tabs.json, wifi.json
    allowed = {"tabs.json", "wifi.json"} | {f"screen_layout_{i}.json" for i in range(1, 6)}
    if filename not in allowed:
        abort(404)
    data = db.get_manifest_file(device_nick, filename)
    if data is None:
        abort(404)
    return app.response_class(data, mimetype="application/json")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    _startup()
    port = int(os.environ.get("PORT", 8087))
    log.info("layout_server listening on port %d", port)
    app.run(host="0.0.0.0", port=port)
