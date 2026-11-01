#!/usr/bin/env python3
"""Rasterise the WebHMI SVG icons into LVGL A8 images for the CrowPanel HMI.

The web asset stays the single source of the icon artwork. A8 images carry only
coverage, so the HMI recolours them with the profile accent at draw time.
Requires Playwright with Chromium (same as scripts/web_screenshots.py).

Usage: python3 scripts/gen_hmi_icons.py [--check]
"""

import argparse
import re
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "src/variants/web/assets/index.html"
OUTPUT = ROOT / "src/variants/hmi/icons/hmi_icons.c"

# Pixel size and stroke width (in viewBox units) tuned for the 240 px round panel.
NAV_SIZE, NAV_STROKE = 18, 2.0
SPORT_SIZE, SPORT_STROKE = 24, 3.2
BACK = '<path d="M14.5 5.5 8 12l6.5 6.5"/>'


def web_icons():
    html = WEB.read_text(encoding="utf-8")
    nav = {}
    for tab, name in (("dash", "training"), ("dev", "connections"), ("set", "system")):
        match = re.search(r'data-tab="' + tab + r'"[^>]*><svg[^>]*>(.*?)</svg>', html)
        if not match:
            sys.exit(f"navigation icon {tab} not found in {WEB}")
        nav[name] = match.group(1)
    nav["back"] = BACK
    sport = {}
    for key in ("bike", "treadmill", "rower", "cross-trainer", "step-climber", "stair-climber"):
        match = re.search(r'\n "?' + re.escape(key) + r'"?:\{name:[^\n]*?icon:\'([^\']*)\'', html)
        if not match:
            sys.exit(f"profile icon {key} not found in {WEB}")
        sport[key.replace("-", "_")] = match.group(1)
    match = re.search(r"const genericProfile=\{[^\n]*?icon:'([^']*)'", html)
    if not match:
        sys.exit(f"generic profile icon not found in {WEB}")
    sport["generic"] = match.group(1)
    return nav, sport


RASTER = """async ([body, box, size, stroke]) => {
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${box} ${box}" width="${size}" height="${size}">
      <style>*{fill:none;stroke:#fff;stroke-width:${stroke};stroke-linecap:round;stroke-linejoin:round}
      .s{opacity:.5}.f{fill:#fff;stroke:none;opacity:.16}</style>${body}</svg>`;
    const image = new Image();
    image.src = "data:image/svg+xml;base64," + btoa(svg);
    await image.decode();
    const canvas = new OffscreenCanvas(size, size), context = canvas.getContext("2d");
    context.drawImage(image, 0, 0, size, size);
    return Array.from(context.getImageData(0, 0, size, size).data.filter((_, i) => i % 4 === 3));
}"""


def c_image(name, alpha, size):
    rows = []
    for y in range(size):
        rows.append("    " + ",".join(f"0x{v:02x}" for v in alpha[y * size:(y + 1) * size]) + ",")
    return (f"static const uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
            f"const lv_image_dsc_t {name} = {{\n"
            f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .flags = 0,\n"
            f"               .w = {size}, .h = {size}, .stride = {size}, .reserved_2 = 0}},\n"
            f"    .data_size = sizeof({name}_map),\n"
            f"    .data = {name}_map,\n"
            f"    .reserved = NULL,\n}};\n")


# Background textures. LVGL 9 has no gradient dithering and the panel is RGB565,
# so smooth dark gradients band visibly; these are baked with ordered noise.
SKY = 0x10283C            # lcars::Sky
BLACK = 0x07111D          # lcars::Black, the screen colour the sky fades into
SKY_SIZE = (240, 112)     # full width
GLOW_SIZE = (220, 110)    # upper half of a disc centred on the bottom edge
GLOW_PEAK = 72            # alpha at the disc centre
GLOW_DITHER = 24          # alpha noise span, about one RGB565 step of the accents


def noise(x, y):
    """Interleaved gradient noise in [0, 1): fine-grained, without a visible grid."""
    return (52.9829189 * ((0.06711056 * x + 0.00583715 * y) % 1.0)) % 1.0


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3 - 2 * t)


def sky_pixels():
    w, h = SKY_SIZE
    rgb = lambda c: ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)
    words = []
    for y in range(h):
        fade = 1.0 - smooth(y / (h - 1))
        for x in range(w):
            n = noise(x, y)
            r, g, b = (min(int((lo + (hi - lo) * fade) / step + n), 255 // step)
                       for hi, lo, step in zip(rgb(SKY), rgb(BLACK), (8, 4, 8)))
            words.append((r << 11) | (g << 5) | b)
    return [byte for word in words for byte in (word & 0xFF, word >> 8)]


def glow_alpha():
    w, h = GLOW_SIZE
    cx, radius = w / 2, w / 2
    alpha = []
    for y in range(h):
        for x in range(w):
            d = ((x + 0.5 - cx) ** 2 + (y + 0.5 - h) ** 2) ** 0.5 / radius
            value = GLOW_PEAK * smooth(1.0 - d) + (noise(x, y) - 0.5) * GLOW_DITHER
            alpha.append(max(0, min(255, round(value))) if d < 1.0 else 0)
    return alpha


def c_texture(name, data, size, cf, bpp):
    w, h = size
    rows = ["    " + ",".join(f"0x{v:02x}" for v in data[y * w * bpp:(y + 1) * w * bpp]) + ","
            for y in range(h)]
    return (f"static const uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
            f"const lv_image_dsc_t {name} = {{\n"
            f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = {cf}, .flags = 0,\n"
            f"               .w = {w}, .h = {h}, .stride = {w * bpp}, .reserved_2 = 0}},\n"
            f"    .data_size = sizeof({name}_map),\n"
            f"    .data = {name}_map,\n"
            f"    .reserved = NULL,\n}};\n")


def generate():
    nav, sport = web_icons()
    parts = ["/* Generated by scripts/gen_hmi_icons.py from src/variants/web/assets/index.html.",
             " * Do not edit; A8 coverage maps recoloured at draw time, plus dithered",
             " * background textures (sky RGB565, glow A8). */",
             '#include "lvgl.h"', ""]
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        page = browser.new_page()
        page.set_content("<html></html>")
        for prefix, icons, box, size, stroke in (("sf_icon_nav", nav, 24, NAV_SIZE, NAV_STROKE),
                                                 ("sf_icon_sport", sport, 48, SPORT_SIZE, SPORT_STROKE)):
            for key, body in icons.items():
                alpha = page.evaluate(RASTER, [body, box, size, stroke])
                parts.append(c_image(f"{prefix}_{key}", alpha, size))
        browser.close()
    parts.append(c_texture("sf_tex_sky", sky_pixels(), SKY_SIZE, "LV_COLOR_FORMAT_RGB565", 2))
    parts.append(c_texture("sf_tex_glow", glow_alpha(), GLOW_SIZE, "LV_COLOR_FORMAT_A8", 1))
    return "\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the generated file is outdated")
    args = parser.parse_args()
    text = generate()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != text:
            sys.exit(f"{OUTPUT.relative_to(ROOT)} is outdated; run scripts/gen_hmi_icons.py")
        print("HMI icons up to date")
        return
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
