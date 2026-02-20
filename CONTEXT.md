# Session Context (Updated: February 20, 2026)

This file is a quick restart point for the next coding session.

## Current Goal

Primary focus is the DSL-driven ADS-B nearest-aircraft widget on ESP32 + ILI9341 CYD (`ESP32-2432S028`), with stable fetch/parse/render behavior and faster iteration via browser tooling.

## Current Runtime State

- Active screen layout is ADS-B only:
  - `data/screen_layout.json`
  - Region `adsb-main`, full screen `320x240`, DSL path `/dsl/adsb_nearest.json`
- ADS-B DSL file:
  - `data/dsl/adsb_nearest.json`
  - `poll_ms = 30000`
  - `debug = true`
- Browser DSL editor exists:
  - `tools/dsl_editor/index.html`

## What Is Working

- ADS-B fetch/parse succeeds intermittently and renders when transport is healthy:
  - `HTTP Fetch 200`, non-zero content-length
  - `DSL parse summary resolved=26 missing=0`
- DSL runtime split into maintainable files:
  - `src/widgets/DslWidget.cpp`
  - `src/widgets/DslWidgetFetch.cpp`
  - `src/widgets/DslWidgetFormat.cpp`
  - `src/widgets/DslWidgetExpr.cpp`
  - `src/widgets/DslWidgetRender.cpp`
- Network/UI concurrency guard in `DisplayManager` to protect shared `widgets_`.
- HTTP diagnostics improved (status, content-length, content-type, transport reason, heap metrics).

## Current Problem

Intermittent ADS-B transport failures still occur (`status = -1`) after periods of success.

Recent mitigation already added:

- Retry `http://` transport for ADS-B primary failure.
- Fallback attempts on `airplanes.live` via HTTPS and HTTP.
- Adaptive cooldown/backoff for ADS-B failure streaks to reduce hammering.
- Reduced ADS-B poll from 15s to 30s.

Code locations:

- Transport/fetch/backoff logic:
  - `src/widgets/DslWidgetFetch.cpp`
- HTTP timeouts + low-level error reason:
  - `src/services/HttpJsonClient.cpp`

## Suspected Cause (Current Best Guess)

Not a clean server throttle yet (no consistent HTTP 429/503 seen). Current evidence points to intermittent connection-level failures (WiFi/AP path, DNS/TLS/socket instability, or endpoint intermittency).

## Quick Start (Next Session)

1. Build firmware:
   - `pio run`
2. Upload filesystem (if JSON changed):
   - `pio run -t uploadfs`
3. Upload firmware:
   - `pio run -t upload`
4. Monitor serial:
   - `pio device monitor`

## Fast UI Iteration (No Reflash)

Use browser editor:

1. `python3 -m http.server 8000`
2. Open `http://localhost:8000/tools/dsl_editor/`
3. Edit DSL + payload and preview in real time.

## Suggested Next Tasks (Priority Order)

1. Improve ADS-B row density/legibility in `data/dsl/adsb_nearest.json` (tighten `y` spacing).
2. Add explicit log when fallback source succeeds (primary vs alt transport vs fallback host).
3. Add optional jitter to ADS-B poll schedule (spread requests, avoid cadence bursts).
4. Add setup option to switch radius (`radius_nm`) from UI.
5. Turn ADS-B debug logs down once stable (`debug: false`) and keep only error logs.

## Important Notes

- User’s IDE may still show `data/widgets.json` tab, but that file no longer exists.
- Default geo fallback location is Google HQ (already configured):
  - `lat=37.4220`, `lon=-122.0841` in `include/AppConfig.h`.
- All display interaction is landscape mode.
