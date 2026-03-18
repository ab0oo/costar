#!/usr/bin/env python3
"""
solar_proxy.py — CoStar space-weather proxy

Fetches solar and geomagnetic data from US government sources (NOAA SWPC),
calculates HF band conditions from SFI + Kp using published propagation physics,
and serves a flat JSON object over plain HTTP so the ESP32 can poll without TLS.

Data sources (all public-domain US government data, no API keys required):
  - https://services.swpc.noaa.gov/products/summary/10cm-flux.json   (SFI)
  - https://services.swpc.noaa.gov/json/planetary_k_index_1m.json    (Kp, live)
  - https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json  (Ap running)
  - https://services.swpc.noaa.gov/json/goes/primary/xray-flares-latest.json  (X-ray class)
  - https://services.swpc.noaa.gov/products/summary/solar-wind-speed.json   (solar wind)
  - https://services.swpc.noaa.gov/products/summary/solar-wind-mag-field.json (Bz/Bt)
  - https://services.swpc.noaa.gov/products/noaa-scales.json          (R/S/G storm scale)
  - https://services.swpc.noaa.gov/json/solar_regions.json            (sunspot count)

HF band conditions (Good/Fair/Poor) are calculated from SFI + Kp using the
threshold table published by N0NBH at hamqsl.com/solar2.html plus standard
ionospheric physics (D-layer day/night, F2-layer SFI sensitivity per band).

Endpoints:
  GET /solar    -> JSON payload (see schema in fetch_all())
  GET /health   -> {"ok": true, "age_s": N, "error": ""}

Usage:
  python3 solar_proxy.py [--port 8086] [--host 0.0.0.0] [--interval 300]
"""

import argparse
import json
import logging
import threading
import time
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

FETCH_INTERVAL_S = 300       # 5 minutes (most NOAA feeds update every 1-5 min)
FETCH_TIMEOUT_S  = 12
USER_AGENT       = "CoStar-SolarProxy/1.0 (github.com/costar-esp32)"

log = logging.getLogger("solar_proxy")

# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

_lock       = threading.Lock()
_last_json  : bytes = b"{}"
_last_fetch : float = 0.0
_last_error : str   = ""

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def _get_json(url: str):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT_S) as resp:
        return json.loads(resp.read())


def _get_text(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT_S) as resp:
        return resp.read().decode("utf-8", errors="replace")


def _num(val, default=None):
    """Coerce string or numeric to int/float, or return default."""
    if val is None:
        return default
    try:
        v = float(str(val).strip())
        return int(v) if v == int(v) else v
    except (ValueError, TypeError):
        return default

# ---------------------------------------------------------------------------
# HF band condition calculation
#
# Based on:
#   - N0NBH severity table: hamqsl.com/solar2.html
#   - ARRL/RSGB ionospheric physics (SFI vs band MUF, D-layer absorption)
#
# Thresholds chosen to match typical N0NBH output across a broad range of
# observed conditions.  Band physics:
#   80m-40m : F-layer night, D-layer absorbs day; geomagnetic > SFI
#   30m-20m : F2 moderate SFI dependence; geo + SFI roughly equal weight
#   17m-15m : F2 heavy SFI dependence; unreliable at night below SFI ~140
#   12m-10m : F2 extreme SFI dependence; closed at night and SFI < 120
# ---------------------------------------------------------------------------

def _geo_score(kp: float) -> int:
    """Convert Kp to a 0-3 geo quality score (3=best)."""
    if kp <= 1:   return 3   # quiet
    if kp <= 2:   return 3
    if kp <= 3:   return 2   # active
    if kp <= 4:   return 1   # minor storm
    return 0                 # moderate+ storm


def _to_label(score: int) -> str:
    if score >= 3: return "Good"
    if score >= 2: return "Fair"
    return "Poor"


