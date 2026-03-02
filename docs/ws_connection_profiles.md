# WebSocket Connection Profiles

Use `shared_settings.ws_profiles` to define connection-level protocol details once per screen.
Widgets then reference a profile with `connection_profile` and keep only widget-specific
`bootstrap`/`subscribe`/field mapping logic.

## Shared Settings Schema

```json
{
  "shared_settings": {
    "ws_profiles": {
      "profile_name": {
        "url": "https://server.example.com",
        "path": "/ws",
        "token": "{{setting.ws_token}}",
        "auth_message": "{\"type\":\"auth\",\"token\":\"{{ws_token}}\"}",
        "auth_required_type": "auth_required",
        "auth_ok_type": "auth_ok",
        "auth_invalid_type": "auth_invalid",
        "ready_type": "ready",
        "init": [
          {"type":"hello","client":"costar"},
          "{\"type\":\"join\",\"room\":\"main\"}"
        ]
      }
    }
  }
}
```

## Widget DSL Schema

```json
{
  "data": {
    "source": "websocket",
    "connection_profile": "{{setting.connection_profile}}",
    "cache_key": "{{setting.entity_id}}",
    "bootstrap": {...},
    "subscribe": {...},
    "result_path": "result",
    "event_path": "event"
  }
}
```

## Resolution Order

- Widget `data.ws_*` keys override profile values when present.
- If missing in widget DSL, runtime uses profile values from `ws_profiles`.
- If no profile is selected/found, runtime falls back to legacy direct settings (`ws_url`, `ws_path`, `ws_token`).
