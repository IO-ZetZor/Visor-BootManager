#!/usr/bin/env python3
import sys
from PIL import ImageFont, Image, ImageDraw

FIRST = 0x20
LAST  = 0x7E

def packbits(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:

        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 2:
            out.append((257 - run) & 0xFF)
            out.append(data[i])
            i += run
        else:

            start = i
            lit = 0
            while i < n and lit < 128 and not (i + 1 < n and data[i] == data[i + 1]):
                i += 1
                lit += 1
            out.append(lit - 1)
            out.extend(data[start:start + lit])
    return out

def main():
    ttf, px, prefix, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    font = ImageFont.truetype(ttf, px)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent

    glyphs = []
    for cp in range(FIRST, LAST + 1):
        ch = chr(cp)

        adv = int(round(font.getlength(ch) * 64))

        bbox = font.getbbox(ch)
        if bbox is None:
            glyphs.append((cp, 0, 0, 0, 0, adv, b""))
            continue
        l, t, r, b = bbox
        w, h = max(0, r - l), max(0, b - t)
        if w == 0 or h == 0:
            glyphs.append((cp, 0, 0, 0, 0, adv, b""))
            continue
        img = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(img)

        d.text((-l, -t), ch, font=font, fill=255)
        data = img.tobytes()

        top = ascent - t
        glyphs.append((cp, w, h, l, top, adv, data))

    with open(out, "w") as f:
        f.write('#include "font.h"\n\n')

        offsets = []
        off = 0
        blob = bytearray()
        for (cp, w, h, l, top, adv, data) in glyphs:
            offsets.append(off)
            blob += data
            off += len(data)
        packed = packbits(blob)

        f.write(f"static const unsigned char {prefix}_pixels[] = {{\n")
        for i in range(0, len(packed), 20):
            row = ",".join(str(b) for b in packed[i:i+20])
            f.write("  " + row + ",\n")
        if len(packed) == 0:
            f.write("  0\n")
        f.write("};\n\n")

        f.write(f"static const glyph_t {prefix}_glyphs[] = {{\n")
        for idx, (cp, w, h, l, top, adv, data) in enumerate(glyphs):
            f.write(f"  {{ {w}, {h}, {l}, {top}, {adv}, {offsets[idx]} }},\n")
        f.write("};\n\n")

        f.write(f"const font_t {prefix}_font = {{\n")
        f.write(f"  {px}, {ascent}, {descent}, {line_height}, {FIRST}, {LAST},\n")
        f.write(f"  {prefix}_glyphs, {prefix}_pixels, {len(packed)}, {len(blob)}\n")
        f.write("};\n")

    sys.stderr.write(
        f"baked {ttf} @ {px}px -> {out} "
        f"({len(blob)} bytes of glyphs, {len(packed)} packed, "
        f"{100*len(packed)//max(1,len(blob))}%)\n")

if __name__ == "__main__":
    main()
