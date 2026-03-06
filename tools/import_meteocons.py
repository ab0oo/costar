#!/usr/bin/env python3
import io
import os
import urllib.request

from PIL import Image
try:
    from cairosvg import svg2png
except ImportError:
    svg2png = None

SIZE = 28
PNG_BASE_URL = "https://raw.githubusercontent.com/basmilius/weather-icons/dev/production/fill/png/32/"
SVG_BASE_URL = "https://raw.githubusercontent.com/basmilius/weather-icons/dev/production/fill/svg-static/"

ICONS = {
    "clear-day": "clear-day.raw",
    "clear-night": "clear-night.raw",
    "partly-cloudy-day": "partly-cloudy-day.raw",
    "partly-cloudy-night": "partly-cloudy-night.raw",
    "cloudy": "cloudy.raw",
    "overcast-day": "overcast-day.raw",
    "overcast-night": "overcast-night.raw",
    "fog": "fog.raw",
    "fog-day": "fog-day.raw",
    "fog-night": "fog-night.raw",
    "drizzle": "drizzle.raw",
    "overcast-day-drizzle": "drizzle-day.raw",
    "overcast-night-drizzle": "drizzle-night.raw",
    "rain": "rain.raw",
    "overcast-day-rain": "rain-day.raw",
    "overcast-night-rain": "rain-night.raw",
    "overcast-day-sleet": "sleet-day.raw",
    "overcast-night-sleet": "sleet-night.raw",
    "snow": "snow.raw",
    "overcast-day-snow": "snow-day.raw",
    "overcast-night-snow": "snow-night.raw",
    "thunderstorms-day": "thunderstorms-day.raw",
    "thunderstorms-night": "thunderstorms-night.raw",
    "sunrise": "sunrise.raw",
    "sunset": "sunset.raw",
}


def rgb565_bytes(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def fetch_icon(url: str) -> bytes:
    with urllib.request.urlopen(url) as r:
        return r.read()


def render_png(png_bytes: bytes, size: int) -> Image.Image:
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    if img.width != size or img.height != size:
        img = img.resize((size, size), Image.Resampling.LANCZOS)
    return img


def render_svg(svg_bytes: bytes, size: int) -> Image.Image:
    png_bytes = svg2png(bytestring=svg_bytes, output_width=size, output_height=size)
    return render_png(png_bytes, size)


def write_raw(img: Image.Image, path: str):
    out = bytearray()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = img.getpixel((x, y))
            if a < 255:
                r = (r * a) // 255
                g = (g * a) // 255
                b = (b * a) // 255
            out += rgb565_bytes(r, g, b)
    with open(path, "wb") as f:
        f.write(out)


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "data", "icons", "meteocons")
    os.makedirs(out_dir, exist_ok=True)
    use_svg = svg2png is not None
    if use_svg:
        print("import_meteocons: using cairosvg from svg-static")
    else:
        print("import_meteocons: cairosvg unavailable, falling back to png/32")
    for icon_key, raw_name in ICONS.items():
        if use_svg:
            svg_bytes = fetch_icon(SVG_BASE_URL + icon_key + ".svg")
            img = render_svg(svg_bytes, SIZE)
        else:
            png_bytes = fetch_icon(PNG_BASE_URL + icon_key + ".png")
            img = render_png(png_bytes, SIZE)
        write_raw(img, os.path.join(out_dir, raw_name))


if __name__ == "__main__":
    main()
