// BatteryDeck: a lightweight Win32 tray app showing battery levels of mice, keyboards,
// headsets and controllers from several vendors. Each vendor/transport is a Provider.
// Started as a native port of LGSTrayBattery (https://github.com/andyvorld/LGSTrayBattery).

#include "common.h"
#include "hid_win.h"
#include "provider.h"
#include "providers/bluetooth.h"
#include "providers/gamesir.h"
#include "providers/logitech.h"
#include "icons.h"
#include "resource.h"
#include "settings.h"

#include <dbt.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>

namespace {

constexpr wchar_t kAppName[] = L"BatteryDeck";
constexpr wchar_t kVersion[] = L"BatteryDeck v1.0.0";
constexpr wchar_t kWindowClass[] = L"BatteryDeckWindow";
// Device icons start at ID 0 to match the .NET app, so Windows keeps showing
// an icon the user had already pinned to the taskbar.
constexpr UINT kMainIconId = 1000;
constexpr UINT_PTR kTimerRescan = 1;
constexpr UINT_PTR kTimerResume = 2;
constexpr UINT_PTR kTimerRetryIcons = 3;

enum : UINT { ID_RESCAN = 1, ID_NUMERIC, ID_AUTOSTART, ID_EXIT, ID_DEVICE_BASE = 1000 };

HWND g_hwnd = nullptr;
UINT g_taskbarCreated = 0;
bool g_debug = false;
std::wstring g_exeDir;
std::mutex g_logMutex;

std::mutex g_devicesMutex;
std::map<std::wstring, DeviceState> g_devices;  // by device id, written by device threads

std::vector<std::unique_ptr<Provider>> g_providers;

void RescanAll() {
    for (auto& p : g_providers) p->Rescan();
}
void StopAll() {
    for (auto& p : g_providers) p->Stop();
}
void PollSoonAll() {
    for (auto& p : g_providers) p->PollSoon();
}
bool g_lightTheme = true;
int g_iconSize = 16;

struct TrayIcon {
    UINT uid = 0;
    bool hasUid = false;
    HICON icon = nullptr;
    std::wstring drawKey;  // what the current icon shows; skip redraws when unchanged
    std::wstring tip;
    bool added = false;
};
std::map<std::wstring, TrayIcon> g_icons;  // one per selected device
TrayIcon g_mainIcon;                        // shown while no device is selected
std::map<std::wstring, std::wstring> g_nameCache;

// --- tray icon helpers -------------------------------------------------------

bool ApplyIcon(TrayIcon& t, HICON newIcon, const std::wstring& tip) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = t.uid;
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | NIF_ICON;
    nid.uCallbackMessage = WM_APP_TRAY;

    HICON old = nullptr;
    if (newIcon) {
        old = t.icon;
        t.icon = newIcon;
    }
    nid.hIcon = t.icon;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    t.tip = tip;

    bool ok;
    if (!t.added) {
        ok = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
        if (ok) {
            t.added = true;
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
        }
    } else {
        ok = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
    }
    if (old) DestroyIcon(old);  // the shell keeps its own copy
    return ok;
}

void RemoveIcon(TrayIcon& t) {
    if (t.added) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_hwnd;
        nid.uID = t.uid;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (t.icon) DestroyIcon(t.icon);
    t.icon = nullptr;
    t.added = false;
    t.drawKey.clear();
    t.tip.clear();
}

std::wstring Tooltip(const DeviceState& d) {
    wchar_t buf[128];
    if (d.percent < 0) {
        _snwprintf_s(buf, _TRUNCATE, L"%s: unknown", d.name.c_str());
    } else {
        const wchar_t* status = d.status == PowerStatus::Charging ? L", charging"
                                : d.status == PowerStatus::Full   ? L", full"
                                                                  : L"";
        _snwprintf_s(buf, _TRUNCATE, L"%s: %.0f%%%s%s", d.name.c_str(), d.percent, status,
                     d.connected ? L"" : L" (disconnected)");
    }
    return buf;
}

