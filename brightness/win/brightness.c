/*
 * 밝기조절 — Windows 모니터 밝기 조절기 (macOS 제어 센터 스타일)
 *
 *  평소에는 작업표시줄 트레이에서 대기. 다음 경우에 패널이 나타났다가 자동으로 사라짐:
 *   - 트레이 아이콘 클릭 / 트레이 아이콘 위에서 마우스 휠
 *   - 전역 단축키 Ctrl+Alt+PageUp / PageDown / Home (화면 아래 가운데 HUD)
 *
 *  50~100% : 모니터 자체 밝기(백라이트) 0~100% — 외장 모니터 DDC/CI, 노트북 WMI
 *   1~50%  : 백라이트 최저 + 화면 전체에 클릭이 통과하는 검은 막(오버레이)
 *     0%   : 완전 검정 (꺼진 것처럼 보임)
 *
 *  실행 인자 --background : 패널을 띄우지 않고 조용히 시작 (자동 실행용)
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
#include <wchar.h>
#include "resource.h"

#define APP_NAME L"밝기조절"
#define STEP 2.0
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"BrightnessControl"

#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_HWREADY (WM_APP + 2)
#define WM_APP_WHEEL   (WM_APP + 3)
#define WM_APP_SHOW    (WM_APP + 4)

enum { HK_UP = 1, HK_DOWN, HK_RESET };
enum { TM_TOP = 1, TM_FADE, TM_IDLE };
enum { CMD_EXIT = 100, CMD_AUTORUN, CMD_SHOW, CMD_LEVEL_BASE = 200 };
enum { WHEEL_FROM_PANEL = 0, WHEEL_FROM_TRAY = 1 };
enum { POS_TRAY, POS_HUD };

/* ---------------------------------------------------------------- 전역 상태 */
static HINSTANCE g_inst;
static HWND g_main, g_overlay;
static HHOOK g_mouse_hook;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;

static double g_level = 100.0;   /* 0..100 */
static double g_split = 50.0;    /* 하드웨어 제어 불가 시 100 */
static int g_hw_ready = 0;
static int g_hw_original = -1;
static int g_touched = 0;
static int g_overlay_trans = -1; /* 0..255, 255 = 숨김 */
static int g_dragging = 0;
static int g_wheel_accum = 0;
static UINT g_dpi = 96;

/* 패널 표시 상태 */
static int g_shown = 0;          /* 보이는 중(또는 나타나는 중) */
static int g_alpha = 0;          /* 페이드 0..255 */
static int g_activated = 0;      /* 클릭으로 열려 포커스를 가진 경우: 바깥 클릭 시 닫힘 */
static DWORD g_last_activity;
static DWORD g_hide_delay = 1600;
static int g_show_hint = 0;      /* 첫 실행 안내 문구 */
static int g_dark = 1;           /* 패널 다크 모드 */
static int g_taskbar_dark = 1;
static DWORD g_deact_tick;       /* 바깥 클릭으로 닫힌 시각 (트레이 재클릭 토글 처리용) */

#define S(v) ((int)lround((v) * (double)g_dpi / 96.0))

/* 패널 디자인 기준(96 DPI) */
#define PW 300      /* 패널 너비 */
#define PH 112      /* 패널 높이 */
#define PM 28       /* 그림자 여백 */
#define PR 18       /* 모서리 반경 */
#define TRACK_X 16
#define TRACK_Y 44
#define TRACK_H 28

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
    /* 작업표시줄 등 다른 최상위 창이 덮어도 오버레이 -> 패널 순서로 다시 맨 위로 */
    if (g_overlay_trans < 255)
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_shown)
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

/* ---------------------------------------------------------------- 렌더링 (안티에일리어싱 + 픽셀 단위 투명도) */
/* 32bpp premultiplied BGRA 캔버스에 SDF(부호 거리)로 도형을 그린다. */
typedef struct { int w, h; UINT32 *px; HDC dc; HBITMAP bmp; HGDIOBJ old; } Canvas;
typedef struct { double r, g, b, a; } Rgba; /* 0..1, straight alpha */

static Canvas g_cv, g_mask;

static void canvas_free(Canvas *c)
{
    if (c->dc) {
        SelectObject(c->dc, c->old);
        DeleteObject(c->bmp);
        DeleteDC(c->dc);
    }
    ZeroMemory(c, sizeof(*c));
}

