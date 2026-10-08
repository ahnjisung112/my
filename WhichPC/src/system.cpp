// Hardware / session / shell queries.
#include "common.h"

#include <dwmapi.h>
#include <powerbase.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <cwctype>

// --------------------------------------------------------------------------
// laptop / desktop detection
// --------------------------------------------------------------------------
namespace {

#pragma pack(push, 1)
struct RawSMBIOSData {
    BYTE Used20CallingMethod;
    BYTE SMBIOSMajorVersion;
    BYTE SMBIOSMinorVersion;
    BYTE DmiRevision;
    DWORD Length;
    BYTE SMBIOSTableData[1];
};
#pragma pack(pop)

constexpr DWORD kRsmb = ('R' << 24) | ('S' << 16) | ('M' << 8) | 'B';

// SMBIOS "System Enclosure" (type 3) chassis type, or 0 when unavailable.
int ReadChassisType() {
    UINT size = GetSystemFirmwareTable(kRsmb, 0, nullptr, 0);
    if (size < sizeof(RawSMBIOSData)) return 0;
    std::vector<BYTE> buf(size);
    if (GetSystemFirmwareTable(kRsmb, 0, buf.data(), size) != size) return 0;
    auto* raw = reinterpret_cast<RawSMBIOSData*>(buf.data());
    const BYTE* p = raw->SMBIOSTableData;
    const BYTE* end = p + std::min<DWORD>(raw->Length, size - offsetof(RawSMBIOSData, SMBIOSTableData));
    while (p + 4 <= end) {
        BYTE type = p[0], len = p[1];
        if (len < 4 || p + len > end) break;
        if (type == 3 && len > 5) return p[5] & 0x7F;
        if (type == 127) break;   // end-of-table
        const BYTE* q = p + len;  // skip the string-set (double NUL terminated)
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        p = q + 2;
    }
    return 0;
}

}  // namespace

DetectResult DetectDeviceType() {
    switch (ReadChassisType()) {
        case 8:   // Portable
        case 9:   // Laptop
        case 10:  // Notebook
        case 11:  // Hand Held
        case 14:  // Sub Notebook
        case 30:  // Tablet
        case 31:  // Convertible
        case 32:  // Detachable
            return {DEVICE_LAPTOP, DETECT_CHASSIS};
        case 3:   // Desktop
        case 4:   // Low Profile Desktop
        case 5:   // Pizza Box
        case 6:   // Mini Tower
        case 7:   // Tower
        case 13:  // All in One
        case 15:  // Space-saving
        case 16:  // Lunch Box
        case 17:  // Main Server Chassis
        case 23:  // Rack Mount
        case 24:  // Sealed-case PC
        case 35:  // Mini PC
        case 36:  // Stick PC
            return {DEVICE_DESKTOP, DETECT_CHASSIS};
        default:
            break;
    }
    // Fall back to the battery: a long-term system battery means a laptop,
    // a UPS reports itself as a short-term battery and is ignored.
    SYSTEM_POWER_CAPABILITIES caps = {};
    if (CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &caps, sizeof(caps)) == 0) {
        if (caps.SystemBatteriesPresent && !caps.BatteriesAreShortTerm) return {DEVICE_LAPTOP, DETECT_BATTERY};
        return {DEVICE_DESKTOP, DETECT_BATTERY};
    }
    SYSTEM_POWER_STATUS sps = {};
    if (GetSystemPowerStatus(&sps)) {
        if (sps.BatteryFlag == 128) return {DEVICE_DESKTOP, DETECT_BATTERY};
        if (sps.BatteryFlag != 255) return {DEVICE_LAPTOP, DETECT_BATTERY};
    }
    return {DEVICE_DESKTOP, DETECT_DEFAULT};
}

// --------------------------------------------------------------------------
// session
// --------------------------------------------------------------------------
SessionInfo QuerySessionInfo() {
    SessionInfo si;
    si.remote = GetSystemMetrics(SM_REMOTESESSION) != 0;
    LPWSTR buf = nullptr;
    DWORD bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSClientProtocolType, &buf,
                                    &bytes)) {
        if (buf && bytes >= sizeof(USHORT) && *reinterpret_cast<USHORT*>(buf) == 2) si.remote = true;
        WTSFreeMemory(buf);
    }
    if (si.remote) {
        buf = nullptr;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSClientName, &buf, &bytes)) {
            if (buf) si.clientName = buf;
            WTSFreeMemory(buf);
        }
    }
    return si;
}