def calc_hf_conditions(sfi: float, kp: float) -> dict:
    """
    Return dict with keys '80m-40m', '30m-20m', '17m-15m', '12m-10m',
    each containing {'day': label, 'night': label}.
    """
    geo = _geo_score(kp)
    result = {}

    # --- 80m-40m ---
    # Day: D-layer absorption; geo dominates; SFI provides a small floor bonus
    day_base   = 3 if sfi >= 70 else 2            # high SFI slightly improves noise floor
    day_score  = min(day_base, geo)
    # Night: D-layer gone; F-layer reflects; geo still the primary variable
    night_score = min(3, geo + (1 if sfi >= 90 else 0))
    result['80m-40m'] = {'day': _to_label(day_score), 'night': _to_label(night_score)}

    # --- 30m-20m ---
    # SFI 80 = Poor, 90 = Fair, 100+ = Good base; geo degrades
    if sfi >= 100: sfi_day = 3
    elif sfi >= 85: sfi_day = 2
    else:           sfi_day = 1
    # Night: F2 persists but slightly weaker; penalty only at very low SFI
    sfi_night = sfi_day if sfi >= 90 else max(1, sfi_day - 1)
    result['30m-20m'] = {
        'day':   _to_label(min(sfi_day,   geo)),
        'night': _to_label(min(sfi_night, geo)),
    }

    # --- 17m-15m ---
    # SFI 100 = Poor, 120 = Fair, 150+ = Good; night degrades by ~1 level
    if sfi >= 150: sfi_day = 3
    elif sfi >= 115: sfi_day = 2
    else:            sfi_day = 1
    sfi_night = max(1, sfi_day - 1)   # night always one level lower
    result['17m-15m'] = {
        'day':   _to_label(min(sfi_day,   geo)),
        'night': _to_label(min(sfi_night, geo)),
    }

    # --- 12m-10m ---
    # SFI 120 = Poor, 150 = Fair, 175+ = Good; closed at night
    if sfi >= 175: sfi_day = 3
    elif sfi >= 140: sfi_day = 2
    elif sfi >= 110: sfi_day = 1
    else:            sfi_day = 1
    # Night: essentially closed below SFI ~200
    sfi_night = 1 if sfi < 200 else 2
    result['12m-10m'] = {
        'day':   _to_label(min(sfi_day,   geo)),
        'night': _to_label(min(sfi_night, geo)),
    }

    return result


# ---------------------------------------------------------------------------
# Sunspot total from solar_regions.json
# ---------------------------------------------------------------------------

def _sum_sunspots(regions: list) -> int:
    """Sum number_spots across all active regions for current day."""
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    total = 0
    seen = set()
    for r in regions:
        # Each region may appear multiple times (one per observatory); deduplicate
        region_id = r.get("region") or r.get("Region")
        obs_date  = (r.get("observed_date") or r.get("Obsdate") or "")[:10]
        if obs_date != today:
            continue
        key = str(region_id)
        if key in seen:
            continue
        seen.add(key)
        spots = r.get("number_spots") or r.get("Numspot") or 0
        total += _num(spots, 0)
    return total


# ---------------------------------------------------------------------------
# Main fetch
# ---------------------------------------------------------------------------

def fetch_all() -> dict:
    errors = []

    # --- SFI ---
    sfi = None
    try:
        d = _get_json("https://services.swpc.noaa.gov/products/summary/10cm-flux.json")
        sfi = _num(d.get("Flux"))
    except Exception as e:
        errors.append(f"sfi: {e}")

    # --- Live Kp (most recent 1-minute estimate) ---
    kp = None
    try:
        entries = _get_json("https://services.swpc.noaa.gov/json/planetary_k_index_1m.json")
        if entries:
            last = entries[-1]
            kp = _num(last.get("estimated_kp") or last.get("kp_index"))
    except Exception as e:
        errors.append(f"kp: {e}")

    # --- Running Ap (A-index) from 3-hour Kp product ---
    a_index = None
    try:
        rows = _get_json("https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json")
        # rows[0] is header; find most recent row with a numeric a_running
        for row in reversed(rows[1:]):
            val = _num(row[2]) if len(row) > 2 else None
            if val is not None:
                a_index = val
                break
    except Exception as e:
        errors.append(f"a_index: {e}")

    # --- X-ray class (current instantaneous classification) ---
    xray_class = None
    try:
        flares = _get_json("https://services.swpc.noaa.gov/json/goes/primary/xray-flares-latest.json")
        if flares:
            xray_class = flares[0].get("current_class") or flares[0].get("max_class")
    except Exception as e:
        errors.append(f"xray: {e}")

    # --- Solar wind speed ---
    solar_wind = None
    try:
        d = _get_json("https://services.swpc.noaa.gov/products/summary/solar-wind-speed.json")
        solar_wind = _num(d.get("WindSpeed"))
    except Exception as e:
        errors.append(f"solar_wind: {e}")

    # --- IMF Bz and Bt ---
    bz = None
    bt = None
    try:
        d = _get_json("https://services.swpc.noaa.gov/products/summary/solar-wind-mag-field.json")
        bz = _num(d.get("Bz"))
        bt = _num(d.get("Bt"))
    except Exception as e:
        errors.append(f"mag_field: {e}")

    # --- NOAA R/S/G storm scales (current conditions) ---
    geomag_scale = None
    radio_scale  = None
    try:
        scales = _get_json("https://services.swpc.noaa.gov/products/noaa-scales.json")
        current = scales.get("0") or scales.get(0) or {}
        g = current.get("G", {})
        r = current.get("R", {})
        geomag_scale = f"G{g.get('Scale','0')} {g.get('Text','none')}".strip()
        radio_scale  = f"R{r.get('Scale','0')} {r.get('Text','none')}".strip()
    except Exception as e:
        errors.append(f"scales: {e}")

    # --- Sunspot count from solar regions ---
    sunspots = None
    try:
        regions = _get_json("https://services.swpc.noaa.gov/json/solar_regions.json")
        sunspots = _sum_sunspots(regions)
    except Exception as e:
        errors.append(f"sunspots: {e}")

    # --- HF band conditions (calculated) ---
    hf_conditions = {}
    if sfi is not None and kp is not None:
        try:
            hf_conditions = calc_hf_conditions(float(sfi), float(kp))
        except Exception as e:
            errors.append(f"hf_calc: {e}")

    # --- Assemble result ---
    result = {
        "sfi":           sfi,
        "a_index":       a_index,
        "k_index":       kp,
        "xray":          xray_class,
        "sunspots":      sunspots,
        "solar_wind":    solar_wind,
        "bz":            bz,
        "bt":            bt,
        "geomag_scale":  geomag_scale,   # e.g. "G0 none" or "G1 minor"
        "radio_scale":   radio_scale,    # e.g. "R0 none" or "R1 minor"
        "hf_conditions": hf_conditions,
        "fetched_utc":   datetime.now(timezone.utc).strftime("%d %b %Y %H%M UTC"),
        "_errors":       errors if errors else None,
    }

    if errors:
        log.warning("fetch completed with %d partial errors: %s", len(errors), "; ".join(errors))

    return result


