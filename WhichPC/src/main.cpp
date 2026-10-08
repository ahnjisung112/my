// WhichPC - tells you whether the screen you are looking at is the laptop or
// the desktop. Each PC shows its own icon in the notification area, so when
// you remote from the laptop into the desktop, the taskbar you see always
// tells you which machine it belongs to.
#include "common.h"

#include <dwmapi.h>
#include <objbase.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>

#include "resource.h"

namespace {

constexpr UINT kTrayUid = 1;
constexpr int kHotkeyId = 1;
constexpr wchar_t kTaskbarBtnClass[] = L"WhichPC.TaskbarButton";
constexpr wchar_t kMutexName[] = L"Local\\WhichPC.SingleInstance";
#define WM_TASKBTN_REMINIMIZE (WM_APP + 10)

enum TimerId {
    TIMER_TRAY_RETRY = 1,
    TIMER_PROMOTE,
    TIMER_REFRESH,
    TIMER_OVERLAY,
    TIMER_OVERLAY_ANIM,
    TIMER_FLASH,
    TIMER_DELAYED_FLASH,
    TIMER_SESSION,
    TIMER_STARTUP,
};

struct App {
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr;
    Settings s;
    SessionInfo session;
    std::wstring computer;
    UINT msgTaskbarCreated = 0;
    UINT msgCommand = 0;
    bool trayAdded = false;
    int trayRetries = 0;
    int promoteAttempts = 0;
    HICON trayIcon = nullptr;
    HICON balloonIcon = nullptr;
    HWND taskbarBtn = nullptr;
    HICON tbIconBig = nullptr, tbIconSmall = nullptr;
    bool hotkeyOn = false;
    bool sessionNotify = false;
    bool pendingRemoteNotice = false;
    bool quietStart = false;
    bool openSettingsOnStart = false;
    ULONGLONG lastFlash = 0;
};

App g;

// --------------------------------------------------------------------------
// text helpers
// --------------------------------------------------------------------------
std::wstring Label() { return EffectiveLabel(g.s); }
COLORREF Color() { return EffectiveColor(g.s); }

std::wstring StatusText() {
    if (!g.session.remote) return L"이 PC의 화면입니다";
    if (g.session.clientName.empty()) return L"원격 데스크톱으로 접속 중";
    return g.session.clientName + L"에서 원격 접속 중";
}

void CopyTrunc(wchar_t* dst, size_t cap, const std::wstring& src) {
    size_t n = std::min(src.size(), cap - 1);
    wmemcpy(dst, src.c_str(), n);
    dst[n] = 0;
}

CardContent MakeCard() {
    CardContent c;
    c.device = g.s.device;
    c.color = Color();
    c.title = Label();
    c.line1 = g.computer;
    c.line2 = StatusText();
    c.remote = g.session.remote;
    return c;
}

BadgeContent MakeBadge() {
    BadgeContent b;
    b.device = g.s.device;
    b.color = Color();
    b.label = Label();
    b.remote = g.session.remote;
    return b;
}

// --------------------------------------------------------------------------
// tray icon
// --------------------------------------------------------------------------
int SmallIconSize() { return GetSystemMetricsForDpi(SM_CXSMICON, PrimaryDpi()); }
int LargeIconSize() { return GetSystemMetricsForDpi(SM_CXICON, PrimaryDpi()); }

NOTIFYICONDATAW BaseNid() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = kTrayUid;
    return nid;
}

void SchedulePromote(UINT delay) {
    if (WindowsBuild() >= 22000 || WindowsBuild() == 0) SetTimer(g.hwnd, TIMER_PROMOTE, delay, nullptr);
}

