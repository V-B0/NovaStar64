#!/usr/bin/env python3
"""Build a Windows .ico from the NovaStar PNG.

    python3 scripts/make-icon-ico.py [source.png] [output.ico]

Defaults to assets/icon/Novastar.png -> assets/icon/Novastar.ico. The .ico holds
the sizes Windows asks for (16 to 256 px), each resampled straight from the
source and stored PNG-compressed. Needs Pillow (`pip install pillow`).
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image

SIZES = (16, 24, 32, 48, 64, 128, 256)


def main() -> None:
    root = Path(__file__).resolve().parent.parent / "assets" / "icon"
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "Novastar.png"
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "Novastar.ico"

    image = Image.open(src).convert("RGBA")
    if image.width != image.height:
        sys.exit(f"{src} is {image.width}x{image.height}; an icon must be square")

    frames = []
    for size in SIZES:
        frame = image if size == image.width else image.resize((size, size), Image.LANCZOS)
        buf = io.BytesIO()
        frame.save(buf, format="PNG", optimize=True)
        frames.append((size, buf.getvalue()))

    header = struct.pack("<HHH", 0, 1, len(frames))
    offset = len(header) + 16 * len(frames)
    entries, payload = b"", b""
    for size, data in frames:
        # Width/height 0 means 256.
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        payload += data
        offset += len(data)
    dst.write_bytes(header + entries + payload)
    print(f"wrote {dst} ({dst.stat().st_size} bytes, sizes {', '.join(map(str, SIZES))})")


if __name__ == "__main__":
    main()
