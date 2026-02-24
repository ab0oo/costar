# ESP-IDF Scaffold

This directory is the standalone ESP-IDF target used for migration off Arduino.

## Current scope

- Brings up a minimal IDF app (`app_main`) with baseline-style boot/loop logs.
- Uses the shared platform abstraction header: `include/platform/Platform.h`
- Provides IDF backend implementation in `idf/main/PlatformEspIdf.cpp`.

## Build and flash

From this directory:

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

## Notes

- This scaffold does not yet run full CoStar widgets/layout runtime.
- Next step is wiring core app modules onto this target incrementally (filesystem, prefs, network, display, touch).
