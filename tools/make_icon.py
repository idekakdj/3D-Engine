#!/usr/bin/env python3
"""Generates the Aether logo: resources/aether.png (256 px), resources/aether.ico (16..256 px)
and resources/splash.png (the startup splash). Requires Pillow. Run from the repository root:
    python3 tools/make_icon.py
The artwork is procedural (no external assets): a luminous triangular "A" inside an orbit, on a
dark rounded tile."""
import math
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # master resolution; everything is downsampled for anti-aliasing


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def gradient(size, c0, c1, angle_deg):
    """Linear gradient image (RGBA) from c0 to c1 along angle."""
    img = Image.new("RGBA", (size, size))
    px = img.load()
    a = math.radians(angle_deg)
    dx, dy = math.cos(a), math.sin(a)
    for y in range(size):
        for x in range(size):
            t = ((x / size - 0.5) * dx + (y / size - 0.5) * dy) + 0.5
            px[x, y] = lerp(c0, c1, min(max(t, 0.0), 1.0))
    return img


def logo(size=S, tile=True):
    base = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    if tile:
        bg = gradient(size, (18, 24, 44, 255), (8, 10, 22, 255), 60)
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=int(size * 0.22), fill=255)
        base.paste(bg, (0, 0), mask)

    # Luminous strokes are drawn as white masks, coloured by a cyan->violet gradient.
    strokes = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(strokes)
    c = size / 2
    w = int(size * 0.055)
    # Orbit: a tilted ellipse (drawn as a polyline for an even stroke).
    pts = []
    for i in range(361):
        t = math.radians(i)
        x, y = math.cos(t) * size * 0.40, math.sin(t) * size * 0.15
        r = math.radians(-18)
        pts.append((c + x * math.cos(r) - y * math.sin(r), c + 40 * size / 1024 + x * math.sin(r) + y * math.cos(r)))
    d.line(pts, fill=160, width=int(w * 0.55), joint="curve")
    # The "A": an open triangle with a crossbar.
    top = (c, size * 0.17)
    left = (size * 0.24, size * 0.80)
    right = (size * 0.76, size * 0.80)
    d.line([left, top, right], fill=255, width=w, joint="curve")
    bar_y = size * 0.58
    d.line([(size * 0.37, bar_y), (size * 0.63, bar_y)], fill=255, width=int(w * 0.8))
    # Round caps.
    for p in (left, right, top):
        d.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=255)
    # A small "star" where the orbit crosses the right leg.
    sx, sy = size * 0.70, size * 0.46
    d.ellipse((sx - w * 0.6, sy - w * 0.6, sx + w * 0.6, sy + w * 0.6), fill=255)

    colour = gradient(size, (80, 220, 255, 255), (170, 110, 255, 255), 35)
    glow = strokes.filter(ImageFilter.GaussianBlur(size * 0.03))
    glow_layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    glow_layer.paste(colour, (0, 0), glow.point(lambda v: int(v * 0.55)))
    stroke_layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    stroke_layer.paste(colour, (0, 0), strokes)
    out = Image.alpha_composite(base, glow_layer)
    out = Image.alpha_composite(out, stroke_layer)
    return out


def main():
    master = logo()
    master.resize((256, 256), Image.LANCZOS).save("resources/aether.png")
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    master.save("resources/aether.ico", sizes=sizes)
    # Splash: 640x360, logo centred on a dark panel (title and progress are drawn by the app).
    splash = gradient(640, (14, 18, 34, 255), (6, 8, 18, 255), 70).crop((0, 0, 640, 360))
    mark = logo(1024, tile=False).resize((220, 220), Image.LANCZOS)
    splash.alpha_composite(mark, ((640 - 220) // 2, (360 - 220) // 2))
    splash.save("resources/splash.png")
    print("wrote resources/aether.png, resources/aether.ico, resources/splash.png")


if __name__ == "__main__":
    main()
