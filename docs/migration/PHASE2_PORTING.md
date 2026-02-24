# ESP-IDF Migration Phase 2 (Porting Seam)

This phase introduces a platform abstraction layer while preserving current Arduino behavior.

## Completed in this pass

- Added platform API: `include/platform/Platform.h`
- Added Arduino backend: `src/platform/PlatformArduino.cpp`
- Migrated core timing/logging/heap calls to `platform::` in:
  - `src/main.cpp`
  - `src/core/Widget.h`
  - `src/core/DisplayManager.cpp`
- Added shared prefs abstraction:
  - `include/platform/Prefs.h`
  - `src/platform/PrefsArduino.cpp`
  - `idf/main/PrefsEspIdf.cpp`
- Migrated `RuntimeSettings` persistence to `platform::prefs`:
  - `src/core/RuntimeSettings.cpp`
- Added filesystem seam and migrated key callers:
  - `include/platform/Fs.h`
  - `src/platform/FsArduino.cpp`
  - `idf/main/FsEspIdf.cpp` (stub mount for now)
  - callers: `src/main.cpp`, `src/core/DisplayManager.cpp`, `src/dsl/DslParser.cpp`,
    `src/services/GeoIpService.cpp`, `src/widgets/DslWidgetRender.cpp`
- Added network seam and migrated key callers:
  - `include/platform/Net.h`
  - `src/platform/NetArduino.cpp`
  - `idf/main/NetEspIdf.cpp`
  - callers: `src/main.cpp`, `src/services/GeoIpService.cpp`,
    `src/services/HttpJsonClient.cpp`, `src/widgets/DslWidgetFetch.cpp`,
    `src/widgets/DslWidgetRender.cpp`
- IDF scaffold now compiles shared `RuntimeSettings` module and logs loaded settings.

## Why this matters

- App logic now depends less on direct `Arduino.h` globals (`millis`, `delay`, `Serial`, `ESP`).
- Next step can add an ESP-IDF backend implementing the same API without changing higher-level app logic.

## Next implementation steps

1. Replace FS stub in `idf/main/FsEspIdf.cpp` with a real mount + file I/O backend.
2. Port Wi-Fi provisioning + HTTP execution paths from Arduino classes to ESP-IDF APIs.
3. Bring layout/DSL parsing and display runtime up under IDF target incrementally.
