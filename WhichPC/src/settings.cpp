// Settings persistence (HKCU\Software\WhichPC) and the settings dialog.
#include "common.h"

#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>

#include "resource.h"

const ColorPreset kColorPresets[] = {
    {L"파랑", RGB(0x25, 0x63, 0xEB)}, {L"주황", RGB(0xEA, 0x58, 0x0C)}, {L"초록", RGB(0x16, 0xA3, 0x4A)},
    {L"보라", RGB(0x7C, 0x3A, 0xED)}, {L"빨강", RGB(0xDC, 0x26, 0x26)}, {L"청록", RGB(0x0D, 0x94, 0x88)},
    {L"분홍", RGB(0xDB, 0x27, 0x77)}, {L"남색", RGB(0x31, 0x2E, 0x81)}, {L"회색", RGB(0x47, 0x55, 0x69)},
};
const int kColorPresetCount = ARRAYSIZE(kColorPresets);

// Remote-viewer processes. When one of their windows is in front, the badge of
// *this* PC is hidden because the user is looking at the other PC's screen.
const wchar_t kDefaultRemoteApps[] =
    L"mstsc.exe; msrdc.exe; msrdcw.exe; rdclient.windows.exe; windows365.exe; windowsapp.exe; "
    L"parsecd.exe; moonlight.exe; anydesk.exe; rustdesk.exe; teamviewer.exe; vncviewer.exe; "
    L"tvnviewer.exe; strwinclt.exe; nxplayer.bin; streaming_client.exe; remotedesktopmanager.exe; "
    L"mremoteng.exe; royalts.exe; ultraviewer_desktop.exe; todesk.exe; remoting_desktop.exe; "
    L"jumpdesktop.exe; remoteview.exe";

const wchar_t* DeviceName(int device) { return device == DEVICE_LAPTOP ? L"노트북" : L"데스크탑"; }

const wchar_t* CornerName(int corner) {
    static const wchar_t* names[CORNER_COUNT] = {L"왼쪽 위",   L"위 가운데",   L"오른쪽 위",
                                                 L"왼쪽 아래", L"아래 가운데", L"오른쪽 아래"};
    return (corner >= 0 && corner < CORNER_COUNT) ? names[corner] : names[CORNER_BOTTOM_RIGHT];
}

COLORREF EffectiveColor(const Settings& s) {
    if (s.colorIndex >= 0 && s.colorIndex < kColorPresetCount) return kColorPresets[s.colorIndex].color;
    if (s.colorIndex == kCustomColor) return s.customColor;
    return kColorPresets[s.device == DEVICE_LAPTOP ? 0 : 1].color;
}

std::wstring EffectiveLabel(const Settings& s) { return s.label.empty() ? DeviceName(s.device) : s.label; }

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::wstring> ParseAppList(const std::wstring& list) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&] {
        std::wstring t = Trim(cur);
        std::transform(t.begin(), t.end(), t.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (wchar_t c : list) {
        if (c == L';' || c == L',' || c == L'\n' || c == L'\r')
            flush();
        else
            cur += c;
    }
    flush();
    return out;
}

// --------------------------------------------------------------------------
// registry
// --------------------------------------------------------------------------
static DWORD ReadDword(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = 0, bytes = sizeof(v);
    if (k && RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &bytes) == ERROR_SUCCESS) return v;
    return def;
}

static std::wstring ReadString(HKEY k, const wchar_t* name, const std::wstring& def) {
    if (!k) return def;
    DWORD bytes = 0;
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return def;
    std::wstring s(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, &s[0], &bytes) != ERROR_SUCCESS) return def;
    s.resize(wcslen(s.c_str()));
    return s;
}

