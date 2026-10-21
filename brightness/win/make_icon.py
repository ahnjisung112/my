# 해 모양 아이콘(app.ico) 생성
import math
from PIL import Image, ImageDraw

def sun(size):
    ss = 8
    n = size * ss
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = n / 2
    col = (255, 204, 77, 255)
    for i in range(8):
        a = i * math.pi / 4
        r0, r1 = n * 0.34, n * 0.47
        d.line([(c + r0 * math.cos(a), c + r0 * math.sin(a)),
                (c + r1 * math.cos(a), c + r1 * math.sin(a))], fill=col, width=int(n * 0.09))
    r = n * 0.24
    d.ellipse([c - r, c - r, c + r, c + r], fill=col)
    # 왼쪽 절반을 어둡게 -> 밝기 조절 느낌
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse([c - r * 0.78, c - r * 0.78, c + r * 0.78, c + r * 0.78], fill=255)
    half = Image.new("L", (n, n), 0)
    ImageDraw.Draw(half).rectangle([0, 0, c, n], fill=255)
    from PIL import ImageChops
    m = ImageChops.multiply(mask, half)
    im.paste((60, 60, 70, 255), (0, 0), m)
    return im.resize((size, size), Image.LANCZOS)

big = sun(256)
big.save("app.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
