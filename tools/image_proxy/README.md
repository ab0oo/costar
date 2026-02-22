# image_proxy

Small utility service that fetches a remote image, rescales it, and returns raw RGB565 (little-endian) bytes.

## Run

```bash
cd tools/image_proxy
go run . -listen :8085 -cache-ttl 10m -cache-max-entries 256
```

## Build

```bash
cd tools/image_proxy
go build -o image_proxy .
./image_proxy -listen :8085 -cache-ttl 10m -cache-max-entries 256
```

## API

`GET /cmh`

Required query params:

- `url`: `http` or `https` image URL
- `size`: target size as either:
  - square: `size=28`
  - explicit: `size=320x240` (or `size=320,240`)

Alternative to `size`: `w` and `h` query params.

### Example

```bash
curl -L "http://localhost:8085/cmh?url=https://example.com/image.png&size=64" \
  --output image_64x64.raw
```

`GET /mdi`

Required query params:

- `icon`: MDI icon name (`weather-partly-cloudy`) or `mdi:weather-partly-cloudy`

Optional query params:

- `size`: target size as either:
  - square: `size=28` (default if omitted)
  - explicit: `size=64x64` (or `size=64,64`)
- `color`: icon color hex (`RRGGBB` or `#RRGGBB`)

Example:

```bash
curl -L "http://localhost:8085/mdi?icon=mdi:weather-partly-cloudy&size=28&color=FFFFFF" \
  --output mdi_weather_partly_cloudy_28.raw
```

### Response

- Body: packed `rgb565le` bytes (`width * height * 2` bytes)
- Headers:
  - `X-Image-Format: rgb565le`
  - `X-Width`
  - `X-Height`
  - `X-Source-Format`
  - `X-Source-URL`
  - `X-Cache: HIT|MISS`
  - `X-Cache-Age-Seconds`

### Notes

- Resizing uses nearest-neighbor scaling.
- Alpha is premultiplied against black before conversion (matches existing icon import behavior).
- Max accepted source payload defaults to `8 MiB` (`-max-bytes` flag).
- In-memory response cache is enabled by default:
  - `-cache-ttl` controls per-entry lifetime (default `10m`; `0` disables TTL expiry)
  - `-cache-max-entries` bounds memory use (default `256`; `0` disables caching)

## MDI icon source

- Official icon catalog: https://pictogrammers.com/library/mdi/
- Canonical SVG repo/package source:
  - https://github.com/Pictogrammers/pictogrammers.com
  - https://www.npmjs.com/package/@mdi/svg
- This utility defaults to an MDI raster mirror endpoint (`api.iconify.design`) for simple on-the-fly conversion.
