// WhichPC - shows on the taskbar whether the screen you are looking at
// belongs to the laptop or the desktop.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <string>
#include <vector>

#define WHICHPC_VERSION_W L"1.0.0"

enum DeviceType { DEVICE_LAPTOP = 0, DEVICE_DESKTOP = 1 };

enum Corner {
    CORNER_TOP_LEFT = 0,
    CORNER_TOP_CENTER,
    CORNER_TOP_RIGHT,
    CORNER_BOTTOM_LEFT,
    CORNER_BOTTOM_CENTER,
    CORNER_BOTTOM_RIGHT,
    CORNER_COUNT
};

constexpr wchar_t kMainClass[] = L"WhichPC.MainWindow";
constexpr wchar_t kRegKey[] = L"Software\\WhichPC";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"WhichPC";

#define WM_TRAYICON (WM_APP + 1)  // notification-area callback (only Explorer sends it)
// Commands from a second instance use a registered message (wParam = AppCommand), so
// that WM_APP-range broadcasts from other programs can never trigger them.
constexpr wchar_t kCommandMessage[] = L"WhichPC.AppCommand.5c1e2b7a";

enum AppCommand { CMD_SHOW_SETTINGS = 1, CMD_FLASH = 2, CMD_QUIT = 3, CMD_RELOAD = 4 };

// --------------------------------------------------------------------------
// settings.cpp
// --------------------------------------------------------------------------
struct Settings {
    int device = -1;      // DeviceType, -1 = not configured yet
    std::wstring label;   // custom display name, empty = default
    int colorIndex = -1;  // -1 = default for the device, else preset index, kCustomColor = custom
    COLORREF customColor = RGB(0x25, 0x63, 0xEB);
    bool trayAlwaysShow = true;
    bool taskbarButton = false;
    bool overlay = false;
    int overlayCorner = CORNER_BOTTOM_RIGHT;
    int overlaySize = 1;      // 0 small, 1 normal, 2 large
    int overlayMonitors = 1;  // 0 primary only, 1 all monitors
    int overlayOpacity = 90;  // percent
    bool hideOnHover = true;
    bool hideOverRemote = true;
    bool hideOnFullscreen = false;
    std::wstring remoteApps;
    bool notifyRemote = true;
    bool flashOnUnlock = true;
    bool hotkey = true;
    bool firstRunDone = false;
    bool win10HintShown = false;
};

struct ColorPreset {
    const wchar_t* name;
    COLORREF color;
};
extern const ColorPreset kColorPresets[];
extern const int kColorPresetCount;
constexpr int kCustomColor = 100;

extern const wchar_t kDefaultRemoteApps[];

void LoadSettings(Settings& s);
void SaveSettings(const Settings& s);
bool GetAutostart();
void SetAutostart(bool enable);
COLORREF EffectiveColor(const Settings& s);
std::wstring EffectiveLabel(const Settings& s);
const wchar_t* DeviceName(int device);
const wchar_t* CornerName(int corner);
std::vector<std::wstring> ParseAppList(const std::wstring& list);

// Opens the (modal) settings dialog. Applies changes through App_ApplySettings.
void ShowSettingsDialog(HWND owner);
bool SettingsDialogOpen();

// --------------------------------------------------------------------------
// system.cpp
// --------------------------------------------------------------------------
enum DetectReason { DETECT_DEFAULT = 0, DETECT_CHASSIS = 1, DETECT_BATTERY = 2 };
struct DetectResult {
    int device;
    int reason;
};
DetectResult DetectDeviceType();

struct SessionInfo {
    bool remote = false;
    std::wstring clientName;  // computer that is connected over RDP
};
SessionInfo QuerySessionInfo();

std::wstring ComputerName();
// Appends a line to the file named by the WHICHPC_LOG environment variable (no-op when unset).
void DebugLog(const wchar_t* fmt, ...);
std::wstring ExePath();
std::wstring ProcessNameOfWindow(HWND hwnd);  // lower-case file name
DWORD WindowsBuild();
UINT DpiForMonitor(HMONITOR mon);
UINT PrimaryDpi();
RECT WindowVisibleRect(HWND hwnd);

// Sets IsPromoted on our entry under HKCU\Control Panel\NotifyIconSettings
// (Windows 11). Returns true when the entry was found.
bool PromoteTrayIcon(bool promote);

// --------------------------------------------------------------------------
// render.cpp
// --------------------------------------------------------------------------
bool RenderInit(HINSTANCE hinst);
void RenderShutdown();

// Straight (non-premultiplied) BGRA pixels of the device icon.
std::vector<DWORD> ComposeIconPixels(int device, COLORREF color, int size, bool remote);
HICON CreateDeviceIcon(int device, COLORREF color, int size, bool remote);
HBITMAP CreateDeviceMenuBitmap(int device, COLORREF color, int size);

// A premultiplied 32bpp top-down DIB used for layered windows.
struct Image32 {
    HBITMAP bmp = nullptr;
    DWORD* bits = nullptr;
    int w = 0, h = 0;
    void Free();
};

struct BadgeContent {
    int device = DEVICE_LAPTOP;
    COLORREF color = 0;
    std::wstring label;
    bool remote = false;
};
Image32 RenderBadge(const BadgeContent& c, float scale);

struct CardContent {
    int device = DEVICE_LAPTOP;
    COLORREF color = 0;
    std::wstring title;
    std::wstring line1;
    std::wstring line2;
    bool remote = false;
};
Image32 RenderCard(const CardContent& c, float scale);

// Thumbnail-sized card for the taskbar button preview (non-premultiplied is fine for DWM).
HBITMAP RenderThumbnail(const CardContent& c, int maxW, int maxH);

// --------------------------------------------------------------------------
// overlay.cpp
// --------------------------------------------------------------------------
bool OverlayRegisterClasses(HINSTANCE hinst);
void OverlayRebuild(const Settings& s, const BadgeContent& content);
void OverlayDestroyAll();
bool OverlayTick(const Settings& s);  // returns true while an animation is running
bool OverlayAnimate();                // returns true while still animating
void OverlayReassertTopmost();
void OverlaySetSuppressed(bool suppressed);
void OverlaySetNotifyWindow(HWND hwnd);  // hide every badge while a menu is open
bool OverlayIsOwnWindow(HWND hwnd);

void FlashShow(const CardContent& c);
bool FlashAnimate();  // returns true while visible
void FlashHide();

// --------------------------------------------------------------------------
// main.cpp
// --------------------------------------------------------------------------
HINSTANCE AppInstance();
const Settings& App_Settings();
void App_ApplySettings(const Settings& s, bool autostart);
