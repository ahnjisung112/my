// Click-through, always-on-top layered windows: the per-monitor corner badge
// and the big "identify" card shown in the middle of the screen.
#include "common.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace {

constexpr wchar_t kOverlayClass[] = L"WhichPC.Overlay";
const float kSizeFactor[3] = {0.82f, 1.0f, 1.3f};

HINSTANCE g_hinst;

struct Layered {
    HWND hwnd = nullptr;
    Image32 img;
    POINT pos{};
    int alpha = 0;
    int target = 0;
    bool visible = false;

    bool Create() {
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               kOverlayClass, L"WhichPC", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_hinst, nullptr);
        return hwnd != nullptr;
    }

    void SetImage(Image32 next) {
        img.Free();
        img = next;
    }

    void Push() {
        if (!hwnd || !img.bmp) return;
        if (alpha <= 0) {
            if (visible) {
                ShowWindow(hwnd, SW_HIDE);
                visible = false;
            }
            return;
        }
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, img.bmp);
        SIZE sz = {img.w, img.h};
        POINT src = {0, 0};
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)std::clamp(alpha, 0, 255), AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (!visible) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            visible = true;
        }
    }

    // Moves `alpha` one animation step towards `target`. Returns true if not there yet.
    bool Step(int step) {
        if (alpha == target) return false;
        alpha = alpha < target ? std::min(target, alpha + step) : std::max(target, alpha - step);
        Push();
        return alpha != target;
    }

    RECT Rect() const { return RECT{pos.x, pos.y, pos.x + img.w, pos.y + img.h}; }

    void Destroy() {
        if (hwnd) DestroyWindow(hwnd);
        hwnd = nullptr;
        visible = false;
        img.Free();
    }
};

struct Badge {
    Layered win;
    HMONITOR monitor = nullptr;
};

std::vector<Badge> g_badges;
HWND g_lastForeground = nullptr;
bool g_suppressed = false;

Layered g_flash;
ULONGLONG g_flashStart = 0;
POINT g_flashBase{};
float g_flashScale = 1.f;

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(mon);
    return TRUE;
}

POINT CornerPosition(const RECT& work, int corner, int w, int h, int margin) {
    POINT p;
    switch (corner) {
        case CORNER_TOP_LEFT:
        case CORNER_BOTTOM_LEFT:
            p.x = work.left + margin;
            break;
        case CORNER_TOP_CENTER:
        case CORNER_BOTTOM_CENTER:
            p.x = work.left + (work.right - work.left - w) / 2;
            break;
        default:
            p.x = work.right - margin - w;
            break;
    }
    p.y = (corner <= CORNER_TOP_RIGHT) ? work.top + margin : work.bottom - margin - h;
    return p;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

// Chrome Remote Desktop and the Store RDP client have no dedicated process.
bool IsHostedRemoteViewer(HWND fg, const std::wstring& exe) {
    static const wchar_t* hosts[] = {L"chrome.exe", L"msedge.exe",  L"whale.exe",
                                     L"brave.exe",  L"firefox.exe", L"applicationframehost.exe"};
    bool host = false;
    for (const wchar_t* h : hosts) host |= exe == h;
    if (!host) return false;
    wchar_t title[256] = {};
    GetWindowTextW(fg, title, ARRAYSIZE(title));
    std::wstring t = Lower(title);
    return t.find(L"remote desktop") != std::wstring::npos || t.find(L"원격 데스크톱") != std::wstring::npos ||
           t.find(L"원격 데스크탑") != std::wstring::npos;
}

bool IsRemoteViewer(HWND fg, const Settings& s) {
    static std::wstring cachedSource;
    static std::vector<std::wstring> cachedList;
    if (cachedSource != s.remoteApps) {
        cachedSource = s.remoteApps;
        cachedList = ParseAppList(s.remoteApps);
    }
    std::wstring exe = ProcessNameOfWindow(fg);
    if (exe.empty()) return false;
    for (const auto& a : cachedList)
        if (exe == a) return true;
    return IsHostedRemoteViewer(fg, exe);
}

bool IsFullscreen(HWND fg, HMONITOR* monOut) {
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    if (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) return false;  // ordinary maximised window
    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    *monOut = mon;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

}  // namespace

bool OverlayRegisterClasses(HINSTANCE hinst) {
    g_hinst = hinst;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    return RegisterClassExW(&wc) != 0;
}

bool OverlayIsOwnWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

void OverlayDestroyAll() {
    for (auto& b : g_badges) b.win.Destroy();
    g_badges.clear();
}

void OverlayRebuild(const Settings& s, const BadgeContent& content) {
    OverlayDestroyAll();
    if (!s.overlay) return;
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    const int targetAlpha = std::clamp(s.overlayOpacity, 20, 100) * 255 / 100;
    for (HMONITOR mon : monitors) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfoW(mon, &mi)) continue;
        if (s.overlayMonitors == 0 && !(mi.dwFlags & MONITORINFOF_PRIMARY)) continue;
        const float dpiScale = DpiForMonitor(mon) / 96.f;
        Badge b;
        b.monitor = mon;
        b.win.SetImage(RenderBadge(content, dpiScale * kSizeFactor[std::clamp(s.overlaySize, 0, 2)]));
        if (!b.win.img.bmp || !b.win.Create()) {
            b.win.Destroy();
            continue;
        }
        b.win.pos =
            CornerPosition(mi.rcWork, s.overlayCorner, b.win.img.w, b.win.img.h, (int)std::lround(4 * dpiScale));
        b.win.alpha = 0;
        b.win.target = targetAlpha;
        g_badges.push_back(b);
    }
    g_lastForeground = nullptr;
}

