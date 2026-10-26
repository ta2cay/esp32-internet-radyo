# tools/fonts/ içindeki açık lisanslı (SIL OFL) TTF fontlardan TFT_eSPI "smooth font" (VLW)
# dizileri üretir → src/fonts.h
# Dahili TFT fontlarında Türkçe harf yok; bunlarda var ve kenarları yumuşatılmış.
# Noto Sans değişken fonttur: (kalınlık, genişlik) eksenleri verilir; genişlik %93-95 metni biraz daraltır.
# Çalıştır: python tools/make_fonts.py   (Pillow gerekir)
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = Path(__file__).resolve().parent / "fonts"
TEXT = ("".join(chr(c) for c in range(0x21, 0x7F))
        + "ÇçĞğİıÖöŞşÜüÂâÎîÛûÉéÁáÀàÈèÄäËëÍíÑñÓóÚú°·–—‘’“”…")
DIGITS = "0123456789-+.:%°"

FONTS = [  # (dizi adı, dosya, piksel boyu, karakterler, değişken font eksenleri [kalınlık, genişlik])
    ("F_SMALL", "NotoSans-VF.ttf",                  15, TEXT, [400, 95]),
    ("F_BOLD",  "NotoSans-VF.ttf",                  15, TEXT, [700, 93]),
    ("F_MED",   "NotoSans-VF.ttf",                  21, TEXT, [700, 93]),
    ("F_NAME",  "NotoSans-VF.ttf",                  28, TEXT, [700, 93]),
    ("F_BIG",   "BarlowSemiCondensed-SemiBold.ttf", 47, DIGITS, None),
]


def vlw(path, px, chars, variation):
    font = ImageFont.truetype(str(FONT_DIR / path), px)
    if variation:
        font.set_variation_by_axes(variation)
    glyphs = []
    for ch in chars:
        size = px * 3
        img = Image.new("L", (size, size), 0)
        ox, base = px // 2, px * 2  # kalem başı ve taban çizgisi
        ImageDraw.Draw(img).text((ox, base), ch, font=font, fill=255, anchor="ls")
        bbox = img.getbbox()
        adv = round(font.getlength(ch))
        if not bbox:
            continue
        l, t, r, b = bbox
        glyphs.append((ord(ch), b - t, r - l, adv, base - t, l - ox, img.crop(bbox).tobytes()))
    ascent = max(g[4] for g in glyphs)
    descent = max(g[1] - g[4] for g in glyphs)
    out = struct.pack(">6i", len(glyphs), 11, px, 0, ascent, max(descent, 0))
    for code, h, w, adv, dy, dx, _ in glyphs:
        out += struct.pack(">7i", code, h, w, adv, dy, dx, 0)
    for g in glyphs:
        out += g[6]
    name = path.encode()
    out += bytes([len(name)]) + name + b"\0" + bytes([len(name)]) + name + b"\0" + b"\1"
    return out


def main():
    root = Path(__file__).resolve().parent.parent
    lines = ["// tools/make_fonts.py tarafından üretildi, elle düzenleme.", "#pragma once", "#include <Arduino.h>", ""]
    for name, path, px, chars, var in FONTS:
        data = vlw(path, px, chars, var)
        lines.append(f"// {path} {px} px, {len(data)} bayt")
        lines.append(f"const uint8_t {name}[] PROGMEM = {{")
        for i in range(0, len(data), 24):
            lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append("")
        print(name, len(data))
    (root / "src/fonts.h").write_text("\n".join(lines), encoding="utf-8")


main()