# ---------------------------------------------------------------------------
# Background refresh thread
# ---------------------------------------------------------------------------

def _refresh_loop():
    global _last_json, _last_fetch, _last_error
    while True:
        try:
            data    = fetch_all()
            payload = json.dumps(data, separators=(",", ":")).encode()
            with _lock:
                _last_json  = payload
                _last_fetch = time.time()
                _last_error = "; ".join(data.get("_errors") or [])
            log.info("refreshed: sfi=%s kp=%s xray=%s sw=%s bz=%s",
                     data.get("sfi"), data.get("k_index"),
                     data.get("xray"), data.get("solar_wind"), data.get("bz"))
        except Exception as exc:
            msg = str(exc)
            with _lock:
                _last_error = msg
            log.warning("fetch failed: %s", msg)

        time.sleep(FETCH_INTERVAL_S)


def start_refresh_thread():
    t = threading.Thread(target=_refresh_loop, daemon=True, name="solar-refresh")
    t.start()


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        log.debug("http %s - %s", self.address_string(), fmt % args)

    def _send(self, status: int, content_type: str, body: bytes):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/")

        if path == "/solar":
            with _lock:
                body  = _last_json
                error = _last_error
            if body == b"{}" and error:
                self._send(503, "application/json",
                           json.dumps({"error": error}).encode())
            else:
                self._send(200, "application/json", body)

        elif path == "/health":
            with _lock:
                age_s = int(time.time() - _last_fetch) if _last_fetch else -1
                error = _last_error
            body = json.dumps({"ok": not bool(error), "age_s": age_s,
                               "error": error}).encode()
            self._send(200, "application/json", body)

        else:
            self._send(404, "text/plain", b"not found")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    global FETCH_INTERVAL_S
    parser = argparse.ArgumentParser(description="CoStar solar weather proxy")
    parser.add_argument("--port",     type=int, default=8086)
    parser.add_argument("--host",     default="0.0.0.0")
    parser.add_argument("--interval", type=int, default=FETCH_INTERVAL_S,
                        help="upstream fetch interval in seconds (default 300)")
    parser.add_argument("--debug",    action="store_true")
    args = parser.parse_args()
    FETCH_INTERVAL_S = args.interval

    logging.basicConfig(
        level=logging.DEBUG if args.debug else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    log.info("initial fetch from NOAA SWPC …")
    try:
        data = fetch_all()
        with _lock:
            _last_json  = json.dumps(data, separators=(",", ":")).encode()
            _last_fetch = time.time()
        log.info("initial fetch ok: sfi=%s kp=%s xray=%s",
                 data.get("sfi"), data.get("k_index"), data.get("xray"))
    except Exception as exc:
        log.warning("initial fetch failed (will retry in background): %s", exc)

    start_refresh_thread()

    server = HTTPServer((args.host, args.port), Handler)
    log.info("listening on %s:%d  interval=%ds", args.host, args.port, FETCH_INTERVAL_S)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("shutting down")


if __name__ == "__main__":
    main()
