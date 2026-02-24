# Handoff Runbook (Release Prep)

## 0) Session Log - ESP-IDF Port (2026-02-23)

Summary: Major progress on ESP-IDF scaffold, Wi-Fi/touch/config flow, and logging cleanup. Several display/touch parity regressions were debugged. Session ended with one final display fix built but not yet hardware-verified.

### What Worked (Successes)

- ESP-IDF app scaffold boots and runs consistently (`idf/main/app_main.cpp`).
- Logging cleanup moved IDF path away from Arduino-style `Serial.*` toward `ESP_LOG*`.
- LittleFS mount + required asset verification added and functioning when storage image is present.
- IDF config UI and touch interaction loop added:
  - Wi-Fi/locale screen draw
  - touch hit-test, tap markers, action logging
- IDF touch bring-up on SPI works; raw + mapped touch logs are emitted.
- Wi-Fi crash loop fixed:
  - Cause was `esp_wifi_set_config()` during connecting state (`ESP_ERR_WIFI_STATE`).
  - Fixed sequencing: disconnect -> apply config -> connect, with non-fatal handling.
- Firmware identity verification process clarified:
  - runtime `app_init` ELF SHA must match built artifact SHA.
- Build blocker workaround added for offline/pip-restricted environments:
  - `COSTAR_BUILD_LITTLEFS_IMAGE=OFF` option in `idf/main/CMakeLists.txt`.
  - Verified build succeeds with:
    - `idf.py -C idf -D COSTAR_BUILD_LITTLEFS_IMAGE=OFF build`

### What Failed / Regressed

- Display/touch parity did not match Arduino immediately.
- Repeated orientation/mapping regressions:
  - portrait-like config layout despite intended landscape
  - large touch marker offset from tap point
  - temporary left-right mirrored display after an intermediate coordinate transform patch
- Root causes were mixed between:
  - panel raw dimension assumptions
  - MADCTL/rotation handling order
  - touch axis mapping mismatch vs Arduino `TouchMapper`
  - extra software coordinate remap layered on top of hardware rotation

### Current Code State At End Of Day

- `include/AppConfig.h`
  - `kRotation = 1`
  - raw panel dimensions set to `kPanelWidth=240`, `kPanelHeight=320`
  - IDF touch calibration constants aligned to Arduino values.
- `idf/main/TouchInputEspIdf.cpp`
  - touch mapping updated to match Arduino `TouchMapper` axis logic.
- `idf/main/app_main.cpp`
  - Wi-Fi init/connect sequencing hardened; no `ESP_ERROR_CHECK` abort on `ESP_ERR_WIFI_STATE`.
- `idf/main/DisplaySpiEspIdf.cpp`
  - panel init uses explicit MADCTL by rotation.
  - final patch removed extra software logical->panel remap that caused mirror.
  - latest build completed successfully after mirror fix.

### Latest Known Hardware Observation

- User confirmed a new firmware flash occurred.
- Orientation became correct at one point, but display was mirrored L<->R.
- After the final change that removed the extra remap, hardware regressed back to portrait mode with badly misaligned touch points.
- Net state at handoff: orientation/touch parity is still unresolved on IDF and remains the top blocker.

### Build / Flash Notes (Important)

- If pip/PyPI access fails during littlefs image generation:
  - error includes connection refused to `/simple/littlefs-python/`
  - use:
    - `source /home/johgor/esp-idf/export.sh`
    - `idf.py -C idf -D COSTAR_BUILD_LITTLEFS_IMAGE=OFF build`
- With `COSTAR_BUILD_LITTLEFS_IMAGE=OFF`, `storage.bin` is not regenerated/flashed by default.

### First Steps To Resume

1. Flash latest app build and capture full `app_init` + early `tft` + first touch lines.
2. Confirm runtime app SHA matches latest build.
3. Verify whether mirror is gone after last `DisplaySpiEspIdf.cpp` patch.
4. Validate touch alignment on config buttons (`SCAN`, `RETRY`, `OFFLINE`).
5. If still off, port Arduino setup UI layout/hitbox definitions directly (shared module) instead of parallel implementations.

### Resume Command Set

- Build (offline-safe):
  - `source /home/johgor/esp-idf/export.sh`
  - `idf.py -C idf -D COSTAR_BUILD_LITTLEFS_IMAGE=OFF build`
- Flash:
  - `idf.py -C idf -p <PORT> flash`
- Verify running image:
  - compare runtime `app_init: ELF file SHA256: ...` to local image metadata.

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
