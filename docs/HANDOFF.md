# Handoff Runbook (Release Prep)

## 0) Session Log - PlatformIO IDF Default Cutover (2026-02-25, Night)

Summary: We cut PlatformIO over to ESP-IDF as the default environment, preserved Arduino as a legacy env, and fixed the build routing so `pio run` now compiles the IDF app path instead of Arduino `src/` sources.

### What Was Completed

- PlatformIO default env switched to IDF:
  - `platformio.ini`
  - `[platformio] default_envs = esp32dev_idf`
  - Added `[env:esp32dev_idf]`
  - Renamed Arduino env to `[env:esp32dev_arduino_legacy]`

- PlatformIO ESP-IDF source routing fixed:
  - Added explicit ESP-IDF component definition under:
    - `src/CMakeLists.txt`
  - This component now builds from:
    - `idf/main/*.cpp`
    - shared runtime files in `src/core/*` used by IDF

- PlatformIO IDF component dependency manifest added:
  - `src/idf_component.yml`
  - Includes:
    - `joltwallet/littlefs`
    - `lvgl/lvgl`
    - `espressif/esp_websocket_client` pinned to `==1.4.0` for PIO/IDF 5.5 compatibility

- IDF env partition size alignment fixed for PlatformIO:
  - `platformio.ini` now sets:
    - `board_build.partitions = idf/partitions.csv`
  - This resolves false app-size overflow checks from default 1MB app partition assumptions.

- Build warning cleanup:
  - Fixed `-Wformat-truncation` in:
    - `idf/main/ConfigScreenEspIdf.cpp`

- Repository hygiene:
  - Added root-level PIO ESP-IDF generated-artifact ignores in:
    - `.gitignore`

### Build Verification (PlatformIO)

- Verified with local venv binary:
  - `/home/johgor/.venv/bin/pio project config`
  - `/home/johgor/.venv/bin/pio run`
- Result:
  - `pio run` succeeds on default `esp32dev_idf`.
  - Output includes successful `firmware.bin` generation.

### Current Caveats

- Configure-time warning still appears:
  - `Flash memory size mismatch detected. Expected 4MB, found 2MB`
- This did not block build success, but board/flash profile should be double-checked on target hardware before release tagging.

## 0) Session Log - HA WS Narrowing, Soak Prep, and Parity Audit (2026-02-25, Evening)

Summary: This session converted HA WS from a global event firehose to per-entity trigger subscriptions, added WS debugging/probe tooling, improved first-paint latency for HA cards, and performed a direct Arduino-vs-IDF DSL primitive comparison.

### What Was Completed

- Home Assistant WS transport/runtime:
  - Replaced global `subscribe_events(state_changed)` with per-entity `subscribe_trigger` subscriptions in:
    - `idf/main/DslWidgetRuntimeEspIdf.cpp`
  - Kept bootstrap model (`render_template`) for initial entity state, then updates via trigger events.
  - Added WS diagnostics:
    - large-frame payload logging with heap context
    - disconnect logging with heap context
  - Added bootstrap trigger on WS auth ready so HA widgets bootstrap immediately.

- HA card first-paint latency fix:
  - Fixed scheduler behavior so deferred fetches do not consume the full initial cadence window.
  - `lastFetchMs` now advances on successful updates only.
  - Deferred fetches now use short retry backoff (`~250ms`) to accelerate first visible render after bootstrap.

- Home Assistant icon behavior:
  - Restored remote icon fetching path for HA widgets after temporary cache-only gating.
  - Icon retry/backoff behavior remains in place for failed icon fetches.

- TLS memory tuning:
  - Reduced mbedTLS content buffers to lower recurring dynamic allocation pressure:
    - `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=2048`
    - `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048`
  - Files:
    - `idf/sdkconfig`
    - `idf/sdkconfig.defaults`

- Tooling:
  - Added HA WS probe script:
    - `tools/ha_ws_probe.py`
  - Script now pretty-prints full TX/RX JSON frames and byte sizes.
  - Default flow mirrors current ESP path for a single entity (`light.john_s_lamp`):
    - auth
    - `subscribe_trigger`
    - bootstrap `render_template`
    - stream trigger events

### Current Soak Status / Known Risk

- Good:
  - HA WS connects/auths/bootstraps correctly.
  - Per-entity trigger subscriptions are active.
  - HA card population latency improved substantially vs prior behavior.
- Still open:
  - Long-soak stability issue remains: intermittent TLS read alloc failure leading to WS disconnect/reconnect under memory pressure.
  - Disconnect is not intentional business logic; it follows transport failure.

### Arduino vs IDF DSL Primitive Parity Check (Code Comparison)

