/*
 * 밝기조절 — Windows 모니터 밝기 조절기
 *
 *  50~100% : 모니터 자체 밝기(백라이트) 0~100% — 외장 모니터 DDC/CI, 노트북 WMI
 *   1~50%  : 백라이트 최저 + 화면 전체에 클릭이 통과하는 검은 막(오버레이)
 *     0%   : 완전 검정 (꺼진 것처럼 보임)
 *
 *  창 위에서 마우스 휠 (Shift 미세, Ctrl 크게), 막대 클릭/드래그, ↑↓
 *  전역 단축키 Ctrl+Alt+PageUp/PageDown/Home, 트레이 아이콘
 */

#define COBJMACROS
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <wbemcli.h>
#include <math.h>
#include <stdio.h>
#include "resource.h"

#define APP_NAME L"밝기조절"
#define STEP 2.0

#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_HWREADY (WM_APP + 2)
#define WM_APP_WHEEL   (WM_APP + 3)

enum { HK_UP = 1, HK_DOWN, HK_RESET };
enum { ID_TIMER_TOP = 1 };
enum { CMD_EXIT = 100, CMD_SHOW, CMD_LEVEL_BASE = 200 };

/* ---------------------------------------------------------------- 전역 상태 */
static HINSTANCE g_inst;
static HWND g_main, g_overlay;
static HHOOK g_mouse_hook;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;
static HICON g_icon_big, g_icon_small;

static double g_level = 100.0;   /* 0..100 */
static double g_split = 50.0;    /* 하드웨어 제어 불가 시 100 */
static int g_hw_ready = 0;
static int g_hw_original = -1;
static int g_touched = 0;
static int g_hover = 0;
static int g_overlay_trans = -1; /* 0..255, 255 = 숨김 */
static int g_dragging_bar = 0;
static int g_wheel_accum = 0;
static UINT g_dpi = 96;

#define S(v) MulDiv((v), (int)g_dpi, 96)

static const COLORREF C_BG = RGB(0x1b, 0x1d, 0x22), C_FG = RGB(0xf2, 0xf3, 0xf5),
                      C_SUB = RGB(0x8a, 0x8f, 0x99), C_HINT = RGB(0x5d, 0x62, 0x6c),
                      C_ACCENT = RGB(0xff, 0xcc, 0x4d), C_TRACK = RGB(0x33, 0x36, 0x3d),
                      C_BORDER = RGB(0x2c, 0x2f, 0x36), C_TICK = RGB(0x55, 0x5a, 0x63);

/* 디자인 기준(96 DPI) 크기 */
#define WIN_W 280
#define WIN_H 158
#define PAD 16

/* ---------------------------------------------------------------- 밝기 곡선 */
/* 0..split 구간 -> 오버레이 투과량(0~255). 어두운 쪽을 촘촘하게 (감마 2.2). */
static int overlay_transmission(double level, double split)
{
    if (level >= split) return 255;
    if (level <= 0) return 0;
    double t = (level - 1.0) / (split - 1.0);
    if (t < 0) t = 0;
    int v = (int)lround(1.0 + 254.0 * pow(t, 2.2));
    return v < 1 ? 1 : (v > 255 ? 255 : v);
}

static int hw_percent(void)
{
    if (g_split >= 100.0) return -1;
    double v = g_level - g_split;
    if (v < 0) v = 0;
    return (int)lround(v * 100.0 / (100.0 - g_split));
}

/* ---------------------------------------------------------------- 하드웨어: DDC/CI */
#define MAX_PHYS 16
typedef struct { HANDLE h; DWORD mn, mx, cur; } Phys;
static Phys g_phys[MAX_PHYS];
static int g_nphys;

static BOOL CALLBACK enum_mon(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp)
{
    (void)hdc; (void)rc; (void)lp;
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &n) || n == 0) return TRUE;
    PHYSICAL_MONITOR *arr = (PHYSICAL_MONITOR *)calloc(n, sizeof(*arr));
    if (!arr) return TRUE;
    if (GetPhysicalMonitorsFromHMONITOR(hmon, n, arr)) {
        for (DWORD i = 0; i < n; i++) {
            DWORD mn, cur, mx;
            if (g_nphys < MAX_PHYS &&
                GetMonitorBrightness(arr[i].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn) {
                g_phys[g_nphys].h = arr[i].hPhysicalMonitor;
                g_phys[g_nphys].mn = mn;
                g_phys[g_nphys].mx = mx;
                g_phys[g_nphys].cur = cur;
                g_nphys++;
            } else {
                DestroyPhysicalMonitor(arr[i].hPhysicalMonitor);
            }
        }
    }
    free(arr);
    return TRUE;
}

