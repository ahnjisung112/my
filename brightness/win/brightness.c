/*
 * 밝기조절 — Windows 모니터 밝기 조절기 (macOS 제어 센터 스타일)
 *
 *  평소에는 작업표시줄 트레이에서 대기. 다음 경우에 패널이 나타났다가 자동으로 사라짐:
 *   - 트레이 아이콘 클릭 / 트레이 아이콘 위에서 마우스 휠
 *   - 전역 단축키 Ctrl+Alt+PageUp / PageDown / Home (화면 아래 가운데 HUD)
 *
 *  모니터마다 따로 조절 (패널에 모니터별 슬라이더)
 *  20~100% : 모니터 자체 밝기(백라이트) 0~100% — 외장 모니터 DDC/CI, 노트북 WMI
 *   1~20%  : 백라이트 최저 + 그 모니터에 클릭이 통과하는 검은 막(오버레이)
 *     0%   : 완전 검정 (꺼진 것처럼 보임)
 *
 *  밝기 예약: 패널의 시계 버튼 -> 시각·밝기를 휠로 설정 (예: 07:00 에 100%)
 *           절전 중에 지나간 예약은 깨어날 때 가장 최근 것을 적용
 *
 *  실행 인자 --background : 패널을 띄우지 않고 조용히 시작 (자동 실행용)
 *           --replace    : 실행 중인 인스턴스를 종료시키고 대신 실행 (모니터 구성 변경 시 재시작용)
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
#define APP_VERSION 6   /* 실행 중인 예전 버전을 교체할 때 비교 */
#define STEP 2.0
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"BrightnessControl"
#define SETTINGS_KEY L"Software\\BrightnessControl"
#define HW_SPLIT 20.0  /* 20~100%: 모니터 백라이트, 0~20%: 검은 막(백라이트 최저보다 더 어둡게) */
#define MAX_MON 8
#define MAX_PHYS_PER 4

#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_HWREADY (WM_APP + 2)
#define WM_APP_WHEEL   (WM_APP + 3)
#define WM_APP_SHOW    (WM_APP + 4)

enum { HK_UP = 1, HK_DOWN, HK_RESET };
enum { TM_TOP = 1, TM_FADE, TM_IDLE, TM_DISPLAY, TM_SCHED, TM_RESUME, TM_DISPLAY_ON };
enum { CMD_EXIT = 100, CMD_AUTORUN, CMD_SHOW, CMD_RESTORE, CMD_USE_HW_BASE = 150, CMD_LEVEL_BASE = 200 };
enum { WHEEL_FROM_PANEL = 0, WHEEL_FROM_TRAY = 1 };
enum { POS_TRAY, POS_HUD };

/* ---------------------------------------------------------------- 모니터 */
typedef struct {
    /* 메인 스레드 */
    HMONITOR hmon;
    RECT rc;
    wchar_t dev[CCHDEVICENAME];
    wchar_t name[64];
    HWND overlay;
    int overlay_trans;   /* 0..255, 255 = 숨김, -1 = 아직 없음 */
    double level;        /* 0..100 */
    double split;        /* 모니터 밝기를 쓰면 HW_SPLIT, 아니면 100 (전 구간 검은 막) */
    int use_hw;          /* 설정: 이 모니터의 밝기를 직접 조절 */
    int hw_original;     /* 시작 시 모니터 밝기 (-1 = 조절 불가) — HWREADY 이후 유효 */
    /* 작업 스레드 */
    HANDLE phys[MAX_PHYS_PER];
    DWORD mn[MAX_PHYS_PER], mx[MAX_PHYS_PER];
    int nphys;
    int wmi;             /* 노트북 내장 화면 (WMI 로 조절) */
    int target;          /* 요청값 (-1 = 없음), g_hw_cs 로 보호 */
    int force;           /* 같은 값이어도 다시 보냄 (화면이 다시 켜졌을 때), g_hw_cs 로 보호 */
    int last;
} Mon;

static Mon g_mon[MAX_MON];
static int g_nmon;

/* ---------------------------------------------------------------- 전역 상태 */
static HINSTANCE g_inst;
static HWND g_main;
static HHOOK g_mouse_hook;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;

static int g_hw_ready = 0;
static int g_touched = 0;
static int g_active = 0;         /* 마지막으로 조절한 모니터 (캡션 표시용) */
static int g_drag_row = -1;
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
static int g_close_hover = 0;    /* 닫기(✕) 버튼 위에 커서 */

/* 밝기 예약 */
#define MAX_SCHED 6
typedef struct { int h, m, level, on; int fired_day; } Sched;
static Sched g_sched[MAX_SCHED];
static int g_nsched;
static int g_view = 0;           /* 0 = 밝기, 1 = 예약 */
static int g_hover_field = -1;   /* 휠/클릭할 수 있는 항목 위에 커서 (강조 표시) */
static wchar_t g_notice[96];     /* 잠깐 보여줄 캡션 (예약 실행 알림) */
static DWORD g_notice_until;
static ULONGLONG g_suspend_min;  /* 절전에 들어간 시각 (분 단위, 0 = 없음) */
static UINT g_msg_version, g_msg_quit;
static DWORD g_deact_tick;       /* 바깥 클릭으로 닫힌 시각 (트레이 재클릭 토글 처리용) */

#define S(v) ((int)lround((v) * (double)g_dpi / 96.0))

/* 패널 디자인 기준(96 DPI) */
#define PW 300      /* 패널 너비 */
#define PM 28       /* 그림자 여백 */
#define PR 18       /* 모서리 반경 */
#define PAD 16
#define ROWS_Y 44   /* 첫 모니터 줄 시작 */
#define ROW_H 58    /* 모니터 한 줄: 이름 18 + 간격 4 + 슬라이더 26 + 간격 10 */
#define LABEL_H 18
#define TRACK_DY 22
#define TRACK_H 26
#define SCHED_Y 48  /* 예약 화면: 첫 줄 시작 */
#define SCHED_H 46  /* 예약 한 줄 */
#define PH panel_h() /* 패널 높이 (캡션 포함) */

static int panel_h(void)
{
    if (g_view == 1) return SCHED_Y + SCHED_H * g_nsched + (g_nsched < MAX_SCHED ? 40 : 0) + 34;
    return ROWS_Y + ROW_H * (g_nmon > 0 ? g_nmon : 1) + 28;
}

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

static int hw_percent(const Mon *m)
{
    if (m->split >= 100.0) return -1;
    double v = m->level - m->split;
    if (v < 0) v = 0;
    return (int)lround(v * 100.0 / (100.0 - m->split));
}

/* 시작 시 모니터 밝기 -> 슬라이더 위치 */
static double level_from_hw(const Mon *m)
{
    if (m->hw_original < 0 || !m->use_hw) return 100.0;
    return HW_SPLIT + m->hw_original * (100.0 - HW_SPLIT) / 100.0;
}

