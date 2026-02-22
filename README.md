# CoStar: ESP32 Live Information Dashboard

CoStar turns an ESP32 + 320x240 TFT into a compact, always-on information panel.
It is designed for fast iteration: you can change what appears on screen by editing JSON files instead of rewriting firmware.

<p align="center">
  <img src="docs/images/screen_shot.jpg" alt="CoStar dashboard screenshot" width="800">
</p>

Initial widget layout inspiration: [lachimalaif/DataDisplay-V1-instalator](https://github.com/lachimalaif/DataDisplay-V1-instalator).

## Why flash this?

- Build a custom wall/status display without writing custom widget classes.
- Mix local device info, internet APIs, and Home Assistant in one screen.
- Move from idea to on-screen result quickly using JSON + browser preview.
- Keep it maintainable: layout, data mapping, and rendering are all declarative.

## What can you display?

Almost anything that can be fetched as JSON (or computed locally), including:

- Weather now + forecast
- Clocks (digital and analog)
- Home Assistant entities (state, label, icon)
- ADS-B nearby flights (sorted by distance)
- Crypto prices
- Moon phase and day/date info
- Any external image or icon converted to display-friendly format

The system supports multiple layout profiles (`A/B`) and switches between them using the USER button on-device.

## JSON DSL highlights

The on-screen UI is described with JSON nodes and drawing primitives.
You can compose rich widgets from:

- `label`, `value_box`
- `line`, `arc`
- `progress`, `sparkline`
- `icon`, `moon_phase`
- `repeat` (generate rows/items without copy-paste)

Data and UI are connected through template bindings and field extraction:

- Bind text/values with `{{field}}`, `{{geo.*}}`, `{{pref.*}}`, `{{setting.*}}`
- Pull nested JSON paths from API payloads
- Sort and filter list data (`sort_num`, `sort_alpha`, `distance_sort`)
- Use expressions (including distance math) for runtime positioning and calculations
- Wrap long text labels with width/line controls

## Home Assistant support

CoStar can query Home Assistant directly from the ESP32 over HTTPS.
Use per-widget settings for base URL, token, entity ID, and render the returned state/icon in your card.

## Quick start

1. Install dependencies (PlatformIO + board support as usual).
2. Put your layout/DSL JSON files in `data/`.
3. Flash assets and firmware:
   1. `pio run -t uploadfs`
   2. `pio run -t upload`
   3. `pio device monitor`

## Browser editor (fast iteration)

Preview and tune DSL widgets without reflashing:

1. `python3 -m http.server 8000`
2. Open `http://localhost:8000/tools/dsl_editor/`
3. Edit DSL JSON and sample payload JSON side-by-side

This is the fastest way to dial in fonts, spacing, wrapping, and API mappings before deploying to hardware.

## Optional image proxy (for arbitrary icons/images)

`tools/image_proxy` can fetch remote images, resize them, and return display-ready RGB565 raw bytes.
This is useful for MDI icons and external logos without embedding image decoders on the ESP32.

- Build/run details: `tools/image_proxy/README.md`
- Shared dev relay currently used by this project: `http://vps.gorkos.net:8085`

## Project docs

- `CONTEXT.md`: deep technical reference and current runtime details
- `docs/HANDOFF.md`: AI-agent handoff/runbook for continuing work safely

If you are hacking on internals, use those two files as the authoritative technical source.

## Third-party assets

- Meteocons weather icons by Bas Milius (MIT License)
  - Source: https://github.com/basmilius/weather-icons
  - License text: `third_party/meteocons/LICENSE`
