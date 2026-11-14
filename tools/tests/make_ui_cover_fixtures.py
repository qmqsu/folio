#!/usr/bin/env python3
"""Regenerate original, test-only raster covers; never part of the firmware UI.

Requires Pillow. The font is used only for rendering test images; no font file
is copied. Existing checked-in PNG/JPEG fixtures keep normal UI tests portable.
"""
from pathlib import Path
import argparse
from PIL import Image, ImageDraw, ImageFont


def create_cover(destination: Path, kind: str, title: str, font_path: str, face: int) -> None:
    scale = 3
    image = Image.new("RGB", (256 * scale, 384 * scale), (246, 242, 230))
    draw = ImageDraw.Draw(image)

    def rect(box, fill, outline=None, width=1):
        draw.rectangle(tuple(round(v * scale) for v in box), fill=fill, outline=outline, width=width * scale)

    def line(points, fill, width=1):
        draw.line([(round(x * scale), round(y * scale)) for x, y in points], fill=fill, width=width * scale)

    def polygon(points, fill):
        draw.polygon([(round(x * scale), round(y * scale)) for x, y in points], fill=fill)

    def text(center, y, value, size, fill):
        font = ImageFont.truetype(font_path, size * scale, index=face)
        left, top, right, bottom = draw.textbbox((0, 0), value, font=font)
        draw.text((center * scale - (right - left) / 2, y * scale - top), value, font=font, fill=fill)

    if kind == "light":
        rect((0, 0, 255, 383), (231, 224, 200))
        rect((28, 154, 228, 329), (70, 91, 86))
        draw.ellipse((106 * scale, 172 * scale, 150 * scale, 216 * scale), fill=(247, 242, 214))
        polygon(((28, 280), (86, 229), (124, 266), (174, 235), (228, 293), (228, 329), (28, 329)), (25, 43, 42))
        line(((29, 301), (227, 301)), (198, 204, 178))
        text(128, 38, title, 31, (28, 43, 42))
        text(128, 91, "QUIET LIGHT", 12, (59, 72, 66))
    elif kind == "letter":
        rect((0, 0, 255, 383), (244, 237, 212))
        rect((0, 0, 16, 383), (45, 62, 73))
        text(135, 38, title, 31, (35, 52, 64))
        text(135, 91, "LETTERS FROM THE SOUTH", 9, (70, 83, 88))
        rect((45, 168, 226, 295), (229, 219, 184), (56, 70, 80), 2)
        line(((45, 168), (135, 235), (226, 168)), (56, 70, 80), 2)
        line(((45, 295), (112, 222)), (56, 70, 80), 2)
        line(((226, 295), (158, 222)), (56, 70, 80), 2)
        rect((180, 180, 211, 207), (56, 70, 80))
        line(((73, 318), (200, 318)), (56, 70, 80))
    else:
        rect((0, 0, 255, 383), (35, 54, 64))
        text(128, 38, title, 31, (245, 243, 222))
        text(128, 91, "NOTES FROM THE HILLS", 10, (217, 226, 214))
        polygon(((0, 261), (68, 150), (141, 263), (196, 182), (255, 259), (255, 342), (0, 342)), (198, 206, 192))
        polygon(((0, 307), (73, 240), (133, 303), (190, 233), (255, 310), (255, 342), (0, 342)), (120, 144, 139))
        polygon(((0, 341), (60, 289), (127, 334), (191, 282), (255, 339), (255, 383), (0, 383)), (74, 96, 100))
    text(128, 354, "FOLIO  /  UI TEST", 9, (220, 226, 218) if kind == "mountain" else (65, 76, 72))
    image = image.resize((256, 384), Image.Resampling.LANCZOS)
    if destination.suffix == ".jpg":
        image.save(destination, quality=92, optimize=True, progressive=False)
    else:
        image.save(destination, optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", required=True, help="Local font with Chinese glyphs; not copied")
    parser.add_argument("--face", type=int, default=0)
    args = parser.parse_args()
    output = Path(__file__).parent / "fixtures"
    output.mkdir(exist_ok=True)
    for filename, kind, title in (
        ("ui-cover-light.png", "light", "一页微光"),
        ("ui-cover-letter.jpg", "letter", "南方来信"),
        ("ui-cover-mountain.png", "mountain", "山间日记"),
    ):
        create_cover(output / filename, kind, title, args.font, args.face)
        print(output / filename)