static void WriteDword(HKEY k, const wchar_t* name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

static void WriteString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                   (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

void LoadSettings(Settings& s) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) k = nullptr;
    Settings d;
    DWORD dev = ReadDword(k, L"DeviceType", 0xFFFFFFFF);
    s.device = (dev == DEVICE_LAPTOP || dev == DEVICE_DESKTOP) ? (int)dev : -1;
    s.label = ReadString(k, L"Label", L"");
    s.colorIndex = (int)ReadDword(k, L"ColorIndex", (DWORD)d.colorIndex);
    if (!(s.colorIndex == -1 || s.colorIndex == kCustomColor ||
          (s.colorIndex >= 0 && s.colorIndex < kColorPresetCount)))
        s.colorIndex = -1;
    s.customColor = ReadDword(k, L"CustomColor", d.customColor) & 0xFFFFFF;
    s.trayAlwaysShow = ReadDword(k, L"TrayAlwaysShow", d.trayAlwaysShow) != 0;
    s.taskbarButton = ReadDword(k, L"TaskbarButton", d.taskbarButton) != 0;
    s.overlay = ReadDword(k, L"Overlay", d.overlay) != 0;
    s.overlayCorner = std::clamp((int)ReadDword(k, L"OverlayCorner", d.overlayCorner), 0, CORNER_COUNT - 1);
    s.overlaySize = std::clamp((int)ReadDword(k, L"OverlaySize", d.overlaySize), 0, 2);
    s.overlayMonitors = std::clamp((int)ReadDword(k, L"OverlayMonitors", d.overlayMonitors), 0, 1);
    s.overlayOpacity = std::clamp((int)ReadDword(k, L"OverlayOpacity", d.overlayOpacity), 20, 100);
    s.hideOnHover = ReadDword(k, L"HideOnHover", d.hideOnHover) != 0;
    s.hideOverRemote = ReadDword(k, L"HideOverRemote", d.hideOverRemote) != 0;
    s.hideOnFullscreen = ReadDword(k, L"HideOnFullscreen", d.hideOnFullscreen) != 0;
    s.remoteApps = ReadString(k, L"RemoteApps", kDefaultRemoteApps);
    s.notifyRemote = ReadDword(k, L"NotifyRemote", d.notifyRemote) != 0;
    s.flashOnUnlock = ReadDword(k, L"FlashOnUnlock", d.flashOnUnlock) != 0;
    s.hotkey = ReadDword(k, L"Hotkey", d.hotkey) != 0;
    s.firstRunDone = ReadDword(k, L"FirstRunDone", 0) != 0;
    s.win10HintShown = ReadDword(k, L"Win10HintShown", 0) != 0;
    if (k) RegCloseKey(k);
}

void SaveSettings(const Settings& s) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (s.device == DEVICE_LAPTOP || s.device == DEVICE_DESKTOP) WriteDword(k, L"DeviceType", (DWORD)s.device);
    WriteString(k, L"Label", s.label);
    WriteDword(k, L"ColorIndex", (DWORD)s.colorIndex);
    WriteDword(k, L"CustomColor", s.customColor);
    WriteDword(k, L"TrayAlwaysShow", s.trayAlwaysShow);
    WriteDword(k, L"TaskbarButton", s.taskbarButton);
    WriteDword(k, L"Overlay", s.overlay);
    WriteDword(k, L"OverlayCorner", (DWORD)s.overlayCorner);
    WriteDword(k, L"OverlaySize", (DWORD)s.overlaySize);
    WriteDword(k, L"OverlayMonitors", (DWORD)s.overlayMonitors);
    WriteDword(k, L"OverlayOpacity", (DWORD)s.overlayOpacity);
    WriteDword(k, L"HideOnHover", s.hideOnHover);
    WriteDword(k, L"HideOverRemote", s.hideOverRemote);
    WriteDword(k, L"HideOnFullscreen", s.hideOnFullscreen);
    WriteString(k, L"RemoteApps", s.remoteApps);
    WriteDword(k, L"NotifyRemote", s.notifyRemote);
    WriteDword(k, L"FlashOnUnlock", s.flashOnUnlock);
    WriteDword(k, L"Hotkey", s.hotkey);
    WriteDword(k, L"FirstRunDone", s.firstRunDone);
    WriteDword(k, L"Win10HintShown", s.win10HintShown);
    RegCloseKey(k);
}

bool GetAutostart() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void SetAutostart(bool enable) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (enable)
        WriteString(k, kRunValue, L"\"" + ExePath() + L"\" --autostart");
    else
        RegDeleteValueW(k, kRunValue);
    RegCloseKey(k);
}

