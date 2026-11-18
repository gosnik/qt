#!/usr/bin/env python3
"""Regenerate the qt application icon set from one geometric description.

Writes, next to this script:
  qt.svg                 scalable source (installed as hicolor/scalable/apps/qt.svg)
  png/qt-<N>.png         N in 16, 32, 48, 64, 128, 256, 512, 1024
  qt.ico                 Windows icon (16..256)

The icon is a rounded tile with a "q" (ring plus descender) and an amber
block cursor. Requires Pillow (`python3 -m pip install pillow`). Output is
deterministic for a given Pillow version.
"""

from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
CANVAS = 1024
SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
SUPERSAMPLE = 2048  # draw once large, then downscale with Lanczos

BACKGROUND = "#1F2A44"
RING = "#7DD3FC"
CURSOR = "#FBBF24"

# All coordinates are on a 1024 x 1024 canvas.
TILE = (64, 64, 960, 960, 200)  # x0, y0, x1, y1, corner radius
RING_CENTER = (460, 450)
RING_OUTER = 250
RING_INNER = 160
DESCENDER = (620, 450, 710, 840, 45)  # x0, y0, x1, y1, corner radius
CURSOR_BOX = (770, 720, 870, 840, 18)


def svg() -> str:
    tx0, ty0, tx1, ty1, tr = TILE
    cx, cy = RING_CENTER
    dx0, dy0, dx1, dy1, dr = DESCENDER
    kx0, ky0, kx1, ky1, kr = CURSOR_BOX
    stroke = RING_OUTER - RING_INNER
    mid = (RING_OUTER + RING_INNER) // 2
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
        'viewBox="0 0 1024 1024">\n'
        "  <title>qt</title>\n"
        f'  <rect x="{tx0}" y="{ty0}" width="{tx1 - tx0}" height="{ty1 - ty0}" '
        f'rx="{tr}" fill="{BACKGROUND}"/>\n'
        f'  <circle cx="{cx}" cy="{cy}" r="{mid}" fill="none" stroke="{RING}" '
        f'stroke-width="{stroke}"/>\n'
        f'  <rect x="{dx0}" y="{dy0}" width="{dx1 - dx0}" height="{dy1 - dy0}" '
        f'rx="{dr}" fill="{RING}"/>\n'
        f'  <rect x="{kx0}" y="{ky0}" width="{kx1 - kx0}" height="{ky1 - ky0}" '
        f'rx="{kr}" fill="{CURSOR}"/>\n'
        "</svg>\n"
    )


def raster() -> Image.Image:
    scale = SUPERSAMPLE / CANVAS

    def s(*values):
        return [round(v * scale) for v in values]

    image = Image.new("RGBA", (SUPERSAMPLE, SUPERSAMPLE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    tx0, ty0, tx1, ty1, tr = TILE
    draw.rounded_rectangle(s(tx0, ty0, tx1, ty1), radius=s(tr)[0], fill=BACKGROUND)
    cx, cy = RING_CENTER
    draw.ellipse(
        s(cx - RING_OUTER, cy - RING_OUTER, cx + RING_OUTER, cy + RING_OUTER), fill=RING
    )
    draw.ellipse(
        s(cx - RING_INNER, cy - RING_INNER, cx + RING_INNER, cy + RING_INNER),
        fill=BACKGROUND,
    )
    dx0, dy0, dx1, dy1, dr = DESCENDER
    draw.rounded_rectangle(s(dx0, dy0, dx1, dy1), radius=s(dr)[0], fill=RING)
    kx0, ky0, kx1, ky1, kr = CURSOR_BOX
    draw.rounded_rectangle(s(kx0, ky0, kx1, ky1), radius=s(kr)[0], fill=CURSOR)
    return image


def main() -> None:
    (HERE / "qt.svg").write_text(svg(), encoding="utf-8")
    master = raster()
    png_dir = HERE / "png"
    png_dir.mkdir(exist_ok=True)
    for size in SIZES:
        master.resize((size, size), Image.LANCZOS).save(
            png_dir / f"qt-{size}.png", optimize=True
        )
    master.resize((256, 256), Image.LANCZOS).save(
        HERE / "qt.ico", sizes=[(n, n) for n in ICO_SIZES]
    )


if __name__ == "__main__":
    main()
