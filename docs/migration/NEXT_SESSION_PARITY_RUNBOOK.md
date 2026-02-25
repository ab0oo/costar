# ESP-IDF Next Session Parity Runbook

Status date: 2026-02-25

This runbook is the concrete verification sequence for final Arduino parity sign-off on the ESP-IDF path.

## 0) Active Soak Findings (Must Close)

1. Touch hit-rate is too low (~50% of taps register).
2. Runtime menu button (top-right) flickers and can be overdrawn by widget content.

Exit expectation for both:
1. Touch hit-rate is consistently high in normal use (target: at least 90% successful taps across menu + widget actions).
2. Menu button remains visible/stable in runtime and does not visibly flicker during idle updates.

## 1) Preflight

1. Export IDF env:
```bash
source /home/johgor/esp-idf/export.sh
```
2. Set your serial port:
```bash
export PORT=/dev/ttyUSB0
```
3. Create a log folder:
```bash
mkdir -p logs
```
4. PlatformIO path (default env is now IDF):
```bash
/home/johgor/.venv/bin/pio project config
/home/johgor/.venv/bin/pio run
```
Expected: default env is `esp32dev_idf` and build reaches `SUCCESS`.

## 2) Build + Flash

1. Normal parity build (offline-safe, does not regenerate `storage.bin`):
```bash
idf.py -C idf -D COSTAR_BUILD_LITTLEFS_IMAGE=OFF build
```
2. Flash app:
```bash
idf.py -C idf -p "$PORT" flash
```
3. If DSL/layout assets changed and you need updated LittleFS:
```bash
idf.py -C idf build flash
```
4. Optional clean slate (wipes NVS + LittleFS + app):
```bash
idf.py -C idf -p "$PORT" erase-flash
idf.py -C idf -p "$PORT" flash
```
5. PlatformIO equivalents (if preferred):
```bash
/home/johgor/.venv/bin/pio run
/home/johgor/.venv/bin/pio run -t upload
```

## 3) Capture Session Log

Run monitor and tee to a file:
```bash
idf.py -C idf -p "$PORT" monitor | tee "logs/$(date +%F_%H%M%S)_idf_parity.log"
```

Keep that single log running through all checks below.

## 4) Identity + Boot Sanity Gate

Manual checks:
1. Boot shows expected app metadata (`App version`, `Compile time`, `ELF file SHA256`, `ESP-IDF`).
2. `boot: littlefs=1`.
3. `fs: required assets OK`.
4. Runtime starts cleanly with no panic/reboot loop.

Useful commands (in another terminal):
```bash
LOG=logs/<your_log_file>.log
rg -n "App version|Compile time|ELF file SHA256|ESP-IDF|boot: littlefs=|fs: required assets OK|required assets missing|missing asset=" "$LOG"
rg -n "Guru Meditation|Backtrace:|panic|assert failed|rst:0x|WDT" "$LOG"
```

## 5) Runtime/Menu Gate (Top Priority)

Exercise in this order:
1. Open menu button.
2. Select Layout A, B, NYT.
3. Re-open menu and select WiFi/Units.
4. Re-open menu and select Touch Calibrate.
5. Confirm no stuck redraw/unresponsive row behavior.

Pass evidence:
1. Every tap emits `ui: menu action=...`.
2. Layout changes emit `ui: switch layout path=...`.
3. Config and touch-cal entries emit expected logs.
4. Tap dispatch shows `handled=1` for menu taps.
5. Menu button remains continuously visible (no overdraw disappearance) while widgets update.

Commands:
```bash
rg -n "ui: menu action=|ui: switch layout path=|ui: open runtime config|ui: open runtime touch calibration|touch: runtime tap dispatch" "$LOG"
```

Menu action value map:
1. `1` toggle
2. `2` select layout A
3. `3` select layout B
4. `4` select layout NYT
5. `5` open config
6. `6` open touch calibration
7. `7` dismiss menu

## 6) NYT Fullscreen + Modal Gate

