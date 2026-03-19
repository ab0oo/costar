#!/usr/bin/env python3
"""
solar_proxy.py - CoStar space-weather proxy

Fetches solar and geomagnetic data exclusively from US government / scientific
sources (NOAA SWPC), calculates HF band conditions and the aurora activity index
from first-principles data, and serves a flat JSON object over plain HTTP so the
ESP32 can poll without TLS.

Data sources (all public-domain US government data, no API keys required):
  - https://services.swpc.noaa.gov/products/summary/10cm-flux.json         (SFI)
  - https://services.swpc.noaa.gov/json/planetary_k_index_1m.json          (Kp live)
  - https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json    (Ap running)
  - https://services.swpc.noaa.gov/json/goes/primary/xray-flares-latest.json (X-ray class)
  - https://services.swpc.noaa.gov/products/summary/solar-wind-speed.json  (solar wind)
  - https://services.swpc.noaa.gov/products/summary/solar-wind-mag-field.json (Bz/Bt)
  - https://services.swpc.noaa.gov/products/noaa-scales.json               (R/S/G scales)
  - https://services.swpc.noaa.gov/json/solar_regions.json                 (sunspots)
  - https://services.swpc.noaa.gov/json/goes/primary/euvs-6-hour.json      (EUV 304 A)
  - https://services.swpc.noaa.gov/json/goes/primary/integral-protons-1-day.json  (p+ flux)
  - https://services.swpc.noaa.gov/json/goes/primary/integral-electrons-1-day.json (e- flux)
  - https://services.swpc.noaa.gov/text/aurora-nowcast-hemi-power.txt      (hemispheric power)

HF band conditions (Good/Fair/Poor) are calculated from SFI + Kp using standard
ionospheric physics (D-layer day/night, F2-layer SFI sensitivity per band).

Aurora activity index is calculated from NOAA hemispheric power (HP) using the
same linear mapping described by N0NBH: HP 0-150 GW -> index 0-10, where each
unit represents ~15 GW of combined auroral energy dissipation.  Index >10 is
reported as-is (e.g. 11, 12) rather than clamped.

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
# Particle flux, EUV, and aurora — all from NOAA SWPC
# ---------------------------------------------------------------------------

def _fmt_flux(val) -> str:
    """
    Format a particle flux for display on a small screen.
    Values < 1000 show as decimals (e.g. "0.18", "123.4");
    larger values use compact scientific notation (e.g. "1.2e4").
    """
    if val is None:
        return "N/A"
    try:
        f = float(val)
        if f == 0:
            return "0"
        if abs(f) < 1000:
            # Show up to 2 sig figs as a decimal
            if abs(f) < 1:
                return f"{f:.2f}"
            if abs(f) < 10:
                return f"{f:.1f}"
            return f"{f:.0f}"
        exp = int(f"{f:.1e}".split("e")[1])
        mant = f / (10 ** exp)
        return f"{mant:.1f}e{exp}"
    except (ValueError, TypeError):
        return str(val)


def _fetch_euv_304a() -> float:
    """
    Return the most recent GOES-R EUVS 304 A irradiance in mW/m^2
    (AU-distance-corrected, eclipse/contamination entries skipped).
    Source: https://services.swpc.noaa.gov/json/goes/primary/euvs-6-hour.json
    """
    data = _get_json("https://services.swpc.noaa.gov/json/goes/primary/euvs-6-hour.json")
    for rec in reversed(data):
        if rec.get("line") != "304":
            continue
        flags = rec.get("flags") or {}
        if flags.get("eclipse") or flags.get("lunar_transit"):
            continue
        val = _num(rec.get("value"))
        au  = _num(rec.get("au_factor"), 1.0)
        if val is None:
            continue
        # Convert W/m^2 -> mW/m^2, apply AU correction, round to 3 decimal places
        return round(float(val) * float(au) * 1000.0, 3)
    return None


def _fetch_proton_flux() -> float:
    """
    Return the most recent GOES integral proton flux for >=10 MeV in pfu.
    Source: https://services.swpc.noaa.gov/json/goes/primary/integral-protons-1-day.json
    """
    data = _get_json(
        "https://services.swpc.noaa.gov/json/goes/primary/integral-protons-1-day.json"
    )
    for rec in reversed(data):
        if rec.get("energy") == ">=10 MeV":
            val = _num(rec.get("flux"))
            if val is not None:
                return val
    return None


def _fetch_electron_flux() -> str:
    """
    Return the most recent GOES integral electron flux for >=2 MeV,
    pre-formatted as a compact string (e.g. "1.9e3") for display.
    Source: https://services.swpc.noaa.gov/json/goes/primary/integral-electrons-1-day.json
    """
    data = _get_json(
        "https://services.swpc.noaa.gov/json/goes/primary/integral-electrons-1-day.json"
    )
    # All entries in this feed are >=2 MeV; grab the last non-null value
    for rec in reversed(data):
        val = _num(rec.get("flux"))
        if val is not None:
            return _fmt_flux(val)
    return None


def _fetch_hemi_power() -> tuple:
    """
    Parse the NOAA aurora hemispheric-power nowcast text product.
    Returns (north_gw, south_gw) floats for the most recent observation,
    or (None, None) on failure.
    Source: https://services.swpc.noaa.gov/text/aurora-nowcast-hemi-power.txt
    Columns: obs_time  forecast_time  north_GW  south_GW
    """
    text = _get_text("https://services.swpc.noaa.gov/text/aurora-nowcast-hemi-power.txt")
    for line in reversed(text.splitlines()):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 4:
            north = _num(parts[2])
            south = _num(parts[3])
            if north is not None and south is not None:
                return float(north), float(south)
    return None, None


def _fetch_flare_probs() -> dict:
    """
    Return 24-hour M-class and X-class flare probabilities (0-100 %) from the
    NOAA SWPC solar probabilities product.
    Source: https://services.swpc.noaa.gov/json/solar_probabilities.json
    """
    data = _get_json("https://services.swpc.noaa.gov/json/solar_probabilities.json")
    latest = data[-1] if data else {}
    return {
        "m_class": _num(latest.get("m_class_1_day")),
        "x_class": _num(latest.get("x_class_1_day")),
    }


def _fetch_dst() -> int:
    """
    Return the most recent Kyoto Dst index in nT.
    Negative values indicate geomagnetic storm activity.
    Source: https://services.swpc.noaa.gov/products/kyoto-dst.json
    Response: array of [timestamp_str, dst_str] rows (no header row).
    """
    data = _get_json("https://services.swpc.noaa.gov/products/kyoto-dst.json")
    for row in reversed(data):
        if len(row) >= 2:
            val = _num(row[1])
            if val is not None:
                return int(val)
    return None


def _aurora_index(north_gw: float, south_gw: float) -> int:
    """
    Compute the aurora activity index (0-10+) from combined hemispheric power.

    Replicates the mapping described by N0NBH (hamqsl.com/solar2.html):
      "Data is now calculated from the current hemispheric power value (0-150 GW)
       to give the old reported scaled factor value from 0 to 10++."

    Linear mapping: total HP / 15 GW per index unit.
    No ceiling - values above 10 are valid (extreme storm conditions).
    """
    total = north_gw + south_gw
    return max(0, round(total / 15.0))


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

    # --- EUV 304 Å (GOES-R EUVS) ---
    euv_304a = None
    try:
        euv_304a = _fetch_euv_304a()
    except Exception as e:
        errors.append(f"euv_304a: {e}")

    # --- Proton flux >=10 MeV (GOES integral protons) ---
    proton_flux = None
    try:
        proton_flux = _fetch_proton_flux()
    except Exception as e:
        errors.append(f"proton_flux: {e}")

    # --- Electron flux >=2 MeV (GOES integral electrons) ---
    electron_flux = None
    try:
        electron_flux = _fetch_electron_flux()
    except Exception as e:
        errors.append(f"electron_flux: {e}")

    # --- Hemispheric power -> aurora activity index ---
    aurora_idx = None
    try:
        north_gw, south_gw = _fetch_hemi_power()
        if north_gw is not None and south_gw is not None:
            aurora_idx = _aurora_index(north_gw, south_gw)
    except Exception as e:
        errors.append(f"aurora: {e}")

    # --- Solar flare probabilities (24h M-class and X-class) ---
    flare_prob_m = None
    flare_prob_x = None
    try:
        probs = _fetch_flare_probs()
        flare_prob_m = probs.get("m_class")
        flare_prob_x = probs.get("x_class")
    except Exception as e:
        errors.append(f"flare_probs: {e}")

    # --- Kyoto Dst index ---
    dst = None
    try:
        dst = _fetch_dst()
    except Exception as e:
        errors.append(f"dst: {e}")

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
        "euv_304a":      euv_304a,        # GOES-R EUVS 304 A, AU-corrected mW/m^2
        "proton_flux":   proton_flux,    # GOES integral p+ >=10 MeV, pfu
        "electron_flux": electron_flux,  # GOES integral e- >=2 MeV, formatted string
        "aurora_idx":    aurora_idx,     # HP-based aurora index (0-150 GW -> 0-10+)
        "flare_prob_m":  flare_prob_m,   # 24h M-class flare probability, %
        "flare_prob_x":  flare_prob_x,   # 24h X-class flare probability, %
        "dst":           dst,            # Kyoto Dst index, nT (negative = storm)
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
            log.info(
                "refreshed: sfi=%s kp=%s xray=%s sw=%s bz=%s bt=%s dst=%s "
                "304a=%s p+=%s e-=%s aurora=%s fM=%s%% fX=%s%%",
                data.get("sfi"), data.get("k_index"),
                data.get("xray"), data.get("solar_wind"),
                data.get("bz"), data.get("bt"), data.get("dst"),
                data.get("euv_304a"), data.get("proton_flux"),
                data.get("electron_flux"), data.get("aurora_idx"),
                data.get("flare_prob_m"), data.get("flare_prob_x"),
            )
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
