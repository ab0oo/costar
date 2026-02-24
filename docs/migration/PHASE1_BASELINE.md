# ESP-IDF Migration Phase 1 Baseline

This phase locks current Arduino behavior and records baseline metrics before any ESP-IDF cutover work.

## Must-Match Behavior (Release Gates)

1. Layout profile toggle
- USER button toggles A/B layouts.
- Active profile persists across reboot (`layout.profile`).

2. Network + provisioning
- Stored Wi-Fi credentials reconnect automatically.
- Provisioning UI allows scan/select/password and successful connect.

3. Geo + time
- Manual geo override loads first when present.
- Cached/online geo fallback works as documented.
- UTC time sync works and UI local offset behavior is unchanged.

4. DSL runtime
- `repeat`, expression functions, sorting transforms, and `label.path` rendering continue to work.
- Wrapping settings (`wrap`, `line_height`, `max_lines`, `overflow`) behave the same.

5. Home Assistant direct REST
- Header-based auth (`data.headers`) succeeds for entity state endpoint.
- Error behavior and on-screen fallback messaging remain acceptable.

6. Remote icon pipeline
- HTTP/HTTPS icon URL path renders correctly.
- Cache behavior is preserved: RAM cache, then `/icon_cache/*.raw`, then network fetch.

## Baseline Instrumentation

`src/main.cpp` now emits stage and periodic metrics when `AppConfig::kBaselineMetricsEnabled` is true.

Boot-stage lines:
- `[baseline] stage=<name> t_ms=<ms> heap_free=<bytes> heap_min=<bytes>`

Periodic loop lines:
- `[baseline] uptime_s=<sec> heap_free=<bytes> heap_min=<bytes> wifi=<0|1> rssi=<dBm>`

## Capture Procedure

1. Build and flash current firmware + assets
- `pio run`
- `pio run -t uploadfs`
- `pio run -t upload`

2. Capture monitor output
- `pio device monitor | tee baseline_run.log`

3. Exercise behavior gates
- Reboot twice (verifies persisted layout and startup consistency).
- Toggle USER button profile.
- Open setup screen, verify prefs + manual geo path.
- Validate HA widget fetch.
- Validate remote icon first fetch and cache hit.

4. Record metrics in `docs/migration/baseline_metrics.csv`.

## Acceptance Thresholds (Suggested)

Use these as migration guardrails. Tighten after 3+ baseline runs.

- Boot to `display_ready`: no worse than +20% vs baseline median.
- Free heap after setup: no more than 15% below baseline median.
- Wi-Fi connect time: no worse than +25% vs baseline median.
- No regression in mandatory behavior gates above.

## Notes

- Keep this instrumentation enabled until ESP-IDF implementation reproduces all must-match behaviors.
- Once parity is proven, either disable via `kBaselineMetricsEnabled=false` or remove the logs.
