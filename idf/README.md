# ESP-IDF Scaffold

This directory contains the ESP-IDF application target for CoStar.

## Current scope

- Contains the active `app_main` runtime and UI/widget stack.
- Uses shared platform headers in `include/platform/*`.
- Provides IDF backend implementations in `idf/main/*EspIdf.cpp`.

## Build and flash

From this directory:

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

## Notes

- Build from repository root with PlatformIO (`esp32dev_idf`) for the default workflow.