static void canvas_init(Canvas *c, int w, int h)
{
    if (c->dc && c->w == w && c->h == h) return;
    canvas_free(c);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    c->dc = CreateCompatibleDC(NULL);
    c->bmp = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    c->old = SelectObject(c->dc, c->bmp);
    c->px = (UINT32 *)bits;
    c->w = w;
    c->h = h;
}

static inline void blend_px(UINT32 *d, Rgba c, double cov)
{
    double a = c.a * cov;
    if (a <= 0.0) return;
    if (a > 1.0) a = 1.0;
    UINT32 v = *d;
    double da = ((v >> 24) & 255) / 255.0, dr = ((v >> 16) & 255) / 255.0,
           dg = ((v >> 8) & 255) / 255.0, db = (v & 255) / 255.0;
    double k = 1.0 - a;
    double oa = a + da * k, or_ = c.r * a + dr * k, og = c.g * a + dg * k, ob = c.b * a + db * k;
    *d = ((UINT32)lround(oa * 255) << 24) | ((UINT32)lround(or_ * 255) << 16) |
         ((UINT32)lround(og * 255) << 8) | (UINT32)lround(ob * 255);
}

static double sdf_rrect(double px, double py, double x0, double y0, double x1, double y1, double r)
{
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    double qx = fabs(px - cx) - hx, qy = fabs(py - cy) - hy;
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    double inside = (qx > qy ? qx : qy);
    if (inside > 0) inside = 0;
    return sqrt(ox * ox + oy * oy) + inside - r;
}

static double sdf_capsule(double px, double py, double ax, double ay, double bx, double by, double r)
{
    double pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    double h = (pax * bax + pay * bay) / (bax * bax + bay * bay);
    if (h < 0) h = 0;
    if (h > 1) h = 1;
    double dx = pax - bax * h, dy = pay - bay * h;
    return sqrt(dx * dx + dy * dy) - r;
}

static inline double cov_of(double d) { d = 0.5 - d; return d < 0 ? 0 : (d > 1 ? 1 : d); }

static void fill_rrect(Canvas *c, double x0, double y0, double x1, double y1, double r, Rgba col)
{
    int ix0 = (int)floor(x0) - 1, iy0 = (int)floor(y0) - 1, ix1 = (int)ceil(x1) + 1, iy1 = (int)ceil(y1) + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > c->w) ix1 = c->w;
    if (iy1 > c->h) iy1 = c->h;
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            double cv = cov_of(sdf_rrect(x + 0.5, y + 0.5, x0, y0, x1, y1, r));
            if (cv > 0) blend_px(&c->px[y * c->w + x], col, cv);
        }
}

/* 둥근 사각형 테두리 (안쪽 1px 링) */
static void stroke_rrect(Canvas *c, double x0, double y0, double x1, double y1, double r, double wd, Rgba col)
{
    for (int y = (int)y0; y < (int)ceil(y1) && y < c->h; y++)
        for (int x = (int)x0; x < (int)ceil(x1) && x < c->w; x++) {
            double d = sdf_rrect(x + 0.5, y + 0.5, x0, y0, x1, y1, r);
            double cv = cov_of(d) * cov_of(-d - wd);
            if (cv > 0) blend_px(&c->px[y * c->w + x], col, cv);
        }
}

/* 부드러운 그림자 */
static void shadow_rrect(Canvas *c, double x0, double y0, double x1, double y1, double r, double blur, double alpha)
{
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            double d = sdf_rrect(x + 0.5, y + 0.5, x0, y0, x1, y1, r);
            double t = d <= 0 ? 1.0 : exp(-(d * d) / (2 * blur * blur / 4));
            if (t > 0.002) {
                Rgba k = {0, 0, 0, alpha};
                blend_px(&c->px[y * c->w + x], k, t);
            }
        }
}