// --------------------------------------------------------------------------
// misc
// --------------------------------------------------------------------------
void DebugLog(const wchar_t* fmt, ...) {
    static int enabled = -1;
    static wchar_t path[MAX_PATH];
    if (enabled < 0) {
        DWORD n = GetEnvironmentVariableW(L"WHICHPC_LOG", path, MAX_PATH);
        enabled = n > 0 && n < MAX_PATH;
    }
    if (!enabled) return;
    wchar_t line[1024];
    int n = swprintf(line, ARRAYSIZE(line), L"%10llu [%lu] ", GetTickCount64(), GetCurrentProcessId());
    va_list args;
    va_start(args, fmt);
    vswprintf(line + n, ARRAYSIZE(line) - n - 2, fmt, args);
    va_end(args);
    wcscat(line, L"\r\n");
    char utf8[4096];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    if (len > 1) WriteFile(f, utf8, len - 1, &written, nullptr);
    CloseHandle(f);
}

std::wstring ComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 64] = {};
    DWORD n = ARRAYSIZE(name);
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n) && name[0]) return name;
    n = ARRAYSIZE(name);
    if (GetComputerNameW(name, &n)) return name;
    return L"PC";
}

std::wstring ExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &path[0], (DWORD)path.size());
        if (n == 0) return L"";
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring ProcessNameOfWindow(HWND hwnd) {
    static DWORD cachedPid = 0;
    static std::wstring cachedName;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    if (pid == cachedPid) return cachedName;
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            const wchar_t* slash = wcsrchr(path, L'\\');
            name = Lower(slash ? slash + 1 : path);
        }
        CloseHandle(h);
    }
    cachedPid = pid;
    cachedName = name;
    return name;
}

DWORD WindowsBuild() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    static DWORD build = 0;
    if (build) return build;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                    : nullptr;
    OSVERSIONINFOW vi = {sizeof(vi)};
    if (fn && fn(&vi) == 0) build = vi.dwBuildNumber;
    return build;
}

UINT DpiForMonitor(HMONITOR mon) {
    UINT x = 96, y = 96;
    if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y)) && x >= 48) return x;
    return 96;
}

UINT PrimaryDpi() { return DpiForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)); }

RECT WindowVisibleRect(HWND hwnd) {
    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) || IsRectEmpty(&r))
        GetWindowRect(hwnd, &r);
    return r;
}

// --------------------------------------------------------------------------
// Windows 11 notification-area promotion
// --------------------------------------------------------------------------
// Explorer stores one subkey per tray icon under
// HKCU\Control Panel\NotifyIconSettings. ExecutablePath may start with a
// known-folder GUID ("{F1B32785-...}\Programs\WhichPC\WhichPC.exe").
static std::wstring ResolveKnownFolderPath(const std::wstring& path) {
    if (path.size() < 39 || path[0] != L'{') return path;
    size_t close = path.find(L'}');
    if (close == std::wstring::npos) return path;
    GUID id;
    if (FAILED(CLSIDFromString(path.substr(0, close + 1).c_str(), &id))) return path;
    PWSTR folder = nullptr;
    std::wstring out = path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &folder)) && folder) {
        out = std::wstring(folder) + path.substr(close + 1);
    }
    CoTaskMemFree(folder);
    return out;
}

static bool SameFile(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

static std::wstring FileNameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

bool PromoteTrayIcon(bool promote) {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return false;
    const std::wstring self = ExePath();
    std::vector<std::wstring> exact, byName;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD n = ARRAYSIZE(sub);
        if (RegEnumKeyExW(root, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t value[MAX_PATH * 2] = {};
        DWORD bytes = sizeof(value) - sizeof(wchar_t);
        if (RegGetValueW(root, sub, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
            continue;
        std::wstring path = ResolveKnownFolderPath(value);
        if (SameFile(path, self))
            exact.push_back(sub);
        else if (SameFile(FileNameOf(path), FileNameOf(self)))
            byName.push_back(sub);
    }
    const std::vector<std::wstring>& targets = exact.empty() ? byName : exact;
    for (const auto& name : targets) {
        HKEY k;
        if (RegOpenKeyExW(root, name.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) continue;
        DWORD cur = 0, bytes = sizeof(cur);
        bool has = RegQueryValueExW(k, L"IsPromoted", nullptr, nullptr, reinterpret_cast<BYTE*>(&cur), &bytes) ==
                   ERROR_SUCCESS;
        DWORD want = promote ? 1 : 0;
        if (!has || cur != want)
            RegSetValueExW(k, L"IsPromoted", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&want), sizeof(want));
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return !targets.empty();
}
