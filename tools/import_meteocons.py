#!/usr/bin/env python3
import io
import os
import urllib.request

from cairosvg import svg2png
from PIL import Image

SIZE = 28
BASE_URL = "https://raw.githubusercontent.com/basmilius/weather-icons/dev/production/fill/svg-static/"

ICONS = {
    "clear-day.svg": "clear-day.raw",
    "partly-cloudy-day.svg": "partly-cloudy-day.raw",
    "cloudy.svg": "cloudy.raw",
    "fog.svg": "fog.raw",
    "drizzle.svg": "drizzle.raw",
    "rain.svg": "rain.raw",
    "snow.svg": "snow.raw",
    "thunderstorms-day.svg": "thunderstorms-day.raw",
    "sunrise.svg": "sunrise.raw",
    "sunset.svg": "sunset.raw",
}


def rgb565_bytes(r, g, b):
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def fetch_svg(name: str) -> bytes:
    url = BASE_URL + name
    with urllib.request.urlopen(url) as r:
        return r.read()


def render_svg(svg_bytes: bytes, size: int) -> Image.Image:
    png_bytes = svg2png(bytestring=svg_bytes, output_width=size, output_height=size)
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    return img


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
    for svg_name, raw_name in ICONS.items():
        svg = fetch_svg(svg_name)
        img = render_svg(svg, SIZE)
        write_raw(img, os.path.join(out_dir, raw_name))


if __name__ == "__main__":
    main()
