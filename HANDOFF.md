# CoStar Handoff

Updated: 2026-03-01

## Current Runtime Snapshot

- Firmware entrypoint is `idf/main/app_main.cpp`.
- Layout runtime is JSON-driven via `layout_runtime` + `dsl_widget_runtime`.
- Supported DSL data sources in runtime:
  - `http`
  - `websocket` (also accepts alias `ws`)
  - `local_time`
- Touch runtime menu is active and can select 4 layouts:
  - `/littlefs/screen_layout_a.json`
  - `/littlefs/screen_layout_b.json`
  - `/littlefs/screen_layout_nyt.json`
  - `/littlefs/screen_layout_quakes.json`
- Selected layout is persisted in NVS:
  - namespace/key: `ui/layout`

## WebSocket + Home Assistant Status

- Generic websocket path is the active HA path (`source: "websocket"`).
- HA DSL files now using websocket:
  - `data/dsl_available/homeassistant_entity.json`
  - `data/dsl_available/homeassistant_control_card.json`
  - `data/dsl_available/ha_speaker_card.json`
  - `data/dsl_available/websocket_entity.json`
  - `data/dsl_available/websocket_entities_card.json`
- Screen-level websocket connection profiles are supported through:
  - `shared_settings.ws_profiles`
  - widget setting / `data.connection_profile`
- Connection profile docs:
  - `docs/ws_connection_profiles.md`
- DSL transform/node docs:
  - `docs/dsl_transforms_and_visibility.md`
- Tap actions supported in runtime:
  - `refresh`
  - `http`
  - `ws_publish` (also accepts `ws` / `websocket`)
- For websocket data sources, tap action configured as `http` is auto-promoted to websocket publish.

## Important Current Details

- `data/screen_layout_a.json` is currently a websocket HA multi-entity fullscreen card.
- `data/screen_layout_b.json` is currently weather + forecast + full analog clock.
- `include/AppConfig.h` currently sets both `kLayoutPathA` and `kLayoutPathB` to `/screen_layout_b.json`.
  - Note: app runtime layout selection in `app_main.cpp` uses hardcoded `/littlefs/screen_layout_*.json` constants, not `AppConfig::kLayoutPath*`.
- Build fingerprint logging is enabled at boot:
  - `boot: build id=<64 hex>`
  - `boot: build meta version=... date=... time=...`
- Baseline telemetry logging remains enabled (`kBaselineEnabled=true` in `app_main.cpp`).

## Known Gaps / Follow-ups

1. `ha_speaker_card.json` still has `touch_regions` using HTTP volume service calls; decide whether to keep HTTP or move to websocket publish for consistency.
2. `CONTEXT.md` is stale relative to current runtime behavior (layout switching semantics and websocket migration).
3. Repo has an active dirty working tree with many modified JSON/runtime files; review before release/merge.
4. Credentials/tokens are present in some layout JSON during development; scrub before sharing repo snapshots externally.

## Build / Flash / Validate

From repo root:

1. `pio run -t uploadfs`
2. `pio run -e esp32dev_idf -t upload`
3. `pio device monitor`

Recommended post-flash checks:

1. Confirm boot fingerprint lines are printed.
2. Confirm required LittleFS assets are detected (`required assets OK` log path in boot).
3. Open runtime menu via touch and verify layout switching persists across reboot.
4. Validate websocket cards receive bootstrap + subscribe updates.

## Key Paths

- Runtime entry: `idf/main/app_main.cpp`
- Layout loader: `idf/main/LayoutRuntimeEspIdf.cpp`
- DSL runtime + data transport: `idf/main/DslWidgetRuntimeEspIdf.cpp`
- Platform adapters (ESP-IDF): `idf/main/PlatformEspIdf.cpp`, `FsEspIdf.cpp`, `NetEspIdf.cpp`, `PrefsEspIdf.cpp`
- Header interfaces: `include/`