Exercise:
1. Switch to NYT layout.
2. Wait for feed refresh.
3. Tap headline region(s).
4. Confirm modal open, close tap, and auto-dismiss path.

Pass evidence:
1. Region hit/miss logs from layout runtime.
2. Modal open/close logs from DSL widget runtime.
3. No repeated error/backoff loop.

Commands:
```bash
rg -n "layout-runtime: tap hit region|layout-runtime: tap miss|dsl-widget: tap modal open|dsl-widget: tap modal close|dsl-widget: modal auto close" "$LOG"
rg -n "dsl-widget: fetch fail widget=|dsl-widget: update ok widget=" "$LOG"
```

## 7) HA Speaker Touch HTTP Gate

Exercise:
1. Open HA speaker layout/card.
2. Tap modal region.
3. Tap VOL- and VOL+ regions repeatedly.

Pass evidence:
1. `dsl-widget: tap touch_region http ok widget=...` on successful actions.
2. No persistent `tap touch_region http fail`.
3. Post-tap delayed refresh scheduling appears.

Commands:
```bash
rg -n "dsl-widget: tap touch_region http ok|dsl-widget: tap touch_region http fail|dsl-widget: tap scheduled refresh" "$LOG"
```

## 8) HA 6-Card + Icon Cache Gate

Exercise:
1. Load HA 6-card screen from cold boot.
2. Note first render latency.
3. Switch away and back twice.
4. Verify subsequent renders are faster and stable.

Current log caveat:
1. Per-request HTTP start/done/header logs are `DEBUG`.
2. Current `sdkconfig` has default/max log level at `INFO`.
3. Cache hit/miss is therefore primarily validated by behavior right now (not by detailed HTTP traces).

Failure evidence to grep:
```bash
rg -n "dsl-widget: icon fetch fail|dsl-widget: icon fetch status=|dsl-widget: icon fetch size mismatch|dsl-widget: icon cache dir create failed" "$LOG"
```

## 9) Weather/Forecast/Clock Gate

Exercise:
1. Confirm active screen set renders:
- `weather_now`
- `forecast`
- `clock_analog_full`
2. Confirm no blank regions, stale values, or broken labels.

Pass evidence:
```bash
rg -n "layout-runtime: loaded regions=|layout-runtime: dsl widgets started=|dsl-widget: begin widget=|dsl-widget: update ok widget=" "$LOG"
```

## 10) Soak Gate (2-4 hours)

Keep runtime active for 2-4 hours with periodic interaction:
1. Layout switches every 10-20 minutes.
2. NYT headline modal taps.
3. HA speaker touch actions.
4. Config screen open/close once per hour.

Pass criteria:
1. No reboot/panic/watchdog.
2. No permanently stuck menu/touch state.
3. No progressive heap collapse.
4. No escalating fetch-fail streak that never recovers.
5. Touch interactions remain reliable over time (no degradation toward missed taps).
6. Menu button remains visible and stable throughout the soak window.

Commands:
```bash
rg -n "baseline: uptime_s=|perf: heap_largest_8bit=|perf: heap_largest_dma=" "$LOG"
rg -n "dsl-widget: fetch fail widget=|dsl-widget: update ok widget=" "$LOG"
rg -n "Guru Meditation|Backtrace:|panic|assert failed|rst:0x|WDT" "$LOG"
```

## 11) Quick Source Anchors (for log meaning)

```bash
rg -n "enum class RuntimeMenuAction|menu action=|switch layout path=|open runtime config|open runtime touch calibration" idf/main/app_main.cpp
rg -n "tap hit region|tap miss" idf/main/LayoutRuntimeEspIdf.cpp
rg -n "tap modal open|tap modal close|modal auto close|tap touch_region http ok|tap touch_region http fail|tap scheduled refresh|kTapPostHttpRefreshDelayMs" idf/main/DslWidgetRuntimeEspIdf.cpp
rg -n "kHttpWorkerCore|xTaskCreatePinnedToCore\\(httpWorkerTask" idf/main/DslWidgetRuntimeEspIdf.cpp
```
