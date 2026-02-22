# Handoff Runbook (Release Prep)

## 1) Environment

- Project root: `costar/`
- Board/framework: ESP32 + Arduino (PlatformIO)
- Display/touch: `TFT_eSPI` + `XPT2046_Touchscreen`
- Filesystem: LittleFS

## 2) Current Runtime Architecture

- Active layout profiles:
  - `data/screen_layout_a.json`
  - `data/screen_layout_b.json`
- Profile select:
  - USER button (`GPIO 0`, active-low)
  - persisted in NVS key `layout.profile`
- Legacy compatibility layout file remains:
  - `data/screen_layout.json` (not the active profile source)

## 3) Widget/DSL Runtime

- Widget type: DSL-only (`type: "dsl"`)
- Core files:
  - `src/widgets/DslWidget.cpp`
  - `src/widgets/DslWidgetFetch.cpp`
  - `src/widgets/DslWidgetFormat.cpp`
  - `src/widgets/DslWidgetExpr.cpp`
  - `src/widgets/DslWidgetRender.cpp`
  - `src/dsl/DslParser.cpp`
  - `src/dsl/DslExpr.cpp`
  - `src/services/HttpJsonClient.cpp`

Key supported DSL features:

- `repeat` parse-time expansion
- numeric expressions (`sin/cos/...`, `haversine_m`, unit conversions)
- sortable path transforms:
  - `sort_num(...)`
  - `sort_alpha(...)`
  - `distance_sort(...)` / `sort_distance(...)`
- label path rendering:
  - `label.path`
  - optional `{{value}}` injection in `text`
- label wrapping:
  - `wrap`, `line_height`, `max_lines`, `overflow`

## 4) Global Runtime Bindings

- Geo:
  - `{{geo.lat}}`, `{{geo.lon}}`, `{{geo.tz}}`, `{{geo.offset_min}}`, `{{geo.label}}`
- Preferences:
  - `{{pref.clock_24h}}`, `{{pref.temp_unit}}`, `{{pref.distance_unit}}`
- Widget-local settings:
  - `{{setting.<key>}}`

## 5) Home Assistant Direct Support

- DSL now supports HTTP headers under `data.headers`.
- Typical HA call:
  - URL: `{{setting.ha_base_url}}/api/states/{{setting.entity_id}}`
  - Header: `Authorization: Bearer {{setting.ha_token}}`
- Reference DSL:
  - `data/dsl/homeassistant_entity.json`

## 6) Remote Icon Pipeline (No Reflash Asset Work)

- Go utility:
  - `tools/image_proxy/`
  - endpoints:
    - `/cmh` (arbitrary image URL -> RGB565 raw)
    - `/mdi` (MDI icon name -> RGB565 raw)
  - shared dev relay endpoint:
    - `http://vps.gorkos.net:8085`
  - in-memory cache flags:
    - `-cache-ttl`
    - `-cache-max-entries`

- Firmware icon render behavior:
  - local icon path: read from LittleFS directly
  - remote icon path (`http/https`):
    1. check in-memory icon cache
    2. check LittleFS cache (`/icon_cache/*.raw`)
    3. fetch/download/store on miss
  - retry/backoff state for failed remote fetches is bounded and pruned

## 7) Geo Persistence

- Manual setup location persists per SSID:
  - LittleFS file: `/geo_manual_by_ssid.json`
- Load priority:
  1. SSID-scoped manual
  2. legacy global NVS manual override
  3. cached/online Geo-IP

## 8) Hardware Flags

- Board blue LED forced off at boot:
  - `AppConfig::kBoardBlueLedPin = 17`

## 9) Cleanup Done This Pass

- Removed obsolete DSL file:
  - `data/dsl/clock_analog_quad.json`
  - canonical quarter clock DSL is `data/dsl/clock_analog_quarter.json`
- Added Go-tooling artifact ignore:
  - `tools/image_proxy/.gitignore` (`image_proxy`)
- Updated docs to reflect active A/B layout profile model and remote icon caching.

## 10) Build/Run Commands

- Build firmware: `pio run`
- Upload assets: `pio run -t uploadfs`
- Upload firmware: `pio run -t upload`
- Monitor: `pio device monitor`

Go utility:

- `cd tools/image_proxy`
- `go build -o image_proxy .`
- `./image_proxy -listen :8085 -cache-ttl 10m -cache-max-entries 256`
- `go run . -listen :8085 -cache-ttl 10m -cache-max-entries 256`

## 11) Immediate Release Checklist

1. Verify A/B toggle and persistence across reboot.
2. Verify HA direct fetch with auth header from one entity endpoint.
3. Verify remote MDI icon first-fetch then LittleFS cache hit behavior.
4. Confirm no regressions in weather/forecast/clock/ADSB widgets.
