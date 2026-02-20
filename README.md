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
- Supports DSL node types: `label`, `value_box`, `progress`, `sparkline`, `circle`, `hand`
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
- `ui.nodes`: `label`, `value_box`, `progress`, `sparkline`

Template bindings:

- value bindings: `{{field_name}}`
- runtime bindings in URL/path/text:
  - `{{setting.<key>}}`
  - `{{geo.lat}}`, `{{geo.lon}}`, `{{geo.tz}}`, `{{geo.offset_min}}`

## Runtime notes

- Rendering and network fetches are split across ESP32 cores.
- Screen updates stay responsive while HTTP fetches run.
- Widget health indicator is a 5px dot:
  - green = `ok`
  - red = non-OK