static void ddc_set(int pct)
{
    for (int i = 0; i < g_nphys; i++) {
        DWORD v = g_phys[i].mn + (DWORD)lround((g_phys[i].mx - g_phys[i].mn) * pct / 100.0);
        SetMonitorBrightness(g_phys[i].h, v);
    }
}

/* ---------------------------------------------------------------- 하드웨어: WMI (노트북) */
static IWbemServices *g_wmi;

static int wmi_init(void)
{
    IWbemLocator *loc = NULL;
    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWbemLocator, (void **)&loc)))
        return 0;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &g_wmi);
    SysFreeString(ns);
    IWbemLocator_Release(loc);
    if (FAILED(hr)) { g_wmi = NULL; return 0; }
    CoSetProxyBlanket((IUnknown *)g_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return 1;
}

/* 첫 번째 결과 객체 하나를 돌려줌 */
static IWbemClassObject *wmi_first(const wchar_t *query)
{
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = IWbemServices_ExecQuery(g_wmi, lang, q,
                                         WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(lang);
    SysFreeString(q);
    if (SUCCEEDED(hr) && en) {
        ULONG got = 0;
        if (IEnumWbemClassObject_Next(en, 3000, 1, &obj, &got) != WBEM_S_NO_ERROR || got == 0)
            obj = NULL;
        IEnumWbemClassObject_Release(en);
    }
    return obj;
}

static int wmi_get(void)
{
    if (!g_wmi) return -1;
    IWbemClassObject *obj = wmi_first(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    if (!obj) return -1;
    VARIANT v;
    VariantInit(&v);
    int r = -1;
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"CurrentBrightness", 0, &v, NULL, NULL))) {
        VARIANT i4;
        VariantInit(&i4);
        if (SUCCEEDED(VariantChangeType(&i4, &v, 0, VT_I4))) r = i4.lVal;
    }
    VariantClear(&v);
    IWbemClassObject_Release(obj);
    return r;
}

static void wmi_set(int pct)
{
    if (!g_wmi) return;
    IWbemClassObject *inst = wmi_first(L"SELECT * FROM WmiMonitorBrightnessMethods");
    if (!inst) return;
    VARIANT path;
    VariantInit(&path);
    IWbemClassObject *cls = NULL, *inDef = NULL, *in = NULL, *out = NULL;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    BSTR method = SysAllocString(L"WmiSetBrightness");
    if (SUCCEEDED(IWbemClassObject_Get(inst, L"__PATH", 0, &path, NULL, NULL)) &&
        SUCCEEDED(IWbemServices_GetObject(g_wmi, clsName, 0, NULL, &cls, NULL)) &&
        SUCCEEDED(IWbemClassObject_GetMethod(cls, method, 0, &inDef, NULL)) &&
        SUCCEEDED(IWbemClassObject_SpawnInstance(inDef, 0, &in))) {
        VARIANT a, b;
        VariantInit(&a);
        a.vt = VT_I4;
        a.lVal = 1;
        IWbemClassObject_Put(in, L"Timeout", 0, &a, 0);
        VariantInit(&b);
        b.vt = VT_UI1;
        b.bVal = (BYTE)pct;
        IWbemClassObject_Put(in, L"Brightness", 0, &b, 0);
        IWbemServices_ExecMethod(g_wmi, path.bstrVal, method, 0, NULL, in, &out, NULL);
    }
    if (out) IWbemClassObject_Release(out);
    if (in) IWbemClassObject_Release(in);
    if (inDef) IWbemClassObject_Release(inDef);
    if (cls) IWbemClassObject_Release(cls);
    SysFreeString(clsName);
    SysFreeString(method);
    VariantClear(&path);
    IWbemClassObject_Release(inst);
}

/* ---------------------------------------------------------------- 하드웨어 작업 스레드 */
/* DDC/WMI 호출은 느려서(수십~수백 ms) 별도 스레드가 '마지막 요청값'만 적용한다. */
static CRITICAL_SECTION g_hw_cs;
static CONDITION_VARIABLE g_hw_cv;
static int g_hw_target = -1;
static int g_hw_stop = 0;
static HANDLE g_hw_thread;

