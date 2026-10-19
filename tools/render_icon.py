#!/usr/bin/env python3
"""Regenerates the application icon files from resources/icons/music_sample_organizer.svg.

Outputs (committed, so building needs no SVG renderer):
  resources/icons/png/music_sample_organizer_<size>.png   window icon sizes for Qt (qt_add_resources)
  resources/icons/music_sample_organizer.ico              Windows executable icon (PNG-compressed entries)

Needs `rsvg-convert` (librsvg) on PATH: MSYS2 `mingw-w64-clang-x86_64-librsvg`, Debian/Ubuntu
`librsvg2-bin`. Usage: python tools/render_icon.py
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
ICON_DIRECTORY = REPOSITORY / "resources" / "icons"
SOURCE_SVG = ICON_DIRECTORY / "music_sample_organizer.svg"
PNG_DIRECTORY = ICON_DIRECTORY / "png"
ICO_FILE = ICON_DIRECTORY / "music_sample_organizer.ico"

# Sizes Qt picks from for title bars, the taskbar and Alt+Tab.
WINDOW_ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)
# Sizes Windows Explorer and the shell request from an executable's icon resource.
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)


def render_png(size: int) -> bytes:
    """Renders the SVG at size x size pixels and returns the PNG bytes."""
    result = subprocess.run(
        ["rsvg-convert", "--width", str(size), "--height", str(size), str(SOURCE_SVG)],
        check=True,
        capture_output=True,
    )
    return result.stdout


def build_ico(images: dict[int, bytes]) -> bytes:
    """Packs PNG images into an ICO container (ICONDIR, one ICONDIRENTRY per image, then the data).

    PNG-compressed entries are supported by Windows Vista and later. A width or height of 256 is
    stored as 0, as the format requires.
    """
    count = len(images)
    header = struct.pack("<HHH", 0, 1, count)  # reserved, type 1 = icon, image count
    directory = b""
    data = b""
    offset = 6 + 16 * count
    for size, png in sorted(images.items()):
        dimension = 0 if size >= 256 else size
        # width, height, palette colours, reserved, colour planes, bits per pixel, size, offset
        directory += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(png), offset)
        data += png
        offset += len(png)
    return header + directory + data


def main() -> int:
    if shutil.which("rsvg-convert") is None:
        print("rsvg-convert not found; install librsvg (see the module docstring)", file=sys.stderr)
        return 1
    PNG_DIRECTORY.mkdir(parents=True, exist_ok=True)
    rendered: dict[int, bytes] = {}
    for size in sorted(set(WINDOW_ICON_SIZES) | set(ICO_SIZES)):
        rendered[size] = render_png(size)
    for size in WINDOW_ICON_SIZES:
        (PNG_DIRECTORY / f"music_sample_organizer_{size}.png").write_bytes(rendered[size])
    ICO_FILE.write_bytes(build_ico({size: rendered[size] for size in ICO_SIZES}))
    print(f"wrote {len(WINDOW_ICON_SIZES)} PNGs to {PNG_DIRECTORY.relative_to(REPOSITORY)} and "
          f"{ICO_FILE.relative_to(REPOSITORY)} ({len(ICO_SIZES)} sizes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
