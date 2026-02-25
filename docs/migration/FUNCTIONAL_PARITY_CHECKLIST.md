# ESP-IDF Functional Parity Checklist

Status date: 2026-02-25

Legend:
- `[x]` done
- `[~]` in progress / partial
- `[ ]` not done

## 1. DSL Render + Runtime Parity

- `[x]` `repeat` expansion in `ui.nodes`
- `[x]` expression-driven numeric node fields (`x/y/r/length/...`)
- `[x]` sorting transforms: `sort_num`, `sort_alpha`, `distance_sort` / `sort_distance`
- `[~]` `label.path` parity (binding works for runtime + transform outputs; alignment/datum behavior still pending)
- `[~]` wrapping parity: `wrap`, `line_height`, `max_lines`, `overflow` implemented in IDF renderer; needs hardware visual verification
- `[x]` node primitives used by active profiles:
  - `label`, `icon`, `line/hand`, `arc/circle`, `moon_phase`
- `[~]` node primitives from full DSL model:
  - `value_box`, `progress`, `sparkline` implemented in IDF, but need on-device visual verification

## 2. Fetch/Transport Parity

- `[x]` poll cadence + startup poll behavior
- `[x]` retry backoff with failure streak
- `[x]` HTTP status/transport logging
- `[x]` `data.headers` support in HTTP fetch path
- `[~]` `adsb_nearest` migration in progress:
  - transform engine (`map`, `compute_distance`, `sort`, `take`, `index_rows`) added in IDF runtime
  - pilot JSON conversion done for `data/dsl_available/adsb_nearest.json`
  - hardware/runtime behavior validation still pending

## 3. Layout + Activation Parity

- `[~]` A/B layout parse and draw frame present
- `[x]` full DSL region activation enabled (IDF layout runtime now starts/ticks all valid DSL regions)

## 4. Interaction Parity

- `[ ]` tap-action parity for DSL widget actions (HTTP tap actions)

## 5. Stability + Verification

- `[x]` IDF build succeeds with `COSTAR_BUILD_LITTLEFS_IMAGE=OFF`
- `[ ]` hardware validation of active screens:
  - `weather_now`
  - `forecast`
  - `clock_analog_full`
- `[ ]` HA authenticated widget end-to-end validation
- `[ ]` multi-hour soak run (no crashes/regressions)

## Exit Criteria Before Removing Arduino Path

- All active DSL files render with acceptable visual parity.
- Required data sources (`http`, `local_time`, and release-required widgets) fetch reliably.
- Home Assistant authenticated fetch path is verified on hardware.
- No critical regressions across provisioning, geo/time, and runtime stability.