Reference compared:
- Arduino:
  - `src/dsl/DslParser.cpp`
  - `src/dsl/DslModel.h`
  - `src/widgets/DslWidget.cpp`
  - `src/widgets/DslWidgetRender.cpp`
- ESP-IDF:
  - `idf/main/DslWidgetRuntimeEspIdf.cpp`

Parity status:
- Node primitives:
  - Present on both: `label`, `value_box`, `progress`, `sparkline`, `arc/circle`, `line/hand`, `icon`, `moon_phase`.
- Repeat node expansion:
  - Present on both (`type: "repeat"` with `count/times/start/step/var`).
- Sort path helpers:
  - Present on both: `sort_num(...)`, `sort_alpha(...)`, `distance_sort(...)`, `sort_distance(...)`.
- Runtime formatting:
  - Numeric/unit/locale/time formatting support is present on both.
- HTTP headers in DSL:
  - Present on both.

Not fully 1:1 yet (Arduino primitive/behavior not fully mirrored in IDF):
- Legacy data source literal:
  - Arduino supports `data.source == "adsb_nearest"` as a specialized source path.
  - IDF currently supports `http`, `ha_ws`, `local_time`; ADS-B is expected via generic HTTP+transform DSL.
- Text datum semantics:
  - Arduino render model uses TFT datum semantics (including baseline variants).
  - IDF uses `align`/`valign` abstraction and does not currently expose full datum baseline semantics as a first-class equivalent.

Net assessment:
- For current active DSL files in this repo, runtime parity is close and operational.
- Remaining parity deltas are mostly legacy-compatibility semantics, plus soak stability under constrained heap.

### Recommended Next Steps

1. Complete long soak with per-entity trigger subscriptions enabled and collect:
   - `ha_ws large frame ...`
   - `ha_ws disconnected ... heap_largest=... heap_free=...`
2. If disconnect persists, cap/trim inbound HA payload handling further (entity-scoped event extraction only, tighter frame/drop policy).
3. Decide whether to implement explicit compatibility for:
   - `source: adsb_nearest`
   - baseline datum semantics
   or formally deprecate them in favor of current DSL patterns.

## 0) Session Log - Final Transition Push (2026-02-25, Late)

Summary: This session moved the IDF path from “core runtime parity” into “final-stage release parity”, including DSL interaction features (tap HTTP + modal), NYT fullscreen layout, HA speaker touch-region actions, icon caching, and runtime menu-based layout switching/config access. The remaining work is now heavily hardware-validation and soak-test focused.

### What Was Completed This Session

- Runtime/task behavior:
  - Runtime loop pinned to core 1; HTTP worker pinned to core 0.
  - Added touch dispatch logging and region hit/miss logging.
  - Added post-tap delayed refresh (`~750ms`) for HTTP tap actions.

- Transport/logging:
  - Added/expanded HTTP request logging for provider/proxy calls and headers (with sensitive redaction).
  - Reduced noisy `http start/http done/http hdr` logs from INFO to DEBUG to improve touch/menu debugging visibility.

- Display/touch setup:
  - Added display mode tuning support and calibration persistence.
  - Updated behavior to skip interactive color-mode calibration when saved display tuning already exists.
  - Added runtime entry to force touch recalibration from menu.

- Layout/runtime:
  - Added runtime menu button + overlay in top-right with actions:
    - switch layouts
    - open Wi-Fi/units config
    - launch touch calibration
  - Added persisted preferred layout (`ui/layout`) with boot fallback to layout A.
  - Added fullscreen NYT layout:
    - `data/screen_layout_nyt.json`
  - Updated runtime menu to include NYT option.

- DSL engine parity additions:
  - Added `ui.modals` parsing/rendering.
  - Added `ui.touch_regions` support:
    - `on_touch.action = "modal"` (+ optional `dismiss_ms`)
    - `on_touch.action = "http"` (url/method/content_type/headers/body)
  - Added modal open/close and auto-dismiss handling.
  - Added HTTP touch-region action execution path with delayed refresh.

- Content/widgets:
  - Restored HA icon state coloring in DSL (`on` -> yellow, otherwise blue).
  - Added icon memory + LittleFS cache path for remote icon fetches (`/littlefs/icon_cache`).
  - Added and validated inclusion of:
    - `data/dsl_available/nyt_headlines.json`
    - `data/dsl_available/ha_speaker_card.json`

### Latest Built Image (for verification)

- App version: `498608a-dirty`
- Compile time: `Feb 24 2026 23:00:57`
- ELF SHA256: `3ef3f1b537c38a7b5d0d7c895baa97fa90a1e1dcfecbf04e1a87e559448d6f1b`
- ESP-IDF: `v6.1-dev-2636-g97d9585357`

### Known Open Issue (Top Priority)

