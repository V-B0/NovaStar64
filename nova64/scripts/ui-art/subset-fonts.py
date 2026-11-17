#!/usr/bin/env python3
"""Build the launcher's fonts: Inter (text) and Phosphor (icons), cut down to the glyphs it uses.

The full fonts are about 2.5 MB; the subsets are a small fraction of that, which is what gets
compiled into the program. Needs fontTools (`pip install fonttools`).

    subset-fonts.py <inter-folder> <phosphor-folder>

<inter-folder> holds Inter-Regular.ttf, Inter-SemiBold.ttf and Inter-Bold.ttf (from the Inter 4.1 release,
extras/ttf/); <phosphor-folder> holds Phosphor.ttf, Phosphor-Fill.ttf and style.css for the regular and fill
styles (from @phosphor-icons/web 2.1.1, src/regular/ and src/fill/, saved as regular-style.css and
fill-style.css). The output goes to assets/fonts/. To use another icon, add its Phosphor name to ICONS
here, add it to `Icon` and IconCodepoint() in UI/UiKit.*, and run this again.
"""
import os
import re
import sys

from fontTools import subset

ICONS = [
    "house", "game-controller", "clock-counter-clockwise", "star", "gear", "info", "play", "heart", "dots-three",
    "magnifying-glass", "folder-open", "plus", "trash", "pencil-simple", "x", "minus", "square", "copy", "code",
    "cpu", "trophy", "share-network", "sliders-horizontal", "monitor", "speaker-high", "keyboard", "rewind",
    "stop", "pause", "check", "warning", "image", "arrow-right", "arrow-left", "caret-right", "caret-down",
    "calendar-blank", "buildings", "lightning", "list", "tag", "flask", "terminal-window", "hard-drives",
    "fast-forward", "arrows-out-simple", "arrows-in-simple", "play-circle", "download-simple", "link",
]

# Everything the launcher prints: ASCII, Latin-1 letters, and a few typographic marks.
TEXT = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) +
        [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2192, 0x2190, 0x00D7, 0x00B7, 0x2212])


def codepoints(css_path, prefix):
    css = open(css_path).read()
    out = {}
    for name in ICONS:
        m = re.search(r"\." + re.escape(prefix) + r"\.ph(?:-fill)?-" + re.escape(name) + r":before\s*\{\s*content:\s*\"\\([0-9a-f]+)\"", css)
        if not m:
            m = re.search(r"\.ph(?:-fill)?-" + re.escape(name) + r":before\s*\{\s*content:\s*\"\\([0-9a-f]+)\"", css)
        if not m:
            sys.exit(f"icon not found in {css_path}: {name}")
        out[name] = int(m.group(1), 16)
    return out


def cut(src, dst, unicodes):
    opts = subset.Options()
    opts.layout_features = ["kern", "liga", "calt"]
    opts.name_IDs = [1, 2, 3, 4, 6]
    opts.notdef_outline = True
    opts.hinting = False
    opts.drop_tables += ["DSIG"]
    font = subset.load_font(src, opts)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=unicodes)
    sub.subset(font)
    subset.save_font(font, dst, opts)
    print(f"{os.path.basename(dst)}: {os.path.getsize(src) // 1024} KB -> {os.path.getsize(dst) // 1024} KB")


def main():
    inter, phosphor = sys.argv[1], sys.argv[2]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "fonts")
    os.makedirs(out, exist_ok=True)
    for w in ("Regular", "SemiBold", "Bold"):
        cut(os.path.join(inter, f"Inter-{w}.ttf"), os.path.join(out, f"Inter-{w}.ttf"), TEXT)
    fill = codepoints(os.path.join(phosphor, "fill-style.css"), "ph-fill")
    regular = codepoints(os.path.join(phosphor, "regular-style.css"), "ph")
    cut(os.path.join(phosphor, "fill-Phosphor-Fill.ttf"), os.path.join(out, "Phosphor-Fill.ttf"), sorted(fill.values()))
    cut(os.path.join(phosphor, "regular-Phosphor.ttf"), os.path.join(out, "Phosphor.ttf"), sorted(regular.values()))
    # The codepoints, for the code that draws them: both styles use the same ones.
    with open(os.path.join(out, "phosphor-codepoints.txt"), "w") as f:
        f.write("# name codepoint (hex), same in the fill and regular styles\n")
        for name in ICONS:
            if fill[name] != regular[name]:
                sys.exit(f"{name} differs between styles")
            f.write(f"{name} {fill[name]:04x}\n")


main()
