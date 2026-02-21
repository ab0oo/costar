#!/usr/bin/env python3
import math
import os

SIZE = 24


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def s(v):
    return int(round(v * SIZE / 16.0))


def new_icon(bg=0):
    return [[bg for _ in range(SIZE)] for _ in range(SIZE)]


def set_px(img, x, y, color):
    if 0 <= x < SIZE and 0 <= y < SIZE:
        img[y][x] = color


def fill_rect(img, x0, y0, x1, y1, color):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            set_px(img, x, y, color)


def draw_line(img, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        set_px(img, x0, y0, color)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def fill_circle(img, cx, cy, r, color):
    rr = r * r
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr:
                set_px(img, x, y, color)


def draw_cloud(img, color):
    fill_circle(img, s(5), s(9), s(3), color)
    fill_circle(img, s(9), s(8), s(4), color)
    fill_circle(img, s(12), s(9), s(3), color)
    fill_rect(img, s(3), s(9), s(13), s(12), color)


def icon_sun():
    img = new_icon()
    yellow = rgb565(255, 213, 74)
    cx = s(8)
    cy = s(8)
    r = s(4)
    fill_circle(img, cx, cy, r, yellow)
    for angle in range(0, 360, 45):
        rad = math.radians(angle)
        x0 = int(round(cx + math.cos(rad) * s(6)))
        y0 = int(round(cy + math.sin(rad) * s(6)))
        x1 = int(round(cx + math.cos(rad) * s(7)))
        y1 = int(round(cy + math.sin(rad) * s(7)))
        draw_line(img, x0, y0, x1, y1, yellow)
    return img


def icon_cloud():
    img = new_icon()
    gray = rgb565(176, 190, 197)
    draw_cloud(img, gray)
    return img


def icon_partly():
    img = new_icon()
    yellow = rgb565(255, 213, 74)
    gray = rgb565(176, 190, 197)
    fill_circle(img, s(5), s(6), s(3), yellow)
    draw_line(img, s(2), s(6), s(1), s(6), yellow)
    draw_line(img, s(5), s(2), s(5), s(1), yellow)
    draw_line(img, s(8), s(6), s(9), s(6), yellow)
    draw_line(img, s(5), s(10), s(5), s(11), yellow)
    draw_cloud(img, gray)
    return img


def icon_rain():
    img = new_icon()
    gray = rgb565(176, 190, 197)
    blue = rgb565(100, 181, 246)
    draw_cloud(img, gray)
    for x in (s(5), s(8), s(11)):
        draw_line(img, x, s(13), x - s(1), s(15), blue)
    return img


def icon_drizzle():
    img = new_icon()
    gray = rgb565(176, 190, 197)
    blue = rgb565(100, 181, 246)
    draw_cloud(img, gray)
    for x in (s(6), s(10)):
        set_px(img, x, s(14), blue)
        set_px(img, x - s(1), s(15), blue)
    return img


def icon_snow():
    img = new_icon()
    gray = rgb565(176, 190, 197)
    white = rgb565(235, 235, 235)
    draw_cloud(img, gray)
    for x in (s(5), s(8), s(11)):
        set_px(img, x, s(14), white)
        set_px(img, x - s(1), s(15), white)
        set_px(img, x + s(1), s(15), white)
    return img


def icon_fog():
    img = new_icon()
    gray = rgb565(176, 190, 197)
    light = rgb565(150, 160, 170)
    draw_cloud(img, gray)
    draw_line(img, s(2), s(13), s(13), s(13), light)
    draw_line(img, s(3), s(15), s(14), s(15), light)
    return img


def icon_storm():
    img = new_icon()
    gray = rgb565(176, 190, 197)
    yellow = rgb565(255, 235, 59)
    draw_cloud(img, gray)
    draw_line(img, s(8), s(12), s(6), s(15), yellow)
    draw_line(img, s(6), s(15), s(9), s(15), yellow)
    return img


def icon_sunrise():
    img = new_icon()
    sun = rgb565(255, 213, 74)
    arrow = rgb565(255, 255, 255)
    horizon = rgb565(255, 213, 74)
    fill_circle(img, s(8), s(10), s(4), sun)
    fill_rect(img, 0, s(11), SIZE - 1, SIZE - 1, 0)
    draw_line(img, s(2), s(11), s(13), s(11), horizon)
    draw_line(img, s(8), s(2), s(8), s(7), arrow)
    draw_line(img, s(6), s(4), s(8), s(2), arrow)
    draw_line(img, s(10), s(4), s(8), s(2), arrow)
    draw_line(img, s(6), s(7), s(10), s(7), arrow)
    return img


def icon_sunset():
    img = new_icon()
    sun = rgb565(240, 98, 95)
    arrow = rgb565(255, 255, 255)
    horizon = rgb565(240, 98, 95)
    fill_circle(img, s(8), s(10), s(4), sun)
    fill_rect(img, 0, s(11), SIZE - 1, SIZE - 1, 0)
    draw_line(img, s(2), s(11), s(13), s(11), horizon)
    draw_line(img, s(8), s(5), s(8), s(9), arrow)
    draw_line(img, s(6), s(7), s(8), s(9), arrow)
    draw_line(img, s(10), s(7), s(8), s(9), arrow)
    draw_line(img, s(6), s(5), s(10), s(5), arrow)
    return img


def icon_moon():
    img = new_icon()
    moon = rgb565(200, 200, 220)
    shadow = 0
    fill_circle(img, s(8), s(8), s(5), moon)
    fill_circle(img, s(10), s(8), s(5), shadow)
    return img


def write_raw(path, img):
    with open(path, "wb") as f:
        for row in img:
            for val in row:
                f.write(bytes([val & 0xFF, (val >> 8) & 0xFF]))


ICONS = {
    "weather_sun.raw": icon_sun,
    "weather_cloud.raw": icon_cloud,
    "weather_partly.raw": icon_partly,
    "weather_rain.raw": icon_rain,
    "weather_drizzle.raw": icon_drizzle,
    "weather_snow.raw": icon_snow,
    "weather_fog.raw": icon_fog,
    "weather_storm.raw": icon_storm,
    "sunrise.raw": icon_sunrise,
    "sunset.raw": icon_sunset,
    "moon.raw": icon_moon,
}


def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "data", "icons")
    os.makedirs(out_dir, exist_ok=True)
    for name, fn in ICONS.items():
        img = fn()
        write_raw(os.path.join(out_dir, name), img)


if __name__ == "__main__":
    main()