void UpdateTrayIcon() {
    HICON icon = CreateDeviceIcon(g.s.device, Color(), SmallIconSize(), g.session.remote);
    NOTIFYICONDATAW nid = BaseNid();
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = icon;
    std::wstring tip = Label() + L" (" + g.computer + L")\n" +
                       (g.session.remote ? StatusText() : std::wstring(L"클릭: 크게 표시 · 우클릭: 메뉴"));
    CopyTrunc(nid.szTip, ARRAYSIZE(nid.szTip), tip);

    bool ok;
    if (g.trayAdded) {
        ok = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
        if (!ok) g.trayAdded = false;  // explorer lost our icon; add it again below
    }
    if (!g.trayAdded) {
        ok = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (!ok) {  // a stale entry with the same id may survive an explorer crash
            Shell_NotifyIconW(NIM_DELETE, &nid);
            ok = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        }
        if (ok) {
            g.trayAdded = true;
            g.trayRetries = 0;
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            g.promoteAttempts = 0;
            if (g.s.trayAlwaysShow) SchedulePromote(1500);
        } else if (g.trayRetries++ < 90) {
            SetTimer(g.hwnd, TIMER_TRAY_RETRY, 2000, nullptr);  // explorer not ready yet (early autostart)
        }
    }
    if (g.trayIcon) DestroyIcon(g.trayIcon);
    g.trayIcon = icon;
}

void ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g.trayAdded) return;
    HICON big = CreateDeviceIcon(g.s.device, Color(), LargeIconSize(), g.session.remote);
    NOTIFYICONDATAW nid = BaseNid();
    nid.uFlags = NIF_INFO | NIF_SHOWTIP;  // without NIF_SHOWTIP v4 icons lose their hover tooltip
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = big;
    CopyTrunc(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    CopyTrunc(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (g.balloonIcon) DestroyIcon(g.balloonIcon);
    g.balloonIcon = big;
}

// --------------------------------------------------------------------------
// big identify card
// --------------------------------------------------------------------------
void Flash() {
    g.lastFlash = GetTickCount64();
    FlashShow(MakeCard());
    SetTimer(g.hwnd, TIMER_FLASH, 15, nullptr);
}

// --------------------------------------------------------------------------
// corner badges
// --------------------------------------------------------------------------
void RebuildOverlays() {
    OverlayRebuild(g.s, MakeBadge());
    if (g.s.overlay) {
        SetTimer(g.hwnd, TIMER_OVERLAY, 200, nullptr);
        OverlayTick(g.s);
        SetTimer(g.hwnd, TIMER_OVERLAY_ANIM, 16, nullptr);
    } else {
        KillTimer(g.hwnd, TIMER_OVERLAY);
        KillTimer(g.hwnd, TIMER_OVERLAY_ANIM);
    }
}

// --------------------------------------------------------------------------
// optional taskbar button: a permanently minimised window whose taskbar
// button shows the big device icon. Restoring is refused via WM_QUERYOPEN.
// --------------------------------------------------------------------------
void SetTaskbarButtonIcons() {
    if (!g.taskbarBtn) return;
    HICON big = CreateDeviceIcon(g.s.device, Color(), LargeIconSize(), g.session.remote);
    HICON small = CreateDeviceIcon(g.s.device, Color(), SmallIconSize(), g.session.remote);
    SendMessageW(g.taskbarBtn, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(g.taskbarBtn, WM_SETICON, ICON_SMALL, (LPARAM)small);
    if (g.tbIconBig) DestroyIcon(g.tbIconBig);
    if (g.tbIconSmall) DestroyIcon(g.tbIconSmall);
    g.tbIconBig = big;
    g.tbIconSmall = small;
    std::wstring title = Label() + L" · " + g.computer;
    SetWindowTextW(g.taskbarBtn, title.c_str());
    DwmInvalidateIconicBitmaps(g.taskbarBtn);
}

void UpdateTaskbarButton() {
    if (!g.s.taskbarButton) {
        if (g.taskbarBtn) DestroyWindow(g.taskbarBtn);
        g.taskbarBtn = nullptr;
        return;
    }
    if (!g.taskbarBtn) {
        g.taskbarBtn = CreateWindowExW(WS_EX_APPWINDOW, kTaskbarBtnClass, L"WhichPC",
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, -32000, -32000, 240,
                                       120, nullptr, nullptr, g.hinst, nullptr);
        if (!g.taskbarBtn) return;
        BOOL on = TRUE;
        DwmSetWindowAttribute(g.taskbarBtn, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof(on));
        DwmSetWindowAttribute(g.taskbarBtn, DWMWA_HAS_ICONIC_BITMAP, &on, sizeof(on));
        DwmSetWindowAttribute(g.taskbarBtn, DWMWA_DISALLOW_PEEK, &on, sizeof(on));
        SetTaskbarButtonIcons();
        // Park the restored position off-screen, then show it minimised: the
        // taskbar adds a button for every visible, unowned top-level window.
        WINDOWPLACEMENT wp = {sizeof(wp)};
        wp.showCmd = SW_HIDE;
        wp.rcNormalPosition = RECT{-32000, -32000, -31760, -31880};
        SetWindowPlacement(g.taskbarBtn, &wp);
        ShowWindow(g.taskbarBtn, SW_SHOWMINNOACTIVE);
        return;
    }
    SetTaskbarButtonIcons();
}

LRESULT CALLBACK TaskbarBtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_QUERYOPEN:  // taskbar click / Alt+Tab on the minimised window
            Flash();
            return FALSE;
        case WM_SYSCOMMAND:
            switch (wp & 0xFFF0) {
                case SC_MINIMIZE:  // the taskbar minimises an "active" button on the next click
                    if (IsIconic(h)) {
                        Flash();
                        return 0;
                    }
                    break;
                case SC_MAXIMIZE:
                case SC_MOVE:
                case SC_SIZE:
                    return 0;
                case SC_CLOSE: {
                    Settings s = g.s;
                    s.taskbarButton = false;
                    App_ApplySettings(s, GetAutostart());
                    return 0;
                }
            }
            break;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) PostMessageW(h, WM_TASKBTN_REMINIMIZE, 0, 0);
            break;
        case WM_TASKBTN_REMINIMIZE:
            if (!IsIconic(h)) ShowWindow(h, SW_SHOWMINNOACTIVE);
            return 0;
        case WM_CLOSE: {
            Settings s = g.s;
            s.taskbarButton = false;
            App_ApplySettings(s, GetAutostart());
            return 0;
        }
        case WM_DWMSENDICONICTHUMBNAIL: {
            HBITMAP bmp = RenderThumbnail(MakeCard(), HIWORD(lp), LOWORD(lp));
            if (bmp) {
                DwmSetIconicThumbnail(h, bmp, 0);
                DeleteObject(bmp);
            }
            return 0;
        }
        case WM_DESTROY:
            if (g.tbIconBig) DestroyIcon(g.tbIconBig);
            if (g.tbIconSmall) DestroyIcon(g.tbIconSmall);
            g.tbIconBig = g.tbIconSmall = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --------------------------------------------------------------------------
// hotkey
// --------------------------------------------------------------------------
void UpdateHotkey() {
    if (g.s.hotkey && !g.hotkeyOn)
        g.hotkeyOn = RegisterHotKey(g.hwnd, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'W') != FALSE;
    else if (!g.s.hotkey && g.hotkeyOn) {
        UnregisterHotKey(g.hwnd, kHotkeyId);
        g.hotkeyOn = false;
    }
}

void RefreshVisuals() {
    UpdateTrayIcon();
    RebuildOverlays();
    UpdateTaskbarButton();
}

// --------------------------------------------------------------------------
// tray menu
// --------------------------------------------------------------------------
void ShowAbout() {
    std::wstring text = L"WhichPC " WHICHPC_VERSION_W L"\n"
                        L"지금 보고 있는 화면이 노트북인지 데스크탑인지 작업 표시줄에 표시합니다.\n\n";
    text += L"이 PC: " + Label() + L" (" + g.computer + L")\n";
    text += L"상태: " + StatusText() + L"\n\n";
    text += L"• 아이콘 클릭 또는 Ctrl+Alt+Shift+W: 화면 가운데에 크게 표시\n";
    text += L"• 아이콘 우클릭: 메뉴 / 설정\n";
    text += L"• 초록 점이 붙은 아이콘: 원격 데스크톱(RDP)으로 접속된 상태";
    MSGBOXPARAMSW mb = {sizeof(mb)};
    mb.hwndOwner = g.hwnd;
    mb.hInstance = g.hinst;
    mb.lpszText = text.c_str();
    mb.lpszCaption = L"WhichPC 정보";
    mb.dwStyle = MB_OK | MB_USERICON | MB_SETFOREGROUND;
    mb.lpszIcon = MAKEINTRESOURCEW(IDI_APP);
    MessageBoxIndirectW(&mb);
}

void OnMenuCommand(UINT id) {
    Settings s = g.s;
    switch (id) {
        case 0:
            return;
        case IDM_FLASH:
            Flash();
            return;
        case IDM_OVERLAY:
            s.overlay = !s.overlay;
            break;
        case IDM_TASKBAR_BTN:
            s.taskbarButton = !s.taskbarButton;
            break;
        case IDM_LAPTOP:
            s.device = DEVICE_LAPTOP;
            break;
        case IDM_DESKTOP:
            s.device = DEVICE_DESKTOP;
            break;
        case IDM_MONITORS_PRIMARY:
            s.overlayMonitors = 0;
            break;
        case IDM_MONITORS_ALL:
            s.overlayMonitors = 1;
            break;
        case IDM_SETTINGS:
            ShowSettingsDialog(g.hwnd);
            return;
        case IDM_AUTOSTART:
            SetAutostart(!GetAutostart());
            return;
        case IDM_ABOUT:
            ShowAbout();
            return;
        case IDM_EXIT:
            DestroyWindow(g.hwnd);
            return;
        default:
            if (id >= IDM_CORNER_BASE && id < IDM_CORNER_BASE + CORNER_COUNT) {
                s.overlayCorner = (int)(id - IDM_CORNER_BASE);
                s.overlay = true;
                break;
            }
            return;
    }
    App_ApplySettings(s, GetAutostart());
}

void ShowTrayMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    std::wstring head = Label() + L"  —  " + g.computer;
    AppendMenuW(m, MF_STRING, IDM_FLASH, head.c_str());
    SetMenuDefaultItem(m, IDM_FLASH, FALSE);
    HBITMAP headBmp = CreateDeviceMenuBitmap(g.s.device, Color(), SmallIconSize());
    MENUITEMINFOW mii = {sizeof(mii)};
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = headBmp;
    SetMenuItemInfoW(m, IDM_FLASH, FALSE, &mii);
    std::wstring status = L"    " + StatusText();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING | (g.s.overlay ? MF_CHECKED : 0), IDM_OVERLAY, L"화면 모서리 배지 표시");
    HMENU pos = CreatePopupMenu();
    for (int i = 0; i < CORNER_COUNT; ++i) AppendMenuW(pos, MF_STRING, IDM_CORNER_BASE + i, CornerName(i));
    CheckMenuRadioItem(pos, IDM_CORNER_BASE, IDM_CORNER_BASE + CORNER_COUNT - 1, IDM_CORNER_BASE + g.s.overlayCorner,
                       MF_BYCOMMAND);
    AppendMenuW(pos, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(pos, MF_STRING, IDM_MONITORS_ALL, L"모든 모니터에 표시");
    AppendMenuW(pos, MF_STRING, IDM_MONITORS_PRIMARY, L"주 모니터에만 표시");
    CheckMenuRadioItem(pos, IDM_MONITORS_PRIMARY, IDM_MONITORS_ALL,
                       g.s.overlayMonitors ? IDM_MONITORS_ALL : IDM_MONITORS_PRIMARY, MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pos, L"배지 위치");
    AppendMenuW(m, MF_STRING | (g.s.taskbarButton ? MF_CHECKED : 0), IDM_TASKBAR_BTN, L"작업 표시줄에 큰 버튼 표시");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU dev = CreatePopupMenu();
    AppendMenuW(dev, MF_STRING, IDM_LAPTOP, L"노트북");
    AppendMenuW(dev, MF_STRING, IDM_DESKTOP, L"데스크탑");
    CheckMenuRadioItem(dev, IDM_LAPTOP, IDM_DESKTOP, g.s.device == DEVICE_LAPTOP ? IDM_LAPTOP : IDM_DESKTOP,
                       MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dev, L"이 PC 종류");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"설정...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (GetAutostart() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Windows 시작 시 자동 실행");
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"WhichPC 정보");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"종료");

    OverlaySetSuppressed(true);  // keep the always-on-top badge from covering the menu
    SetForegroundWindow(g.hwnd);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN |
                 (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    UINT cmd = (UINT)TrackPopupMenuEx(m, flags, pt.x, pt.y, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    OverlaySetSuppressed(false);
    DestroyMenu(m);
    DeleteObject(headBmp);
    OnMenuCommand(cmd);
}

// --------------------------------------------------------------------------
// startup
// --------------------------------------------------------------------------
void OnStartup() {
    if (!g.s.firstRunDone) {
        std::wstring text = L"이 PC를 '" + Label() +
                            L"'(으)로 표시합니다. 아이콘을 클릭하면 크게 보여 주고, "
                            L"우클릭하면 종류·색·화면 배지를 바꿀 수 있어요.";
        if (WindowsBuild() && WindowsBuild() < 22000) {
            text += L"\n아이콘이 안 보이면 작업 표시줄의 ^ 에서 끌어다 놓으세요.";
            g.s.win10HintShown = true;
        }
        ShowBalloon(L"WhichPC 실행 중", text);
        g.s.firstRunDone = true;
        SaveSettings(g.s);
    }
    if (g.openSettingsOnStart) {
        ShowSettingsDialog(g.hwnd);
    } else if (!g.quietStart) {
        Flash();
    }
}

void HandleCommand(HWND h, WPARAM cmd) {
    DebugLog(L"command %u", (unsigned)cmd);
    switch (cmd) {
        case CMD_SHOW_SETTINGS:
            ShowSettingsDialog(h);
            break;
        case CMD_FLASH:
            Flash();
            break;
        case CMD_QUIT:
            DestroyWindow(h);
            break;
        case CMD_RELOAD: {
            Settings s;
            LoadSettings(s);
            if (s.device < 0) s.device = g.s.device;
            App_ApplySettings(s, GetAutostart());
            break;
        }
    }
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.msgCommand && msg == g.msgCommand) {  // from a second instance
        HandleCommand(h, wp);
        return 0;
    }
    if (g.msgTaskbarCreated && msg == g.msgTaskbarCreated) {  // explorer (re)started
        g.trayAdded = false;
        UpdateTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            g.hwnd = h;
            g.sessionNotify = WTSRegisterSessionNotification(h, NOTIFY_FOR_THIS_SESSION) != FALSE;
            return 0;
        case WM_TRAYICON:
            if (HIWORD(lp) != kTrayUid) return 0;  // not from our icon
            if (LOWORD(lp) != WM_MOUSEMOVE) DebugLog(L"tray event 0x%04x", LOWORD(lp));
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    Flash();
                    break;
                case WM_LBUTTONDBLCLK:
                    DebugLog(L"settings: tray double-click");
                    ShowSettingsDialog(h);
                    break;
                case WM_CONTEXTMENU:
                    if (SettingsDialogOpen())  // a menu change would be overwritten by the dialog's OK
                        ShowSettingsDialog(h);
                    else
                        ShowTrayMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                    break;
            }
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkeyId) Flash();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_REMOTE_CONNECT) g.pendingRemoteNotice = true;
            SetTimer(h, TIMER_SESSION, 800, nullptr);
            if (g.s.flashOnUnlock &&
                (wp == WTS_SESSION_UNLOCK || wp == WTS_REMOTE_CONNECT || wp == WTS_CONSOLE_CONNECT))
                SetTimer(h, TIMER_DELAYED_FLASH, 2000, nullptr);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            SetTimer(h, TIMER_REFRESH, 700, nullptr);
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA || wp == SPI_SETNONCLIENTMETRICS || wp == SPI_SETICONMETRICS)
                SetTimer(h, TIMER_REFRESH, 700, nullptr);
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_TRAY_RETRY:
                    KillTimer(h, TIMER_TRAY_RETRY);
                    UpdateTrayIcon();
                    break;
                case TIMER_PROMOTE:
                    KillTimer(h, TIMER_PROMOTE);
                    // Explorer writes our NotifyIconSettings entry a moment after NIM_ADD.
                    if (!PromoteTrayIcon(g.s.trayAlwaysShow) && ++g.promoteAttempts < 8)
                        SchedulePromote(1500u * g.promoteAttempts + 1500u);
                    break;
                case TIMER_REFRESH:
                    KillTimer(h, TIMER_REFRESH);
                    RefreshVisuals();
                    break;
                case TIMER_OVERLAY:
                    if (OverlayTick(g.s)) SetTimer(h, TIMER_OVERLAY_ANIM, 16, nullptr);
                    break;
                case TIMER_OVERLAY_ANIM:
                    if (!OverlayAnimate()) KillTimer(h, TIMER_OVERLAY_ANIM);
                    break;
                case TIMER_FLASH:
                    if (!FlashAnimate()) KillTimer(h, TIMER_FLASH);
                    break;
                case TIMER_DELAYED_FLASH:
                    KillTimer(h, TIMER_DELAYED_FLASH);
                    if (GetTickCount64() - g.lastFlash > 4000) Flash();
                    break;
                case TIMER_SESSION: {
                    KillTimer(h, TIMER_SESSION);
                    SessionInfo now = QuerySessionInfo();
                    bool changed = now.remote != g.session.remote || now.clientName != g.session.clientName;
                    g.session = now;
                    if (changed) RefreshVisuals();
                    if (g.pendingRemoteNotice && g.session.remote && g.s.notifyRemote) {
                        std::wstring who = g.session.clientName.empty() ? L"다른 PC" : g.session.clientName;
                        ShowBalloon(L"원격 접속됨 — " + Label(),
                                    who + L"에서 이 " + Label() + L"(" + g.computer + L")에 원격으로 연결했습니다.");
                    }
                    g.pendingRemoteNotice = false;
                    break;
                }
                case TIMER_STARTUP:
                    KillTimer(h, TIMER_STARTUP);
                    OnStartup();
                    break;
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                NOTIFYICONDATAW nid = BaseNid();
                Shell_NotifyIconW(NIM_DELETE, &nid);
                g.trayAdded = false;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            NOTIFYICONDATAW nid = BaseNid();
            Shell_NotifyIconW(NIM_DELETE, &nid);
            g.trayAdded = false;
            OverlayDestroyAll();
            FlashHide();
            if (g.taskbarBtn) DestroyWindow(g.taskbarBtn);
            g.taskbarBtn = nullptr;
            if (g.hotkeyOn) UnregisterHotKey(h, kHotkeyId);
            if (g.sessionNotify) WTSUnRegisterSessionNotification(h);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// --------------------------------------------------------------------------
