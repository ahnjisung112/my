#!/usr/bin/env python3
"""Generates every bitmap asset WhichPC needs.

Icons are drawn separately for every pixel size (no downscaling of one big
master) so the 16/20/24 px tray icons stay crisp: straight edges are snapped
to the pixel grid and only corners are anti-aliased (8x supersampling).

Outputs (all under assets/):
  masks/<kind>_<size>.png   8-bit alpha masks used by the app at runtime
  masks.rc / masks.h        resource script + lookup table for the masks
  app.ico laptop.ico desktop.ico
  wizard.bmp header.bmp     NSIS installer artwork
  preview.png               contact sheet for eyeballing the design
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets")
MASKS = os.path.join(OUT, "masks")

K = 8  # supersampling factor

# Sizes the runtime can pick from without resampling.
MASK_SIZES = [16, 20, 24, 28, 30, 32, 36, 40, 48, 56, 64, 72, 80, 96, 128, 192, 256]
ICO_SIZES = [16, 20, 24, 28, 32, 40, 48, 56, 64, 96, 128, 256]

LAPTOP_RGB = (0x25, 0x63, 0xEB)   # blue
DESKTOP_RGB = (0xEA, 0x58, 0x0C)  # orange
DOT_RGB = (0x22, 0xC5, 0x5E)      # green "remote session" dot


# --------------------------------------------------------------------------
# primitive helpers (coordinates in target pixels, drawn K times larger)
# --------------------------------------------------------------------------
def canvas(s):
    return Image.new("L", (s * K, s * K), 0)


def rrect(d, x0, y0, x1, y1, r, fill=255):
    """Rounded rect covering pixel span [x0, x1) x [y0, y1)."""
    box = [round(x0 * K), round(y0 * K), round(x1 * K) - 1, round(y1 * K) - 1]
    rr = max(0, round(r * K))
    if rr <= 0:
        d.rectangle(box, fill=fill)
    else:
        d.rounded_rectangle(box, radius=rr, fill=fill)


def finish(img, s):
    return img.resize((s, s), Image.LANCZOS if s >= 96 else Image.BOX)


def even_fit(total, want):
    """Pick a span close to `want` that centres exactly inside `total`."""
    w = max(1, round(want))
    if (total - w) % 2:
        w += 1 if (w + 1) <= total else -1
    return w


# --------------------------------------------------------------------------
# shapes
# --------------------------------------------------------------------------
def tile_mask(s):
    img = canvas(s)
    d = ImageDraw.Draw(img)
    rrect(d, 0, 0, s, s, s * 0.22)
    return finish(img, s)


def stroke_for(s):
    if s <= 20:
        return 1
    if s <= 36:
        return 2
    return max(2, round(s / 15))


def laptop_mask(s):
    img = canvas(s)
    d = ImageDraw.Draw(img)
    st = stroke_for(s)
    sw = even_fit(s, s * 0.625)           # screen width
    sh = max(5, round(s * 0.44))          # screen height (incl. stroke)
    bw = even_fit(s, sw + 2 * max(1, round(s * 0.09)))  # base wider than screen
    bh = max(2, round(s * 0.13))
    gap = 0 if s < 40 else max(1, round(s * 0.02))
    total = sh + gap + bh
    top = (s - total) // 2
    sx = (s - sw) // 2
    bx = (s - bw) // 2
    cr = 0 if s < 24 else st * 0.9        # screen corner radius
    # screen frame + translucent glass
    rrect(d, sx, top, sx + sw, top + sh, cr, 255)
    rrect(d, sx + st, top + st, sx + sw - st, top + sh - st, max(0, cr - st), 0)
    rrect(d, sx + st, top + st, sx + sw - st, top + sh - st, max(0, cr - st), 85)
    # keyboard deck
    by = top + sh + gap
    br = 0 if s < 24 else bh * 0.45
    rrect(d, bx, by, bx + bw, by + bh, br, 255)
    if s >= 28:  # hinge notch
        nw = even_fit(s, s * 0.16)
        nh = max(1, round(bh * 0.4))
        rrect(d, (s - nw) // 2, by, (s - nw) // 2 + nw, by + nh, nh * 0.5, 0)
    return finish(img, s)


def desktop_mask(s):
    img = canvas(s)
    d = ImageDraw.Draw(img)
    st = stroke_for(s)
    mw = even_fit(s, s * 0.75)            # monitor width
    mh = max(6, round(s * 0.5))           # monitor height
    nw = even_fit(s, max(2, s * 0.13))    # neck width
    nh = max(2, round(s * 0.12))
    fw = even_fit(s, max(4, s * 0.38))    # foot width
    fh = max(1, st if s < 40 else round(st * 1.1))
    total = mh + nh + fh
    top = (s - total) // 2
    mx = (s - mw) // 2
    cr = 0 if s < 24 else st * 0.9
    rrect(d, mx, top, mx + mw, top + mh, cr, 255)
    rrect(d, mx + st, top + st, mx + mw - st, top + mh - st, max(0, cr - st), 0)
    rrect(d, mx + st, top + st, mx + mw - st, top + mh - st, max(0, cr - st), 85)
    ny = top + mh
    rrect(d, (s - nw) // 2, ny, (s - nw) // 2 + nw, ny + nh, 0, 255)
    fy = ny + nh
    rrect(d, (s - fw) // 2, fy, (s - fw) // 2 + fw, fy + fh, 0 if s < 24 else fh * 0.5, 255)
    return finish(img, s)


def dot_masks(s):
    """Status dot in the bottom-right corner plus the larger 'cut-out' ring."""
    r = max(2.5, s * 0.18)
    ring = max(1.0, s * 0.065)
    cx = cy = s - r - max(0.0, s * 0.01)
    out = []
    for rad in (r, r + ring):
        img = canvas(s)
        d = ImageDraw.Draw(img)
        d.ellipse([(cx - rad) * K, (cy - rad) * K, (cx + rad) * K, (cy + rad) * K], fill=255)
        out.append(img.resize((s, s), Image.LANCZOS))
    return out  # fill, cut


# --------------------------------------------------------------------------
# composition (mirrors the C++ runtime so previews match the real icons)
# --------------------------------------------------------------------------
def compose(s, glyph, rgb, remote=False):
    tile = tile_mask(s)
    g = glyph(s)
    base = Image.new("RGBA", (s, s), rgb + (0,))
    base.putalpha(tile)
    white = Image.new("RGBA", (s, s), (255, 255, 255, 0))
    white.putalpha(g)
    out = Image.alpha_composite(base, white)
    if remote:
        fill, cut = dot_masks(s)
        a = out.getchannel("A")
        a = Image.composite(Image.new("L", (s, s), 0), a, cut)
        out.putalpha(a)
        dot = Image.new("RGBA", (s, s), DOT_RGB + (0,))
        dot.putalpha(fill)
        out = Image.alpha_composite(out, dot)
    return out


def app_icon(s):
    """Two overlapping tiles: desktop (back, orange) and laptop (front, blue)."""
    if s <= 24:
        # Too small for two tiles: a single split tile.
        big = compose_split(s)
        return big
    sub = round(s * 0.70)
    back = compose(sub, desktop_mask, DESKTOP_RGB)
    front = compose(sub, laptop_mask, LAPTOP_RGB)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    img.alpha_composite(back, (0, 0))
    # cut-out halo around the front tile for separation
    halo_w = max(1, round(s * 0.035))
    halo = Image.new("L", (s * K, s * K), 0)
    hd = ImageDraw.Draw(halo)
    off = s - sub
    hd.rounded_rectangle([(off - halo_w) * K, (off - halo_w) * K, s * K + halo_w * K, s * K + halo_w * K],
                         radius=round((sub * 0.22 + halo_w) * K), fill=255)
    halo = halo.resize((s, s), Image.LANCZOS)
    a = img.getchannel("A")
    a = Image.composite(Image.new("L", (s, s), 0), a, halo)
    img.putalpha(a)
    img.alpha_composite(front, (off, off))
    return img


def compose_split(s):
    tile = tile_mask(s)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    px = img.load()
    for y in range(s):
        for x in range(s):
            px[x, y] = (DESKTOP_RGB if x + y < s else LAPTOP_RGB) + (255,)
    img.putalpha(tile)
    g = laptop_mask(s)
    white = Image.new("RGBA", (s, s), (255, 255, 255, 0))
    white.putalpha(g)
    return Image.alpha_composite(img, white)


def write_ico(path, imgs):
    import io
    import struct
    entries = []
    blobs = []
    for im in imgs:
        s = im.size[0]
        buf = io.BytesIO()
        if s >= 64:
            im.save(buf, format="PNG")
        else:
            # classic 32bpp BMP entry (best compatibility for small sizes)
            w = h = s
            pix = im.tobytes("raw", "BGRA")
            rows = [pix[y * w * 4:(y + 1) * w * 4] for y in range(h)][::-1]
            mask_stride = ((w + 31) // 32) * 4
            hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4 + mask_stride * h, 0, 0, 0, 0)
            buf.write(hdr)
            buf.write(b"".join(rows))
            buf.write(b"\x00" * (mask_stride * h))
        blobs.append(buf.getvalue())
        entries.append(s)
    out = io.BytesIO()
    out.write(struct.pack("<HHH", 0, 1, len(imgs)))
    offset = 6 + 16 * len(imgs)
    for s, blob in zip(entries, blobs):
        dim = 0 if s >= 256 else s
        out.write(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset))
        offset += len(blob)
    for blob in blobs:
        out.write(blob)
    with open(path, "wb") as f:
        f.write(out.getvalue())


# --------------------------------------------------------------------------
# installer artwork
# --------------------------------------------------------------------------
def find_font(bold=True):
    cands = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    ]
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def gradient(w, h, top, bottom):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(w):
            px[x, y] = c
    return img


def wizard_bmp(path):
    w, h = 164, 314
    img = gradient(w, h, (0x1E, 0x3A, 0x8A), (0x0F, 0x17, 0x2A)).convert("RGBA")
    # soft glow behind the tiles
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([10, 60, 154, 204], fill=(80, 140, 255, 70))
    glow = glow.filter(ImageFilter.GaussianBlur(18))
    img.alpha_composite(glow)
    d_icon = compose(64, desktop_mask, DESKTOP_RGB)
    l_icon = compose(64, laptop_mask, LAPTOP_RGB)
    img.alpha_composite(d_icon, (24, 78))
    img.alpha_composite(l_icon, (76, 126))
    d = ImageDraw.Draw(img)
    fp = find_font(True)
    if fp:
        f = ImageFont.truetype(fp, 22)
        tw = d.textlength("WhichPC", font=f)
        d.text(((w - tw) / 2, 226), "WhichPC", font=f, fill=(255, 255, 255))
        f2 = ImageFont.truetype(find_font(False) or fp, 10)
        t2 = "laptop  |  desktop"
        tw2 = d.textlength(t2, font=f2)
        d.text(((w - tw2) / 2, 256), t2, font=f2, fill=(170, 190, 230))
    img.convert("RGB").save(path, format="BMP")


def header_bmp(path):
    w, h = 150, 57
    img = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    d_icon = compose(36, desktop_mask, DESKTOP_RGB)
    l_icon = compose(36, laptop_mask, LAPTOP_RGB)
    img.alpha_composite(d_icon, (w - 36 - 8 - 26, 6))
    img.alpha_composite(l_icon, (w - 36 - 8, 15))
    img.convert("RGB").save(path, format="BMP")


# --------------------------------------------------------------------------
def main():
    os.makedirs(MASKS, exist_ok=True)
    kinds = {
        "tile": tile_mask,
        "laptop": laptop_mask,
        "desktop": desktop_mask,
    }
    rc_lines = ["// generated by tools/gen_assets.py - do not edit", '#include "masks.h"', ""]
    h_lines = [
        "// generated by tools/gen_assets.py - do not edit",
        "#pragma once",
        "#define MASK_KIND_TILE    0",
        "#define MASK_KIND_LAPTOP  1",
        "#define MASK_KIND_DESKTOP 2",
        "#define MASK_KIND_DOT     3",
        "#define MASK_KIND_DOTCUT  4",
        "#define MASK_KIND_COUNT   5",
        "#define MASK_RES_BASE     1000",
        "#define MASK_RES_ID(kind, idx) (MASK_RES_BASE + (kind) * 100 + (idx))",
        "#define MASK_SIZE_COUNT   %d" % len(MASK_SIZES),
        "#ifndef RC_INVOKED",
        "static const int kMaskSizes[MASK_SIZE_COUNT] = { %s };" % ", ".join(map(str, MASK_SIZES)),
        "#endif",
    ]
    order = ["tile", "laptop", "desktop", "dot", "dotcut"]
    for idx, s in enumerate(MASK_SIZES):
        imgs = {k: fn(s) for k, fn in kinds.items()}
        imgs["dot"], imgs["dotcut"] = dot_masks(s)
        for kind_i, k in enumerate(order):
            name = "%s_%d.png" % (k, s)
            imgs[k].save(os.path.join(MASKS, name), optimize=True)
            rc_lines.append('%d RCDATA "../assets/masks/%s"' % (1000 + kind_i * 100 + idx, name))
    with open(os.path.join(OUT, "masks.rc"), "w") as f:
        f.write("\n".join(rc_lines) + "\n")
    with open(os.path.join(OUT, "masks.h"), "w") as f:
        f.write("\n".join(h_lines) + "\n")

    write_ico(os.path.join(OUT, "app.ico"), [app_icon(s) for s in ICO_SIZES])
    write_ico(os.path.join(OUT, "laptop.ico"), [compose(s, laptop_mask, LAPTOP_RGB) for s in ICO_SIZES])
    write_ico(os.path.join(OUT, "desktop.ico"), [compose(s, desktop_mask, DESKTOP_RGB) for s in ICO_SIZES])
    wizard_bmp(os.path.join(OUT, "wizard.bmp"))
    header_bmp(os.path.join(OUT, "header.bmp"))

    # contact sheet: every size, normal + remote, on dark and light taskbars
    if "--preview" in sys.argv:
        sizes = [16, 20, 24, 32, 40, 48, 64, 96]
        cell = 110
        sheet = Image.new("RGBA", (cell * len(sizes), cell * 6), (0, 0, 0, 255))
        rows = [
            (lambda s: compose(s, laptop_mask, LAPTOP_RGB), (32, 32, 32)),
            (lambda s: compose(s, desktop_mask, DESKTOP_RGB), (32, 32, 32)),
            (lambda s: compose(s, desktop_mask, DESKTOP_RGB, True), (32, 32, 32)),
            (lambda s: compose(s, laptop_mask, LAPTOP_RGB), (238, 238, 238)),
            (lambda s: compose(s, desktop_mask, DESKTOP_RGB, True), (238, 238, 238)),
            (app_icon, (238, 238, 238)),
        ]
        for r, (fn, bg) in enumerate(rows):
            for c, s in enumerate(sizes):
                ic = fn(s)
                block = Image.new("RGBA", (cell, cell), bg + (255,))
                # nearest-neighbour zoom to inspect pixels of small sizes
                z = max(1, min(4, 96 // s))
                ic2 = ic.resize((s * z, s * z), Image.NEAREST)
                block.alpha_composite(ic2, ((cell - s * z) // 2, (cell - s * z) // 2))
                sheet.alpha_composite(block, (c * cell, r * cell))
        sheet.save(os.path.join(OUT, "preview.png"))
    print("assets written to", OUT)


if __name__ == "__main__":
    main()
