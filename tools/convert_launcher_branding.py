#!/usr/bin/env python3
"""Convert rendered, original branding SVGs to the launcher's image formats."""
import argparse
from pathlib import Path

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cover_png", type=Path)
    parser.add_argument("icon_png", type=Path)
    parser.add_argument("--viewport-capture", action="store_true",
                        help="Trim extra browser viewport around top-left, natural-size SVGs")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    with Image.open(args.cover_png) as cover, Image.open(args.icon_png) as icon:
        if args.viewport_capture:
            if cover.width < 640 or cover.height < 480 or icon.width < 256 or icon.height < 256:
                raise ValueError("Browser viewport is smaller than the artwork")
            cover = cover.crop((0, 0, 640, 480))
            icon = icon.crop((0, 0, 256, 256))
        if cover.size != (640, 480) or icon.size != (256, 256):
            raise ValueError("Render the cover at 640x480 and the icon at 256x256")
        branding = args.root / "assets" / "branding"
        images = args.root / "assets" / "img"
        branding.mkdir(parents=True, exist_ok=True)
        images.mkdir(parents=True, exist_ok=True)
        cover.convert("RGB").save(images / "boxart_stadium2.tga")
        icon.convert("RGB").save(branding / "stadium2-icon.bmp")
        icon.convert("RGBA").save(branding / "stadium2-icon.ico",
                                  sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
        icon.convert("RGBA").save(images / "brand_mark.tga")


if __name__ == "__main__":
    main()
