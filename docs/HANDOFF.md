# Handoff Runbook (Primary Track: ESP-IDF Parity)

## 1) Project Direction

- Primary goal: full ESP-IDF rewrite with functional parity to the existing Arduino DSL runtime.
- Arduino codebase is now reference behavior, not the long-term target runtime.
- Work must prioritize:
  1. IDF parity and stability
  2. deterministic networking and rendering behavior
  3. clean migration path for DSL widgets/layouts

## 2) Branch / Merge Status

- User requested merge of `http_client_rewrite` into `main`; merge has been completed and conflicts were resolved.
- Conflict resolution strategy used:
  - keep `main` (IDF-focused platform abstraction direction) as baseline
  - pull in safe additions from `http_client_rewrite` where applicable
  - retain buildability on current `main`
- Post-merge firmware build status:
  - `pio run` succeeds on `main`.

## 3) What Was Brought In

- Added/updated DSL assets and layouts (under current tree structure):
  - `data/dsl_available/ha_speaker_card.json`
  - `data/dsl_available/nyt_headlines.json`
  - `data/layouts.json`
  - `data/screen_layout_nyt.json`
  - updates to `data/screen_layout_a.json`
- Runtime/networking additions now present:
  - `src/services/HttpTransportGate.cpp`
  - `src/services/HttpTransportGate.h`
  - `src/widgets/DslRuntimeCaches.h`
  - `src/services/HttpJsonClient.cpp` migrated to `esp_http_client` backend (JSON path)
  - cache clear hook on layout reload in `src/core/DisplayManager.cpp`
- DSL enhancements present:
  - conditional template helpers (`if_eq`, `if_ne`, `if_true`, `if_gt`, `if_gte`, `if_lt`, `if_lte`) in `src/widgets/DslWidget.cpp`.

## 4) Current Known Risk Areas

- Transport stability on device still needs validation after merge and post-conflict reconciliation.
- `HttpJsonClient` is now `esp_http_client`, but remote icon fetch path in `DslWidgetRender` is still using Arduino `HTTPClient`.
- Mixed network stack usage (IDF HTTP + Arduino HTTP) is transitional and may still cause inconsistent behavior under load.

## 5) Immediate Next Steps (IDF-Focused)

1. Validate merged `main` runtime on hardware:
   - boot path
   - HA card polling
   - weather/forecast on layout switch
   - NYT RSS widget.
2. Finish network stack unification:
   - migrate `fetchRemoteIconToFile` in `src/widgets/DslWidgetRender.cpp` to `esp_http_client`.
3. Introduce single outbound request worker queue for JSON + icon fetches.
4. Keep refining error taxonomy/logging for ESP-side failure classes:
   - DNS, connect, TLS, no-status transport, HTTP status, parse.
5. Continue IDF parity implementation using Arduino behavior as reference.

## 6) Build / Run Commands

- Build firmware: `pio run`
- Upload assets: `pio run -t uploadfs`
- Upload firmware: `pio run -t upload`
- Monitor serial: `pio device monitor`

## 7) Notes For Next Agent

- Do not treat Arduino path as the destination architecture.
- If a change helps Arduino behavior but blocks/complicates IDF parity, defer it.
- Preserve the merged branch state first; avoid rebasing away the integration unless explicitly requested.
