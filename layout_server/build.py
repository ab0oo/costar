"""
build.py — LittleFS image assembly for CoStar layout server.

Assembles a staging directory from a layout record, then invokes mklittlefs
to produce a flashable .bin image.

Partition geometry (must match idf/partitions.csv and sdkconfig):
  size       = 0x1f0000  (1,994,752 bytes  ≈ 1.95 MB)
  block size = 4096      (ESP32 flash sector)
  page size  = 256       (CONFIG_LITTLEFS_PAGE_SIZE)

The build is deterministic: same layout content always produces the same image.
Images are cached by content hash and reused without rebuilding.
"""

import hashlib
import json
import logging
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import db

log = logging.getLogger("build")

# Must match idf/partitions.csv storage partition and sdkconfig
LITTLEFS_SIZE       = 0x1F0000   # 1,994,752 bytes
LITTLEFS_BLOCK_SIZE = 4096       # ESP32 flash sector size
LITTLEFS_PAGE_SIZE  = 256        # CONFIG_LITTLEFS_PAGE_SIZE

# PlatformIO ships mklittlefs; fall back to PATH for Docker environments
_MKLITTLEFS_CANDIDATES = [
    os.environ.get("MKLITTLEFS_PATH", ""),
    os.path.expanduser("~/.platformio/packages/tool-mklittlefs/mklittlefs"),
    "mklittlefs",
]


def _mklittlefs_bin() -> str:
    for candidate in _MKLITTLEFS_CANDIDATES:
        if candidate and shutil.which(candidate):
            return candidate
    raise RuntimeError(
        "mklittlefs not found. Set MKLITTLEFS_PATH or install PlatformIO."
    )


def image_path(layout_id: int) -> Path:
    """Return the path where a built image for this layout_id is stored."""
    return db.BUILDS_DIR / f"layout_{layout_id}.bin"


def _content_hash(layout: dict) -> str:
    """Stable SHA-256 of the layout's assembled file content."""
    h = hashlib.sha256()
    screens = sorted(layout["screens"], key=lambda s: s["position"])
    for slot in screens:
        content = db._read_screen_file(slot["filename"])
        h.update(json.dumps(content, sort_keys=True).encode())
    h.update(db._build_tabs_json(layout["screens"]))
    h.update(json.dumps({"version": 1, "networks": layout["wifi"]},
                        sort_keys=True).encode())
    h.update(json.dumps(layout["locale"], sort_keys=True).encode())
    return h.hexdigest()


def build_layout_image(layout: dict) -> dict:
    """
    Assemble and build a LittleFS image for the given layout dict
    (as returned by db.get_layout).

    Returns:
      {
        "layout_id": int,
        "image_size": int,       # bytes
        "content_hash": str,     # SHA-256 of assembled content
        "cached": bool,          # True if existing image was reused
        "files": [str, ...]      # list of filenames packed into the image
      }

    Raises RuntimeError on build failure.
    """
    layout_id    = layout["id"]
    content_hash = _content_hash(layout)
    hash_file    = db.BUILDS_DIR / f"layout_{layout_id}.sha256"
    out_path     = image_path(layout_id)

    # Return cached image if content hasn't changed
    if out_path.exists() and hash_file.exists():
        if hash_file.read_text().strip() == content_hash:
            log.info("layout %d: cache hit (%s)", layout_id, content_hash[:12])
            return {
                "layout_id":    layout_id,
                "image_size":   out_path.stat().st_size,
                "content_hash": content_hash,
                "cached":       True,
                "files":        _list_packed_files(layout),
            }

    with tempfile.TemporaryDirectory(prefix="costar_build_") as staging:
        staging_path = Path(staging)
        packed_files = _populate_staging(layout, staging_path)
        _run_mklittlefs(staging_path, out_path)

    hash_file.write_text(content_hash)
    log.info("layout %d: built %d bytes (%s)", layout_id,
             out_path.stat().st_size, content_hash[:12])

    return {
        "layout_id":    layout_id,
        "image_size":   out_path.stat().st_size,
        "content_hash": content_hash,
        "cached":       False,
        "files":        packed_files,
    }


def _populate_staging(layout: dict, staging: Path) -> list[str]:
    """
    Write all files for the layout into the staging directory.
    Returns the list of filenames written (relative to staging root).
    """
    files = []

    # Screen layout JSONs — named by slot position to match tabs.json paths
    for slot in layout["screens"]:
        fname   = f"screen_layout_{slot['position']}.json"
        content = db._read_screen_file(slot["filename"])
        (staging / fname).write_text(json.dumps(content, separators=(",", ":")))
        files.append(fname)

    # tabs.json — assembled from slot order and tab_label fields
    tabs_raw = db._build_tabs_json(layout["screens"])
    (staging / "tabs.json").write_bytes(tabs_raw)
    files.append("tabs.json")

    # wifi.json
    wifi_raw = json.dumps(
        {"version": 1, "networks": layout["wifi"]},
        separators=(",", ":"),
    ).encode()
    (staging / "wifi.json").write_bytes(wifi_raw)
    files.append("wifi.json")

    # locale.json — device display preferences
    if layout.get("locale"):
        locale_raw = json.dumps(layout["locale"], separators=(",", ":")).encode()
        (staging / "locale.json").write_bytes(locale_raw)
        files.append("locale.json")

    # Static assets — copy icons and maps from the firmware data/ directory
    data_dir = Path(os.environ.get("COSTAR_DATA", "../data"))
    for asset_dir in ("icons", "maps"):
        src = data_dir / asset_dir
        if src.is_dir():
            dst = staging / asset_dir
            shutil.copytree(src, dst)
            for f in dst.rglob("*"):
                if f.is_file():
                    files.append(str(f.relative_to(staging)))

    return sorted(files)


def _run_mklittlefs(staging: Path, out_path: Path):
    """Invoke mklittlefs to pack staging/ into out_path."""
    cmd = [
        _mklittlefs_bin(),
        "--create",  str(staging),
        "--size",    str(LITTLEFS_SIZE),
        "--block",   str(LITTLEFS_BLOCK_SIZE),
        "--page",    str(LITTLEFS_PAGE_SIZE),
        str(out_path),
    ]
    log.debug("mklittlefs: %s", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"mklittlefs failed (exit {result.returncode}):\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def _list_packed_files(layout: dict) -> list[str]:
    """List filenames that would be packed without rebuilding."""
    files = [f"screen_layout_{s['position']}.json" for s in layout["screens"]]
    files += ["tabs.json", "wifi.json"]
    if layout.get("locale"):
        files.append("locale.json")
    data_dir = Path(os.environ.get("COSTAR_DATA", "../data"))
    for asset_dir in ("icons", "maps"):
        src = data_dir / asset_dir
        if src.is_dir():
            for f in sorted(src.rglob("*")):
                if f.is_file():
                    files.append(str(asset_dir / f.relative_to(src)))
    return sorted(files)
