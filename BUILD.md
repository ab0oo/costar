# Build Guide

## 1) Python Environment

Create and activate a local virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

This installs:
- `platformio` for firmware/filesystem build + flash
- `CairoSVG` + `Pillow` for weather icon import/conversion
- `websockets` for `tools/ha_ws_probe.py`

## 2) Build Firmware

```bash
pio run
```

Build outputs:
- `.pio/build/esp32dev_idf/firmware.elf`
- `.pio/build/esp32dev_idf/firmware.bin`

Build hashes are auto-recorded to:
- `.pio/last_build_hashes.txt`
- `.pio/build/esp32dev_idf/build_hashes.txt`

## 3) Flash Device

If data assets changed (`data/`), flash filesystem first:

```bash
pio run -t uploadfs
```

Then flash firmware:

```bash
pio run -t upload
```

Open serial monitor:

```bash
pio device monitor
```

## 4) Regenerate Meteocons

To refresh weather icons from upstream SVG assets:

```bash
python tools/import_meteocons.py
```

Expected output when SVG rendering is active:

```text
import_meteocons: using cairosvg from svg-static
```

If `cairosvg` is unavailable in the active interpreter, the script falls back to upstream PNG sources.