std::wstring DrawKey(const DeviceState* d) {
    wchar_t buf[96];
    swprintf_s(buf, L"%d|%d|%d|%.0f|%d|%d", GetSettings().numericDisplay ? 1 : 0, g_lightTheme ? 1 : 0, g_iconSize,
               d ? d->percent : -1.0, d ? (int)d->status : -1, d ? (int)d->kind : -1);
    return buf;
}

std::wstring CachedName(const std::wstring& id) {
    auto it = g_nameCache.find(id);
    if (it == g_nameCache.end()) it = g_nameCache.emplace(id, GetCachedDeviceName(id)).first;
    return it->second;
}

std::map<std::wstring, DeviceState> SnapshotDevices() {
    std::lock_guard lk(g_devicesMutex);
    return g_devices;
}

void RefreshIcons() {
    auto devices = SnapshotDevices();
    const auto& selected = GetSettings().selectedDevices;
    bool failed = false;

    // Remember names so icons of switched-off devices still show one.
    for (const auto& [id, d] : devices) {
        if (!d.name.empty() && CachedName(id) != d.name) {
            SetCachedDeviceName(id, d.name);
            g_nameCache[id] = d.name;
        }
    }

    // Only selected devices that are connected get an icon; switched-off devices
    // disappear and come back when they reconnect.
    auto isVisible = [&](const std::wstring& id) {
        auto f = devices.find(id);
        return f != devices.end() && f->second.connected &&
               std::find(selected.begin(), selected.end(), id) != selected.end();
    };

    // Icon id = position in the saved selection: stable across restarts and reconnects,
    // so Windows keeps each icon where the user pinned it.
    auto positionOf = [&](const std::wstring& id) {
        return (UINT)(std::find(selected.begin(), selected.end(), id) - selected.begin());
    };

    // Drop icons that are no longer shown, and icons whose position changed (e.g. after
    // unticking and re-ticking a device). All removals happen before any icon is added,
    // so two icons can never share an id (Windows would keep only one of them).
    for (auto it = g_icons.begin(); it != g_icons.end();) {
        if (!isVisible(it->first)) {
            RemoveIcon(it->second);
            it = g_icons.erase(it);
            continue;
        }
        if (it->second.hasUid && it->second.uid != positionOf(it->first)) {
            RemoveIcon(it->second);
            it->second.hasUid = false;
        }
        ++it;
    }

    for (size_t i = 0; i < selected.size(); ++i) {
        const std::wstring& id = selected[i];
        if (!isVisible(id)) continue;
        const DeviceState& d = devices.at(id);

        TrayIcon& t = g_icons[id];
        if (!t.hasUid) {
            t.uid = (UINT)i;
            t.hasUid = true;
        }

        HICON icon = nullptr;
        std::wstring key = DrawKey(&d);
        if (key != t.drawKey || !t.icon) {
            icon = DrawTrayIcon(&d, GetSettings().numericDisplay, g_lightTheme, g_iconSize);
            t.drawKey = key;
        }
        std::wstring tip = Tooltip(d);
        if (icon || tip != t.tip || !t.added) failed |= !ApplyIcon(t, icon, tip);
    }

    if (g_icons.empty()) {
        g_mainIcon.uid = kMainIconId;
        HICON icon = nullptr;
        std::wstring key = DrawKey(nullptr);
        if (key != g_mainIcon.drawKey || !g_mainIcon.icon) {
            icon = DrawTrayIcon(nullptr, GetSettings().numericDisplay, g_lightTheme, g_iconSize);
            g_mainIcon.drawKey = key;
        }
        std::wstring tip = std::wstring(kAppName) + (selected.empty() ? L": right-click to choose devices"
                                                                      : L": no selected device connected");
        if (icon || !g_mainIcon.added) failed |= !ApplyIcon(g_mainIcon, icon, tip);
    } else {
        RemoveIcon(g_mainIcon);
    }

    // The shell may not be ready yet right after logon; try again shortly.
    if (failed) SetTimer(g_hwnd, kTimerRetryIcons, 5000, nullptr);
}

