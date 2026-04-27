"""
db.py — SQLite schema and query layer for the CoStar layout server.

All screen JSON content lives as flat files under SCREENS_DIR; SQLite holds
only metadata and relational structure.  Layout-built LittleFS images are
ephemeral and stored under BUILDS_DIR (not tracked in the DB).
"""

import hashlib
import json
import os
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path

DB_PATH      = Path(os.environ.get("COSTAR_DB",      "costar.db"))
SCREENS_DIR  = Path(os.environ.get("COSTAR_SCREENS", "screens"))
BUILDS_DIR   = Path(os.environ.get("COSTAR_BUILDS",  "builds"))

SCREENS_DIR.mkdir(parents=True, exist_ok=True)
BUILDS_DIR.mkdir(parents=True, exist_ok=True)

SCHEMA = """
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS screens (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL UNIQUE,
    description TEXT    NOT NULL DEFAULT '',
    tab_label   TEXT    NOT NULL DEFAULT '',  -- short label shown in device tab bar (≤6 chars)
    filename    TEXT    NOT NULL UNIQUE,   -- relative to SCREENS_DIR
    created_at  REAL    NOT NULL,
    updated_at  REAL    NOT NULL
);

CREATE TABLE IF NOT EXISTS layouts (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL UNIQUE,
    device_nick TEXT    NOT NULL DEFAULT '',
    created_at  REAL    NOT NULL,
    updated_at  REAL    NOT NULL
);

-- Ordered screen slots for a layout (position 1-5)
CREATE TABLE IF NOT EXISTS layout_screens (
    layout_id   INTEGER NOT NULL REFERENCES layouts(id) ON DELETE CASCADE,
    position    INTEGER NOT NULL CHECK (position BETWEEN 1 AND 5),
    screen_id   INTEGER NOT NULL REFERENCES screens(id),
    PRIMARY KEY (layout_id, position)
);

-- Per-layout device configuration (wifi + locale)
CREATE TABLE IF NOT EXISTS layout_config (
    layout_id   INTEGER PRIMARY KEY REFERENCES layouts(id) ON DELETE CASCADE,
    -- JSON array of {ssid, password} objects
    wifi_json   TEXT    NOT NULL DEFAULT '[]',
    -- JSON object: {temp_unit, distance_unit, clock_24h, timezone}
    locale_json TEXT    NOT NULL DEFAULT '{}'
);
"""


@contextmanager
def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def init_db():
    with get_db() as conn:
        conn.executescript(SCHEMA)


# ---------------------------------------------------------------------------
# Screens
# ---------------------------------------------------------------------------

def list_screens() -> list[dict]:
    with get_db() as conn:
        rows = conn.execute(
            "SELECT id, name, description, tab_label, filename, created_at, updated_at "
            "FROM screens ORDER BY name"
        ).fetchall()
    return [dict(r) for r in rows]


def get_screen(screen_id: int) -> dict | None:
    with get_db() as conn:
        row = conn.execute(
            "SELECT id, name, description, tab_label, filename, created_at, updated_at "
            "FROM screens WHERE id = ?", (screen_id,)
        ).fetchone()
    if row is None:
        return None
    s = dict(row)
    s["content"] = _read_screen_file(s["filename"])
    return s


def create_screen(name: str, description: str, tab_label: str, content: dict) -> dict:
    filename = _screen_filename(name)
    now = time.time()
    _write_screen_file(filename, content)
    try:
        with get_db() as conn:
            cur = conn.execute(
                "INSERT INTO screens (name, description, tab_label, filename, created_at, updated_at) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (name, description, tab_label, filename, now, now),
            )
            screen_id = cur.lastrowid
    except Exception:
        _delete_screen_file(filename)
        raise
    return get_screen(screen_id)


def update_screen(screen_id: int, name: str | None, description: str | None,
                  tab_label: str | None, content: dict | None) -> dict | None:
    existing = get_screen(screen_id)
    if existing is None:
        return None
    now = time.time()
    new_name        = name        if name        is not None else existing["name"]
    new_description = description if description is not None else existing["description"]
    new_tab_label   = tab_label   if tab_label   is not None else existing["tab_label"]
    new_filename    = _screen_filename(new_name)

    if content is not None:
        _write_screen_file(new_filename, content)
        if new_filename != existing["filename"]:
            _delete_screen_file(existing["filename"])
    elif new_filename != existing["filename"]:
        # rename only — move the file
        src = SCREENS_DIR / existing["filename"]
        dst = SCREENS_DIR / new_filename
        src.rename(dst)

    with get_db() as conn:
        conn.execute(
            "UPDATE screens SET name=?, description=?, tab_label=?, filename=?, updated_at=? WHERE id=?",
            (new_name, new_description, new_tab_label, new_filename, now, screen_id),
        )
    return get_screen(screen_id)