// --------------------------------------------------------------------------
// dialog
// --------------------------------------------------------------------------
namespace {

struct DialogState {
    Settings s;
    int lastColorSel = 0;
    HWND hwnd = nullptr;
};

DialogState* g_dlg = nullptr;

int DlgDevice(HWND h) { return IsDlgButtonChecked(h, IDC_TYPE_LAPTOP) == BST_CHECKED ? DEVICE_LAPTOP : DEVICE_DESKTOP; }

std::wstring GetText(HWND h, int id) {
    HWND c = GetDlgItem(h, id);
    int n = GetWindowTextLengthW(c);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(c, &s[0], n + 1);
    s.resize(n);
    return s;
}

// Collects the controls into a Settings value (based on the original one).
Settings Collect(HWND h, const Settings& base) {
    Settings s = base;
    s.device = DlgDevice(h);
    s.label = Trim(GetText(h, IDC_LABEL));
    HWND color = GetDlgItem(h, IDC_COLOR);
    int sel = ComboBox_GetCurSel(color);
    if (sel >= 0) s.colorIndex = (int)ComboBox_GetItemData(color, sel);
    s.trayAlwaysShow = IsDlgButtonChecked(h, IDC_TRAY_ALWAYS) == BST_CHECKED;
    s.taskbarButton = IsDlgButtonChecked(h, IDC_TASKBAR_BTN) == BST_CHECKED;
    s.overlay = IsDlgButtonChecked(h, IDC_OVERLAY) == BST_CHECKED;
    s.overlayCorner = std::max(0, ComboBox_GetCurSel(GetDlgItem(h, IDC_CORNER)));
    s.overlaySize = std::max(0, ComboBox_GetCurSel(GetDlgItem(h, IDC_BADGE_SIZE)));
    s.overlayMonitors = ComboBox_GetCurSel(GetDlgItem(h, IDC_MONITORS)) == 1 ? 0 : 1;
    s.overlayOpacity = (int)SendDlgItemMessageW(h, IDC_OPACITY, TBM_GETPOS, 0, 0);
    s.hideOnHover = IsDlgButtonChecked(h, IDC_HIDE_HOVER) == BST_CHECKED;
    s.hideOverRemote = IsDlgButtonChecked(h, IDC_HIDE_REMOTE) == BST_CHECKED;
    s.hideOnFullscreen = IsDlgButtonChecked(h, IDC_HIDE_FULLSCREEN) == BST_CHECKED;
    std::wstring apps = GetText(h, IDC_REMOTE_APPS);
    std::wstring joined;
    for (const auto& a : ParseAppList(apps)) joined += (joined.empty() ? L"" : L"; ") + a;
    s.remoteApps = joined;
    s.notifyRemote = IsDlgButtonChecked(h, IDC_NOTIFY_REMOTE) == BST_CHECKED;
    s.flashOnUnlock = IsDlgButtonChecked(h, IDC_FLASH_UNLOCK) == BST_CHECKED;
    s.hotkey = IsDlgButtonChecked(h, IDC_HOTKEY) == BST_CHECKED;
    return s;
}

void UpdateOpacityLabel(HWND h) {
    int v = (int)SendDlgItemMessageW(h, IDC_OPACITY, TBM_GETPOS, 0, 0);
    wchar_t buf[16];
    wsprintfW(buf, L"%d%%", v);
    SetDlgItemTextW(h, IDC_OPACITY_LABEL, buf);
}

void UpdateEnabled(HWND h) {
    BOOL on = IsDlgButtonChecked(h, IDC_OVERLAY) == BST_CHECKED;
    const int ids[] = {IDC_CORNER,        IDC_BADGE_SIZE,  IDC_MONITORS,      IDC_OPACITY,
                       IDC_OPACITY_LABEL, IDC_HIDE_HOVER,  IDC_HIDE_REMOTE,   IDC_HIDE_FULLSCREEN,
                       IDC_STATIC_CORNER, IDC_STATIC_SIZE, IDC_STATIC_OPACITY};
    for (int id : ids) EnableWindow(GetDlgItem(h, id), on);
    BOOL remoteOn = on && IsDlgButtonChecked(h, IDC_HIDE_REMOTE) == BST_CHECKED;
    EnableWindow(GetDlgItem(h, IDC_REMOTE_APPS), remoteOn);
    EnableWindow(GetDlgItem(h, IDC_REMOTE_DEFAULT), remoteOn);
    EnableWindow(GetDlgItem(h, IDC_STATIC_REMOTE), remoteOn);
}

void SetDetectInfo(HWND h, const DetectResult& r) {
    const wchar_t* why = r.reason == DETECT_CHASSIS   ? L"섀시 정보"
                         : r.reason == DETECT_BATTERY ? L"배터리 유무"
                                                      : L"기본값";
    std::wstring t = std::wstring(L"자동 감지 결과: ") + DeviceName(r.device) + L" (" + why + L" 기준)";
    SetDlgItemTextW(h, IDC_DETECT_INFO, t.c_str());
}

void FillColorCombo(HWND h, const Settings& s) {
    HWND c = GetDlgItem(h, IDC_COLOR);
    ComboBox_ResetContent(c);
    int idx = ComboBox_AddString(c, L"기본 색");
    ComboBox_SetItemData(c, idx, (LPARAM)-1);
    for (int i = 0; i < kColorPresetCount; ++i) {
        idx = ComboBox_AddString(c, kColorPresets[i].name);
        ComboBox_SetItemData(c, idx, i);
    }
    idx = ComboBox_AddString(c, L"사용자 지정...");
    ComboBox_SetItemData(c, idx, kCustomColor);
    int sel = 0;
    for (int i = 0; i < ComboBox_GetCount(c); ++i)
        if ((int)ComboBox_GetItemData(c, i) == s.colorIndex) sel = i;
    ComboBox_SetCurSel(c, sel);
    g_dlg->lastColorSel = sel;
}

void InitDialog(HWND h) {
    const Settings& s = g_dlg->s;
    HICON big = LoadIconW(AppInstance(), MAKEINTRESOURCEW(IDI_APP));
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)big);

    CheckRadioButton(h, IDC_TYPE_LAPTOP, IDC_TYPE_DESKTOP,
                     s.device == DEVICE_LAPTOP ? IDC_TYPE_LAPTOP : IDC_TYPE_DESKTOP);
    SetDetectInfo(h, DetectDeviceType());
    SetDlgItemTextW(h, IDC_LABEL, s.label.c_str());
    SendDlgItemMessageW(h, IDC_LABEL, EM_LIMITTEXT, 24, 0);
    SendDlgItemMessageW(h, IDC_LABEL, EM_SETCUEBANNER, TRUE, (LPARAM)DeviceName(s.device));
    FillColorCombo(h, s);

    CheckDlgButton(h, IDC_TRAY_ALWAYS, s.trayAlwaysShow ? BST_CHECKED : BST_UNCHECKED);
    if (WindowsBuild() && WindowsBuild() < 22000) {
        SetDlgItemTextW(h, IDC_TRAY_ALWAYS, L"알림 영역에 아이콘 항상 표시 (Windows 11 전용)");
        EnableWindow(GetDlgItem(h, IDC_TRAY_ALWAYS), FALSE);
    }
    CheckDlgButton(h, IDC_TASKBAR_BTN, s.taskbarButton ? BST_CHECKED : BST_UNCHECKED);

    CheckDlgButton(h, IDC_OVERLAY, s.overlay ? BST_CHECKED : BST_UNCHECKED);
    HWND corner = GetDlgItem(h, IDC_CORNER);
    for (int i = 0; i < CORNER_COUNT; ++i) ComboBox_AddString(corner, CornerName(i));
    ComboBox_SetCurSel(corner, s.overlayCorner);
    HWND size = GetDlgItem(h, IDC_BADGE_SIZE);
    ComboBox_AddString(size, L"작게");
    ComboBox_AddString(size, L"보통");
    ComboBox_AddString(size, L"크게");
    ComboBox_SetCurSel(size, s.overlaySize);
    HWND mons = GetDlgItem(h, IDC_MONITORS);
    ComboBox_AddString(mons, L"모든 모니터");
    ComboBox_AddString(mons, L"주 모니터만");
    ComboBox_SetCurSel(mons, s.overlayMonitors == 1 ? 0 : 1);
    SendDlgItemMessageW(h, IDC_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100));
    SendDlgItemMessageW(h, IDC_OPACITY, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(h, IDC_OPACITY, TBM_SETPOS, TRUE, s.overlayOpacity);
    UpdateOpacityLabel(h);
    CheckDlgButton(h, IDC_HIDE_HOVER, s.hideOnHover ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_HIDE_REMOTE, s.hideOverRemote ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_HIDE_FULLSCREEN, s.hideOnFullscreen ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(h, IDC_REMOTE_APPS, s.remoteApps.c_str());

    CheckDlgButton(h, IDC_NOTIFY_REMOTE, s.notifyRemote ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_FLASH_UNLOCK, s.flashOnUnlock ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_HOTKEY, s.hotkey ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(h, IDC_AUTOSTART, GetAutostart() ? BST_CHECKED : BST_UNCHECKED);
    UpdateEnabled(h);
}

void PaintRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void DrawPreview(HWND h, const DRAWITEMSTRUCT* di) {
    Settings s = Collect(h, g_dlg->s);
    const UINT dpi = GetDpiForWindow(h);
    const float scale = dpi / 96.f;
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    const int w = rc.right - rc.left, hgt = rc.bottom - rc.top;

    // double buffer
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, hgt);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT local = {0, 0, w, hgt};
    FillRect(mem, &local, GetSysColorBrush(COLOR_BTNFACE));
    PaintRoundRect(mem, local, (int)(10 * scale), GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_3DSHADOW));

    const COLORREF color = EffectiveColor(s);
    const int iconSize = (int)(40 * scale);
    const int pad = (int)(12 * scale);
    HICON icon = CreateDeviceIcon(s.device, color, iconSize, false);
    DrawIconEx(mem, pad, (hgt - iconSize) / 2, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    DestroyIcon(icon);

    // text
    // Start from the dialog font (Malgun Gothic): the system message font may lack Hangul glyphs.
    LOGFONTW lf = {};
    HFONT dlgFont = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!dlgFont || !GetObjectW(dlgFont, sizeof(lf), &lf)) {
        NONCLIENTMETRICSW ncm = {sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        lf = ncm.lfMessageFont;
    }
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = -(LONG)(17 * scale);
    HFONT bold = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_NORMAL;
    lf.lfHeight = -(LONG)(12 * scale);
    HFONT small = CreateFontIndirectW(&lf);
    SetBkMode(mem, TRANSPARENT);
    const int tx = pad + iconSize + (int)(10 * scale);
    HGDIOBJ of = SelectObject(mem, bold);
    SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
    std::wstring title = EffectiveLabel(s);
    RECT tr = {tx, hgt / 2 - (int)(20 * scale), w / 2, hgt / 2};
    DrawTextW(mem, title.c_str(), -1, &tr, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, small);
    SetTextColor(mem, GetSysColor(COLOR_GRAYTEXT));
    std::wstring sub = ComputerName() + L" · 작업 표시줄 아이콘";
    RECT sr = {tx, hgt / 2 + (int)(2 * scale), w / 2 + (int)(40 * scale), hgt};
    DrawTextW(mem, sub.c_str(), -1, &sr, DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, of);
    DeleteObject(bold);
    DeleteObject(small);

    // the real screen badge, right aligned
    BadgeContent bc;
    bc.device = s.device;
    bc.color = color;
    bc.label = title;
    static const float sizeFactor[3] = {0.82f, 1.0f, 1.3f};
    Image32 badge = RenderBadge(bc, scale * sizeFactor[std::clamp(s.overlaySize, 0, 2)]);
    if (badge.bmp) {
        HDC bdc = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(bdc, badge.bmp);
        BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        int bx = w - badge.w - pad / 2, by = (hgt - badge.h) / 2;
        GdiAlphaBlend(mem, bx, by, badge.w, badge.h, bdc, 0, 0, badge.w, badge.h, bf);
        SelectObject(bdc, ob);
        DeleteDC(bdc);
        badge.Free();
    }

    BitBlt(dc, rc.left, rc.top, w, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void DrawColorItem(HWND h, const DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1) return;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool selected = (di->itemState & ODS_SELECTED) != 0;
    FillRect(dc, &r, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    int data = (int)di->itemData;
    Settings tmp = g_dlg->s;
    tmp.device = DlgDevice(h);
    tmp.colorIndex = data;
    COLORREF c = EffectiveColor(tmp);
    const int hgt = r.bottom - r.top;
    const int sw = hgt - 6;
    RECT sr = {r.left + 4, r.top + 3, r.left + 4 + sw, r.top + 3 + sw};
    PaintRoundRect(dc, sr, sw / 3, c, c);
    wchar_t text[64] = {};
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    std::wstring label = text;
    if (data == -1) label += std::wstring(L"  (") + (tmp.device == DEVICE_LAPTOP ? L"파랑" : L"주황") + L")";
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    RECT tr = {sr.right + 6, r.top, r.right, r.bottom};
    DrawTextW(dc, label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (di->itemState & ODS_FOCUS) DrawFocusRect(dc, &r);
}

void OnColorChanged(HWND h) {
    HWND c = GetDlgItem(h, IDC_COLOR);
    int sel = ComboBox_GetCurSel(c);
    if (sel < 0) return;
    if ((int)ComboBox_GetItemData(c, sel) == kCustomColor) {
        static COLORREF custom[16];
        CHOOSECOLORW cc = {sizeof(cc)};
        cc.hwndOwner = h;
        cc.rgbResult = g_dlg->s.customColor;
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            g_dlg->s.customColor = cc.rgbResult;
        } else if (g_dlg->s.colorIndex != kCustomColor) {
            ComboBox_SetCurSel(c, g_dlg->lastColorSel);
            return;
        }
    }
    g_dlg->lastColorSel = ComboBox_GetCurSel(c);
    InvalidateRect(GetDlgItem(h, IDC_PREVIEW), nullptr, FALSE);
}

bool Apply(HWND h) {
    Settings s = Collect(h, g_dlg->s);
    g_dlg->s = s;
    App_ApplySettings(s, IsDlgButtonChecked(h, IDC_AUTOSTART) == BST_CHECKED);
    return true;
}

INT_PTR CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG:
            g_dlg->hwnd = h;
            InitDialog(h);
            SetForegroundWindow(h);
            return TRUE;
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlID == IDC_COLOR) {
                mi->itemHeight = MulDiv(20, GetDpiForWindow(h), 96);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (di->CtlID == IDC_PREVIEW) {
                DrawPreview(h, di);
                return TRUE;
            }
            if (di->CtlID == IDC_COLOR) {
                DrawColorItem(h, di);
                return TRUE;
            }
            break;
        }
        case WM_HSCROLL:
            if ((HWND)lp == GetDlgItem(h, IDC_OPACITY)) UpdateOpacityLabel(h);
            break;
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == IDC_TRAY_SETTINGS && (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
                ShellExecuteW(h, L"open", L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            switch (id) {
                case IDC_TYPE_LAPTOP:
                case IDC_TYPE_DESKTOP:
                    SendDlgItemMessageW(h, IDC_LABEL, EM_SETCUEBANNER, TRUE, (LPARAM)DeviceName(DlgDevice(h)));
                    InvalidateRect(GetDlgItem(h, IDC_COLOR), nullptr, TRUE);
                    InvalidateRect(GetDlgItem(h, IDC_PREVIEW), nullptr, FALSE);
                    return TRUE;
                case IDC_DETECT: {
                    DetectResult r = DetectDeviceType();
                    CheckRadioButton(h, IDC_TYPE_LAPTOP, IDC_TYPE_DESKTOP,
                                     r.device == DEVICE_LAPTOP ? IDC_TYPE_LAPTOP : IDC_TYPE_DESKTOP);
                    SetDetectInfo(h, r);
                    SendMessageW(h, WM_COMMAND, MAKEWPARAM(IDC_TYPE_LAPTOP, BN_CLICKED), 0);
                    return TRUE;
                }
                case IDC_LABEL:
                    if (code == EN_CHANGE) InvalidateRect(GetDlgItem(h, IDC_PREVIEW), nullptr, FALSE);
                    return TRUE;
                case IDC_COLOR:
                    if (code == CBN_SELCHANGE) OnColorChanged(h);
                    return TRUE;
                case IDC_BADGE_SIZE:
                    if (code == CBN_SELCHANGE) InvalidateRect(GetDlgItem(h, IDC_PREVIEW), nullptr, FALSE);
                    return TRUE;
                case IDC_OVERLAY:
                case IDC_HIDE_REMOTE:
                    UpdateEnabled(h);
                    return TRUE;
                case IDC_REMOTE_DEFAULT:
                    SetDlgItemTextW(h, IDC_REMOTE_APPS, kDefaultRemoteApps);
                    return TRUE;
                case IDC_APPLY:
                    Apply(h);
                    return TRUE;
                case IDOK:
                    Apply(h);
                    EndDialog(h, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(h, IDCANCEL);
                    return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

}  // namespace

bool SettingsDialogOpen() { return g_dlg != nullptr; }

void ShowSettingsDialog(HWND owner) {
    if (g_dlg) {  // already open: bring it to the front
        if (g_dlg->hwnd) {
            ShowWindow(g_dlg->hwnd, SW_RESTORE);
            SetForegroundWindow(g_dlg->hwnd);
        }
        return;
    }
    DialogState state;
    state.s = App_Settings();
    g_dlg = &state;
    DialogBoxParamW(AppInstance(), MAKEINTRESOURCEW(IDD_SETTINGS), owner, SettingsProc, 0);
    g_dlg = nullptr;
}