/* ---------------------------------------------------------------- 모니터 목록 */
static BOOL CALLBACK enum_collect(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp)
{
    (void)hdc; (void)rc; (void)lp;
    if (g_nmon >= MAX_MON) return FALSE;
    Mon *m = &g_mon[g_nmon];
    ZeroMemory(m, sizeof(*m));
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (MONITORINFO *)&mi)) return TRUE;
    m->hmon = hmon;
    m->rc = mi.rcMonitor;
    wcsncpy(m->dev, mi.szDevice, CCHDEVICENAME - 1);
    g_nmon++;
    return TRUE;
}

/* 모니터 이름 (EDID 이름, 예: "LG ULTRAGEAR") */
static void monitor_names(void)
{
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np > 0) {
        DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof(*paths));
        DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof(*modes));
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
                ZeroMemory(&src, sizeof(src));
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof(src);
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
                ZeroMemory(&tgt, sizeof(tgt));
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof(tgt);
                tgt.header.adapterId = paths[i].targetInfo.adapterId;
                tgt.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS || !tgt.monitorFriendlyDeviceName[0])
                    continue;
                for (int k = 0; k < g_nmon; k++)
                    if (!g_mon[k].name[0] && wcscmp(g_mon[k].dev, src.viewGdiDeviceName) == 0)
                        wcsncpy(g_mon[k].name, tgt.monitorFriendlyDeviceName, 63);
            }
        }
        free(paths);
        free(modes);
    }
    for (int k = 0; k < g_nmon; k++) {
        if (!g_mon[k].name[0]) swprintf(g_mon[k].name, 64, L"모니터 %d", k + 1);
        /* 같은 이름이 있으면 번호를 붙임 */
        for (int j = 0; j < k; j++)
            if (wcscmp(g_mon[j].name, g_mon[k].name) == 0) {
                wchar_t t[64];
                swprintf(t, 64, L"%ls (%d)", g_mon[k].name, k + 1);
                wcscpy(g_mon[k].name, t);
                break;
            }
    }
}

static int setting_get(const wchar_t *name, int def);
static void setting_set(const wchar_t *name, int value);

static void use_hw_key(const Mon *m, wchar_t *buf, size_t n)
{
    swprintf(buf, n, L"UseHardware:%ls", m->name);
}

static void enumerate_monitors(void)
{
    g_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, enum_collect, 0);
    /* 화면 배치 순서(왼쪽 -> 오른쪽, 위 -> 아래)로 정렬 */
    for (int i = 1; i < g_nmon; i++)
        for (int j = i; j > 0; j--) {
            RECT a = g_mon[j - 1].rc, b = g_mon[j].rc;
            if (b.left < a.left || (b.left == a.left && b.top < a.top)) {
                Mon t = g_mon[j - 1];
                g_mon[j - 1] = g_mon[j];
                g_mon[j] = t;
            }
        }
#ifdef FAKE_DUAL /* 테스트 전용: 모니터 하나를 좌우로 나눠 듀얼 모니터처럼 */
    if (g_nmon == 1) {
        g_mon[1] = g_mon[0];
        g_mon[0].rc.right = g_mon[1].rc.left = (g_mon[0].rc.left + g_mon[0].rc.right) / 2;
        g_nmon = 2;
    }
#endif
    monitor_names();
    for (int i = 0; i < g_nmon; i++) {
        Mon *m = &g_mon[i];
        wchar_t key[96];
        use_hw_key(m, key, 96);
        m->use_hw = setting_get(key, 1);
        m->hw_original = -1;
        m->split = 100.0;   /* 모니터 밝기 확인 전에는 검은 막만 */
        m->level = 100.0;
        m->overlay_trans = -1;
        m->target = -1;
        m->last = -1;
    }
}

/* ---------------------------------------------------------------- 하드웨어: DDC/CI */
static int ddc_set(Mon *m, int pct)
{
    int ok = 1;
    for (int i = 0; i < m->nphys; i++) {
        DWORD v = m->mn[i] + (DWORD)lround((m->mx[i] - m->mn[i]) * pct / 100.0);
        if (!SetMonitorBrightness(m->phys[i], v)) ok = 0;
    }
    return ok;
}

