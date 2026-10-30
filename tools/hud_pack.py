#!/usr/bin/env python3
"""Pack the gothic HUD art (assets/hud) into hud.pak for src/hud.c.

    python3 -I tools/hud_pack.py assets/hud build/hud.pak

Standard library only (the PNGs are decoded here so the game needs no image
library). Layout, little endian:

    "N3DHUD1\\n", u32 count, then per image:
    char name[24], u16 w, u16 h, i16 aux[4], w*h u32 premultiplied ARGB, row-major

aux: portrait-frame = the opening (x0, y0, x1, y1, exclusive end), followed
by "portrait-mask" (same size, opaque where the portrait shows);
glyph "g<code>" = (0, 0, xadvance, code), cropped to its ink (the kit's
boxes are padded cells, so src/hud.c sets glyphs on the baseline by their
ink); "font" (0x0) = (lineHeight, base, 0, 0). Font glyphs are stored at
FONT_SCALE of the atlas.
"""
import os
import struct
import sys
import zlib

SPRITES = [
    "portrait-frame", "health-frame", "health-fill",
    "ammo-ornament-left", "ammo-ornament-right",
    "ammo-bullet", "ammo-silver-bullet", "ammo-silver-bullets", "spell-book",
    "key-gold", "key-silver", "key-red", "key-green", "keycard-red", "keycard-yellow",
]
FONT_SCALE = 2  # atlas glyphs are ~150 px; half is plenty up to 4K


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, plte, trns = 8, [], None, None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            plte = body
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise SystemExit(f"{path}: only 8-bit non-interlaced PNGs are supported")
    bpp = {6: 4, 2: 3, 3: 1, 4: 2, 0: 1}[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        if ctype == 6:
            rgba[i * 4:i * 4 + 4] = out[i * 4:i * 4 + 4]
        elif ctype == 2:
            rgba[i * 4:i * 4 + 4] = out[i * 3:i * 3 + 3] + b"\xff"
        elif ctype == 3:
            k = out[i]
            a = trns[k] if trns and k < len(trns) else 255
            rgba[i * 4:i * 4 + 4] = plte[k * 3:k * 3 + 3] + bytes([a])
        elif ctype == 4:
            g, a = out[i * 2], out[i * 2 + 1]
            rgba[i * 4:i * 4 + 4] = bytes([g, g, g, a])
        else:
            g = out[i]
            rgba[i * 4:i * 4 + 4] = bytes([g, g, g, 255])
    return w, h, rgba


def premultiply(w, h, rgba):
    px = bytearray(w * h * 4)
    for i in range(w * h):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        struct.pack_into("<I", px, i * 4, a << 24 | (r * a // 255) << 16 | (g * a // 255) << 8 | (b * a // 255))
    return px


def crop_downscale(w, rgba, x, y, cw, ch, k):
    """Box-filter a crop of an RGBA image by k (premultiplied averaging)."""
    ow, oh = (cw + k - 1) // k, (ch + k - 1) // k
    out = bytearray(ow * oh * 4)
    for oy in range(oh):
        for ox in range(ow):
            s = [0, 0, 0, 0]
            for dy in range(k):
                for dx in range(k):
                    sx, sy = ox * k + dx, oy * k + dy
                    if sx >= cw or sy >= ch:
                        continue
                    i = ((y + sy) * w + x + sx) * 4
                    a = rgba[i + 3]
                    s[0] += rgba[i] * a
                    s[1] += rgba[i + 1] * a
                    s[2] += rgba[i + 2] * a
                    s[3] += a
            n = k * k
            a = s[3] // n
            col = [(c // s[3]) if s[3] else 0 for c in s[:3]]
            out[(oy * ow + ox) * 4:(oy * ow + ox) * 4 + 4] = bytes(col + [a])
    return ow, oh, out


def flood(w, h, rgba):
    seen = bytearray(w * h)
    start = (h // 2) * w + w // 2
    stack = [start]
    seen[start] = 1
    while stack:
        i = stack.pop()
        x, y = i % w, i // w
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                j = ny * w + nx
                if not seen[j] and rgba[j * 4 + 3] < 128:
                    seen[j] = 1
                    stack.append(j)
    return seen


def opening_mask(w, h, rgba, grow=8):
    """The portrait's opening, grown under the frame so no gap shows (opaque white)."""
    m = flood(w, h, rgba)
    for _ in range(grow):
        g = bytearray(m)
        for i in range(w * h):
            if not m[i] and rgba[i * 4 + 3] >= 128:
                x, y = i % w, i // w
                if (x > 0 and m[i - 1]) or (x < w - 1 and m[i + 1]) or (y > 0 and m[i - w]) or (y < h - 1 and m[i + w]):
                    g[i] = 1
        m = g
    px = bytearray(w * h * 4)
    for i in range(w * h):
        if m[i]:
            struct.pack_into("<I", px, i * 4, 0xffffffff)
    return px


def opening(w, h, rgba):
    """Bounding box of the transparent region around the frame's centre."""
    seen = bytearray(w * h)
    start = (h // 2) * w + w // 2
    stack = [start]
    seen[start] = 1
    x0, y0, x1, y1 = w, h, 0, 0
    while stack:
        i = stack.pop()
        x, y = i % w, i // w
        x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x + 1), max(y1, y + 1)
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < w and 0 <= ny < h:
                j = ny * w + nx
                if not seen[j] and rgba[j * 4 + 3] < 128:
                    seen[j] = 1
                    stack.append(j)
    return x0, y0, x1, y1


def ink(w, rgba, x, y, cw, ch, threshold=24):
    """Tight box (x, y, w, h) of the pixels in a cell that are not (nearly) transparent."""
    xs = [x + i for i in range(cw) if any(rgba[((y + j) * w + x + i) * 4 + 3] > threshold for j in range(ch))]
    ys = [y + j for j in range(ch) if any(rgba[((y + j) * w + x + i) * 4 + 3] > threshold for i in range(cw))]
    if not xs or not ys:
        return None
    xs, ys = longest_run(xs), longest_run(ys)  # drop specks of the neighbouring cells
    return xs[0], ys[0], xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1


def longest_run(v, gap=3):
    runs, cur = [], [v[0]]
    for a in v[1:]:
        if a - cur[-1] > gap:
            runs.append(cur)
            cur = []
        cur.append(a)
    runs.append(cur)
    return max(runs, key=lambda r: r[-1] - r[0])


def entry(name, w, h, aux, px):
    return struct.pack("<24sHH4h", name.encode(), w, h, *aux) + bytes(px)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    out = []
    for name in SPRITES:
        w, h, rgba = read_png(os.path.join(src, name + ".png"))
        aux = opening(w, h, rgba) if name == "portrait-frame" else (0, 0, 0, 0)
        out.append(entry(name, w, h, aux, premultiply(w, h, rgba)))
        if name == "portrait-frame":
            out.append(entry("portrait-mask", w, h, aux, opening_mask(w, h, rgba)))
    fw, fh, frgba = read_png(os.path.join(src, "gothic-font.png"))
    line_h = base = 0
    for line in open(os.path.join(src, "gothic-font.fnt")):
        f = line.split()
        if not f:
            continue
        kv = dict(p.split("=", 1) for p in f[1:] if "=" in p)
        if f[0] == "common":
            line_h, base = int(kv["lineHeight"]), int(kv["base"])
        elif f[0] == "char":
            code, x, y, cw, ch = (int(kv[k]) for k in ("id", "x", "y", "width", "height"))
            xo, yo, adv = (int(kv[k]) for k in ("xoffset", "yoffset", "xadvance"))
            # the descriptor's boxes are padded atlas cells: crop to the ink
            box = ink(fw, frgba, x, y, cw, ch) if cw and ch else None
            if box:
                gw, gh, g = crop_downscale(fw, frgba, box[0], box[1], box[2], box[3], FONT_SCALE)
            else:
                gw = gh = 0
                g = b""
            out.append(entry(f"g{code}", gw, gh, (0, 0, adv // FONT_SCALE, code), premultiply(gw, gh, g)))
    out.append(entry("font", 0, 0, (line_h // FONT_SCALE, base // FONT_SCALE, 0, 0), b""))
    with open(dst, "wb") as f:
        f.write(b"N3DHUD1\n" + struct.pack("<I", len(out)))
        for e in out:
            f.write(e)
    print(f"{dst}: {len(out)} images, {os.path.getsize(dst) // 1024} KiB")


if __name__ == "__main__":
    main()