/* SF Symbols 'sun.max.fill' 스타일 해 아이콘 */
static void draw_sun(Canvas *c, double cx, double cy, double size, Rgba col)
{
    double core = size * 0.20, r0 = size * 0.33, r1 = size * 0.45, rw = size * 0.045;
    int x0 = (int)(cx - size / 2) - 1, x1 = (int)(cx + size / 2) + 2;
    int y0 = (int)(cy - size / 2) - 1, y1 = (int)(cy + size / 2) + 2;
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= c->h) continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= c->w) continue;
            double px = x + 0.5, py = y + 0.5;
            double d = sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - core;
            for (int i = 0; i < 8; i++) {
                double a = i * 3.14159265358979 / 4;
                double dd = sdf_capsule(px, py, cx + r0 * cos(a), cy + r0 * sin(a),
                                        cx + r1 * cos(a), cy + r1 * sin(a), rw);
                if (dd < d) d = dd;
            }
            double cv = cov_of(d);
            if (cv > 0) blend_px(&c->px[y * c->w + x], col, cv);
        }
    }
}

/* 텍스트: GDI로 흑백 마스크에 그린 뒤 커버리지로 색을 합성 */
static HFONT make_font(const wchar_t *face, int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static void draw_text(Canvas *c, const wchar_t *s, RECT rc, HFONT f, UINT fmt, Rgba col)
{
    canvas_init(&g_mask, c->w, c->h);
    RECT clr = rc;
    if (clr.left < 0) clr.left = 0;
    if (clr.top < 0) clr.top = 0;
    if (clr.right > c->w) clr.right = c->w;
    if (clr.bottom > c->h) clr.bottom = c->h;
    for (int y = clr.top; y < clr.bottom; y++)
        for (int x = clr.left; x < clr.right; x++) g_mask.px[y * c->w + x] = 0;
    HGDIOBJ of = SelectObject(g_mask.dc, f);
    SetBkMode(g_mask.dc, TRANSPARENT);
    SetTextColor(g_mask.dc, RGB(255, 255, 255));
    DrawTextW(g_mask.dc, s, -1, &rc, fmt | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(g_mask.dc, of);
    GdiFlush();
    for (int y = clr.top; y < clr.bottom; y++)
        for (int x = clr.left; x < clr.right; x++) {
            int g = (g_mask.px[y * c->w + x] >> 8) & 255;
            if (g) blend_px(&c->px[y * c->w + x], col, g / 255.0);
        }
}

/* ---------------------------------------------------------------- 패널 */
static RECT panel_rect_client(void) /* 창 안에서 패널(그림자 제외) 위치 */
{
    RECT r = {S(PM), S(PM), S(PM) + S(PW), S(PM) + S(PH)};
    return r;
}

static void track_geom(double *x0, double *y0, double *x1, double *y1)
{
    RECT p = panel_rect_client();
    *x0 = p.left + S(TRACK_X);
    *x1 = p.right - S(TRACK_X);
    *y0 = p.top + S(TRACK_Y);
    *y1 = *y0 + S(TRACK_H);
}

static void format_level(wchar_t *buf, size_t n)
{
    if (g_level < 10 && g_level != floor(g_level))
        swprintf(buf, n, L"%.1f%%", g_level);
    else
        swprintf(buf, n, L"%d%%", (int)lround(g_level));
}

static void mode_text(wchar_t *buf, size_t n)
{
    int hw = hw_percent();
    if (g_show_hint)
        wcsncpy(buf, L"아이콘 클릭 또는 아이콘 위에서 휠로 조절", n);
    else if (!g_hw_ready)
        wcsncpy(buf, L"모니터 확인 중…", n);
    else if (g_level <= 0)
        wcsncpy(buf, L"화면 꺼짐 · Ctrl+Alt+Home 으로 복구", n);
    else if (g_level < g_split)
        wcsncpy(buf, g_split < 100 ? L"모니터 최저 밝기 + 화면 어둡게" : L"화면 어둡게", n);
    else if (hw >= 0)
        swprintf(buf, n, L"모니터 밝기 %d%%", hw);
    else
        wcsncpy(buf, L"원래 밝기", n);
    buf[n - 1] = 0;
}

static void render_panel(void)
{
    int W = S(PW + 2 * PM), H = S(PH + 2 * PM);
    canvas_init(&g_cv, W, H);
    ZeroMemory(g_cv.px, (size_t)W * H * 4);

    /* 색상: macOS 다크/라이트 */
    Rgba bg, border, primary, secondary, track, fill, glyph;
    if (g_dark) {
        bg = (Rgba){0.141, 0.141, 0.149, 0.97};
        border = (Rgba){1, 1, 1, 0.13};
        primary = (Rgba){0.961, 0.961, 0.969, 1};
        secondary = (Rgba){0.922, 0.922, 0.961, 0.55};
        track = (Rgba){1, 1, 1, 0.11};
        fill = (Rgba){1, 1, 1, 0.96};
        glyph = (Rgba){0.24, 0.24, 0.26, 0.75};
    } else {
        bg = (Rgba){0.965, 0.965, 0.973, 0.97};
        border = (Rgba){0, 0, 0, 0.10};
        primary = (Rgba){0.114, 0.114, 0.122, 1};
        secondary = (Rgba){0.235, 0.235, 0.263, 0.6};
        track = (Rgba){0, 0, 0, 0.09};
        fill = (Rgba){1, 1, 1, 1};
        glyph = (Rgba){0.24, 0.24, 0.26, 0.7};
    }

    RECT p = panel_rect_client();
    double r = S(PR);
    shadow_rrect(&g_cv, p.left, p.top + S(6), p.right, p.bottom + S(6), r, S(22), g_dark ? 0.45 : 0.22);
    fill_rrect(&g_cv, p.left, p.top, p.right, p.bottom, r, bg);
    stroke_rrect(&g_cv, p.left, p.top, p.right, p.bottom, r, 1.0, border);

    HFONT f_title = make_font(L"Malgun Gothic", 13, FW_BOLD);
    HFONT f_value = make_font(L"Segoe UI Semibold", 13, FW_SEMIBOLD);
    HFONT f_caption = make_font(L"Malgun Gothic", 12, FW_NORMAL);

    RECT rt = {p.left + S(16), p.top + S(12), p.right - S(60), p.top + S(36)};
    draw_text(&g_cv, L"디스플레이", rt, f_title, DT_LEFT | DT_VCENTER, primary);
    wchar_t lv[16];
    format_level(lv, 16);
    RECT rv = {p.right - S(80), p.top + S(12), p.right - S(16), p.top + S(36)};
    draw_text(&g_cv, lv, rv, f_value, DT_RIGHT | DT_VCENTER, secondary);

    /* 캡슐 슬라이더: 채움은 최소 트랙 높이(원)부터 시작 — macOS 제어 센터와 동일 */
    double tx0, ty0, tx1, ty1;
    track_geom(&tx0, &ty0, &tx1, &ty1);
    double th = ty1 - ty0, tr = th / 2;
    fill_rrect(&g_cv, tx0, ty0, tx1, ty1, tr, track);
    double fw = th + (tx1 - tx0 - th) * g_level / 100.0;
    if (!g_dark) shadow_rrect(&g_cv, tx0, ty0 + 1, tx0 + fw, ty1 + 1, tr, S(3), 0.10);
    fill_rrect(&g_cv, tx0, ty0, tx0 + fw, ty1, tr, fill);
    draw_sun(&g_cv, tx0 + tr, ty0 + tr, th * 0.62, glyph);

    wchar_t mode[96];
    mode_text(mode, 96);
    RECT rm = {p.left + S(16), p.top + S(80), p.right - S(16), p.top + S(100)};
    draw_text(&g_cv, mode, rm, f_caption, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, secondary);

    DeleteObject(f_title);
    DeleteObject(f_value);
    DeleteObject(f_caption);
}

static void present(void)
{
    RECT wr;
    GetWindowRect(g_main, &wr);
    POINT dst = {wr.left, wr.top}, src = {0, 0};
    SIZE sz = {g_cv.w, g_cv.h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)g_alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_main, NULL, &dst, &sz, g_cv.dc, &src, 0, &bf, ULW_ALPHA);
}

static void redraw(void)
{
    if (!g_shown && g_alpha == 0) return;
    render_panel();
    present();
}

static void read_theme(void)
{
    DWORD v = 0, sz = sizeof(v);
    g_dark = 1;
    g_taskbar_dark = 1;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        g_dark = v == 0;
    sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        g_taskbar_dark = v == 0;
}

static void place_panel(int where)
{
    int W = S(PW + 2 * PM), H = S(PH + 2 * PM);
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = where == POS_HUD ? MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY)
                                    : MonitorFromWindow(FindWindowW(L"Shell_TrayWnd", NULL), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int x, y;
    if (where == POS_HUD) {
        x = (wa.left + wa.right) / 2 - W / 2;
        y = wa.bottom - S(72) - S(PH) - S(PM);
    } else {
        /* 트레이 근처 (작업표시줄 위치에 맞춤) */
        RECT mr = mi.rcMonitor;
        x = wa.right - S(12) - S(PW) - S(PM);
        y = wa.bottom - S(12) - S(PH) - S(PM);
        if (wa.top > mr.top) y = wa.top + S(12) - S(PM);          /* 작업표시줄이 위쪽 */
        if (wa.left > mr.left) x = wa.left + S(12) - S(PM);       /* 작업표시줄이 왼쪽 */
    }
    SetWindowPos(g_main, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE);
}

static void show_panel(int where, int activate, DWORD hide_delay)
{
    g_last_activity = GetTickCount();
    g_hide_delay = hide_delay;
    if (!g_shown || g_alpha == 0) {
        place_panel(where);
        g_alpha = 0;
    }
    g_shown = 1;
    render_panel();
    present();
    ShowWindow(g_main, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) {
        SetForegroundWindow(g_main);
        g_activated = 1;
    }
    raise_windows();
    SetTimer(g_main, TM_FADE, 15, NULL);
    SetTimer(g_main, TM_IDLE, 150, NULL);
}

static void hide_panel(void)
{
    if (!g_shown) return;
    g_shown = 0;
    g_activated = 0;
    g_dragging = 0;
    ReleaseCapture();
    SetTimer(g_main, TM_FADE, 15, NULL);
}

static void fade_step(void)
{
    int target = g_shown ? 255 : 0;
    int speed = g_shown ? 48 : 28;
    if (g_alpha < target) g_alpha = g_alpha + speed > 255 ? 255 : g_alpha + speed;
    else if (g_alpha > target) g_alpha = g_alpha - speed < 0 ? 0 : g_alpha - speed;
    present();
    if (g_alpha == target) {
        KillTimer(g_main, TM_FADE);
        if (!g_shown) {
            ShowWindow(g_main, SW_HIDE);
            KillTimer(g_main, TM_IDLE);
            g_show_hint = 0;
        }
    }
}

static int cursor_in_panel(void)
{
    if (!IsWindowVisible(g_main)) return 0;
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(g_main, &wr);
    RECT p = panel_rect_client();
    OffsetRect(&p, wr.left, wr.top);
    return PtInRect(&p, pt);
}

/* ---------------------------------------------------------------- 트레이 */
static HICON tray_icon(void)
{
    /* DPI 인식 프로세스이므로 SM_CXSMICON 이 현재 배율에 맞는 크기(16/20/24/32…)를 돌려줌 */
    int sz = GetSystemMetrics(SM_CXSMICON);
    return (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(g_taskbar_dark ? IDI_TRAY_WHITE : IDI_TRAY_BLACK),
                             IMAGE_ICON, sz, sz, 0);
}

static void update_tray(int add)
{
    if (add) {
        ZeroMemory(&g_nid, sizeof(g_nid));
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = g_main;
        g_nid.uID = 1;
        g_nid.uCallbackMessage = WM_APP_TRAY;
    }
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    g_nid.hIcon = tray_icon();
    wchar_t lv[16];
    format_level(lv, 16);
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"밝기 %ls · 아이콘 위에서 휠로 조절", lv);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

static void update_tray_tip(void)
{
    wchar_t lv[16];
    format_level(lv, 16);
    swprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"밝기 %ls · 아이콘 위에서 휠로 조절", lv);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static int cursor_on_tray(POINT pt)
{
    NOTIFYICONIDENTIFIER id;
    ZeroMemory(&id, sizeof(id));
    id.cbSize = sizeof(id);
    id.hWnd = g_main;
    id.uID = 1;
    RECT r;
    if (FAILED(Shell_NotifyIconGetRect(&id, &r))) return 0;
    return PtInRect(&r, pt);
}

/* ---------------------------------------------------------------- 자동 실행 */
static int autorun_enabled(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void autorun_set(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --background", exe);
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}

/* ---------------------------------------------------------------- 밝기 적용 */
static void apply(void)
{
    overlay_set(overlay_transmission(g_level, g_split));
    int hw = hw_percent();
    if (hw >= 0 && g_hw_ready) hw_request(hw);
    update_tray_tip();
    redraw();
}

static void set_level(double v)
{
    g_touched = 1;
    g_show_hint = 0;
    v = round(v * 2.0) / 2.0;
    g_level = v < 0 ? 0 : (v > 100 ? 100 : v);
    g_last_activity = GetTickCount();
    apply();
}

static void step(double d) { set_level(g_level + d); }

static void track_set_from_x(int x)
{
    double tx0, ty0, tx1, ty1;
    track_geom(&tx0, &ty0, &tx1, &ty1);
    double th = ty1 - ty0;
    double t = (x - tx0 - th / 2) / (tx1 - tx0 - th);
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
    AppendMenuW(m, MF_STRING | (autorun_enabled() ? MF_CHECKED : 0), CMD_AUTORUN, L"Windows 시작 시 자동 실행");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"종료");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, h, NULL);
    PostMessageW(h, WM_NULL, 0, 0);
    DestroyMenu(m);
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

/* 저수준 마우스 훅: 패널 위 또는 트레이 아이콘 위의 휠을 가로챔 (포커스와 무관하게 동작) */
static LRESULT CALLBACK mouse_hook(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && w == WM_MOUSEWHEEL) {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)l;
        int src = -1;
        if (IsWindowVisible(g_main) && g_shown) {
            HWND under = WindowFromPoint(ms->pt);
            if (under && GetAncestor(under, GA_ROOT) == g_main) src = WHEEL_FROM_PANEL;
        }
        if (src < 0 && cursor_on_tray(ms->pt)) src = WHEEL_FROM_TRAY;
        if (src >= 0) {
            short delta = (short)HIWORD(ms->mouseData);
            int mods = (GetAsyncKeyState(VK_SHIFT) < 0 ? 1 : 0) | (GetAsyncKeyState(VK_CONTROL) < 0 ? 2 : 0);
            PostMessageW(g_main, WM_APP_WHEEL, (WPARAM)(LONG_PTR)delta, (LPARAM)(mods | (src << 4)));
            return 1;
        }
    }
    return CallNextHookEx(g_mouse_hook, code, w, l);
}