- Runtime menu still had field reports of repeated redraw + unresponsive row selection.
- Last patch changed menu rendering to dirty/event-driven and added explicit `ui: menu action=...` logs.
- This patch is built but needs hardware confirmation.

### Where We Stand vs Parity

- DSL/runtime/interaction feature parity: largely in place.
- Remaining gap is now mostly verification quality:
  - on-device visual/touch behavior across all key layouts/widgets
  - HA authenticated control reliability
  - stability/soak confidence

### Tomorrow’s Recommended Verification Order

1. Flash latest image + LittleFS and confirm image identity via boot logs.
2. Validate runtime menu:
   - open/close behavior
   - row hit selection for Layout A/B/NYT + Config + Touch Cal
   - verify `ui: menu action=...` logs on each tap
3. Validate NYT fullscreen:
   - feed fetch
   - headline taps open modal
   - modal close and auto-dismiss behavior
4. Validate HA speaker card:
   - modal tap region
   - VOL-/VOL+ touch-region HTTP actions
5. Validate HA 6-card screen:
   - icon cache hit behavior (reduced repeated fetches)
   - tap actions + post-tap refresh responsiveness
6. Run 2-4 hour soak:
   - no crashes, no stuck touch/menu state, no progressive heap collapse

### Final Transition Exit Criteria (Practical)

- Menu navigation + layout switching consistently usable.
- NYT + HA speaker + HA 6-card + weather/forecast/clock all function on hardware.
- Tap actions and modal interactions stable.
- Soak run clean enough to retire Arduino path safely.

## 0) Session Log - ESP-IDF DSL Engine Parity (2026-02-25)

Summary: Ported the generic DSL runtime in ESP-IDF toward Arduino parity, using `origin/http_client_rewrite` as the reference for fetch/runtime behavior. This was done as an engine implementation (not widget-specific C++).

### What Was Completed

- Reviewed Arduino DSL engine and `origin/http_client_rewrite` branch for parity targets.
- Replaced IDF DSL runtime internals with a generic parser/resolver in:
  - `idf/main/DslWidgetRuntimeEspIdf.cpp`
- Added DSL config parsing support for:
  - `data.source`
  - `data.url`
  - `data.poll_ms`
  - `data.debug`
  - `data.fields` (both string and object field specs)
  - `format` block (`round`, `unit`, `locale`, `prefix`, `suffix`, `tz`, `time_format`)
  - `ui.nodes` label parsing
- Added runtime template binding support:
  - `{{geo.*}}`, `{{pref.*}}`, field values
  - conditionals: `if_eq`, `if_ne`, `if_true`, `if_gt`, `if_gte`, `if_lt`, `if_lte`
- Added JSON path resolution for nested objects + arrays:
  - examples: `current.temperature_2m`, `daily.sunrise[0]`
- Added source handling:
  - `http`
  - `local_time`
- Added derived weather mapping in runtime:
  - `code_now/day1_code/day2_code` -> `cond_*` + `icon_*`
- Added HTTP transport hardening inspired by rewrite branch:
  - mutex gate around HTTP transport
  - retry backoff using failure streak
  - explicit transport/status logs

### Build Verification

- Verified successful build with:
  - `source /home/johgor/esp-idf/export.sh`
  - `/home/johgor/esp-idf/tools/idf.py -C idf -D COSTAR_BUILD_LITTLEFS_IMAGE=OFF build`
- Result:
  - `idf/build/costar_idf.bin` generated
  - app size check passed

### Known Gaps / Remaining Work

- Sort transforms are still pending in IDF runtime:
  - `sort_num(...)`
  - `sort_alpha(...)`
  - `distance_sort(...)` / `sort_distance(...)`
- Current IDF runtime node rendering is still label-centric in this file; full node parity (icons/arcs/progress/repeat/sparkline/etc.) remains to be completed.
- Current layout runtime still activates first DSL widget in migration path (intentional earlier step).

### Workspace State (Important)

- Local changes present:
  - `idf/main/app_main.cpp` (modified from earlier work)
  - `idf/main/DslWidgetRuntimeEspIdf.cpp` (new/untracked; now contains major runtime port)
  - `idf/main/LayoutRuntimeEspIdf.cpp` (new/untracked from earlier migration steps)

### Suggested Next Step At Home

1. Flash latest IDF app and validate `weather_now` end-to-end (confirm sunrise/sunset bind correctly).
2. Confirm runtime logs show stable HTTP fetch cadence/backoff behavior.
3. Start next parity chunk:
   - implement sort transforms in IDF runtime
   - then expand node renderer parity (repeat + graphics nodes)

## 0b) Session Log - ESP-IDF Port (2026-02-23)

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
