# Handoff Runbook

## 1) Environment

- Project root: `costar/`
- Board/framework: ESP32 + Arduino (PlatformIO)
- Display stack: `TFT_eSPI` + `XPT2046_Touchscreen`
- Filesystem: SPIFFS

## 2) Core Files to Know

- Main boot + provisioning + setup UI:
  - `src/main.cpp`
- Layout manager:
  - `src/core/DisplayManager.cpp`
- DSL parser:
  - `src/dsl/DslParser.cpp`
- DSL widget runtime:
  - `src/widgets/DslWidget.cpp`
  - `src/widgets/DslWidgetFetch.cpp`
  - `src/widgets/DslWidgetFormat.cpp`
  - `src/widgets/DslWidgetExpr.cpp`
  - `src/widgets/DslWidgetRender.cpp`
- HTTP client:
  - `src/services/HttpJsonClient.cpp`

## 3) Current Active Widget Config

- Screen layout:
  - `data/screen_layout.json`
- ADS-B widget DSL:
  - `data/dsl/adsb_nearest.json`

## 4) Common Commands

- Build:
  - `pio run`
- Upload FS data:
  - `pio run -t uploadfs`
- Upload firmware:
  - `pio run -t upload`
- Serial monitor:
  - `pio device monitor`

## 5) Browser DSL Editor (Fast Iteration)

- Launch:
  - `python3 -m http.server 8000`
- Open:
  - `http://localhost:8000/tools/dsl_editor/`

Use it to validate DSL layout, path bindings, and formatting before flashing.

Editor notes:

- The editor uses JSONEditor (tree + code) loaded from CDN.
- Tab/indent behaves like a real code editor inside the JSON panes.

## 5.1) DSL Advanced Features (Repeat + Math)

- `repeat` nodes expand at parse time to generate multiple nodes.
- Fields: `count` (or `times`), `start`, `step`, `var` (default `i`), and `nodes` (array) or `node` (single object).
- The repeat variable is available in numeric expressions and in text/path via `{{i}}`.
- Numeric expression fields: `x`, `y`, `x2`, `y2`, `r`, `length`, `thickness`, `min`, `max`, `start_deg`, `end_deg`.
- Supported functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan` (degrees), `abs`, `sqrt`, `floor`, `ceil`, `round`, `min`, `max`, `pow`, `rad`, `deg`, `pi`.
- Safety: repeat expansion is capped at 512 iterations per node.

Label alignment:

- `align`: `left`, `center`, `right`
- `valign`: `top`, `middle`, `bottom`, `baseline`

Format units:

- `unit: "pressure"` pins to temperature units (`F` → `inHg`, `C` → `hPa`).

## 6) Logging Guide

Good cycle:

- `URL ...`
- `HTTP Fetch 200 content-length=...`
- `DSL parse summary resolved=... missing=0 ...`

Failure cycle:

- `HTTP Fetch -1 ...`
- `ADSB err=... reason='...'`
- `ADSB cooldown ...`

## 7) Known Risk Areas

- Intermittent ADS-B transport failures (status `-1`)
- Potential endpoint/network instability despite successful parses in prior cycles
- Row spacing in ADS-B UI currently too loose (DSL `y` coordinates)

## 8) First 15-Minute Plan for Next Session

1. Flash latest (`uploadfs` + `upload`), monitor logs for 10+ minutes.
2. Confirm cooldown behavior during failures and recovery logs on success.
3. Tighten ADS-B row spacing in `data/dsl/adsb_nearest.json`.
4. If failures persist, add endpoint failover ordering with explicit “selected source” telemetry.