def delete_screen(screen_id: int) -> bool:
    """Delete a screen. Raises ValueError if the screen is used by any layout."""
    with get_db() as conn:
        row = conn.execute(
            "SELECT filename FROM screens WHERE id = ?", (screen_id,)
        ).fetchone()
        if row is None:
            return False
        used = conn.execute(
            "SELECT COUNT(*) FROM layout_screens WHERE screen_id = ?", (screen_id,)
        ).fetchone()[0]
        if used:
            raise ValueError(f"Screen {screen_id} is used by {used} layout(s)")
        conn.execute("DELETE FROM screens WHERE id = ?", (screen_id,))
    _delete_screen_file(row["filename"])
    return True


# ---------------------------------------------------------------------------
# Layouts
# ---------------------------------------------------------------------------

def list_layouts() -> list[dict]:
    with get_db() as conn:
        rows = conn.execute(
            "SELECT id, name, device_nick, created_at, updated_at "
            "FROM layouts ORDER BY name"
        ).fetchall()
    return [dict(r) for r in rows]


def get_layout(layout_id: int) -> dict | None:
    with get_db() as conn:
        row = conn.execute(
            "SELECT id, name, device_nick, created_at, updated_at "
            "FROM layouts WHERE id = ?", (layout_id,)
        ).fetchone()
        if row is None:
            return None
        layout = dict(row)

        screens = conn.execute(
            "SELECT ls.position, s.id, s.name, s.description, s.tab_label, s.filename "
            "FROM layout_screens ls "
            "JOIN screens s ON s.id = ls.screen_id "
            "WHERE ls.layout_id = ? "
            "ORDER BY ls.position",
            (layout_id,),
        ).fetchall()
        layout["screens"] = [dict(s) for s in screens]

        cfg = conn.execute(
            "SELECT wifi_json, locale_json FROM layout_config WHERE layout_id = ?",
            (layout_id,),
        ).fetchone()
        if cfg:
            layout["wifi"]   = json.loads(cfg["wifi_json"])
            layout["locale"] = json.loads(cfg["locale_json"])
        else:
            layout["wifi"]   = []
            layout["locale"] = {}

    return layout


def create_layout(name: str, device_nick: str,
                  screen_ids: list[int],
                  wifi: list[dict],
                  locale: dict) -> dict:
    _validate_screen_slots(screen_ids)
    now = time.time()
    with get_db() as conn:
        cur = conn.execute(
            "INSERT INTO layouts (name, device_nick, created_at, updated_at) VALUES (?,?,?,?)",
            (name, device_nick, now, now),
        )
        layout_id = cur.lastrowid
        _insert_layout_screens(conn, layout_id, screen_ids)
        conn.execute(
            "INSERT INTO layout_config (layout_id, wifi_json, locale_json) VALUES (?,?,?)",
            (layout_id, json.dumps(wifi), json.dumps(locale)),
        )
    return get_layout(layout_id)


def update_layout(layout_id: int, name: str | None, device_nick: str | None,
                  screen_ids: list[int] | None,
                  wifi: list[dict] | None,
                  locale: dict | None) -> dict | None:
    existing = get_layout(layout_id)
    if existing is None:
        return None
    if screen_ids is not None:
        _validate_screen_slots(screen_ids)
    now = time.time()
    with get_db() as conn:
        conn.execute(
            "UPDATE layouts SET name=COALESCE(?,name), device_nick=COALESCE(?,device_nick), "
            "updated_at=? WHERE id=?",
            (name, device_nick, now, layout_id),
        )
        if screen_ids is not None:
            conn.execute("DELETE FROM layout_screens WHERE layout_id=?", (layout_id,))
            _insert_layout_screens(conn, layout_id, screen_ids)
        if wifi is not None or locale is not None:
            new_wifi   = json.dumps(wifi   if wifi   is not None else existing["wifi"])
            new_locale = json.dumps(locale if locale is not None else existing["locale"])
            conn.execute(
                "INSERT INTO layout_config (layout_id, wifi_json, locale_json) VALUES (?,?,?) "
                "ON CONFLICT(layout_id) DO UPDATE SET wifi_json=excluded.wifi_json, "
                "locale_json=excluded.locale_json",
                (layout_id, new_wifi, new_locale),
            )
    return get_layout(layout_id)


def delete_layout(layout_id: int) -> bool:
    with get_db() as conn:
        row = conn.execute("SELECT id FROM layouts WHERE id=?", (layout_id,)).fetchone()
        if row is None:
            return False
        conn.execute("DELETE FROM layouts WHERE id=?", (layout_id,))
    return True


# ---------------------------------------------------------------------------
# Device manifest (for ESP32 poll-on-boot)
# ---------------------------------------------------------------------------

