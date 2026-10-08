// Icon composition and GDI+ rendering of the on-screen badge / identify card.
#include "common.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include "masks.h"

using namespace Gdiplus;

static HINSTANCE g_hinst;
static ULONG_PTR g_gdipToken;
static std::map<std::pair<int, int>, std::vector<BYTE>> g_masks;

constexpr COLORREF kDotColor = RGB(0x22, 0xC5, 0x5E);

bool RenderInit(HINSTANCE hinst) {
    g_hinst = hinst;
    GdiplusStartupInput in;
    return GdiplusStartup(&g_gdipToken, &in, nullptr) == Ok;
}

void RenderShutdown() {
    g_masks.clear();
    if (g_gdipToken) GdiplusShutdown(g_gdipToken);
    g_gdipToken = 0;
}

void Image32::Free() {
    if (bmp) DeleteObject(bmp);
    bmp = nullptr;
    bits = nullptr;
    w = h = 0;
}

// --------------------------------------------------------------------------
// masks
// --------------------------------------------------------------------------
static bool LoadMaskResource(int kind, int idx, int size, std::vector<BYTE>& out) {
    HRSRC res = FindResourceW(g_hinst, MAKEINTRESOURCEW(MASK_RES_ID(kind, idx)), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(g_hinst, res);
    const BYTE* data = mem ? static_cast<const BYTE*>(LockResource(mem)) : nullptr;
    DWORD len = SizeofResource(g_hinst, res);
    if (!data || !len) return false;
    IStream* stream = SHCreateMemStream(data, len);
    if (!stream) return false;
    bool ok = false;
    Bitmap* bmp = Bitmap::FromStream(stream);
    if (bmp && bmp->GetLastStatus() == Ok && (int)bmp->GetWidth() == size && (int)bmp->GetHeight() == size) {
        BitmapData bd;
        Rect r(0, 0, size, size);
        if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) == Ok) {
            out.resize(size * size);
            for (int y = 0; y < size; ++y) {
                const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<BYTE*>(bd.Scan0) + y * bd.Stride);
                for (int x = 0; x < size; ++x) out[y * size + x] = (BYTE)((row[x] >> 16) & 0xFF);
            }
            bmp->UnlockBits(&bd);
            ok = true;
        }
    }
    delete bmp;
    stream->Release();
    return ok;
}

// Area-averaging resampler for square 8-bit masks.
static std::vector<BYTE> ResampleMask(const std::vector<BYTE>& src, int ss, int ds) {
    std::vector<float> tmp(ds * ss, 0.f);
    const double scale = double(ss) / ds;
    auto axis = [&](int d, int& i0, int& i1, std::vector<float>& w) {
        double a = d * scale, b = (d + 1) * scale;
        i0 = (int)std::floor(a);
        i1 = std::min(ss - 1, (int)std::ceil(b) - 1);
        w.assign(i1 - i0 + 1, 0.f);
        for (int i = i0; i <= i1; ++i) {
            double lo = std::max(a, (double)i), hi = std::min(b, (double)i + 1);
            w[i - i0] = float(std::max(0.0, hi - lo) / scale);
        }
    };
    std::vector<float> w;
    for (int dx = 0; dx < ds; ++dx) {
        int i0, i1;
        axis(dx, i0, i1, w);
        for (int y = 0; y < ss; ++y) {
            float acc = 0;
            for (int i = i0; i <= i1; ++i) acc += src[y * ss + i] * w[i - i0];
            tmp[y * ds + dx] = acc;
        }
    }
    std::vector<BYTE> out(ds * ds);
    for (int dy = 0; dy < ds; ++dy) {
        int i0, i1;
        axis(dy, i0, i1, w);
        for (int x = 0; x < ds; ++x) {
            float acc = 0;
            for (int i = i0; i <= i1; ++i) acc += tmp[i * ds + x] * w[i - i0];
            out[dy * ds + x] = (BYTE)std::clamp((int)std::lround(acc), 0, 255);
        }
    }
    return out;
}

