#!/usr/bin/env python3
"""Draw the launcher's background: assets/ui/background.jpg. Needs Pillow and numpy.

A deep indigo gradient with a few quiet glows, a faint large star (the NovaStar mark) behind the top of
the window and a faint gamepad silhouette at the bottom left. Everything is deliberately quiet so that
text and cover art stay in front.
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 1920, 1080
rng = np.random.default_rng(64)


def gradient():
    y = np.linspace(0, 1, H)[:, None, None]
    top = np.array([17, 16, 40], float)
    bottom = np.array([8, 7, 24], float)
    return np.broadcast_to(top * (1 - y) + bottom * y, (H, W, 3)).copy()


def glow(img, cx, cy, radius, color, strength):
    yy, xx = np.mgrid[0:H, 0:W]
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / radius
    a = np.clip(1 - d, 0, 1) ** 2 * strength
    img += a[..., None] * (np.array(color, float) - img) * 0.9
    return img


def shape_layer(draw_fn, blur):
    layer = Image.new("L", (W, H), 0)
    draw_fn(ImageDraw.Draw(layer))
    return np.asarray(layer.filter(ImageFilter.GaussianBlur(blur)), float) / 255.0


def star(d):
    cx, cy, r_out, r_in = 1130, 330, 360, 150
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = r_out if i % 2 == 0 else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=255)
    d.ellipse([cx - 70, cy - 70, cx + 70, cy + 70], fill=0)


def gamepad(d):
    # A generic two-grip pad: a wide body, two grips, and a cut-out for the d-pad and buttons.
    cx, cy = 260, 850
    d.rounded_rectangle([cx - 190, cy - 90, cx + 190, cy + 50], radius=70, fill=255)
    d.ellipse([cx - 230, cy - 10, cx - 70, cy + 190], fill=255)
    d.ellipse([cx + 70, cy - 10, cx + 230, cy + 190], fill=255)
    d.rectangle([cx - 135, cy - 50, cx - 85, cy - 38], fill=0)
    d.rectangle([cx - 116, cy - 69, cx - 104, cy - 19], fill=0)
    for dx, dy in ((100, -60), (140, -35), (100, -10), (60, -35)):
        d.ellipse([cx + dx - 10, cy + dy - 10, cx + dx + 10, cy + dy + 10], fill=0)


def main():
    img = gradient()
    img = glow(img, 1650, 120, 900, (58, 52, 140), 0.30)
    img = glow(img, 250, 700, 800, (34, 30, 96), 0.18)
    img = glow(img, 1000, 1150, 900, (26, 22, 70), 0.16)
    for layer, strength, tint in ((shape_layer(star, 6), 0.045, (140, 128, 240)), (shape_layer(gamepad, 3), 0.06, (120, 110, 220))):
        img += layer[..., None] * strength * (np.array(tint, float) - img) * 1.6
    yy, xx = np.mgrid[0:H, 0:W]
    vignette = 1 - 0.35 * (((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2) / 2
    img *= vignette[..., None]
    img += rng.normal(0, 1.1, img.shape)         # a little noise so the gradient does not band
    out = Image.fromarray(np.clip(img, 0, 255).astype("uint8"))
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "ui", "background.jpg")
    out.save(path, quality=88, optimize=True, progressive=True)
    print(path, os.path.getsize(path) // 1024, "KB")


main()