def get_manifest(device_nick: str) -> dict | None:
    """
    Build the manifest a device polls at boot.  Returns a dict:
      {
        "layout_id": int,
        "files": {
          "screen_layout_1.json": {"md5": "...", "size": int},
          ...
          "tabs.json":            {"md5": "...", "size": int},
        }
      }
    Returns None if no layout is assigned to this device_nick.
    """
    with get_db() as conn:
        row = conn.execute(
            "SELECT id FROM layouts WHERE device_nick=?", (device_nick,)
        ).fetchone()
    if row is None:
        return None

    layout = get_layout(row["id"])
    files  = {}

    # Individual screen files (named by slot position on device)
    for slot in layout["screens"]:
        fname   = f"screen_layout_{slot['position']}.json"
        content = _read_screen_file(slot["filename"])
        raw     = json.dumps(content, separators=(",", ":")).encode()
        files[fname] = {"md5": _md5(raw), "size": len(raw)}

    # tabs.json assembled from slot order
    tabs_raw = _build_tabs_json(layout["screens"])
    files["tabs.json"] = {"md5": _md5(tabs_raw), "size": len(tabs_raw)}

    # wifi.json
    wifi_raw = json.dumps({"version": 1, "networks": layout["wifi"]},
                          separators=(",", ":")).encode()
    files["wifi.json"] = {"md5": _md5(wifi_raw), "size": len(wifi_raw)}

    return {"layout_id": layout["id"], "files": files}


def get_manifest_file(device_nick: str, filename: str) -> bytes | None:
    """Return the raw bytes for a specific file in a device's manifest."""
    with get_db() as conn:
        row = conn.execute(
            "SELECT id FROM layouts WHERE device_nick=?", (device_nick,)
        ).fetchone()
    if row is None:
        return None

    layout = get_layout(row["id"])

    if filename == "tabs.json":
        return _build_tabs_json(layout["screens"])

    if filename == "wifi.json":
        return json.dumps({"version": 1, "networks": layout["wifi"]},
                          separators=(",", ":")).encode()

    # screen_layout_<n>.json
    for slot in layout["screens"]:
        if filename == f"screen_layout_{slot['position']}.json":
            content = _read_screen_file(slot["filename"])
            return json.dumps(content, separators=(",", ":")).encode()

    return None


# ---------------------------------------------------------------------------
# Seed existing screens from data/ directory
# ---------------------------------------------------------------------------

def seed_from_data_dir(data_dir: Path):
    """
    Import screen_layout_*.json files from the firmware data/ directory into
    the screen library.  Safe to call repeatedly — skips already-imported files.
    """
    for path in sorted(data_dir.glob("screen_layout_*.json")):
        name = path.stem  # e.g. "screen_layout_a"
        with get_db() as conn:
            existing = conn.execute(
                "SELECT id FROM screens WHERE name=?", (name,)
            ).fetchone()
        if existing:
            continue
        content = json.loads(path.read_text())
        # Derive a default tab label from the filename stem
        label = _tab_label(name)
        create_screen(name=name, description="", tab_label=label, content=content)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _screen_filename(name: str) -> str:
    """Derive a safe filename from a screen name."""
    safe = "".join(c if (c.isalnum() or c in "-_") else "_" for c in name.lower())
    return safe + ".json"


def _read_screen_file(filename: str) -> dict:
    return json.loads((SCREENS_DIR / filename).read_text())


def _write_screen_file(filename: str, content: dict):
    (SCREENS_DIR / filename).write_text(json.dumps(content, indent=2))


def _delete_screen_file(filename: str):
    p = SCREENS_DIR / filename
    if p.exists():
        p.unlink()


def _validate_screen_slots(screen_ids: list[int]):
    if not (1 <= len(screen_ids) <= 5):
        raise ValueError("A layout must have between 1 and 5 screens")
    if len(set(screen_ids)) != len(screen_ids):
        raise ValueError("Duplicate screen_ids in layout")


def _insert_layout_screens(conn, layout_id: int, screen_ids: list[int]):
    for pos, sid in enumerate(screen_ids, start=1):
        conn.execute(
            "INSERT INTO layout_screens (layout_id, position, screen_id) VALUES (?,?,?)",
            (layout_id, pos, sid),
        )


def _build_tabs_json(screens: list[dict]) -> bytes:
    tabs = [
        {
            "label": s["tab_label"] or _tab_label(s["name"]),
            "path":  f"/littlefs/screen_layout_{s['position']}.json",
        }
        for s in screens
    ]
    default = f"/littlefs/screen_layout_{screens[0]['position']}.json" if screens else ""
    return json.dumps({"default": default, "tabs": tabs}, separators=(",", ":")).encode()


def _tab_label(name: str) -> str:
    """Derive a fallback short tab label from a screen name (used only when tab_label is unset)."""
    parts = name.replace("screen_layout_", "").replace("_", " ").replace("-", " ").split()
    if not parts:
        return name[:4].upper()
    return parts[0][:4].upper()


def _md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()