static const std::vector<BYTE>& GetMask(int kind, int size) {
    size = std::clamp(size, 4, 1024);
    auto key = std::make_pair(kind, size);
    auto it = g_masks.find(key);
    if (it != g_masks.end()) return it->second;

    std::vector<BYTE> out;
    int pick = MASK_SIZE_COUNT - 1;
    for (int i = 0; i < MASK_SIZE_COUNT; ++i) {
        if (kMaskSizes[i] >= size) {
            pick = i;
            break;
        }
    }
    std::vector<BYTE> src;
    if (LoadMaskResource(kind, pick, kMaskSizes[pick], src)) {
        out = (kMaskSizes[pick] == size) ? std::move(src) : ResampleMask(src, kMaskSizes[pick], size);
    } else {
        out.assign(size * size, 0);
    }
    return g_masks.emplace(key, std::move(out)).first->second;
}

// --------------------------------------------------------------------------
// icons
// --------------------------------------------------------------------------
std::vector<DWORD> ComposeIconPixels(int device, COLORREF color, int size, bool remote) {
    const auto& tile = GetMask(MASK_KIND_TILE, size);
    const auto& glyph = GetMask(device == DEVICE_LAPTOP ? MASK_KIND_LAPTOP : MASK_KIND_DESKTOP, size);
    const std::vector<BYTE>* dot = remote ? &GetMask(MASK_KIND_DOT, size) : nullptr;
    const std::vector<BYTE>* cut = remote ? &GetMask(MASK_KIND_DOTCUT, size) : nullptr;

    const float tr = GetRValue(color), tg = GetGValue(color), tb = GetBValue(color);
    const float dr = GetRValue(kDotColor), dg = GetGValue(kDotColor), db = GetBValue(kDotColor);
    std::vector<DWORD> px(size * size);
    for (int i = 0; i < size * size; ++i) {
        float a = tile[i] / 255.f;
        float ga = std::min(glyph[i] / 255.f, a);
        // white glyph over the coloured tile (straight alpha "over")
        float oa = ga + a * (1 - ga);
        float r = 0, g = 0, b = 0;
        if (oa > 0) {
            r = (255 * ga + tr * a * (1 - ga)) / oa;
            g = (255 * ga + tg * a * (1 - ga)) / oa;
            b = (255 * ga + tb * a * (1 - ga)) / oa;
        }
        if (remote) {
            oa *= 1 - (*cut)[i] / 255.f;
            float da = (*dot)[i] / 255.f;
            float na = da + oa * (1 - da);
            if (na > 0) {
                r = (dr * da + r * oa * (1 - da)) / na;
                g = (dg * da + g * oa * (1 - da)) / na;
                b = (db * da + b * oa * (1 - da)) / na;
            }
            oa = na;
        }
        auto c8 = [](float v) { return (DWORD)std::clamp((int)std::lround(v), 0, 255); };
        px[i] = (c8(oa * 255) << 24) | (c8(r) << 16) | (c8(g) << 8) | c8(b);
    }
    return px;
}

static HBITMAP CreateDib32(int w, int h, DWORD** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (bmp && p) memset(p, 0, (size_t)w * h * 4);
    *bits = static_cast<DWORD*>(p);
    return bmp;
}

