# ESP32 Widget Dashboard (DSL-Only)

ESP32 + ILI9341 + XPT2046 dashboard using a JSON screen layout and JSON widget DSL files.

Session restart docs:

- `CONTEXT.md`
- `docs/HANDOFF.md`

## Current architecture

- Layout source: `data/screen_layout.json`
- Widget type: `dsl` only
- Widget runtime: `src/widgets/DslWidget.*`
- Layout loader: `src/core/DisplayManager.*`
- HTTP JSON client: `src/services/HttpJsonClient.*`
- Geo + timezone bootstrap: `src/services/GeoIpService.*`, `src/core/RuntimeGeo.*`

Legacy flat manifests and legacy hardcoded widget classes were removed.

## Upload

1. `pio run -t uploadfs`
2. `pio run -t upload`
3. `pio device monitor`

## Browser DSL Editor

Fast local iteration without reflashing:

1. From project root: `python3 -m http.server 8000`
2. Open: `http://localhost:8000/tools/dsl_editor/`
3. Edit widget DSL JSON and payload JSON; preview updates live.

Features:

- 320x240 landscape screen preview
- JSONEditor (tree + code) via CDN for proper JSON editing and tab/indent
- Supports DSL node types: `label`, `value_box`, `progress`, `sparkline`, `icon`, `moon_phase`, `arc`, `line`, `repeat`
- Field-path resolution + basic formatting (`round`, `prefix`, `suffix`, `unit`, `locale`, `tz`)
- Load sample widgets from `data/dsl/`

## Screen layout format

`data/screen_layout.json` defines:

- `screen`: metadata + `regions[]` with exact `x/y/w/h`
- `widget_defs`: reusable widget definitions

Each region points at a widget def by key:

- region: `{ "id": "...", "widget": "weather", "x": ..., "y": ..., "w": ..., "h": ... }`
- widget def: `{ "type": "dsl", "update_ms": ..., "settings": { "dsl_path": "...", ... } }`

## DSL format (v1)

Each DSL file in `data/dsl/` supports:

- `version: 1`
- `data.source`: `http` or `local_time`
- `data.url` (for `http`)
- `data.poll_ms`
- `data.debug` (or widget setting `debug=true` override)
- `data.fields`: JSON path selectors with optional formatting
- `ui.nodes`: `label`, `value_box`, `progress`, `sparkline`, `icon`, `moon_phase`, `arc`, `line`, `repeat`

Template bindings:

- value bindings: `{{field_name}}`
- runtime bindings in URL/path/text:
  - `{{setting.<key>}}`
  - `{{geo.lat}}`, `{{geo.lon}}`, `{{geo.tz}}`, `{{geo.offset_min}}`
  - `{{geo.label}}`

Label alignment:

- `align`: `left`, `center`, `right`
- `valign`: `top`, `middle`, `bottom`, `baseline`

Repeat nodes (advanced):

- Use `"type": "repeat"` to expand a node (or list of nodes) at parse time.
- Fields: `count` (or `times`), `start`, `step`, `var` (default `i`), and `nodes` (array) or `node` (single object).
- The repeat variable is available in numeric expressions (e.g. `"x": "80 + cos(i*6-90)*60"`) and in text/path via `{{i}}`.
- Safety: repeat expansion is capped at 512 iterations per node.

Numeric expressions (advanced):

- String-valued numeric fields (`x`, `y`, `x2`, `y2`, `r`, `length`, `thickness`, `min`, `max`, `start_deg`, `end_deg`) may contain arithmetic.
- Supported functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan` (degrees), `abs`, `sqrt`, `floor`, `ceil`, `round`, `min`, `max`, `pow`, `rad`, `deg`, and constant `pi`.

Format units:

- `unit: "pressure"` pins to temperature units (`F` → `inHg`, `C` → `hPa`).

## Runtime notes

- Rendering and network fetches are split across ESP32 cores.
- Screen updates stay responsive while HTTP fetches run.
- Widget health indicator is a 5px dot:
  - green = `ok`
  - red = non-OK

## Third-party assets

- Meteocons weather icons by Bas Milius (MIT License).
  - Source: https://github.com/basmilius/weather-icons
  - License text: `third_party/meteocons/LICENSE`