void OverlaySetSuppressed(bool suppressed) {
    g_suppressed = suppressed;
    if (!suppressed) return;
    for (auto& b : g_badges) {  // hide at once, no fade
        b.win.alpha = b.win.target = 0;
        b.win.Push();
    }
}

bool OverlayTick(const Settings& s) {
    if (g_badges.empty()) return false;
    if (g_suppressed) return false;
    POINT cur = {};
    GetCursorPos(&cur);
    HWND fg = GetForegroundWindow();
    bool remoteFront = false, fullscreen = false;
    RECT fgRect = {};
    HMONITOR fsMonitor = nullptr;
    if (fg && !OverlayIsOwnWindow(fg)) {
        if (s.hideOverRemote && IsRemoteViewer(fg, s)) {
            remoteFront = true;
            fgRect = WindowVisibleRect(fg);
        }
        if (s.hideOnFullscreen) fullscreen = IsFullscreen(fg, &fsMonitor);
    }
    const int visibleAlpha = std::clamp(s.overlayOpacity, 20, 100) * 255 / 100;
    bool animating = false;
    for (auto& b : g_badges) {
        RECT r = b.win.Rect();
        bool hide = false;
        if (s.hideOnHover) {
            RECT hover = r;
            InflateRect(&hover, 6, 6);
            hide |= PtInRect(&hover, cur) != FALSE;
        }
        RECT tmp;
        if (remoteFront && IntersectRect(&tmp, &r, &fgRect)) hide = true;
        if (fullscreen && fsMonitor == b.monitor) hide = true;
        b.win.target = hide ? 0 : visibleAlpha;
        animating |= b.win.alpha != b.win.target;
    }
    if (fg != g_lastForeground) {
        g_lastForeground = fg;
        if (fg && !OverlayIsOwnWindow(fg)) OverlayReassertTopmost();
    }
    return animating;
}

bool OverlayAnimate() {
    bool any = false;
    for (auto& b : g_badges) any |= b.win.Step(34);
    return any;
}

void OverlayReassertTopmost() {
    for (auto& b : g_badges)
        if (b.win.visible)
            SetWindowPos(b.win.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// --------------------------------------------------------------------------
// identify card
// --------------------------------------------------------------------------
constexpr ULONGLONG kFadeIn = 180, kHold = 1500, kFadeOut = 450;

void FlashShow(const CardContent& c) {
    POINT cur = {};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_flashScale = DpiForMonitor(mon) / 96.f;
    if (!g_flash.hwnd && !g_flash.Create()) return;
    g_flash.SetImage(RenderCard(c, g_flashScale));
    if (!g_flash.img.bmp) return;
    const RECT& w = mi.rcWork;
    g_flashBase.x = w.left + (w.right - w.left - g_flash.img.w) / 2;
    g_flashBase.y = w.top + (w.bottom - w.top - g_flash.img.h) / 2 - (w.bottom - w.top) / 12;
    g_flashStart = GetTickCount64();
    g_flash.alpha = 1;
    g_flash.pos = g_flashBase;
    g_flash.Push();
}

bool FlashAnimate() {
    if (!g_flash.hwnd || !g_flashStart) return false;
    const ULONGLONG t = GetTickCount64() - g_flashStart;
    float a;
    float rise = 0;
    if (t < kFadeIn) {
        float p = t / (float)kFadeIn;
        a = 1 - (1 - p) * (1 - p);  // ease-out
        rise = (1 - a) * 14 * g_flashScale;
    } else if (t < kFadeIn + kHold) {
        a = 1;
    } else if (t < kFadeIn + kHold + kFadeOut) {
        float p = (t - kFadeIn - kHold) / (float)kFadeOut;
        a = 1 - p * p;
    } else {
        FlashHide();
        return false;
    }
    g_flash.alpha = std::max(1, (int)std::lround(a * 250));
    g_flash.pos = POINT{g_flashBase.x, g_flashBase.y + (LONG)std::lround(rise)};
    g_flash.Push();
    return true;
}

void FlashHide() {
    g_flashStart = 0;
    g_flash.alpha = 0;
    g_flash.Push();
}