HICON CreateDeviceIcon(int device, COLORREF color, int size, bool remote) {
    std::vector<DWORD> px = ComposeIconPixels(device, color, size, remote);
    DWORD* bits = nullptr;
    HBITMAP colorBmp = CreateDib32(size, size, &bits);
    if (!colorBmp) return nullptr;
    memcpy(bits, px.data(), px.size() * 4);
    // AND mask (rows WORD aligned): 1 = transparent, for code paths that ignore alpha
    const int stride = ((size + 15) / 16) * 2;
    std::vector<BYTE> mask(stride * size, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if ((px[y * size + x] >> 24) == 0) mask[y * stride + x / 8] |= (BYTE)(0x80 >> (x % 8));
    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, mask.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

HBITMAP CreateDeviceMenuBitmap(int device, COLORREF color, int size) {
    std::vector<DWORD> px = ComposeIconPixels(device, color, size, false);
    DWORD* bits = nullptr;
    HBITMAP bmp = CreateDib32(size, size, &bits);
    if (!bmp) return nullptr;
    for (size_t i = 0; i < px.size(); ++i) {  // menus want premultiplied alpha
        DWORD p = px[i];
        DWORD a = p >> 24;
        DWORD r = ((p >> 16) & 0xFF) * a / 255, g = ((p >> 8) & 0xFF) * a / 255, b = (p & 0xFF) * a / 255;
        bits[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    return bmp;
}

// --------------------------------------------------------------------------
// GDI+ helpers
// --------------------------------------------------------------------------
static Image32 CreateImage32(int w, int h) {
    Image32 img;
    img.w = std::max(1, w);
    img.h = std::max(1, h);
    img.bmp = CreateDib32(img.w, img.h, &img.bits);
    if (!img.bmp) img = Image32();
    return img;
}

static Color Argb(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

static COLORREF Mix(COLORREF c, COLORREF with, float t) {
    auto m = [t](int a, int b) { return (BYTE)std::clamp((int)std::lround(a + (b - a) * t), 0, 255); };
    return RGB(m(GetRValue(c), GetRValue(with)), m(GetGValue(c), GetGValue(with)), m(GetBValue(c), GetBValue(with)));
}

static void RoundRectPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    r = std::min(r, std::min(w, h) / 2);
    REAL d = r * 2;
    p.Reset();
    if (d <= 0.5f) {
        p.AddRectangle(RectF(x, y, w, h));
        return;
    }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static FontFamily* PickFamily() {
    static const wchar_t* names[] = {L"Malgun Gothic",    L"맑은 고딕", L"Noto Sans KR",
                                     L"Noto Sans CJK KR", L"Segoe UI",  L"Tahoma"};
    for (const wchar_t* n : names) {
        FontFamily* f = new FontFamily(n);
        if (f->GetLastStatus() == Ok && f->IsAvailable()) return f;
        delete f;
    }
    return FontFamily::GenericSansSerif()->Clone();
}

static FontFamily& UiFamily() {
    static FontFamily* fam = PickFamily();
    return *fam;
}

struct TextMetrics {
    REAL width;
    REAL ascent;   // px from top of cell to baseline
    REAL descent;  // px from baseline to bottom of cell
};

static TextMetrics Measure(Graphics& g, const std::wstring& text, Font& font, int style) {
    TextMetrics m{};
    FontFamily& fam = UiFamily();
    REAL em = font.GetSize();
    UINT16 emH = fam.GetEmHeight(style);
    m.ascent = em * fam.GetCellAscent(style) / emH;
    m.descent = em * fam.GetCellDescent(style) / emH;
    if (!text.empty()) {
        StringFormat fmt(StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
        RectF box;
        g.MeasureString(text.c_str(), (INT)text.size(), &font, PointF(0, 0), &fmt, &box);
        m.width = box.Width;
    }
    return m;
}

// Draws `text` so that the middle of its cell box sits at `cy`.
static void DrawTextCentered(Graphics& g, const std::wstring& text, Font& font, int style, REAL x, REAL cy,
                             const Brush& brush) {
    TextMetrics m = Measure(g, text, font, style);
    // Hangul glyphs are visually centred on the em box, which sits slightly
    // below the cell centre for fonts with tall Latin ascenders.
    REAL top = cy - (m.ascent + m.descent) / 2;
    StringFormat fmt(StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), (INT)text.size(), &font, PointF(x, top), &fmt, &brush);
}

static void DrawGlyph(Graphics& g, int kind, int x, int y, int size, BYTE alpha = 255) {
    const auto& mask = GetMask(kind, size);
    Bitmap bmp(size, size, PixelFormat32bppARGB);
    BitmapData bd;
    Rect r(0, 0, size, size);
    if (bmp.LockBits(&r, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) return;
    for (int yy = 0; yy < size; ++yy) {
        DWORD* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(bd.Scan0) + yy * bd.Stride);
        for (int xx = 0; xx < size; ++xx) {
            DWORD a = mask[yy * size + xx] * alpha / 255;
            row[xx] = (a << 24) | 0x00FFFFFF;
        }
    }
    bmp.UnlockBits(&bd);
    InterpolationMode im = g.GetInterpolationMode();
    PixelOffsetMode pm = g.GetPixelOffsetMode();
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(&bmp, Rect(x, y, size, size), 0, 0, size, size, UnitPixel);
    g.SetInterpolationMode(im);
    g.SetPixelOffsetMode(pm);
}

static void BoxBlur(std::vector<float>& a, int w, int h, int radius) {
    if (radius < 1) return;
    std::vector<float> tmp(a.size());
    const float inv = 1.f / (2 * radius + 1);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            float acc = 0;
            for (int i = -radius; i <= radius; ++i) acc += a[y * w + std::clamp(i, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                tmp[y * w + x] = acc * inv;
                acc += a[y * w + std::min(x + radius + 1, w - 1)] - a[y * w + std::max(x - radius, 0)];
            }
        }
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (int i = -radius; i <= radius; ++i) acc += tmp[std::clamp(i, 0, h - 1) * w + x];
            for (int y = 0; y < h; ++y) {
                a[y * w + x] = acc * inv;
                acc += tmp[std::min(y + radius + 1, h - 1) * w + x] - tmp[std::max(y - radius, 0) * w + x];
            }
        }
    }
}

// Writes a soft drop shadow of a rounded rect into an empty premultiplied image.
static void DrawShadow(Image32& img, REAL x, REAL y, REAL w, REAL h, REAL radius, int blur, float opacity) {
    Bitmap tmp(img.w, img.h, PixelFormat32bppARGB);
    {
        Graphics g(&tmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        GraphicsPath p;
        RoundRectPath(p, x, y, w, h, radius);
        SolidBrush b(Color(255, 0, 0, 0));
        g.FillPath(&b, &p);
    }
    BitmapData bd;
    Rect r(0, 0, img.w, img.h);
    if (tmp.LockBits(&r, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return;
    std::vector<float> a(img.w * img.h);
    for (int yy = 0; yy < img.h; ++yy) {
        const DWORD* row = reinterpret_cast<const DWORD*>(static_cast<BYTE*>(bd.Scan0) + yy * bd.Stride);
        for (int xx = 0; xx < img.w; ++xx) a[yy * img.w + xx] = (row[xx] >> 24) / 255.f;
    }
    tmp.UnlockBits(&bd);
    BoxBlur(a, img.w, img.h, std::max(1, blur / 2));
    for (size_t i = 0; i < a.size(); ++i) {
        DWORD v = (DWORD)std::clamp((int)std::lround(a[i] * opacity * 255), 0, 255);
        img.bits[i] = v << 24;  // premultiplied black
    }
}

static void SetupGraphics(Graphics& g) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetCompositingQuality(CompositingQualityHighQuality);
}

static int GlyphKind(int device) { return device == DEVICE_LAPTOP ? MASK_KIND_LAPTOP : MASK_KIND_DESKTOP; }

// --------------------------------------------------------------------------
// screen-corner badge
// --------------------------------------------------------------------------
Image32 RenderBadge(const BadgeContent& c, float s) {
    const int shadow = std::max(3, (int)std::lround(7 * s));
    const REAL H = std::round(30 * s);
    const REAL padL = std::round(9 * s), padR = std::round(13 * s);
    const int G = (int)std::lround(20 * s);
    const REAL gap = std::round(6 * s);
    const int fontStyle = FontStyleBold;
    Font font(&UiFamily(), 13.5f * s, fontStyle, UnitPixel);
    Font chipFont(&UiFamily(), 10.5f * s, FontStyleBold, UnitPixel);
    const std::wstring chipText = L"원격";

    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics pg(&probe);
    SetupGraphics(pg);
    TextMetrics tm = Measure(pg, c.label, font, fontStyle);
    TextMetrics cm = Measure(pg, chipText, chipFont, FontStyleBold);
    const REAL chipPad = std::round(6 * s);
    const REAL chipW = c.remote ? std::ceil(cm.width + chipPad * 2) : 0;
    const REAL chipH = std::round(H - 12 * s);

    REAL W = std::ceil(padL + G + gap + tm.width + (c.remote ? gap + chipW : 0) + padR);
    const REAL ox = shadow, oy = std::round(shadow * 0.7f);
    Image32 img = CreateImage32((int)W + shadow * 2, (int)H + shadow * 2);
    if (!img.bmp) return img;
    DrawShadow(img, ox, oy + std::round(1.5f * s), W, H, H / 2, shadow, 0.38f);

    Bitmap canvas(img.w, img.h, img.w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(img.bits));
    Graphics g(&canvas);
    SetupGraphics(g);

    GraphicsPath pill;
    RoundRectPath(pill, ox, oy, W, H, H / 2);
    LinearGradientBrush fill(PointF(0, oy), PointF(0, oy + H), Argb(Mix(c.color, RGB(255, 255, 255), 0.10f)),
                             Argb(Mix(c.color, RGB(0, 0, 0), 0.08f)));
    g.FillPath(&fill, &pill);
    Pen edge(Color(70, 0, 0, 0), std::max(1.f, s));
    GraphicsPath edgePath;
    RoundRectPath(edgePath, ox + 0.5f, oy + 0.5f, W - 1, H - 1, (H - 1) / 2);
    g.DrawPath(&edge, &edgePath);
    Pen shine(Color(55, 255, 255, 255), std::max(1.f, s));
    GraphicsPath shinePath;
    RoundRectPath(shinePath, ox + 1.5f * s, oy + 1.5f * s, W - 3 * s, H - 3 * s, (H - 3 * s) / 2);
    g.DrawPath(&shine, &shinePath);

    DrawGlyph(g, GlyphKind(c.device), (int)(ox + padL), (int)std::lround(oy + (H - G) / 2), G);

    SolidBrush white(Color(255, 255, 255, 255));
    REAL tx = ox + padL + G + gap;
    DrawTextCentered(g, c.label, font, fontStyle, tx, oy + H / 2, white);

    if (c.remote) {
        REAL cx = tx + tm.width + gap;
        REAL cy = oy + (H - chipH) / 2;
        GraphicsPath chip;
        RoundRectPath(chip, cx, cy, chipW, chipH, chipH / 2);
        SolidBrush chipBg(Color(235, 255, 255, 255));
        g.FillPath(&chipBg, &chip);
        SolidBrush chipFg(Argb(Mix(c.color, RGB(0, 0, 0), 0.25f)));
        DrawTextCentered(g, chipText, chipFont, FontStyleBold, cx + chipPad, oy + H / 2, chipFg);
    }
    g.Flush(FlushIntentionSync);
    return img;
}

// --------------------------------------------------------------------------
// identify card (shown in the middle of the screen)
// --------------------------------------------------------------------------
static Image32 RenderCardImpl(const CardContent& c, float s, bool withShadow) {
    const int shadow = withShadow ? std::max(6, (int)std::lround(20 * s)) : 0;
    const REAL pad = std::round(22 * s);
    const REAL tile = std::round(100 * s);
    const int G = (int)std::lround(72 * s);
    const REAL gap = std::round(20 * s);

    Font titleFont(&UiFamily(), 34.f * s, FontStyleBold, UnitPixel);
    Font line1Font(&UiFamily(), 15.f * s, FontStyleRegular, UnitPixel);
    Font line2Font(&UiFamily(), 14.f * s, FontStyleBold, UnitPixel);

    Bitmap probe(1, 1, PixelFormat32bppPARGB);
    Graphics pg(&probe);
    SetupGraphics(pg);
    TextMetrics t0 = Measure(pg, c.title, titleFont, FontStyleBold);
    TextMetrics t1 = Measure(pg, c.line1, line1Font, FontStyleRegular);
    TextMetrics t2 = Measure(pg, c.line2, line2Font, FontStyleBold);
    const REAL dotD = std::round(9 * s);
    const REAL dotGap = std::round(7 * s);
    const REAL line2W = t2.width + (c.remote ? dotD + dotGap : 0);

    const REAL h0 = t0.ascent + t0.descent, h1 = t1.ascent + t1.descent, h2 = t2.ascent + t2.descent;
    const REAL spacing = std::round(2 * s);
    const REAL textH = h0 + spacing + h1 + (c.line2.empty() ? 0 : spacing + h2);
    const REAL textW = std::max(t0.width, std::max(t1.width, line2W));
    const REAL W = std::ceil(std::max(pad + tile + gap + textW + pad * 1.4f, 320 * s));
    const REAL H = std::ceil(pad * 2 + std::max(tile, textH));

    const REAL ox = shadow, oy = std::round(shadow * 0.6f);
    Image32 img = CreateImage32((int)W + shadow * 2, (int)H + shadow * 2);
    if (!img.bmp) return img;
    const REAL radius = std::round(22 * s);
    if (withShadow) DrawShadow(img, ox, oy + std::round(5 * s), W, H, radius, shadow, 0.45f);

    Bitmap canvas(img.w, img.h, img.w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(img.bits));
    Graphics g(&canvas);
    SetupGraphics(g);

    GraphicsPath card;
    RoundRectPath(card, ox, oy, W, H, radius);
    LinearGradientBrush fill(PointF(0, oy), PointF(0, oy + H), Argb(Mix(c.color, RGB(255, 255, 255), 0.14f)),
                             Argb(Mix(c.color, RGB(0, 0, 0), 0.12f)));
    g.FillPath(&fill, &card);
    Pen edge(Color(60, 255, 255, 255), std::max(1.f, s));
    GraphicsPath edgePath;
    RoundRectPath(edgePath, ox + 0.5f * s, oy + 0.5f * s, W - s, H - s, radius - 0.5f * s);
    g.DrawPath(&edge, &edgePath);

    // translucent tile behind the big glyph
    const REAL tx = ox + pad, ty = oy + (H - tile) / 2;
    GraphicsPath tilePath;
    RoundRectPath(tilePath, tx, ty, tile, tile, tile * 0.22f);
    SolidBrush tileBg(Color(46, 255, 255, 255));
    g.FillPath(&tileBg, &tilePath);
    DrawGlyph(g, GlyphKind(c.device), (int)std::lround(tx + (tile - G) / 2), (int)std::lround(ty + (tile - G) / 2), G);
    if (c.remote) {
        REAL d = std::round(26 * s), ring = std::round(4 * s);
        REAL cx = tx + tile - d * 0.8f, cy = ty + tile - d * 0.8f;
        SolidBrush ringB(Argb(Mix(c.color, RGB(0, 0, 0), 0.05f)));
        g.FillEllipse(&ringB, cx - ring, cy - ring, d + ring * 2, d + ring * 2);
        SolidBrush dotB(Argb(kDotColor));
        g.FillEllipse(&dotB, cx, cy, d, d);
    }

    // text block
    const REAL x = tx + tile + gap;
    REAL y = oy + (H - textH) / 2;
    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush soft(Color(225, 255, 255, 255));
    DrawTextCentered(g, c.title, titleFont, FontStyleBold, x, y + h0 / 2, white);
    y += h0 + spacing;
    DrawTextCentered(g, c.line1, line1Font, FontStyleRegular, x, y + h1 / 2, soft);
    y += h1 + spacing;
    if (!c.line2.empty()) {
        REAL lx = x;
        if (c.remote) {
            SolidBrush dotB(Argb(kDotColor));
            Pen dotEdge(Color(200, 255, 255, 255), std::max(1.f, 1.5f * s));
            g.FillEllipse(&dotB, lx, y + (h2 - dotD) / 2, dotD, dotD);
            g.DrawEllipse(&dotEdge, lx, y + (h2 - dotD) / 2, dotD, dotD);
            lx += dotD + dotGap;
        }
        DrawTextCentered(g, c.line2, line2Font, FontStyleBold, lx, y + h2 / 2, white);
    }
    g.Flush(FlushIntentionSync);
    return img;
}

Image32 RenderCard(const CardContent& c, float scale) { return RenderCardImpl(c, scale, true); }

HBITMAP RenderThumbnail(const CardContent& c, int maxW, int maxH) {
    Image32 probe = RenderCardImpl(c, 1.0f, false);
    if (!probe.bmp) return nullptr;
    float scale = std::min(maxW / (float)probe.w, maxH / (float)probe.h);
    probe.Free();
    scale = std::clamp(scale, 0.2f, 4.0f);
    for (int attempt = 0; attempt < 4; ++attempt) {
        Image32 img = RenderCardImpl(c, scale, false);
        if (!img.bmp) return nullptr;
        if (img.w <= maxW && img.h <= maxH) return img.bmp;
        img.Free();
        scale *= 0.92f;
    }
    return nullptr;
}