// command line
// --------------------------------------------------------------------------
bool HasArg(const wchar_t* arg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc && !found; ++i) found = _wcsicmp(argv[i], arg) == 0;
    LocalFree(argv);
    return found;
}

int ArgDeviceType() {  // --type=laptop | --type=desktop
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int type = -1;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--type=laptop")) type = DEVICE_LAPTOP;
        if (!_wcsicmp(argv[i], L"--type=desktop")) type = DEVICE_DESKTOP;
    }
    LocalFree(argv);
    return type;
}

// Forwards a command to the running instance. Returns false if none is running.
bool ForwardToRunning(AppCommand cmd, bool waitForExit) {
    HWND other = FindWindowW(kMainClass, nullptr);
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);
    PostMessageW(other, g.msgCommand, cmd, 0);
    if (waitForExit) {
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (proc) {
            WaitForSingleObject(proc, 5000);
            CloseHandle(proc);
        }
    }
    return true;
}

}  // namespace

// --------------------------------------------------------------------------
// shared with the other modules
// --------------------------------------------------------------------------
HINSTANCE AppInstance() { return g.hinst; }
const Settings& App_Settings() { return g.s; }

void App_ApplySettings(const Settings& next, bool autostart) {
    const bool deviceChanged = next.device != g.s.device;
    const bool promoteChanged = next.trayAlwaysShow != g.s.trayAlwaysShow;
    g.s = next;
    SaveSettings(g.s);
    if (autostart != GetAutostart()) SetAutostart(autostart);
    UpdateHotkey();
    RefreshVisuals();
    if (promoteChanged) PromoteTrayIcon(g.s.trayAlwaysShow);
    if (deviceChanged) Flash();
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    g.hinst = hinst;

    if (HasArg(L"--detect")) {  // used by the installer: exit code = device + 10 * reason + 1
        DetectResult r = DetectDeviceType();
        return 1 + r.device + 10 * r.reason;
    }

    g.msgCommand = RegisterWindowMessageW(kCommandMessage);
    DebugLog(L"start: %ls", GetCommandLineW());

    const int forcedType = ArgDeviceType();
    if (forcedType >= 0) {
        Settings s;
        LoadSettings(s);
        s.device = forcedType;
        SaveSettings(s);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    if (HasArg(L"--quit")) {
        ForwardToRunning(CMD_QUIT, true);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (alreadyRunning) {
        AppCommand cmd = HasArg(L"--flash") ? CMD_FLASH : CMD_SHOW_SETTINGS;
        if (forcedType >= 0 || HasArg(L"--reload")) cmd = CMD_RELOAD;
        ForwardToRunning(cmd, false);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof(icc),
                                ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&icc);
    if (!RenderInit(hinst)) {
        MessageBoxW(nullptr, L"GDI+를 초기화하지 못했습니다.", L"WhichPC", MB_ICONERROR);
        return 1;
    }

    LoadSettings(g.s);
    if (g.s.device < 0) {  // first run without the installer
        g.s.device = DetectDeviceType().device;
        SaveSettings(g.s);
    }
    g.computer = ComputerName();
    g.session = QuerySessionInfo();
    g.quietStart = HasArg(L"--autostart");
    g.openSettingsOnStart = HasArg(L"--settings");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kMainClass;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);

    WNDCLASSEXW tb = {sizeof(tb)};
    tb.lpfnWndProc = TaskbarBtnProc;
    tb.hInstance = hinst;
    tb.lpszClassName = kTaskbarBtnClass;
    tb.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tb.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&tb);
    OverlayRegisterClasses(hinst);

    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // A real (never shown) top-level window: message-only windows miss the
    // TaskbarCreated / display-change broadcasts.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"WhichPC", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hinst,
                                nullptr);
    if (!hwnd) return 1;
    OverlaySetNotifyWindow(hwnd);
    // Let the TaskbarCreated broadcast through UIPI if explorer runs elevated.
    ChangeWindowMessageFilterEx(hwnd, g.msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, g.msgCommand, MSGFLT_ALLOW, nullptr);

    UpdateHotkey();
    RefreshVisuals();
    SetTimer(hwnd, TIMER_STARTUP, g.quietStart ? 3000 : 400, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.trayIcon) DestroyIcon(g.trayIcon);
    if (g.balloonIcon) DestroyIcon(g.balloonIcon);
    RenderShutdown();
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