void RedrawAllIcons() {
    for (auto& [id, t] : g_icons) t.drawKey.clear();
    g_mainIcon.drawKey.clear();
    RefreshIcons();
}

// --- context menu ------------------------------------------------------------

void ShowMenu(POINT pt) {
    auto devices = SnapshotDevices();
    auto& settings = MutableSettings();

    struct Entry {
        std::wstring id, label;
    };
    std::vector<Entry> entries;
    for (const auto& [id, d] : devices) {
        if (!d.connected) continue;  // only devices that are on right now
        std::wstring label = d.name;
        if (d.percent >= 0) {
            wchar_t pct[32];
            swprintf_s(pct, L"  (%.0f%%)", d.percent);
            label += pct;
        }
        entries.push_back({id, label});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.label < b.label; });

    auto isSelected = [&](const std::wstring& id) {
        return std::find(settings.selectedDevices.begin(), settings.selectedDevices.end(), id) !=
               settings.selectedDevices.end();
    };

    HMENU deviceMenu = CreatePopupMenu();
    for (size_t i = 0; i < entries.size(); ++i)
        AppendMenuW(deviceMenu, MF_STRING | (isSelected(entries[i].id) ? MF_CHECKED : 0), ID_DEVICE_BASE + i,
                    entries[i].label.c_str());
    if (entries.empty()) AppendMenuW(deviceMenu, MF_STRING | MF_GRAYED, 0, L"No devices found");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(deviceMenu), L"Devices");
    AppendMenuW(menu, MF_STRING, ID_RESCAN, L"Rediscover Devices");
    AppendMenuW(menu, MF_STRING | (settings.numericDisplay ? MF_CHECKED : 0), ID_NUMERIC, L"Display Numeric Icon");
    AppendMenuW(menu, MF_STRING | (GetAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Autostart with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, kVersion);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(g_hwnd);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align,
                                      pt.x, pt.y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu

    if (cmd >= ID_DEVICE_BASE && cmd < ID_DEVICE_BASE + entries.size()) {
        const std::wstring& id = entries[cmd - ID_DEVICE_BASE].id;
        auto& sel = settings.selectedDevices;
        auto it = std::find(sel.begin(), sel.end(), id);
        if (it != sel.end())
            sel.erase(it);
        else
            sel.push_back(id);
        SaveUserSettings();
        RefreshIcons();
        return;
    }

    switch (cmd) {
        case ID_RESCAN:
            StopAll();
            RescanAll();
            break;
        case ID_NUMERIC:
            settings.numericDisplay = !settings.numericDisplay;
            SaveUserSettings();
            RedrawAllIcons();
            break;
        case ID_AUTOSTART:
            SetAutostart(!GetAutostart());
            break;
        case ID_EXIT:
            DestroyWindow(g_hwnd);
            break;
    }
}

// --- window ------------------------------------------------------------------

