# 아이콘 생성
#   app.ico        : 앱 아이콘 (macOS 스타일 스퀘어클 + 그라데이션 + 해)
#   tray_white.ico : 어두운 작업표시줄용 흰색 해 글리프
#   tray_black.ico : 밝은 작업표시줄용 검은색 해 글리프
import math
from PIL import Image, ImageDraw, ImageFilter, ImageChops

SS = 4  # 슈퍼샘플링 배율


def squircle_mask(n, inset, power=5.0):
    """macOS Big Sur 아이콘 모양(초타원) 마스크"""
    big = n * SS
    m = Image.new("L", (big, big), 0)
    px = m.load()
    c = big / 2
    r = big / 2 - inset * SS
    for y in range(big):
        dy = abs((y + 0.5 - c) / r)
        if dy > 1:
            continue
        # |x|^p + |y|^p <= 1
        dx_max = (1 - dy ** power) ** (1 / power)
        x0 = int(c - dx_max * r)
        x1 = int(c + dx_max * r)
        for x in range(max(0, x0), min(big, x1 + 1)):
            px[x, y] = 255
    return m.resize((n, n), Image.LANCZOS)


def vgradient(n, top, bottom):
    g = Image.new("RGBA", (n, n))
    d = ImageDraw.Draw(g)
    for y in range(n):
        t = y / (n - 1)
        col = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        d.line([(0, y), (n, y)], fill=col)
    return g


def sun_glyph(n, color, core=0.20, r0=0.32, r1=0.44, ray_w=0.075):
    """SF Symbols 'sun.max.fill' 느낌: 꽉 찬 원 + 끝이 둥근 광선 8개"""
    big = n * SS
    im = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = big / 2
    w = ray_w * big
    for i in range(8):
        a = i * math.pi / 4 - math.pi / 2
        p0 = (c + r0 * big * math.cos(a), c + r0 * big * math.sin(a))
        p1 = (c + r1 * big * math.cos(a), c + r1 * big * math.sin(a))
        d.line([p0, p1], fill=color, width=round(w))
        for p in (p0, p1):
            d.ellipse([p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2], fill=color)
    rc = core * big
    d.ellipse([c - rc, c - rc, c + rc, c + rc], fill=color)
    return im.resize((n, n), Image.LANCZOS)


def app_icon(n=1024):
    inset = n * 0.09  # macOS 아이콘 여백 비율
    mask = squircle_mask(n, inset)
    bg = vgradient(n, (58, 62, 74), (20, 21, 26))           # 그래파이트
    # 위쪽 은은한 광택
    gloss = vgradient(n, (255, 255, 255), (255, 255, 255))
    gl_mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(gl_mask).ellipse([-n * 0.3, -n * 0.85, n * 1.3, n * 0.45], fill=34)
    gl_mask = gl_mask.filter(ImageFilter.GaussianBlur(n * 0.04))
    bg = Image.composite(gloss, bg, gl_mask)

    # 해: 노랑 -> 주황 그라데이션 + 빛 번짐
    sun_mask = sun_glyph(n, (255, 255, 255, 255), core=0.165, r0=0.255, r1=0.345, ray_w=0.058).split()[3]
    sun_col = vgradient(n, (255, 222, 89), (255, 149, 0))
    glow = Image.new("RGBA", (n, n), (255, 180, 40, 0))
    glow.putalpha(sun_mask.filter(ImageFilter.GaussianBlur(n * 0.05)).point(lambda v: int(v * 0.55)))
    bg = Image.alpha_composite(bg, glow)
    sun = sun_col.copy()
    sun.putalpha(sun_mask)
    bg = Image.alpha_composite(bg, sun)

    icon = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    icon.paste(bg, (0, 0), mask)
    # 안쪽 가장자리 하이라이트(아주 얇게)
    edge = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(5)))
    hl = Image.new("RGBA", (n, n), (255, 255, 255, 0))
    hl.putalpha(edge.point(lambda v: int(v * 0.10)))
    icon = Image.alpha_composite(icon, hl)
    # 바닥 그림자
    shadow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sh_mask = mask.filter(ImageFilter.GaussianBlur(n * 0.018)).point(lambda v: int(v * 0.45))
    shadow.putalpha(ImageChops.offset(sh_mask, 0, round(n * 0.012)))
    return Image.alpha_composite(shadow, icon)


def tray_icon(color):
    # 크기별로 따로 렌더링해야 작은 크기에서도 또렷함
    imgs = []
    for s in (16, 20, 24, 32, 40, 48, 64):
        # 작은 크기일수록 선을 굵게
        w = 0.135 if s <= 20 else (0.11 if s <= 32 else 0.095)
        imgs.append(sun_glyph(s, color, core=0.215, r0=0.355, r1=0.47, ray_w=w))
    return imgs


def save_multi(path, imgs):
    imgs = sorted(imgs, key=lambda im: -im.size[0])
    imgs[0].save(path, format="ICO", sizes=[im.size for im in imgs], append_images=imgs[1:])


if __name__ == "__main__":
    big = app_icon(1024)
    save_multi("app.ico", [big.resize((s, s), Image.LANCZOS) for s in (256, 128, 64, 48, 40, 32, 24, 20, 16)])
    save_multi("tray_white.ico", tray_icon((255, 255, 255, 255)))
    save_multi("tray_black.ico", tray_icon((0, 0, 0, 255)))