static void hw_request(int pct)
{
    EnterCriticalSection(&g_hw_cs);
    g_hw_target = pct;
    LeaveCriticalSection(&g_hw_cs);
    WakeConditionVariable(&g_hw_cv);
}

static DWORD WINAPI hw_thread(LPVOID p)
{
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

    EnumDisplayMonitors(NULL, NULL, enum_mon, 0);
    int wmi_cur = -1;
    if (wmi_init()) {
        wmi_cur = wmi_get();
        if (wmi_cur < 0) { IWbemServices_Release(g_wmi); g_wmi = NULL; }
    }

    int initial = -1;
    if (g_nphys > 0)
        initial = (int)lround((g_phys[0].cur - g_phys[0].mn) * 100.0 / (g_phys[0].mx - g_phys[0].mn));
    else if (wmi_cur >= 0)
        initial = wmi_cur;
    PostMessageW(g_main, WM_APP_HWREADY, (WPARAM)initial, 0);

    int last = initial;
    for (;;) {
        EnterCriticalSection(&g_hw_cs);
        while (g_hw_target < 0 && !g_hw_stop)
            SleepConditionVariableCS(&g_hw_cv, &g_hw_cs, INFINITE);
        int pct = g_hw_target, stopping = g_hw_stop;
        g_hw_target = -1;
        LeaveCriticalSection(&g_hw_cs);

        if (pct >= 0 && pct != last) {
            ddc_set(pct);
            wmi_set(pct);
            last = pct;
        }
        if (stopping) break;
    }

    for (int i = 0; i < g_nphys; i++) DestroyPhysicalMonitor(g_phys[i].h);
    if (g_wmi) IWbemServices_Release(g_wmi);
    CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------------- 오버레이 */
static void overlay_fit(void)
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(g_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void raise_windows(void)
{
    /* 작업표시줄 등 다른 최상위 창이 덮어도 오버레이 -> 조절 창 순서로 다시 맨 위로 */
    if (g_overlay_trans < 255)
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void overlay_set(int trans)
{
    if (trans == g_overlay_trans) return;
    int was_hidden = g_overlay_trans < 0 || g_overlay_trans >= 255;
    g_overlay_trans = trans;
    if (trans >= 255) {
        ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    SetLayeredWindowAttributes(g_overlay, 0, (BYTE)(255 - trans), LWA_ALPHA);
    if (was_hidden) {
        overlay_fit();
        ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        raise_windows();
    }
}

static LRESULT CALLBACK overlay_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

/* ---------------------------------------------------------------- 조절 창 */
static void update_window_alpha(void)
{
    double a = 1.0;
    if (!g_hover) {
        double r = g_level / (g_split < 1 ? 1 : g_split);
        a = 0.35 + 0.65 * (r > 1 ? 1 : r);
    }
    SetLayeredWindowAttributes(g_main, 0, (BYTE)lround(a * 255), LWA_ALPHA);
}

static void format_level(wchar_t *buf, size_t n)
{
    if (g_level < 10 && g_level != floor(g_level))
        swprintf(buf, n, L"%.1f%%", g_level);
    else
        swprintf(buf, n, L"%d%%", (int)lround(g_level));
}

static void update_tray_tip(void)
{
    wchar_t lv[16];
    format_level(lv, 16);
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"밝기 %ls — 휠/Ctrl+Alt+PgUp·PgDn", lv);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void apply(void)
{
    overlay_set(overlay_transmission(g_level, g_split));
    int hw = hw_percent();
    if (hw >= 0 && g_hw_ready) hw_request(hw);
    update_window_alpha();
    update_tray_tip();
    InvalidateRect(g_main, NULL, FALSE);
}

static void set_level(double v)
{
    g_touched = 1;
    v = round(v * 2.0) / 2.0;
    g_level = v < 0 ? 0 : (v > 100 ? 100 : v);
    apply();
}

static void step(double d) { set_level(g_level + d); }

/* 영역 계산 (클라이언트 좌표, 실제 픽셀) */
static RECT rc_close(void) { RECT r = {S(WIN_W - 36), S(4), S(WIN_W - 6), S(32)}; return r; }
static RECT rc_bar_hit(void) { RECT r = {S(PAD - 6), S(84), S(WIN_W - PAD + 6), S(108)}; return r; }
static int bar_x0(void) { return S(PAD); }
static int bar_x1(void) { return S(WIN_W - PAD); }

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Malgun Gothic");
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void text(HDC dc, const wchar_t *s, RECT rc, HFONT f, COLORREF c, UINT fmt)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, fmt | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ oldbmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    fill(dc, 0, 0, cr.right, cr.bottom, C_BORDER);
    fill(dc, 1, 1, cr.right - 1, cr.bottom - 1, C_BG);

    HFONT f_title = make_font(13, FW_NORMAL), f_big = make_font(38, FW_BOLD),
          f_mode = make_font(12, FW_NORMAL), f_hint = make_font(11, FW_NORMAL);
    HGDIOBJ oldf = SelectObject(dc, f_title);

    /* 기호는 글꼴에 따라 깨질 수 있어 직접 그림: 해 아이콘 + 닫기 X */
    HPEN pen = CreatePen(PS_SOLID, S(2) > 1 ? S(2) : 1, C_SUB);
    HGDIOBJ oldpen = SelectObject(dc, pen);
    HGDIOBJ oldbr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int cx = S(PAD + 7), cy = S(19);
    Ellipse(dc, cx - S(3), cy - S(3), cx + S(4), cy + S(4));
    for (int i = 0; i < 8; i++) {
        double a = i * 3.14159265 / 4;
        MoveToEx(dc, cx + (int)lround(S(6) * cos(a)), cy + (int)lround(S(6) * sin(a)), NULL);
        LineTo(dc, cx + (int)lround(S(8) * cos(a)), cy + (int)lround(S(8) * sin(a)));
    }
    RECT rx = rc_close();
    int xm = (rx.left + rx.right) / 2, ym = (rx.top + rx.bottom) / 2, xs = S(5);
    MoveToEx(dc, xm - xs, ym - xs, NULL);
    LineTo(dc, xm + xs + 1, ym + xs + 1);
    MoveToEx(dc, xm + xs, ym - xs, NULL);
    LineTo(dc, xm - xs - 1, ym + xs + 1);
    SelectObject(dc, oldbr);
    SelectObject(dc, oldpen);
    DeleteObject(pen);
    RECT r = {S(PAD + 20), S(8), S(WIN_W - 40), S(30)};
    text(dc, L"밝기", r, f_title, C_SUB, DT_LEFT | DT_VCENTER);

    wchar_t lv[16];
    format_level(lv, 16);
    RECT rv = {0, S(28), cr.right, S(84)};
    text(dc, lv, rv, f_big, C_FG, DT_CENTER | DT_VCENTER);

    int x0 = bar_x0(), x1 = bar_x1(), by0 = S(92), by1 = S(100);
    fill(dc, x0, by0, x1, by1, C_TRACK);
    fill(dc, x0, by0, x0 + (int)lround((x1 - x0) * g_level / 100.0), by1, C_ACCENT);
    if (g_split < 100) {
        int sx = x0 + (int)lround((x1 - x0) * g_split / 100.0);
        fill(dc, sx, by0 - S(4), sx + (S(1) > 1 ? S(1) : 1), by1 + S(4), C_TICK);
    }

    wchar_t mode[96];
    int hw = hw_percent();
    if (g_level <= 0)
        wcscpy(mode, L"완전 소등 (화면 꺼짐 상태)");
    else if (g_level < g_split)
        wcscpy(mode, g_split < 100 ? L"모니터 최저 밝기 + 화면 어둡게" : L"화면 어둡게 (검은 막)");
    else if (hw >= 0)
        swprintf(mode, 96, L"모니터 밝기 %d%%", hw);
    else
        wcscpy(mode, L"원래 밝기 (모니터 자체 조절 미지원)");
    if (!g_hw_ready) wcscat(mode, L" · 모니터 확인 중…");
    RECT rm = {0, S(108), cr.right, S(128)};
    text(dc, mode, rm, f_mode, C_SUB, DT_CENTER | DT_VCENTER);
    RECT rh = {0, S(128), cr.right, S(148)};
    text(dc, L"휠: 조절 · Shift 미세 · Ctrl 크게 · 우클릭 메뉴", rh, f_hint, C_HINT, DT_CENTER | DT_VCENTER);

    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldf);
    DeleteObject(f_title);
    DeleteObject(f_big);
    DeleteObject(f_mode);
    DeleteObject(f_hint);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void bar_set_from_x(int x)
{
    double t = (double)(x - bar_x0()) / (bar_x1() - bar_x0());
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    set_level(round(t * 100.0));
}

static void show_menu(HWND h)
{
    HMENU m = CreatePopupMenu();
    static const int levels[] = {100, 75, 50, 25, 10, 0};
    wchar_t s[16];
    for (int i = 0; i < 6; i++) {
        swprintf(s, 16, L"%d%%", levels[i]);
        AppendMenuW(m, MF_STRING, CMD_LEVEL_BASE + levels[i], s);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_SHOW, IsWindowVisible(g_main) ? L"창 숨기기" : L"창 보이기");
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"종료");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void toggle_visible(void)
{
    if (IsWindowVisible(g_main)) {
        ShowWindow(g_main, SW_HIDE);
    } else {
        ShowWindow(g_main, SW_SHOWNOACTIVATE);
        raise_windows();
    }
}

static void add_tray(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon_small;
    wcscpy(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void do_exit(void)
{
    /* 검은 막 구간(백라이트 0%)에서 끄면 원래 모니터 밝기로 되돌림 */
    EnterCriticalSection(&g_hw_cs);
    if (g_hw_original >= 0 && g_level < g_split) g_hw_target = g_hw_original;
    g_hw_stop = 1;
    LeaveCriticalSection(&g_hw_cs);
    WakeConditionVariable(&g_hw_cv);
    overlay_set(255);
    DestroyWindow(g_main);
}

/* 다른 창에 포커스가 있어도, 커서가 조절 창 위에 있으면 휠을 받도록 저수준 훅 사용 */
static LRESULT CALLBACK mouse_hook(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == WM_MOUSEWHEEL && IsWindowVisible(g_main)) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)l;
        HWND under = WindowFromPoint(ms->pt);
        if (under && GetAncestor(under, GA_ROOT) == g_main) {
            short delta = (short)HIWORD(ms->mouseData);
            int mods = (GetAsyncKeyState(VK_SHIFT) < 0 ? 1 : 0) | (GetAsyncKeyState(VK_CONTROL) < 0 ? 2 : 0);
            PostMessageW(g_main, WM_APP_WHEEL, (WPARAM)(LONG_PTR)delta, mods);
            return 1;
        }
    }
    return CallNextHookEx(g_mouse_hook, code, w, l);
}

static void on_wheel(int delta, int mods)
{
    g_wheel_accum += delta;
    int notches = g_wheel_accum / WHEEL_DELTA;
    if (notches == 0) {
        /* 고해상도 휠: 남은 값도 비율로 반영 */
        if (abs(g_wheel_accum) < WHEEL_DELTA / 4) return;
    }
    double n = (double)g_wheel_accum / WHEEL_DELTA;
    g_wheel_accum = 0;
    double mult = (mods & 1) ? 0.5 : ((mods & 2) ? 5.0 : 1.0);
    step(n * STEP * mult);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_APP_WHEEL:
        on_wheel((int)(LONG_PTR)w, (int)l);
        return 0;
    case WM_MOUSEWHEEL: /* 훅이 없을 때 대비 */
        on_wheel(GET_WHEEL_DELTA_WPARAM(w),
                 ((GET_KEYSTATE_WPARAM(w) & MK_SHIFT) ? 1 : 0) | ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) ? 2 : 0));
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT rc = rc_close(), rb = rc_bar_hit();
        if (PtInRect(&rc, pt)) {
            do_exit();
        } else if (PtInRect(&rb, pt)) {
            g_dragging_bar = 1;
            SetCapture(h);
            bar_set_from_x(pt.x);
        } else {
            ReleaseCapture();
            SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); /* 창 끌어서 이동 */
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_hover) {
            g_hover = 1;
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, HOVER_DEFAULT};
            TrackMouseEvent(&t);
            update_window_alpha();
        }
        if (g_dragging_bar) bar_set_from_x(GET_X_LPARAM(l));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = 0;
        update_window_alpha();
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging_bar) {
            g_dragging_bar = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONUP:
        show_menu(h);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_UP || w == VK_RIGHT) step(STEP);
        else if (w == VK_DOWN || w == VK_LEFT) step(-STEP);
        else if (w == VK_ESCAPE) ShowWindow(h, SW_HIDE);
        return 0;
    case WM_HOTKEY:
        if (w == HK_UP) step(STEP);
        else if (w == HK_DOWN) step(-STEP);
        else if (w == HK_RESET) set_level(100);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == CMD_EXIT) do_exit();
        else if (id == CMD_SHOW) toggle_visible();
        else if (id >= CMD_LEVEL_BASE && id <= CMD_LEVEL_BASE + 100) set_level(id - CMD_LEVEL_BASE);
        return 0;
    }
    case WM_APP_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP) toggle_visible();
        else if (LOWORD(l) == WM_RBUTTONUP) show_menu(h);
        return 0;
    case WM_APP_HWREADY: {
        int initial = (int)w;
        g_hw_ready = 1;
        g_hw_original = initial;
        if (initial < 0) g_split = 100.0; /* 하드웨어 제어 불가 -> 전 구간 검은 막 */
        else if (!g_touched) g_level = g_split + initial * (100.0 - g_split) / 100.0;
        apply();
        return 0;
    }
    case WM_TIMER:
        if (w == ID_TIMER_TOP) raise_windows();
        return 0;
    case WM_DISPLAYCHANGE:
        overlay_fit();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        RECT *nr = (RECT *)l;
        SetWindowPos(h, NULL, nr->left, nr->top, S(WIN_W), S(WIN_H), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        do_exit();
        return 0;
    case WM_DESTROY:
        KillTimer(h, ID_TIMER_TOP);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        if (m == g_taskbar_created && m != 0) {
            add_tray();
            update_tray_tip();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------------------------------------------------------- 시작 */
static void set_dpi_awareness(void)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * Ctx)(HANDLE);
    Ctx f = (Ctx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (f && f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    SetProcessDPIAware();
}

static UINT window_dpi(HWND h)
{
    typedef UINT(WINAPI * Fn)(HWND);
    Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (f) return f(h);
    HDC dc = GetDC(NULL);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    g_inst = inst;

    /* 중복 실행 방지: 이미 실행 중이면 그 창을 보여주고 종료 */
    CreateMutexW(NULL, TRUE, L"Local\\BrightnessControl_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"BrightnessControlMain", NULL);
        if (other) {
            ShowWindow(other, SW_SHOWNOACTIVATE);
            SetWindowPos(other, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        return 0;
    }

    set_dpi_awareness();
    InitializeCriticalSection(&g_hw_cs);
    InitializeConditionVariable(&g_hw_cv);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    g_icon_big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"BrightnessControlOverlay";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = main_proc;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_icon_big;
    wc.hIconSm = g_icon_small;
    wc.lpszClassName = L"BrightnessControlMain";
    RegisterClassExW(&wc);

    /* 클릭 통과 + 반투명 + 작업표시줄/Alt+Tab 제외 + 포커스 안 뺏음 */
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                    WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                L"BrightnessControlOverlay", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    SetLayeredWindowAttributes(g_overlay, 0, 0, LWA_ALPHA);

    g_main = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                             L"BrightnessControlMain", APP_NAME, WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    g_dpi = window_dpi(g_main);

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_main, HWND_TOPMOST, wa.right - S(WIN_W) - S(24), wa.bottom - S(WIN_H) - S(24),
                 S(WIN_W), S(WIN_H), SWP_NOACTIVATE);
    g_dpi = window_dpi(g_main);
    SetWindowPos(g_main, NULL, 0, 0, S(WIN_W), S(WIN_H), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    add_tray();
    RegisterHotKey(g_main, HK_UP, MOD_CONTROL | MOD_ALT, VK_PRIOR);
    RegisterHotKey(g_main, HK_DOWN, MOD_CONTROL | MOD_ALT, VK_NEXT);
    RegisterHotKey(g_main, HK_RESET, MOD_CONTROL | MOD_ALT, VK_HOME);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, inst, 0);
    SetTimer(g_main, ID_TIMER_TOP, 1000, NULL);

    apply();
    ShowWindow(g_main, SW_SHOWNOACTIVATE);
    UpdateWindow(g_main);

    g_hw_thread = CreateThread(NULL, 0, hw_thread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mouse_hook) UnhookWindowsHookEx(g_mouse_hook);
    if (g_hw_thread) {
        WaitForSingleObject(g_hw_thread, 4000);
        CloseHandle(g_hw_thread);
    }
    DestroyWindow(g_overlay);
    return 0;
}