// Any HID device coming or going: USB receivers/dongles, and Bluetooth devices
// (their HID interface appears when they connect and disappears when they drop).
bool IsHidInterfaceEvent(LPARAM lParam) {
    auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
    return hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        // Explorer restarted (or the taskbar DPI changed): add every icon again.
        for (auto& [id, t] : g_icons) t.added = false;
        g_mainIcon.added = false;
        g_iconSize = TrayIconSize();
        RedrawAllIcons();
        return 0;
    }

    switch (msg) {
        case WM_APP_DEVICES_CHANGED:
            RefreshIcons();
            return 0;

        case WM_APP_RESCAN:
            SetTimer(hwnd, kTimerRescan, 1500, nullptr);
            return 0;

        case WM_DEVICECHANGE:
            if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && IsHidInterfaceEvent(lParam))
                SetTimer(hwnd, kTimerRescan, 1500, nullptr);  // debounce: one device adds several interfaces
            return TRUE;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC) SetTimer(hwnd, kTimerResume, 5000, nullptr);
            return TRUE;

        case WM_TIMER:
            KillTimer(hwnd, wParam);
            if (wParam == kTimerRescan) {
                RescanAll();
            } else if (wParam == kTimerResume) {
                RescanAll();
                PollSoonAll();
            } else if (wParam == kTimerRetryIcons) {
                RedrawAllIcons();
            }
            return 0;

        case WM_SETTINGCHANGE:
            if (lParam && wcscmp(reinterpret_cast<LPCWSTR>(lParam), L"ImmersiveColorSet") == 0) {
                bool light = IsLightTaskbar();
                if (light != g_lightTheme) {
                    g_lightTheme = light;
                    RedrawAllIcons();
                }
            }
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED: {
            int size = TrayIconSize();
            if (size != g_iconSize) {
                g_iconSize = size;
                RedrawAllIcons();
            }
            return 0;
        }

        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case WM_CONTEXTMENU:  // right click
                    ShowMenu(POINT{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
                    break;
                case NIN_SELECT:      // left click: switch battery picture <-> number
                case NIN_KEYSELECT:
                    MutableSettings().numericDisplay = !GetSettings().numericDisplay;
                    SaveUserSettings();
                    RedrawAllIcons();
                    break;
            }
            return 0;

        case WM_DESTROY:
            for (auto& [id, t] : g_icons) RemoveIcon(t);
            RemoveIcon(g_mainIcon);
            StopAll();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void EnableEfficiencyMode() {
    // Lower priority + EcoQoS: the app only does a few milliseconds of work every 10 minutes.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    state.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#endif
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

}  // namespace

// --- shared helpers (declared in common.h) ----------------------------------

void PublishDevice(const DeviceState& state) {
    {
        std::lock_guard lk(g_devicesMutex);
        g_devices[state.id] = state;
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DEVICES_CHANGED, 0, 0);
}

void RequestRescan() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_RESCAN, 0, 0);
}

void Log(const wchar_t* fmt, ...) {
    if (!g_debug) return;
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lk(g_logMutex);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_exeDir + L"\\BatteryDeck.log").c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%02d:%02d:%02d.%03d [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                 GetCurrentThreadId(), msg);
        fclose(f);
    }
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir.resize(g_exeDir.find_last_of(L'\\'));
    g_debug = cmdLine && wcsstr(cmdLine, L"--debug") != nullptr;

    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\BatteryDeck");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    Log(L"%s starting", kVersion);
    EnableEfficiencyMode();
    LoadSettings(g_exeDir);
    IconsInit(instance);
    g_lightTheme = IsLightTaskbar();
    g_iconSize = TrayIconSize();

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);

    // Hidden top-level window (message-only windows don't get theme/device broadcasts).
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             instance, nullptr);
    if (!g_hwnd) return 1;
    ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = HidInterfaceGuid();
    HDEVNOTIFY deviceNotify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Providers, in the order their devices appear in the menu when names tie.
    const Settings& cfg = GetSettings();
    if (cfg.logitech.enabled) g_providers.push_back(std::make_unique<LogitechProvider>());
    if (cfg.gamesir.enabled) g_providers.push_back(std::make_unique<GameSirProvider>());
    if (cfg.bluetooth.enabled) g_providers.push_back(std::make_unique<BluetoothProvider>());
    for (auto& p : g_providers) Log(L"Provider enabled: %s", p->Name());

    RefreshIcons();
    RescanAll();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (deviceNotify) UnregisterDeviceNotification(deviceNotify);
    g_hwnd = nullptr;
    g_providers.clear();
    IconsShutdown();
    if (singleInstance) {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
    }
    Log(L"%s exiting", kAppName);
    return 0;
}
