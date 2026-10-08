// Test harness: writes the runtime-rendered icons, badges and identify cards to
// PNG files (with real alpha) so they can be inspected without a Windows desktop.
//   render_preview.exe <output-dir>
#include "../src/common.h"

#include <algorithm>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

static HINSTANCE g_inst;
HINSTANCE AppInstance() { return g_inst; }
static Settings g_settings;
const Settings& App_Settings() { return g_settings; }
void App_ApplySettings(const Settings&, bool) {}

static CLSID PngClsid() {
    UINT n = 0, size = 0;
    GetImageEncodersSize(&n, &size);
    std::vector<BYTE> buf(size);
    auto* enc = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(n, size, enc);
    for (UINT i = 0; i < n; ++i)
        if (!wcscmp(enc[i].MimeType, L"image/png")) return enc[i].Clsid;
    return CLSID{};
}

static void SavePremultiplied(const Image32& img, const std::wstring& path) {
    Bitmap bmp(img.w, img.h, img.w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(img.bits));
    CLSID png = PngClsid();
    bmp.Save(path.c_str(), &png, nullptr);
}

static void SaveStraight(std::vector<DWORD> px, int size, const std::wstring& path) {
    Bitmap bmp(size, size, size * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(px.data()));
    CLSID png = PngClsid();
    bmp.Save(path.c_str(), &png, nullptr);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR cmd, int) {
    g_inst = hinst;
    std::wstring dir = (cmd && *cmd) ? cmd : L".";
    if (!RenderInit(hinst)) return 1;

    const COLORREF blue = kColorPresets[0].color, orange = kColorPresets[1].color;
    for (int size : {16, 20, 24, 32, 40, 48, 64}) {
        SaveStraight(ComposeIconPixels(DEVICE_LAPTOP, blue, size, false), size,
                     dir + L"\\icon_laptop_" + std::to_wstring(size) + L".png");
        SaveStraight(ComposeIconPixels(DEVICE_DESKTOP, orange, size, false), size,
                     dir + L"\\icon_desktop_" + std::to_wstring(size) + L".png");
        SaveStraight(ComposeIconPixels(DEVICE_DESKTOP, orange, size, true), size,
                     dir + L"\\icon_desktop_remote_" + std::to_wstring(size) + L".png");
    }
    // in-between size exercises the resampler
    SaveStraight(ComposeIconPixels(DEVICE_LAPTOP, blue, 18, false), 18, dir + L"\\icon_laptop_18.png");

    struct {
        int device;
        COLORREF color;
        const wchar_t* label;
        bool remote;
        const wchar_t* name;
    } badges[] = {
        {DEVICE_LAPTOP, blue, L"노트북", false, L"laptop"},
        {DEVICE_DESKTOP, orange, L"데스크탑", false, L"desktop"},
        {DEVICE_DESKTOP, orange, L"데스크탑", true, L"desktop_remote"},
        {DEVICE_DESKTOP, kColorPresets[3].color, L"게이밍 PC", false, L"custom"},
    };
    for (auto& b : badges) {
        BadgeContent bc;
        bc.device = b.device;
        bc.color = b.color;
        bc.label = b.label;
        bc.remote = b.remote;
        for (float sc : {1.0f, 1.5f}) {
            Image32 img = RenderBadge(bc, sc);
            SavePremultiplied(img, dir + L"\\badge_" + b.name + (sc > 1 ? L"_150" : L"_100") + L".png");
            img.Free();
        }
        CardContent cc;
        cc.device = b.device;
        cc.color = b.color;
        cc.title = b.label;
        cc.line1 = b.device == DEVICE_LAPTOP ? L"LAPTOP-7Q2K" : L"DESKTOP-GAMING";
        cc.line2 = b.remote ? L"LAPTOP-7Q2K에서 원격 접속 중" : L"이 PC의 화면입니다";
        cc.remote = b.remote;
        Image32 card = RenderCard(cc, 1.0f);
        SavePremultiplied(card, dir + L"\\card_" + b.name + L".png");
        card.Free();
        if (b.remote) {
            HBITMAP th = RenderThumbnail(cc, 200, 120);
            BITMAP bm;
            GetObjectW(th, sizeof(bm), &bm);
            Image32 t;
            t.bmp = th;
            t.bits = static_cast<DWORD*>(bm.bmBits);
            t.w = bm.bmWidth;
            t.h = bm.bmHeight;
            SavePremultiplied(t, dir + L"\\thumb_" + b.name + L".png");
            t.Free();
        }
    }
    RenderShutdown();
    return 0;
}
