"""Writes the Deki app icon: the cat peeking over its ledge, as in the editor's
title bar and the website's nav, at 16 to 256 px by whole pixels so it stays
sharp.

    python tools/make-app-icon.py resources/windows/deki.ico [more.ico ...]

The pixel map and colours are the editor title bar's (kCat in
src/EditorApp.MenuBar.cpp), placed in a square as the website's first favicon
was. Needs Pillow.
"""

import sys

from PIL import Image

CAT = [
    "................",
    "................",
    "................",
    "...A........A...",
    "...AA......AA...",
    "...AAAAAAAAAA...",
    "..AAAAAAAAAAAA..",
    "..AAKSAAAAKSAA..",
    "..AAKKAAAAKKAA..",
    "..AAAAAPPAAAAA..",
    "..AWWAAAAAAWWA..",
    "BBBBBBBBBBBBBBBB",
    "................",
    "................",
    "................",
    "................",
]
COL = {
    "A": (0x3B, 0xC3, 0xFF),  # fur, the accent
    "W": (0x9D, 0xE1, 0xFF),  # paws
    "B": (0x0C, 0x82, 0xBF),  # ledge
    "S": (0xE8, 0xE6, 0xE1),  # shine in the eyes
    "K": (0x0A, 0x0B, 0x0D),  # eyes
    "P": (0xFF, 0x8F, 0xB8),  # mouth
}
SIZES = [16, 32, 48, 64, 128, 256]


def cat():
    image = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for y, row in enumerate(CAT):
        for x, c in enumerate(row):
            if c in COL:
                image.putpixel((x, y), COL[c] + (255,))
    return image


def main(paths):
    if not paths:
        sys.exit(__doc__)
    # Every size scaled from the 16 px cat by whole pixels; Pillow would
    # otherwise shrink the largest one with a smoothing filter.
    base = cat()
    images = [base.resize((s, s), Image.NEAREST) for s in SIZES]
    for path in paths:
        images[-1].save(path, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
        print("wrote", path)


if __name__ == "__main__":
    main(sys.argv[1:])