static void on_wheel(int delta, int mods, int src)
{
    if (src == WHEEL_FROM_TRAY && !g_shown) show_panel(POS_TRAY, 0, 1600);
    g_wheel_accum += delta;
    if (abs(g_wheel_accum) < WHEEL_DELTA / 4) return; /* 고해상도 휠 대응 */
    double n = (double)g_wheel_accum / WHEEL_DELTA;
    g_wheel_accum = 0;
    double mult = (mods & 1) ? 0.5 : ((mods & 2) ? 5.0 : 1.0);
    step(n * STEP * mult);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
        return g_activated ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_APP_WHEEL:
        on_wheel((int)(LONG_PTR)w, (int)(l & 0xF), (int)(l >> 4));
        return 0;
    case WM_MOUSEWHEEL: /* 훅이 없을 때 대비 */
        on_wheel(GET_WHEEL_DELTA_WPARAM(w),
                 ((GET_KEYSTATE_WPARAM(w) & MK_SHIFT) ? 1 : 0) | ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) ? 2 : 0),
                 WHEEL_FROM_PANEL);
        return 0;
    case WM_LBUTTONDOWN: {
        double tx0, ty0, tx1, ty1;
        track_geom(&tx0, &ty0, &tx1, &ty1);
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (x >= tx0 - S(4) && x <= tx1 + S(4) && y >= ty0 - S(8) && y <= ty1 + S(8)) {
            g_dragging = 1;
            SetCapture(h);
            track_set_from_x(x);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        g_last_activity = GetTickCount();
        if (g_dragging) track_set_from_x(GET_X_LPARAM(l));
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONUP:
        show_menu(h);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_UP || w == VK_RIGHT) step(STEP);
        else if (w == VK_DOWN || w == VK_LEFT) step(-STEP);
        else if (w == VK_ESCAPE || w == VK_RETURN) hide_panel();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_activated) {
            g_deact_tick = GetTickCount();
            hide_panel();
        }
        return 0;
    case WM_HOTKEY:
        if (w == HK_UP) step(STEP);
        else if (w == HK_DOWN) step(-STEP);
        else if (w == HK_RESET) set_level(100);
        if (!g_shown || !g_activated) show_panel(g_shown ? POS_TRAY : POS_HUD, 0, 1300);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == CMD_EXIT) do_exit();
        else if (id == CMD_AUTORUN) autorun_set(!autorun_enabled());
        else if (id == CMD_SHOW) show_panel(POS_TRAY, 1, 2500);
        else if (id >= CMD_LEVEL_BASE && id <= CMD_LEVEL_BASE + 100) {
            set_level(id - CMD_LEVEL_BASE);
            show_panel(POS_TRAY, 0, 1300);
        }
        return 0;
    }
    case WM_APP_TRAY:
        if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == NIN_SELECT || LOWORD(l) == NIN_KEYSELECT) {
            /* 열린 상태에서 아이콘을 다시 누르면 닫힘 (포커스를 잃으며 이미 닫혔으면 다시 열지 않음) */
            if (g_shown && g_activated) hide_panel();
            else if (GetTickCount() - g_deact_tick > 400) show_panel(POS_TRAY, 1, 2500);
        } else if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            show_menu(h);
        }
        return 0;
    case WM_APP_SHOW:
        g_show_hint = (int)w;
        show_panel(POS_TRAY, 0, w ? 4000 : 2500);
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
        if (w == TM_TOP) {
            raise_windows();
        } else if (w == TM_FADE) {
            fade_step();
        } else if (w == TM_IDLE) {
            /* 사용하지 않으면 자동으로 사라짐 (마우스가 패널 위에 있거나 드래그 중이면 유지) */
            if (g_dragging || cursor_in_panel()) g_last_activity = GetTickCount();
            else if (g_shown && GetTickCount() - g_last_activity > g_hide_delay) hide_panel();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t *)l, L"ImmersiveColorSet") == 0) {
            read_theme();
            update_tray(0);
            redraw();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        overlay_fit();
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(w);
        redraw();
        return 0;
    case WM_CLOSE:
        hide_panel();
        return 0;
    case WM_DESTROY:
        KillTimer(h, TM_TOP);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        if (m == g_taskbar_created && m != 0) {
            update_tray(1);
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

static UINT system_dpi(void)
{
    typedef UINT(WINAPI * Fn)(void);
    Fn f = (Fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
    if (f) return f();
    HDC dc = GetDC(NULL);
    UINT d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)show;
    g_inst = inst;
    int background = cmd && wcsstr(cmd, L"--background") != NULL;

    /* 중복 실행 방지: 이미 실행 중이면 그 패널을 띄우고 종료 */
    CreateMutexW(NULL, TRUE, L"Local\\BrightnessControl_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"BrightnessControlMain", NULL);
        if (other && !background) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    set_dpi_awareness();
    g_dpi = system_dpi();
    read_theme();
    InitializeCriticalSection(&g_hw_cs);
    InitializeConditionVariable(&g_hw_cv);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

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
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"BrightnessControlMain";
    RegisterClassExW(&wc);

    /* 클릭 통과 + 반투명 + 작업표시줄/Alt+Tab 제외 + 포커스 안 뺏음 */
    g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                    WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                L"BrightnessControlOverlay", L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    SetLayeredWindowAttributes(g_overlay, 0, 0, LWA_ALPHA);

    /* 패널: 픽셀 단위 투명도(둥근 모서리 + 그림자) */
    g_main = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                             L"BrightnessControlMain", APP_NAME, WS_POPUP, 0, 0,
                             S(PW + 2 * PM), S(PH + 2 * PM), NULL, NULL, inst, NULL);

    update_tray(1);
    RegisterHotKey(g_main, HK_UP, MOD_CONTROL | MOD_ALT, VK_PRIOR);
    RegisterHotKey(g_main, HK_DOWN, MOD_CONTROL | MOD_ALT, VK_NEXT);
    RegisterHotKey(g_main, HK_RESET, MOD_CONTROL | MOD_ALT, VK_HOME);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, inst, 0);
    SetTimer(g_main, TM_TOP, 1000, NULL);

    apply();
    if (!background) PostMessageW(g_main, WM_APP_SHOW, 1, 0); /* 첫 실행: 안내와 함께 잠깐 표시 */

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
