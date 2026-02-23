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

## 12) Latest Session Status (2026-02-23 night)

### What changed this session

- Added clearer transport-stage logging for HTTP failures in `DslWidgetFetch`:
  - `request-not-attempted (tls-preflight)`
  - `request-not-attempted (http-begin)`
  - `request-not-attempted (transport-gate-timeout)`
  - `request-skipped (transport-cooldown)`
  - `transport-failure (no-http-status)`
- Added conditional template logic in `bindRuntimeTemplate`:
  - `if_eq`, `if_ne`, `if_true`, `if_gt`, `if_gte`, `if_lt`, `if_lte`
  - use case now active in `data/dsl/homeassistant_control_card.json`:
    - icon color yellow when `state == "on"`.
- Added shared transport gate across JSON fetch and remote icon fetch:
  - `src/services/HttpTransportGate.*`
  - `src/services/HttpJsonClient.cpp`
  - `src/widgets/DslWidgetRender.cpp`
- Added cache purge on layout reload to reduce post-switch memory pressure:
  - `clearDslRuntimeCaches()` in `src/widgets/DslWidgetRender.cpp`
  - called from `src/core/DisplayManager.cpp` after `widgets_.clear()`.
- Started migration of JSON HTTP path to IDF native client:
  - `src/services/HttpJsonClient.cpp` now uses `esp_http_client` (not Arduino `HTTPClient`) for `HttpJsonClient::get`.
  - keeps existing `HttpJsonClient` interface and metadata outputs.

### Current known instability / open issues

- Device still shows intermittent ESP-side connect/TLS failures (`code=-1`) under layout switches + mixed source polling.
- During initial `esp_http_client` migration, startup failed with `UNKNOWN ERROR`; patched by:
  - using CA bundle attach: `arduino_esp_crt_bundle_attach`
  - restored CN checks (`skip_cert_common_name_check = false`)
  - raised outage threshold from 3 to 6 to avoid startup black-hole.
- Need fresh on-device validation after this patch set (not yet confirmed stable by user).

### Important behavior changes to remember

- HTTP widget cooldown tuning now favors faster retries for transport statuses:
  - `-2` ~ 5s
  - `-3` ~ 4s
  - other transport errors exponential up to 30s (not 120s blackout).
- Transport outage cooldown still exists globally in `HttpJsonClient` (12s window once threshold reached).

### Next actions (first thing next session)

1. Flash latest firmware and validate boot + first fetch cycle:
   - Geo startup calls
   - HA cards
   - weather/forecast after layout switch.
2. Capture first failure logs with new `esp_http_client` error reason strings.
3. Migrate remote icon fetch path (`DslWidgetRender` `fetchRemoteIconToFile`) to `esp_http_client` too, so JSON + icons share same backend.
4. Implement a single network worker queue for all outbound requests (JSON + icon/raster fetch) with deterministic scheduling.
5. After transport is stable, continue runtime array-driven rendering work (`repeat_over` style DSL feature).
