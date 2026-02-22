# image_proxy

Small utility service that fetches a remote image, rescales it, and returns raw RGB565 (little-endian) bytes.

## Run

```bash
cd tools/image_proxy
go run . -listen :8085 -cache-ttl 10m -cache-max-entries 256 -user-agent "CoStar-ImageProxy/1.0"
```

## Build

```bash
cd tools/image_proxy
go build -o image_proxy .
./image_proxy -listen :8085 -cache-ttl 10m -cache-max-entries 256 -user-agent "CoStar-ImageProxy/1.0"
```

## API

`GET /cmh`

Required query params:

- `url`: `http` or `https` image URL
- `size`: target size as either:
  - square: `size=28`
  - explicit: `size=320x240` (or `size=320,240`)

Alternative to `size`: `w` and `h` query params.

Optional query params:

- `ua`: override upstream `User-Agent` header for this request
- `referer`: override upstream `Referer` header for this request

### Example

```bash
curl -L "http://localhost:8085/cmh?url=https://example.com/image.png&size=64" \
  --output image_64x64.raw
```

With upstream header overrides:

```bash
curl -L --get "http://localhost:8085/cmh" \
  --data-urlencode "url=https://example.com/logo.png" \
  --data-urlencode "size=64" \
  --data-urlencode "ua=CoStar-ImageProxy/1.0" \
  --data-urlencode "referer=https://example.com/" \
  --output logo_64.raw
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
  - `X-Fallback: oversize-request` (only when size exceeded)

### Notes

- Resizing uses nearest-neighbor scaling.
- `/mdi` fetches SVG and rasterizes server-side to the requested size.
- Alpha is premultiplied against black before conversion (matches existing icon import behavior).
- Max accepted source payload defaults to `8 MiB` (`-max-bytes` flag).
- Requests larger than `320x320`, or `/cmh` sources whose decoded dimensions exceed `320x320`,
  return a generated default `X` fallback image (output dimensions are clamped to max `320x320`).
- In-memory response cache is enabled by default:
  - `-cache-ttl` controls per-entry lifetime (default `10m`; `0` disables TTL expiry)
  - `-cache-max-entries` bounds memory use (default `256`; `0` disables caching)
- Some upstream hosts still block automated traffic (HTTP 403) even with custom headers.

## MDI icon source

- Official icon catalog: https://pictogrammers.com/library/mdi/
- Canonical SVG repo/package source:
  - https://github.com/Pictogrammers/pictogrammers.com
  - https://www.npmjs.com/package/@mdi/svg
- This utility defaults to Iconify's MDI SVG endpoint (`api.iconify.design`) and rasterizes icons locally.
