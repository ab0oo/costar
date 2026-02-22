# Session Context (Updated: February 22, 2026)

Quick restart snapshot for next session.

## Runtime State

- Layout profiles are A/B:
  - `data/screen_layout_a.json`
  - `data/screen_layout_b.json`
- USER button toggles profile at runtime; profile is persisted to NVS key `layout.profile`.
- Default profile path is A (`AppConfig::kDefaultLayoutPath`).

## Active Layout Contents

- Layout A:
  - weather now (top-left)
  - forecast (bottom-left)
  - full-height analog clock (right half)
- Layout B:
  - weather now (top-left)
  - forecast (bottom-left)
  - quarter analog clock (top-right)
  - ADS-B nearest (bottom-right)

## DSL/Runtime Capabilities

- Sort transforms: `sort_num`, `sort_alpha`, `distance_sort` / `sort_distance`
- Repeat expansion: `repeat` with `count/start/step/var`
- Runtime label path mode:
  - `label.path`
  - optional `{{value}}` template replacement in label `text`
- Label wrap controls:
  - `wrap`, `line_height`, `max_lines`, `overflow`
- Expression funcs include:
  - `haversine_m`, `meters_to_miles`, `miles_to_meters`

## Home Assistant Support

- Direct HA REST calls are supported from firmware DSL:
  - use `data.headers` for auth headers
  - template from widget settings (`{{setting.ha_*}}`)
- Example DSL:
  - `data/dsl/homeassistant_entity.json`

## Icon Ingestion Flow

- Go helper (`tools/image_proxy`) provides:
  - `/cmh` image URL -> RGB565 raw
  - `/mdi` icon name -> RGB565 raw
  - in-memory response cache (`-cache-ttl`, `-cache-max-entries`)
  - build/run:
    - `go build -o image_proxy .`
    - `./image_proxy -listen :8085 -cache-ttl 10m -cache-max-entries 256`
- Firmware icon render path now supports remote URLs:
  - check in-memory icon cache
  - check LittleFS `/icon_cache/*.raw`
  - fetch/store on miss
  - retry/backoff tracking is pruned over time

## Geo/Prefs

- Manual geo is SSID-scoped in LittleFS:
  - `/geo_manual_by_ssid.json`
- Runtime global bindings available in DSL:
  - geo: `{{geo.lat}}`, `{{geo.lon}}`, `{{geo.tz}}`, `{{geo.offset_min}}`, `{{geo.label}}`
  - prefs: `{{pref.clock_24h}}`, `{{pref.temp_unit}}`, `{{pref.distance_unit}}`

## Hardware Notes

- Board blue LED disabled at boot via GPIO 17.
- USER button is GPIO 0 active-low with debounce.

## Cleanup Notes

- Obsolete file removed: `data/dsl/clock_analog_quad.json`.
- Added `tools/image_proxy/.gitignore` so local Go build binary is not tracked.

## Next Boot/Test

1. `pio run`
2. `pio run -t uploadfs`
3. `pio run -t upload`
4. `pio device monitor`
5. Verify profile toggle, HA fetch, and remote icon cache behavior.
