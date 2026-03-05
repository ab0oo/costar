# DSL User Guide

This guide documents the CoStar widget DSL used by files in `data/dsl_available/*.json`.

## Scope

This guide covers:
- DSL file structure (`version`, `data`, `ui`)
- data source types (`http`, `websocket`, `local_time`)
- security flag `data.tls_skip_verify`
- practical authoring patterns and limits

For deep transform examples, see:
- `docs/dsl_transforms_and_visibility.md`

For shared websocket profile setup, see:
- `docs/ws_connection_profiles.md`

## Top-Level Structure

Typical DSL file:

```json
{
  "version": 1,
  "data": { ... },
  "ui": { ... }
}
```

- `version`: currently `1`
- `data`: fetch/parse/bind config
- `ui`: node tree to draw and optional touch/modal behavior

## `data` Block

Common keys:
- `source`: `"http"`, `"websocket"` (or `"ws"`), `"local_time"`
- `poll_ms`: refresh interval
- `fields`: key/value bindings from payload to widget values
- `transforms`: optional transform pipeline (map/filter/sort/project/index_rows/etc.)
- `debug`: verbose runtime logging
- `http_max_bytes`: max captured response body (HTTP source)
- `tls_skip_verify`: per-widget TLS verify bypass for HTTPS/WSS

### `data.tls_skip_verify`

```json
{
  "data": {
    "source": "http",
    "url": "https://api.example.com/data",
    "tls_skip_verify": true,
    "fields": {
      "value": "payload.value"
    }
  }
}
```

Behavior:
- `false` (default): normal TLS validation (chain + hostname checks).
- `true`: skip TLS certificate validation for that widget connection.
  - HTTPS widgets: applies to `esp_http_client`.
  - WebSocket widgets: applies to `esp_websocket_client` when using `wss://`.

Important:
- `tls_skip_verify` is per widget DSL (or per widget settings override), not global policy.
- Runtime also accepts alias `tls_insecure` for backward compatibility.

Security implications:
- Vulnerable to man-in-the-middle attacks.
- Server identity is not authenticated.
- Use only when needed (for example, private HA with self-signed certs).
- Recommended pattern: set `tls_skip_verify: true` only on private/self-signed endpoints, leave public APIs verified.

Build requirement:
- Firmware must be built with `CONFIG_ESP_TLS_INSECURE=y` for skip-verify mode to take effect.
- If disabled, runtime logs a warning and falls back to normal verification.

## `ui` Block

`ui.nodes` is the render list. Typical node types include:
- `label`, `value_box`
- `line`, `arc`, `circle`
- `progress`, `sparkline`
- `icon`, `bitmap1`, `moon_phase`
- `repeat` for generated rows/items

Additional UI structures:
- `modals`: popup definitions
- `touch_regions`: tap actions (`modal`, `http`, `ws_publish`, etc.)

## Current Coverage Status

Current docs are now split by topic:
- `docs/dsl_user_guide.md` (this file): core DSL concepts and security flags
- `docs/dsl_transforms_and_visibility.md`: advanced transform/visibility/bitmap details
- `docs/ws_connection_profiles.md`: websocket profile sharing and resolution behavior

What is still not fully documented in one place:
- Complete node-by-node property reference (every field for every node type)
- Full expression syntax reference
- All setting override keys and precedence table
- End-to-end examples for each source type with touch actions and modals

So: coverage is strong for active runtime patterns, but the “single exhaustive reference” is still in progress.
