#!/usr/bin/env python3
"""Render assets/header.png — the README banner.

Same visual language as geistlib's assets/neuron.png: an ASCII field on
near-black, glyphs ramping from muted indigo through periwinkle to white.

The subject is what the library does. A query drops into the store and
rings come back: interfering ripples from a handful of sources, brightest
where they reinforce. Nothing here is a stock image — run this file and you
get the banner back, byte for byte.

    python3 assets/make-header.py
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 1871, 845          # matches geistlib's banner
FONT = "/System/Library/Fonts/Menlo.ttc"
FONT_PX = 15
BG = (7, 9, 17)

# Dark to bright. The vocabulary is neuron.png's.
RAMP = " .,:*/(#%&@"

# Indigo -> periwinkle -> white, indexed by the same brightness as the glyph.
STOPS = [(0.00, (34, 39, 68)),
         (0.35, (63, 71, 122)),
         (0.62, (124, 133, 196)),
         (0.85, (186, 194, 235)),
         (1.00, (238, 242, 252))]


def colour(t):
    for i in range(len(STOPS) - 1):
        a, ca = STOPS[i]
        b, cb = STOPS[i + 1]
        if t <= b:
            f = 0.0 if b == a else (t - a) / (b - a)
            return tuple(int(round(ca[j] + f * (cb[j] - ca[j]))) for j in range(3))
    return STOPS[-1][1]


def field(cols, rows):
    """Two ripple sources, and mostly silence between them.

    Aspect is corrected so the rings read as round even though a character
    cell is roughly twice as tall as it is wide. The low end is clipped hard:
    the banner needs real black to have any structure at all.
    """
    x = (np.arange(cols) + 0.5) / cols
    y = (np.arange(rows) + 0.5) / rows
    gx, gy0 = np.meshgrid(x, y)
    gy = (gy0 - 0.5) * (rows * 2.05) / cols + 0.5   # cell aspect

    # A query lands (left, strong) and the store answers (right, softer).
    sources = [(0.30, 0.50, 1.00, 27.0, 1.55),      # x, y, weight, k, decay
               (0.74, 0.46, 0.62, 21.0, 1.30)]

    v = np.zeros_like(gx)
    for sx, sy, w, k, decay in sources:
        r = np.hypot(gx - sx, gy - sy) + 1e-3
        v += w * np.cos(k * r) * np.exp(-decay * r)

    v = (v - v.min()) / (v.max() - v.min())

    # Vignette in real (uncorrected) coordinates, then a hard floor so the
    # troughs between rings become empty space rather than dim texture.
    vx = np.clip(1.0 - ((gx - 0.5) / 0.66) ** 2, 0.0, 1.0)
    vy = np.clip(1.0 - ((gy0 - 0.5) / 0.72) ** 2, 0.0, 1.0)
    v = v * (0.12 + 0.88 * (vx * vy) ** 0.7)

    v = np.clip((v - 0.28) / 0.62, 0.0, 1.0) ** 0.78
    return v

def main():
    font = ImageFont.truetype(FONT, FONT_PX)
    adv = font.getlength("M")
    line = int(round(FONT_PX * 1.16))
    cols = int(W / adv) + 1
    rows = int(H / line) + 1

    img = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(img)
    v = field(cols, rows)

    for r in range(rows):
        for c in range(cols):
            t = float(v[r, c])
            g = RAMP[min(len(RAMP) - 1, int(t * len(RAMP)))]
            if g == " ":
                continue
            # Colour on a steeper curve than the glyph: the densest glyphs
            # only occupy the top of the range, and without this the bright
            # bands stay periwinkle instead of reaching white.
            draw.text((c * adv, r * line), g, font=font, fill=colour(t ** 0.5))

    img.save("assets/header.png")
    print(f"assets/header.png  {W}x{H}  {cols}x{rows} cells")


if __name__ == "__main__":
    main()