static int ddc_open(Mon *m)
{
    int orig = -1;
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(m->hmon, &n) || n == 0) return -1;
    PHYSICAL_MONITOR *arr = (PHYSICAL_MONITOR *)calloc(n, sizeof(*arr));
    if (!arr) return -1;
    if (GetPhysicalMonitorsFromHMONITOR(m->hmon, n, arr)) {
        for (DWORD j = 0; j < n; j++) {
            DWORD mn, cur, mx;
            if (m->nphys < MAX_PHYS_PER &&
                GetMonitorBrightness(arr[j].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn) {
                if (m->nphys == 0) orig = (int)lround((cur - mn) * 100.0 / (mx - mn));
                m->phys[m->nphys] = arr[j].hPhysicalMonitor;
                m->mn[m->nphys] = mn;
                m->mx[m->nphys] = mx;
                m->nphys++;
            } else {
                DestroyPhysicalMonitor(arr[j].hPhysicalMonitor);
            }
        }
    }
    free(arr);
    return orig;
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
/* DDC/WMI 호출은 느려서(수십~수백 ms) 별도 스레드가 모니터별 '마지막 요청값'만 적용한다. */
static CRITICAL_SECTION g_hw_cs;
static CONDITION_VARIABLE g_hw_cv;
static int g_hw_stop = 0;
static HANDLE g_hw_thread;

static void hw_request(int i, int pct)
{
    EnterCriticalSection(&g_hw_cs);
    g_mon[i].target = pct;
    LeaveCriticalSection(&g_hw_cs);
    WakeConditionVariable(&g_hw_cv);
}

static DWORD WINAPI hw_thread(LPVOID p)
{
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

    int orig[MAX_MON];
    for (int i = 0; i < g_nmon; i++) orig[i] = ddc_open(&g_mon[i]);

    /* 노트북 내장 화면: DDC 가 안 되는 첫 모니터를 WMI 로 조절 */
    if (wmi_init()) {
        int cur = wmi_get();
        int used = 0;
        if (cur >= 0)
            for (int i = 0; i < g_nmon; i++)
                if (g_mon[i].nphys == 0) {
                    g_mon[i].wmi = 1;
                    orig[i] = cur;
                    used = 1;
                    break;
                }
        if (!used) {
            IWbemServices_Release(g_wmi);
            g_wmi = NULL;
        }
    }

    EnterCriticalSection(&g_hw_cs);
    for (int i = 0; i < g_nmon; i++) {
        g_mon[i].hw_original = orig[i];
        g_mon[i].last = orig[i];
    }
    LeaveCriticalSection(&g_hw_cs);
    PostMessageW(g_main, WM_APP_HWREADY, 0, 0);

    /* 모니터가 대기(절전) 상태면 DDC 가 실패한다: 5초 간격으로 최대 2분간 다시 시도 */
    int retry[MAX_MON], tries[MAX_MON];
    for (int i = 0; i < MAX_MON; i++) retry[i] = -1, tries[i] = 0;
    for (;;) {
        int t[MAX_MON], f[MAX_MON], any = 0;
        EnterCriticalSection(&g_hw_cs);
        for (;;) {
            int pending = 0;
            any = 0;
            for (int i = 0; i < g_nmon; i++) {
                if (g_mon[i].target >= 0) any = 1;
                if (retry[i] >= 0) pending = 1;
            }
            if (any || g_hw_stop) break;
            if (pending) {
                if (!SleepConditionVariableCS(&g_hw_cv, &g_hw_cs, 5000)) break; /* 시간 초과 -> 재시도 */
            } else {
                SleepConditionVariableCS(&g_hw_cv, &g_hw_cs, INFINITE);
            }
        }
        for (int i = 0; i < g_nmon; i++) {
            if (g_mon[i].target >= 0) {
                t[i] = g_mon[i].target;
                tries[i] = 0;
            } else {
                t[i] = retry[i];
            }
            f[i] = g_mon[i].force || retry[i] >= 0;
            g_mon[i].target = -1;
            g_mon[i].force = 0;
            retry[i] = -1;
        }
        int stopping = g_hw_stop;
        LeaveCriticalSection(&g_hw_cs);

        for (int i = 0; i < g_nmon; i++) {
            Mon *m = &g_mon[i];
            if (t[i] < 0 || (t[i] == m->last && !f[i])) continue;
            int ok = 1;
            if (m->nphys) ok = ddc_set(m, t[i]);
            if (m->wmi) wmi_set(t[i]);
            if (ok) {
                m->last = t[i];
            } else if (!stopping && ++tries[i] <= 24) {
                retry[i] = t[i];
            }
        }
        if (stopping) break;
    }

    for (int i = 0; i < g_nmon; i++)
        for (int j = 0; j < g_mon[i].nphys; j++) DestroyPhysicalMonitor(g_mon[i].phys[j]);
    if (g_wmi) IWbemServices_Release(g_wmi);
    CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------------- 오버레이 (모니터별) */
static void raise_windows(void)
{
    /* 작업표시줄 등 다른 최상위 창이 덮어도 오버레이 -> 패널 순서로 다시 맨 위로 */
    for (int i = 0; i < g_nmon; i++)
        if (g_mon[i].overlay_trans >= 0 && g_mon[i].overlay_trans < 255)
            SetWindowPos(g_mon[i].overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (g_shown)
        SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void overlay_fit(Mon *m)
{
    SetWindowPos(m->overlay, HWND_TOPMOST, m->rc.left, m->rc.top,
                 m->rc.right - m->rc.left, m->rc.bottom - m->rc.top, SWP_NOACTIVATE);
}

static void overlay_set(Mon *m, int trans)
{
    if (trans == m->overlay_trans) return;
    int was_hidden = m->overlay_trans < 0 || m->overlay_trans >= 255;
    m->overlay_trans = trans;
    if (trans >= 255) {
        ShowWindow(m->overlay, SW_HIDE);
        return;
    }
    SetLayeredWindowAttributes(m->overlay, 0, (BYTE)(255 - trans), LWA_ALPHA);
    if (was_hidden) {
        overlay_fit(m);
        ShowWindow(m->overlay, SW_SHOWNOACTIVATE);
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

/* 닫기 버튼: 오른쪽 위 원형 (중심, 반지름) */
static void close_geom(double *cx, double *cy, double *r)
{
    RECT p = panel_rect_client();
    *r = S(11);
    *cx = p.right - S(16) - *r;
    *cy = p.top + S(24);
}

static int in_close(int x, int y)
{
    double cx, cy, r;
    close_geom(&cx, &cy, &r);
    double dx = x - cx, dy = y - cy, hit = r + S(4);
    return dx * dx + dy * dy <= hit * hit;
}

/* 시계(예약) 버튼: 닫기 버튼 왼쪽 */
static void clock_geom(double *cx, double *cy, double *r)
{
    close_geom(cx, cy, r);
    *cx -= S(30);
}

static int in_clock(int x, int y)
{
    double cx, cy, r;
    clock_geom(&cx, &cy, &r);
    double dx = x - cx, dy = y - cy, hit = r + S(4);
    return dx * dx + dy * dy <= hit * hit;
}

/* 예약 화면 항목: 줄 항목은 row * 8 + 종류 */
enum { F_BACK = 100, F_ADD, F_CLOCK };
enum { K_HOUR = 1, K_MIN, K_LEVEL, K_TOGGLE, K_DEL };

static RECT field_rect(int f) /* 클라이언트 좌표 */
{
    RECT p = panel_rect_client(), r = {0, 0, 0, 0};
    if (f == F_BACK) {
        SetRect(&r, p.left + S(8), p.top + S(8), p.left + S(150), p.top + S(40));
        return r;
    }
    if (f == F_ADD) {
        int t = p.top + S(SCHED_Y + SCHED_H * g_nsched);
        SetRect(&r, p.left + S(12), t, p.right - S(12), t + S(34));
        return r;
    }
    int t = p.top + S(SCHED_Y + SCHED_H * (f / 8)), b = t + S(40), x = p.left;
    switch (f % 8) {
    case K_HOUR: SetRect(&r, x + S(20), t + S(4), x + S(52), b - S(4)); break;
    case K_MIN: SetRect(&r, x + S(60), t + S(4), x + S(92), b - S(4)); break;
    case K_LEVEL: SetRect(&r, x + S(116), t + S(4), x + S(178), b - S(4)); break;
    case K_TOGGLE: SetRect(&r, x + S(204), t + S(9), x + S(242), b - S(9)); break;
    case K_DEL: SetRect(&r, x + S(254), t + S(9), x + S(276), b - S(9)); break;
    }
    return r;
}

static int field_at(int x, int y)
{
    POINT pt = {x, y};
    RECT r;
    if (g_view != 1) return in_clock(x, y) ? F_CLOCK : -1;
    r = field_rect(F_BACK);
    if (PtInRect(&r, pt)) return F_BACK;
    for (int i = 0; i < g_nsched; i++)
        for (int k = K_HOUR; k <= K_DEL; k++) {
            r = field_rect(i * 8 + k);
            InflateRect(&r, S(3), S(3));
            if (PtInRect(&r, pt)) return i * 8 + k;
        }
    if (g_nsched < MAX_SCHED) {
        r = field_rect(F_ADD);
        if (PtInRect(&r, pt)) return F_ADD;
    }
    return -1;
}

static int row_top(int i) { return panel_rect_client().top + S(ROWS_Y + ROW_H * i); }

static void track_geom(int i, double *x0, double *y0, double *x1, double *y1)
{
    RECT p = panel_rect_client();
    *x0 = p.left + S(PAD);
    *x1 = p.right - S(PAD);
    *y0 = row_top(i) + S(TRACK_DY);
    *y1 = *y0 + S(TRACK_H);
}

/* 클라이언트 y 좌표가 몇 번째 모니터 줄인지 (-1 = 줄 밖) */
static int row_at(int y)
{
    for (int i = 0; i < g_nmon; i++) {
        int t = row_top(i);
        if (y >= t - S(4) && y < t + S(TRACK_DY + TRACK_H + 6)) return i;
    }
    return -1;
}

static void format_level(double level, wchar_t *buf, size_t n)
{
    if (level < 10 && level != floor(level))
        swprintf(buf, n, L"%.1f%%", level);
    else
        swprintf(buf, n, L"%d%%", (int)lround(level));
}

static void mode_text(wchar_t *buf, size_t n)
{
    const Mon *m = &g_mon[g_active < g_nmon ? g_active : 0];
    int hw = hw_percent(m);
    wchar_t t[96];
    if (g_notice[0] && GetTickCount() < g_notice_until) {
        wcsncpy(buf, g_notice, n);
        buf[n - 1] = 0;
        return;
    }
    if (g_view == 1) {
        wcsncpy(buf, g_nsched ? L"시각·밝기 위에서 휠을 굴리거나 클릭해서 바꾸세요" : L"예약을 추가하면 정한 시각에 밝기가 바뀝니다", n);
        buf[n - 1] = 0;
        return;
    }
    if (g_show_hint)
        wcsncpy(t, L"아이콘 클릭 또는 아이콘 위에서 휠로 조절", 96);
    else if (!g_hw_ready)
        wcsncpy(t, L"모니터 확인 중…", 96);
    else if (m->level <= 0)
        wcsncpy(t, L"화면 꺼짐 · Ctrl+Alt+Home 으로 복구", 96);
    else if (m->split < 100.0 && m->level < m->split)
        wcsncpy(t, L"모니터 최저 밝기 + 화면 어둡게", 96);
    else if (hw >= 0)
        swprintf(t, 96, L"모니터 밝기 %d%%", hw);
    else if (m->hw_original < 0)
        wcsncpy(t, m->level < 100 ? L"화면 어둡게 (모니터 자체 조절 미지원)" : L"모니터 자체 조절 미지원", 96);
    else
        wcsncpy(t, m->level < 100 ? L"화면 어둡게 (모니터 밝기는 그대로)" : L"원래 밝기", 96);
    t[95] = 0;
    if (g_nmon > 1 && !g_show_hint && g_hw_ready)
        swprintf(buf, n, L"%ls · %ls", m->name, t);
    else
        wcsncpy(buf, t, n);
    buf[n - 1] = 0;
}

/* 원 안의 X (닫기·삭제 버튼) */
static void draw_x(Canvas *c, double cx, double cy, double r, double lw, Rgba col)
{
    double k = r * 0.36;
    for (int y = (int)(cy - r); y < (int)(cy + r) + 1; y++)
        for (int x = (int)(cx - r); x < (int)(cx + r) + 1; x++) {
            if (x < 0 || y < 0 || x >= c->w || y >= c->h) continue;
            double px = x + 0.5, py = y + 0.5;
            double d1 = sdf_capsule(px, py, cx - k, cy - k, cx + k, cy + k, lw);
            double d2 = sdf_capsule(px, py, cx + k, cy - k, cx - k, cy + k, lw);
            double cv = cov_of(d1 < d2 ? d1 : d2);
            if (cv > 0) blend_px(&c->px[y * c->w + x], col, cv);
        }
}

/* 시계 아이콘 (SF Symbols 'clock' 느낌) */
static void draw_clock(Canvas *c, double cx, double cy, double r, double lw, Rgba col)
{
    for (int y = (int)(cy - r) - 1; y < (int)(cy + r) + 2; y++)
        for (int x = (int)(cx - r) - 1; x < (int)(cx + r) + 2; x++) {
            if (x < 0 || y < 0 || x >= c->w || y >= c->h) continue;
            double px = x + 0.5, py = y + 0.5;
            double d = fabs(sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy)) - (r - lw)) - lw;
            double h1 = sdf_capsule(px, py, cx, cy, cx, cy - r * 0.55, lw * 0.9);
            double h2 = sdf_capsule(px, py, cx, cy, cx + r * 0.42, cy, lw * 0.9);
            if (h1 < d) d = h1;
            if (h2 < d) d = h2;
            double cv = cov_of(d);
            if (cv > 0) blend_px(&c->px[y * c->w + x], col, cv);
        }
}

/* iOS 스타일 켜기/끄기 스위치 */
static void draw_toggle(Canvas *c, RECT r, int on, Rgba off_col)
{
    double h = r.bottom - r.top, rad = h / 2;
    Rgba green = {0.204, 0.780, 0.349, 1};
    fill_rrect(c, r.left, r.top, r.right, r.bottom, rad, on ? green : off_col);
    double kr = rad - S(2) * 0.9;
    double kx = on ? r.right - rad : r.left + rad;
    Rgba sh = {0, 0, 0, 0.18}, white = {1, 1, 1, 1};
    fill_rrect(c, kx - kr, r.top + rad - kr + 1, kx + kr, r.top + rad + kr + 1, kr, sh);
    fill_rrect(c, kx - kr, r.top + rad - kr, kx + kr, r.top + rad + kr, kr, white);
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
    HFONT f_name = make_font(L"Malgun Gothic", 12, FW_NORMAL);
    HFONT f_value = make_font(L"Segoe UI Semibold", 12, FW_SEMIBOLD);
    HFONT f_caption = make_font(L"Malgun Gothic", 12, FW_NORMAL);

    RECT rt = {p.left + S(PAD), p.top + S(12), p.right - S(60), p.top + S(36)};
    if (g_view == 1) {
        RECT rb = field_rect(F_BACK);
        if (g_hover_field == F_BACK) fill_rrect(&g_cv, rb.left, rb.top, rb.right, rb.bottom, S(8), track);
        draw_text(&g_cv, L"‹  밝기 예약", rt, f_title, DT_LEFT | DT_VCENTER, primary);
    } else {
        draw_text(&g_cv, L"디스플레이", rt, f_title, DT_LEFT | DT_VCENTER, primary);
        /* 시계(예약) 버튼: 켜진 예약이 있으면 강조색 */
        double kx, ky, kr;
        clock_geom(&kx, &ky, &kr);
        Rgba kbg = track;
        if (g_hover_field == F_CLOCK) kbg.a *= 2.0;
        fill_rrect(&g_cv, kx - kr, ky - kr, kx + kr, ky + kr, kr, kbg);
        int any_on = 0;
        for (int i = 0; i < g_nsched; i++) any_on |= g_sched[i].on;
        Rgba accent = g_dark ? (Rgba){0.039, 0.518, 1, 1} : (Rgba){0, 0.478, 1, 1};
        draw_clock(&g_cv, kx, ky, kr * 0.62, S(1) * 0.75,
                   any_on ? accent : (g_hover_field == F_CLOCK ? primary : secondary));
    }

    /* 닫기 버튼 */
    double ccx, ccy, ccr;
    close_geom(&ccx, &ccy, &ccr);
    Rgba cbg = track;
    if (g_close_hover) cbg.a *= 2.0;
    fill_rrect(&g_cv, ccx - ccr, ccy - ccr, ccx + ccr, ccy + ccr, ccr, cbg);
    draw_x(&g_cv, ccx, ccy, ccr, S(1) * 0.8, g_close_hover ? primary : secondary);

    /* 예약 화면: 줄마다 [시:분] [밝기] [스위치] [삭제] */
    if (g_view == 1) {
        HFONT f_time = make_font(L"Segoe UI Semibold", 21, FW_SEMIBOLD);
        HFONT f_lv = make_font(L"Segoe UI Semibold", 16, FW_SEMIBOLD);
        Rgba card = g_dark ? (Rgba){1, 1, 1, 0.05} : (Rgba){0, 0, 0, 0.035};
        Rgba hover = track;
        hover.a *= 1.4;
        for (int i = 0; i < g_nsched; i++) {
            Sched *sc = &g_sched[i];
            int t = p.top + S(SCHED_Y + SCHED_H * i);
            fill_rrect(&g_cv, p.left + S(12), t, p.right - S(12), t + S(40), S(10), card);
            Rgba tc = sc->on ? primary : secondary;
            wchar_t num[16];
            for (int k = K_HOUR; k <= K_LEVEL; k++) {
                RECT fr = field_rect(i * 8 + k);
                if (g_hover_field == i * 8 + k) fill_rrect(&g_cv, fr.left, fr.top, fr.right, fr.bottom, S(6), hover);
                if (k == K_HOUR) swprintf(num, 16, L"%02d", sc->h);
                else if (k == K_MIN) swprintf(num, 16, L"%02d", sc->m);
                else swprintf(num, 16, L"%d%%", sc->level);
                draw_text(&g_cv, num, fr, k == K_LEVEL ? f_lv : f_time, DT_CENTER | DT_VCENTER, tc);
            }
            RECT colon = field_rect(i * 8 + K_HOUR);
            colon.left = colon.right;
            colon.right = field_rect(i * 8 + K_MIN).left;
            colon.top -= S(2);
            draw_text(&g_cv, L":", colon, f_time, DT_CENTER | DT_VCENTER, tc);
            RECT sun = field_rect(i * 8 + K_LEVEL);
            draw_sun(&g_cv, sun.left - S(9), (sun.top + sun.bottom) / 2.0, S(13), secondary);
            draw_toggle(&g_cv, field_rect(i * 8 + K_TOGGLE), sc->on, track);
            RECT dr = field_rect(i * 8 + K_DEL);
            double dcx = (dr.left + dr.right) / 2.0, dcy = (dr.top + dr.bottom) / 2.0, drr = S(10);
            Rgba dbg = track;
            if (g_hover_field == i * 8 + K_DEL) dbg.a *= 2.0;
            fill_rrect(&g_cv, dcx - drr, dcy - drr, dcx + drr, dcy + drr, drr, dbg);
            draw_x(&g_cv, dcx, dcy, drr, S(1) * 0.7, g_hover_field == i * 8 + K_DEL ? primary : secondary);
        }
        if (g_nsched < MAX_SCHED) {
            RECT ar = field_rect(F_ADD);
            Rgba accent = g_dark ? (Rgba){0.039, 0.518, 1, 1} : (Rgba){0, 0.478, 1, 1};
            if (g_hover_field == F_ADD) fill_rrect(&g_cv, ar.left, ar.top, ar.right, ar.bottom, S(10), card);
            draw_text(&g_cv, L"+  예약 추가", ar, f_name, DT_CENTER | DT_VCENTER, accent);
        }
        DeleteObject(f_time);
        DeleteObject(f_lv);
    }

    /* 모니터별 줄: 이름 + 값 + 캡슐 슬라이더 (채움은 최소 트랙 높이(원)부터 — macOS 제어 센터와 동일) */
    if (g_view == 0)
    for (int i = 0; i < g_nmon; i++) {
        Mon *m = &g_mon[i];
        int top = row_top(i);
        RECT rn = {p.left + S(PAD), top, p.right - S(70), top + S(LABEL_H)};
        draw_text(&g_cv, m->name, rn, f_name, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, secondary);
        wchar_t lv[16];
        format_level(m->level, lv, 16);
        RECT rv = {p.right - S(80), top, p.right - S(PAD), top + S(LABEL_H)};
        draw_text(&g_cv, lv, rv, f_value, DT_RIGHT | DT_VCENTER, secondary);

        double tx0, ty0, tx1, ty1;
        track_geom(i, &tx0, &ty0, &tx1, &ty1);
        double th = ty1 - ty0, tr = th / 2;
        fill_rrect(&g_cv, tx0, ty0, tx1, ty1, tr, track);
        double fw = th + (tx1 - tx0 - th) * m->level / 100.0;
        if (!g_dark) shadow_rrect(&g_cv, tx0, ty0 + 1, tx0 + fw, ty1 + 1, tr, S(3), 0.10);
        fill_rrect(&g_cv, tx0, ty0, tx0 + fw, ty1, tr, fill);
        draw_sun(&g_cv, tx0 + tr, ty0 + tr, th * 0.62, glyph);
    }

    wchar_t mode[160];
    mode_text(mode, 160);
    int cy = g_view == 1 ? p.bottom - S(32) : row_top(g_nmon) - S(6);
    RECT rm = {p.left + S(PAD), cy, p.right - S(PAD), cy + S(20)};
    draw_text(&g_cv, mode, rm, f_caption, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, secondary);

    DeleteObject(f_title);
    DeleteObject(f_name);
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

static UINT monitor_dpi(HMONITOR mon)
{
    typedef HRESULT(WINAPI * Fn)(HMONITOR, int, UINT *, UINT *);
    static Fn f;
    static int tried;
    if (!tried) {
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        if (sh) f = (Fn)(void *)GetProcAddress(sh, "GetDpiForMonitor");
        tried = 1;
    }
    UINT x = 96, y = 96;
    if (f && SUCCEEDED(f(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) return x;
    return g_dpi;
}

static void place_panel(int where)
{
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = where == POS_HUD ? MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY)
                                    : MonitorFromWindow(FindWindowW(L"Shell_TrayWnd", NULL), MONITOR_DEFAULTTOPRIMARY);
    g_dpi = monitor_dpi(mon); /* 모니터마다 배율이 다를 수 있음 */
    int W = S(PW + 2 * PM), H = S(PH + 2 * PM);
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
    g_drag_row = -1;
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
            g_view = 0;
            g_hover_field = -1;
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

static void tray_tip_text(wchar_t *buf, size_t n)
{
    wchar_t lv[64] = L"";
    for (int i = 0; i < g_nmon; i++) {
        wchar_t one[16];
        format_level(g_mon[i].level, one, 16);
        if (i) wcscat(lv, L" · ");
        wcscat(lv, one);
    }
    swprintf(buf, n, L"밝기 %ls\n아이콘 위에서 휠로 조절", lv);
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
    tray_tip_text(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

static void update_tray_tip(void)
{
    tray_tip_text(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
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

/* ---------------------------------------------------------------- 설정 */
static int setting_get(const wchar_t *name, int def)
{
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS)
        return (int)v;
    return def;
}

static void setting_set(const wchar_t *name, int value)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    DWORD v = (DWORD)value;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
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
static void apply_mon(int i)
{
    Mon *m = &g_mon[i];
    overlay_set(m, overlay_transmission(m->level, m->split));
    int hw = hw_percent(m);
    if (hw >= 0 && g_hw_ready && m->use_hw) hw_request(i, hw);
}

static void apply_all(void)
{
    for (int i = 0; i < g_nmon; i++) apply_mon(i);
    update_tray_tip();
    redraw();
}

static void set_level(int i, double v)
{
    g_touched = 1;
    g_show_hint = 0;
    g_active = i;
    v = round(v * 2.0) / 2.0;
    g_mon[i].level = v < 0 ? 0 : (v > 100 ? 100 : v);
    g_last_activity = GetTickCount();
    apply_mon(i);
    update_tray_tip();
    redraw();
}

/* i < 0 이면 모든 모니터 */
static void step(int i, double d)
{
    if (i >= 0) {
        set_level(i, g_mon[i].level + d);
        return;
    }
    for (int k = 0; k < g_nmon; k++) set_level(k, g_mon[k].level + d);
}

static void set_all(double v)
{
    for (int k = 0; k < g_nmon; k++) set_level(k, v);
}

static void track_set_from_x(int i, int x)
{
    double tx0, ty0, tx1, ty1;
    track_geom(i, &tx0, &ty0, &tx1, &ty1);
    double th = ty1 - ty0;
    double t = (x - tx0 - th / 2) / (tx1 - tx0 - th);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    set_level(i, round(t * 100.0));
}

/* 검은 막을 걷고 모니터 밝기를 프로그램 시작 전 값으로 */
static void restore_mon(int i)
{
    Mon *m = &g_mon[i];
    if (m->hw_original >= 0) hw_request(i, m->hw_original);
    m->split = (m->hw_original >= 0 && m->use_hw) ? HW_SPLIT : 100.0;
    m->level = level_from_hw(m);
    apply_mon(i);
}

static void restore_all(void)
{
    g_touched = 1;
    for (int i = 0; i < g_nmon; i++) restore_mon(i);
    update_tray_tip();
    show_panel(POS_TRAY, 0, 1500);
}

/* ---------------------------------------------------------------- 밝기 예약 */
/* 저장 형식 (HKCU\Software\BrightnessControl\Schedules): "07:00=100:1;23:00=20:1;" (시:분=밝기:켜짐) */
static void sched_save(void)
{
    wchar_t buf[256] = L"", one[32];
    for (int i = 0; i < g_nsched; i++) {
        swprintf(one, 32, L"%02d:%02d=%d:%d;", g_sched[i].h, g_sched[i].m, g_sched[i].level, g_sched[i].on);
        wcscat(buf, one);
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"Schedules", 0, REG_SZ, (const BYTE *)buf, (DWORD)((wcslen(buf) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

static void sched_load(void)
{
    wchar_t buf[256];
    DWORD sz = sizeof(buf);
    g_nsched = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Schedules", RRF_RT_REG_SZ, NULL, buf, &sz) != ERROR_SUCCESS)
        return;
    for (wchar_t *q = buf; *q && g_nsched < MAX_SCHED;) {
        int h, m, lv, on;
        if (swscanf(q, L"%d:%d=%d:%d", &h, &m, &lv, &on) == 4 && h >= 0 && h < 24 && m >= 0 && m < 60 &&
            lv >= 0 && lv <= 100) {
            Sched *sc = &g_sched[g_nsched++];
            sc->h = h;
            sc->m = m;
            sc->level = lv;
            sc->on = on != 0;
            sc->fired_day = 0;
        }
        wchar_t *semi = wcschr(q, L';');
        if (!semi) break;
        q = semi + 1;
    }
}

static int today_key(void)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wYear * 10000 + t.wMonth * 100 + t.wDay;
}

/* 현지 시각을 분 단위로 (오늘 날짜 + day_offset 일, h:m). h < 0 이면 지금 */
static ULONGLONG local_minutes(int day_offset, int h, int m)
{
    SYSTEMTIME t;
    FILETIME f;
    GetLocalTime(&t);
    if (h >= 0) {
        t.wHour = (WORD)h;
        t.wMinute = (WORD)m;
        t.wSecond = 0;
        t.wMilliseconds = 0;
    }
    SystemTimeToFileTime(&t, &f);
    ULARGE_INTEGER u;
    u.LowPart = f.dwLowDateTime;
    u.HighPart = f.dwHighDateTime;
    return u.QuadPart / 600000000ULL + (LONGLONG)day_offset * 1440;
}

static void sched_fire(int i)
{
    Sched *sc = &g_sched[i];
    set_all(sc->level);
    swprintf(g_notice, 96, L"예약 %02d:%02d · 밝기 %d%% 로 바꿨어요", sc->h, sc->m, sc->level);
    g_notice_until = GetTickCount() + 3500;
    if (!g_shown) show_panel(POS_HUD, 0, 3000);
    else redraw();
}

static void sched_tick(void)
{
    if (g_shown && g_view == 1) return; /* 편집 중에는 실행하지 않음 (휠로 시각을 넘길 때 바로 실행되지 않게) */
    SYSTEMTIME t;
    GetLocalTime(&t);
    int day = today_key();
    for (int i = 0; i < g_nsched; i++) {
        Sched *sc = &g_sched[i];
        if (sc->on && sc->h == t.wHour && sc->m == t.wMinute && sc->fired_day != day) {
            sc->fired_day = day;
            sched_fire(i);
        }
    }
}

/* 절전 중에 지나간 예약이 있으면 가장 최근 것을 적용 */
static void sched_catch_up(void)
{
    if (!g_suspend_min) return;
    ULONGLONG now = local_minutes(0, -1, 0), best_t = 0;
    int best = -1, best_today = 0;
    for (int i = 0; i < g_nsched; i++) {
        if (!g_sched[i].on) continue;
        for (int off = -1; off <= 0; off++) {
            ULONGLONG o = local_minutes(off, g_sched[i].h, g_sched[i].m);
            if (o > g_suspend_min && o <= now && o >= best_t) {
                best = i;
                best_t = o;
                best_today = off == 0;
            }
        }
    }
    g_suspend_min = 0;
    if (best >= 0) {
        if (best_today) g_sched[best].fired_day = today_key();
        sched_fire(best);
    }
}

/* 화면 전환 (밝기 <-> 예약): 패널 아래쪽 기준으로 크기만 바꿈 */
static void relayout(void)
{
    RECT wr;
    GetWindowRect(g_main, &wr);
    int H = S(PH + 2 * PM);
    SetWindowPos(g_main, HWND_TOPMOST, wr.left, wr.bottom - H, S(PW + 2 * PM), H, SWP_NOACTIVATE);
    redraw();
}

static void set_view(int v)
{
    g_view = v;
    g_hover_field = -1;
    g_hide_delay = v ? 10000 : 2500;
    g_last_activity = GetTickCount();
    relayout();
}

static void sched_add(void)
{
    if (g_nsched >= MAX_SCHED) return;
    static const int defs[][2] = {{7, 100}, {23, 20}, {12, 100}, {18, 70}, {21, 40}, {9, 100}};
    Sched *sc = &g_sched[g_nsched];
    sc->h = defs[g_nsched][0];
    sc->m = 0;
    sc->level = defs[g_nsched][1];
    sc->on = 1;
    sc->fired_day = 0;
    g_nsched++;
    sched_save();
    relayout();
}

static void sched_delete(int i)
{
    for (int k = i; k + 1 < g_nsched; k++) g_sched[k] = g_sched[k + 1];
    g_nsched--;
    g_hover_field = -1;
    sched_save();
    relayout();
}

/* 시/분/밝기 값을 바꿈: d 칸, fine = Shift (분·밝기를 1 단위로) */
static void sched_adjust(int f, int d, int fine)
{
    int i = f / 8, k = f % 8;
    if (i < 0 || i >= g_nsched) return;
    Sched *sc = &g_sched[i];
    if (k == K_HOUR) sc->h = ((sc->h + d) % 24 + 24) % 24;
    else if (k == K_MIN) sc->m = ((sc->m + d * (fine ? 1 : 5)) % 60 + 60) % 60;
    else if (k == K_LEVEL) {
        sc->level += d * (fine ? 1 : 5);
        sc->level = sc->level < 0 ? 0 : (sc->level > 100 ? 100 : sc->level);
    } else return;
    sc->fired_day = 0;
    sched_save();
    redraw();
}

static void field_click(int f)
{
    if (f == F_CLOCK) set_view(1);
    else if (f == F_BACK) set_view(0);
    else if (f == F_ADD) sched_add();
    else if (f >= 0 && f < 8 * MAX_SCHED) {
        int i = f / 8, k = f % 8;
        if (k == K_TOGGLE) {
            g_sched[i].on = !g_sched[i].on;
            g_sched[i].fired_day = 0;
            sched_save();
            redraw();
        } else if (k == K_DEL) {
            sched_delete(i);
        } else {
            sched_adjust(f, 1, GetKeyState(VK_SHIFT) < 0); /* 클릭하면 한 칸 올림 */
        }
    }
}

static void show_menu(HWND h)
{
    HMENU m = CreatePopupMenu();
    static const int levels[] = {100, 75, 50, 25, 10, 0};
    wchar_t s[128];
    for (int i = 0; i < 6; i++) {
        swprintf(s, 128, g_nmon > 1 ? L"모두 %d%%" : L"%d%%", levels[i]);
        AppendMenuW(m, MF_STRING, CMD_LEVEL_BASE + levels[i], s);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_RESTORE, L"원래 밝기로 되돌리기");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g_nmon; i++) {
        const Mon *mo = &g_mon[i];
        int unsupported = g_hw_ready && mo->hw_original < 0;
        swprintf(s, 128, unsupported ? L"모니터 밝기 직접 조절: %ls (지원 안 함)" : L"모니터 밝기 직접 조절: %ls", mo->name);
        AppendMenuW(m, MF_STRING | (mo->use_hw && !unsupported ? MF_CHECKED : 0) | (unsupported ? MF_GRAYED : 0),
                    CMD_USE_HW_BASE + i, s);
    }
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
    /* 종료하면 모든 모니터 밝기를 프로그램 시작 전 값으로 되돌림 */
    EnterCriticalSection(&g_hw_cs);
    for (int i = 0; i < g_nmon; i++)
        if (g_mon[i].hw_original >= 0) g_mon[i].target = g_mon[i].hw_original;
    g_hw_stop = 1;
    LeaveCriticalSection(&g_hw_cs);
    WakeConditionVariable(&g_hw_cv);
    for (int i = 0; i < g_nmon; i++) overlay_set(&g_mon[i], 255);
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
    int row = -1; /* 트레이 위 휠, 또는 패널의 줄 밖: 모든 모니터 */
    if (src == WHEEL_FROM_TRAY && !g_shown) show_panel(POS_TRAY, 0, 1600);
    if (src == WHEEL_FROM_PANEL) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_main, &pt);
        row = row_at(pt.y);
    }
    g_wheel_accum += delta;
    if (src == WHEEL_FROM_PANEL && g_view == 1) {
        /* 예약 화면: 한 칸(120)마다 값 하나씩 */
        if (abs(g_wheel_accum) < WHEEL_DELTA / 2) return;
        int d = (int)lround((double)g_wheel_accum / WHEEL_DELTA);
        if (d == 0) d = g_wheel_accum > 0 ? 1 : -1;
        g_wheel_accum = 0;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(g_main, &pt);
        int f = field_at(pt.x, pt.y);
        g_last_activity = GetTickCount();
        if (f >= 0 && f < 8 * MAX_SCHED) sched_adjust(f, d, mods & 1);
        return;
    }
    if (abs(g_wheel_accum) < WHEEL_DELTA / 4) return; /* 고해상도 휠 대응 */
    double n = (double)g_wheel_accum / WHEEL_DELTA;
    g_wheel_accum = 0;
    double mult = (mods & 1) ? 0.5 : ((mods & 2) ? 5.0 : 1.0);
    step(row, n * STEP * mult);
}

/* 모니터 구성이 바뀌었으면(연결/해제) 새로 시작해서 목록을 다시 만든다 */
static void check_display_change(void)
{
    int n = 0, changed = 0;
    MONITORINFOEXW mi;
    for (int i = 0; i < g_nmon; i++) {
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(g_mon[i].hmon, (MONITORINFO *)&mi) || wcscmp(mi.szDevice, g_mon[i].dev) != 0) {
            changed = 1;
            break;
        }
        g_mon[i].rc = mi.rcMonitor;
    }
    Mon save[MAX_MON];
    memcpy(save, g_mon, sizeof(save));
    int saved_n = g_nmon;
    g_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, enum_collect, 0);
    n = g_nmon;
    memcpy(g_mon, save, sizeof(save));
    g_nmon = saved_n;
    if (n != g_nmon) changed = 1;

    if (!changed) {
        for (int i = 0; i < g_nmon; i++)
            if (g_mon[i].overlay_trans >= 0 && g_mon[i].overlay_trans < 255) overlay_fit(&g_mon[i]);
        return;
    }
    wchar_t exe[MAX_PATH], cmdline[MAX_PATH + 48];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf(cmdline, ARRAYSIZE(cmdline), L"\"%ls\" --background --replace", exe);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (CreateProcessW(exe, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        /* 새 인스턴스가 g_msg_quit 를 보내 이 인스턴스를 종료시킴 */
    }
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
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (in_close(x, y)) {
            hide_panel(); /* 패널만 닫고 트레이에서 계속 대기 */
            return 0;
        }
        int f = field_at(x, y);
        if (f >= 0) {
            field_click(f);
            return 0;
        }
        if (g_view == 1) return 0;
        int row = row_at(y);
        if (row >= 0) {
            g_drag_row = row;
            SetCapture(h);
            track_set_from_x(row, x);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        g_last_activity = GetTickCount();
        int hov = in_close(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hov != g_close_hover) {
            g_close_hover = hov;
            redraw();
        }
        int hf = field_at(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hf != g_hover_field) {
            g_hover_field = hf;
            redraw();
        }
        if (hov || hf >= 0) {
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, HOVER_DEFAULT};
            TrackMouseEvent(&t);
        }
        if (g_drag_row >= 0) track_set_from_x(g_drag_row, GET_X_LPARAM(l));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_close_hover || g_hover_field >= 0) {
            g_close_hover = 0;
            g_hover_field = -1;
            redraw();
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag_row >= 0) {
            g_drag_row = -1;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONUP:
        show_menu(h);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_UP || w == VK_RIGHT) step(g_active, STEP);
        else if (w == VK_DOWN || w == VK_LEFT) step(g_active, -STEP);
        else if (w == VK_TAB && g_nmon > 1) {
            g_active = (g_active + 1) % g_nmon;
            redraw();
        } else if (w == VK_ESCAPE || w == VK_RETURN) hide_panel();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && g_activated) {
            g_deact_tick = GetTickCount();
            hide_panel();
        }
        return 0;
    case WM_HOTKEY:
        if (w == HK_UP) step(-1, STEP);
        else if (w == HK_DOWN) step(-1, -STEP);
        else if (w == HK_RESET) set_all(100);
        if (!g_shown || !g_activated) show_panel(g_shown ? POS_TRAY : POS_HUD, 0, 1300);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == CMD_EXIT) do_exit();
        else if (id == CMD_AUTORUN) autorun_set(!autorun_enabled());
        else if (id == CMD_RESTORE) restore_all();
        else if (id >= CMD_USE_HW_BASE && id < CMD_USE_HW_BASE + g_nmon) {
            int i = id - CMD_USE_HW_BASE;
            Mon *mo = &g_mon[i];
            wchar_t key[96];
            mo->use_hw = !mo->use_hw;
            use_hw_key(mo, key, 96);
            setting_set(key, mo->use_hw);
            restore_mon(i);
            g_active = i;
            update_tray_tip();
            show_panel(POS_TRAY, 0, 1500);
        } else if (id == CMD_SHOW) show_panel(POS_TRAY, 1, 2500);
        else if (id >= CMD_LEVEL_BASE && id <= CMD_LEVEL_BASE + 100) {
            set_all(id - CMD_LEVEL_BASE);
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
    case WM_APP_HWREADY:
        g_hw_ready = 1;
        for (int i = 0; i < g_nmon; i++) {
            Mon *mo = &g_mon[i];
            mo->split = (mo->hw_original >= 0 && mo->use_hw) ? HW_SPLIT : 100.0; /* 쓸 수 없거나 끄면 전 구간 검은 막 */
            if (!g_touched) mo->level = level_from_hw(mo);
        }
        apply_all();
        return 0;
    case WM_TIMER:
        if (w == TM_TOP) {
            raise_windows();
        } else if (w == TM_FADE) {
            fade_step();
        } else if (w == TM_IDLE) {
            /* 사용하지 않으면 자동으로 사라짐 (마우스가 패널 위에 있거나 드래그 중이면 유지) */
            if (g_drag_row >= 0 || cursor_in_panel()) g_last_activity = GetTickCount();
            else if (g_shown && GetTickCount() - g_last_activity > g_hide_delay) hide_panel();
        } else if (w == TM_SCHED) {
            sched_tick();
        } else if (w == TM_RESUME) {
            KillTimer(h, TM_RESUME);
            sched_catch_up();
        } else if (w == TM_DISPLAY_ON) {
            /* 모니터가 대기에서 깨어나면 밝기를 다시 보냄 (자고 있는 동안 바뀐 예약 등) */
            KillTimer(h, TM_DISPLAY_ON);
            EnterCriticalSection(&g_hw_cs);
            for (int i = 0; i < g_nmon; i++) g_mon[i].force = 1;
            LeaveCriticalSection(&g_hw_cs);
            for (int i = 0; i < g_nmon; i++) apply_mon(i);
        } else if (w == TM_DISPLAY) {
            KillTimer(h, TM_DISPLAY);
            check_display_change();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMSUSPEND) {
            g_suspend_min = local_minutes(0, -1, 0);
        } else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
            SetTimer(h, TM_RESUME, 4000, NULL); /* 모니터가 깨어날 시간을 둠 */
        } else if (w == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *ps = (POWERBROADCAST_SETTING *)l;
            if (ps && ps->DataLength >= sizeof(DWORD) && *(DWORD *)ps->Data == 1 && g_hw_ready)
                SetTimer(h, TM_DISPLAY_ON, 2500, NULL);
        }
        return TRUE;
    case WM_SETTINGCHANGE:
        if (l && wcscmp((const wchar_t *)l, L"ImmersiveColorSet") == 0) {
            read_theme();
            update_tray(0);
            redraw();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        SetTimer(h, TM_DISPLAY, 1500, NULL); /* 연결/해제가 끝난 뒤 한 번만 확인 */
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
        if (m == g_msg_version && m != 0) return APP_VERSION;
        if (m == g_msg_quit && m != 0) {
            do_exit();
            return 0;
        }
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
    int replace = cmd && wcsstr(cmd, L"--replace") != NULL;

    /* 중복 실행 방지: 이미 실행 중이면 그 패널을 띄우고 종료 */
    g_msg_version = RegisterWindowMessageW(L"BrightnessControl_Version");
    g_msg_quit = RegisterWindowMessageW(L"BrightnessControl_Quit");
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\BrightnessControl_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"BrightnessControlMain", NULL);
        DWORD_PTR ver = 0;
        if (other)
            SendMessageTimeoutW(other, g_msg_version, 0, 0, SMTO_ABORTIFHUNG, 2000, &ver);
        if (other && ver >= APP_VERSION && !replace) {
            if (!background) {
                AllowSetForegroundWindow(ASFW_ANY);
                PostMessageW(other, WM_APP_SHOW, 0, 0);
            }
            return 0;
        }
        /* 예전 버전(또는 --replace): 종료시키고(예전 버전은 WM_CLOSE 로 종료) 끝날 때까지 기다림 */
        if (other) {
            PostMessageW(other, g_msg_quit, 0, 0);
            if (ver < 4) PostMessageW(other, WM_CLOSE, 0, 0);
        }
        DWORD wr = WaitForSingleObject(mutex, 8000);
        if (wr != WAIT_OBJECT_0 && wr != WAIT_ABANDONED) return 0;
        Sleep(300); /* 이전 인스턴스가 모니터 밝기를 되돌리는 시간 */
    }

    set_dpi_awareness();
    g_dpi = system_dpi();
    read_theme();
    enumerate_monitors();
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

    /* 모니터별 검은 막: 클릭 통과 + 반투명 + 작업표시줄/Alt+Tab 제외 + 포커스 안 뺏음 */
    for (int i = 0; i < g_nmon; i++) {
        g_mon[i].overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                               WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                           L"BrightnessControlOverlay", L"", WS_POPUP, 0, 0, 1, 1,
                                           NULL, NULL, inst, NULL);
        SetLayeredWindowAttributes(g_mon[i].overlay, 0, 0, LWA_ALPHA);
    }

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
    sched_load();
    SetTimer(g_main, TM_SCHED, 10000, NULL);
    {
        /* 화면 켜짐/꺼짐 알림 (GUID_CONSOLE_DISPLAY_STATE) */
        static const GUID display_state = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
        RegisterPowerSettingNotification(g_main, &display_state, DEVICE_NOTIFY_WINDOW_HANDLE);
    }

    apply_all();
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
    for (int i = 0; i < g_nmon; i++) DestroyWindow(g_mon[i].overlay);
    return 0;
}
