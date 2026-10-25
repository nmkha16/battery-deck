#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cwctype>

#include <sstream>

namespace {

Settings g_settings;

const wchar_t* kUserKey = L"Software\\BatteryDeck";
const wchar_t* kNamesKey = L"Software\\BatteryDeck\\DeviceNames";
const wchar_t* kOldUserKey = L"Software\\LGSTray";  // native LGSTray builds
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"BatteryDeck";
const wchar_t* kOldRunValue = L"LGSTrayGUI";  // LGSTray (.NET and native)
// Explorer's per-app on/off switch for Run entries (what Task Manager's Startup tab edits).
// Without an "enabled" record here, Windows may skip the Run entry at sign-in.
const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::string ReadFileBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        s.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(f, s.data(), (DWORD)s.size(), &read, nullptr)) read = 0;
        s.resize(read);
    }
    CloseHandle(f);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

// Drops a '#' comment that is not inside a string.
std::wstring StripComment(const std::wstring& line) {
    bool inString = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == L'"') inString = !inString;
        if (line[i] == L'#' && !inString) return line.substr(0, i);
    }
    return line;
}

std::vector<std::wstring> ParseStringArray(const std::wstring& v) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while ((pos = v.find(L'"', pos)) != std::wstring::npos) {
        size_t end = v.find(L'"', pos + 1);
        if (end == std::wstring::npos) break;
        out.push_back(v.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    return out;
}

// Minimal reader for the keys this app uses from appsettings.toml.
void LoadToml(const std::wstring& path) {
    std::wstring text = Utf8ToWide(ReadFileBytes(path));
    if (text.empty()) {
        Log(L"appsettings.toml not found, using defaults");
        return;
    }

    std::wistringstream in(text);
    std::wstring raw, section, pendingKey, pendingValue;
    bool inArray = false;

    auto toBool = [](const std::wstring& v) { return Lower(v) == L"true"; };
    auto toInt = [](const std::wstring& v, int min) { return std::max(min, _wtoi(v.c_str())); };

    auto apply = [&](const std::wstring& key, const std::wstring& value) {
        if (section == L"native" || section == L"logitech") {
            auto& s = g_settings.logitech;
            if (key == L"enabled") s.enabled = toBool(value);
            else if (key == L"retrytime") s.retryTime = toInt(value, 1);
            else if (key == L"pollperiod") s.pollPeriod = toInt(value, 10);
            else if (key == L"disableddevices") s.disabledDevices = ParseStringArray(value);
        } else if (section == L"gamesir") {
            auto& s = g_settings.gamesir;
            if (key == L"enabled") s.enabled = toBool(value);
            else if (key == L"retrytime") s.retryTime = toInt(value, 1);
            else if (key == L"pollperiod") s.pollPeriod = toInt(value, 10);
        } else if (section == L"bluetooth") {
            auto& s = g_settings.bluetooth;
            if (key == L"enabled") s.enabled = toBool(value);
            else if (key == L"pollperiod") s.pollPeriod = toInt(value, 10);
        }
    };

    while (std::getline(in, raw)) {
        std::wstring line = Trim(StripComment(raw));
        if (inArray) {
            pendingValue += L" " + line;
            if (line.find(L']') != std::wstring::npos) {
                inArray = false;
                apply(pendingKey, pendingValue);
            }
            continue;
        }
        if (line.empty()) continue;
        if (line.front() == L'[') {
            section = Lower(Trim(line.substr(1, line.find(L']') - 1)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq)));
        std::wstring value = Trim(line.substr(eq + 1));
        if (!value.empty() && value.front() == L'[' && value.find(L']') == std::wstring::npos) {
            inArray = true;
            pendingKey = key;
            pendingValue = value;
            continue;
        }
        apply(key, value);
    }
}

// One-time import of the .NET app's user.config (selected devices + numeric display).
bool ImportFromDotNet() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) return false;
    std::wstring root = std::wstring(localAppData) + L"\\LGSTray";
    CoTaskMemFree(localAppData);

    std::wstring best;
    FILETIME bestTime{};
    WIN32_FIND_DATAW fd;
    HANDLE f1 = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (f1 == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring appDir = root + L"\\" + fd.cFileName;
        WIN32_FIND_DATAW fd2;
        HANDLE f2 = FindFirstFileW((appDir + L"\\*").c_str(), &fd2);
        if (f2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd2.cFileName[0] == L'.') continue;
            std::wstring cfg = appDir + L"\\" + fd2.cFileName + L"\\user.config";
            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (GetFileAttributesExW(cfg.c_str(), GetFileExInfoStandard, &attr) &&
                CompareFileTime(&attr.ftLastWriteTime, &bestTime) > 0) {
                best = cfg;
                bestTime = attr.ftLastWriteTime;
            }
        } while (FindNextFileW(f2, &fd2));
        FindClose(f2);
    } while (FindNextFileW(f1, &fd));
    FindClose(f1);
    if (best.empty()) return false;

    std::wstring xml = Utf8ToWide(ReadFileBytes(best));

    size_t num = xml.find(L"name=\"NumericDisplay\"");
    if (num != std::wstring::npos) {
        size_t v = xml.find(L"<value>", num);
        if (v != std::wstring::npos) g_settings.numericDisplay = Lower(xml.substr(v + 7, 4)) == L"true";
    }

    size_t sel = xml.find(L"name=\"SelectedDevices\"");
    if (sel != std::wstring::npos) {
        size_t end = xml.find(L"</setting>", sel);
        size_t pos = sel;
        while ((pos = xml.find(L"<string>", pos)) != std::wstring::npos && pos < end) {
            size_t close = xml.find(L"</string>", pos);
            if (close == std::wstring::npos) break;
            std::wstring id = Trim(xml.substr(pos + 8, close - pos - 8));
            if (!id.empty()) g_settings.selectedDevices.push_back(id);
            pos = close;
        }
    }

    Log(L"Imported %zu selected device(s) from %s", g_settings.selectedDevices.size(), best.c_str());
    return true;
}

void LoadUserSettings();
void MigrateDeviceIds();

}  // namespace

const Settings& GetSettings() { return g_settings; }
Settings& MutableSettings() { return g_settings; }

void LoadSettings(const std::wstring& exeDir) {
    LoadToml(exeDir + L"\\appsettings.toml");

    LoadUserSettings();
    MigrateDeviceIds();

    // Autostart used to be registered as "LGSTrayGUI"; move it to the new name and exe.
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kOldRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
        ERROR_SUCCESS) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kOldRunValue);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kOldRunValue);
        SetAutostart(true);
        Log(L"Moved autostart entry to BatteryDeck");
    }
}

namespace {

void LoadUserSettings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUserKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        // First run as BatteryDeck: take over the settings of the native LGSTray build.
        HKEY oldKey, newKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kOldUserKey, 0, KEY_READ, &oldKey) == ERROR_SUCCESS) {
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &newKey,
                                nullptr) == ERROR_SUCCESS) {
                RegCopyTreeW(oldKey, nullptr, newKey);
                RegCloseKey(newKey);
                Log(L"Copied settings from HKCU\\%s", kOldUserKey);
            }
            RegCloseKey(oldKey);
        }
    }
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUserKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        if (ImportFromDotNet()) SaveUserSettings();
        return;
    }

    DWORD size = 0, type = 0;
    if (RegQueryValueExW(key, L"SelectedDevices", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && size > 0) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
        if (RegQueryValueExW(key, L"SelectedDevices", nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &size) ==
            ERROR_SUCCESS) {
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_settings.selectedDevices.emplace_back(p);
        }
    }

    DWORD numeric = 0;
    size = sizeof(numeric);
    if (RegQueryValueExW(key, L"NumericDisplay", nullptr, nullptr, reinterpret_cast<BYTE*>(&numeric), &size) ==
        ERROR_SUCCESS)
        g_settings.numericDisplay = numeric != 0;

    RegCloseKey(key);
}

// Ids used to be bare Logitech ids; they now carry a provider prefix ("logi:...").
void MigrateDeviceIds() {
    bool changed = false;
    for (auto& id : g_settings.selectedDevices) {
        if (id.find(L':') != std::wstring::npos) continue;
        std::wstring newId = L"logi:" + id;
        std::wstring name = GetCachedDeviceName(id);
        if (!name.empty()) SetCachedDeviceName(newId, name);
        id = newId;
        changed = true;
    }
    if (changed) {
        Log(L"Migrated saved device ids to provider-prefixed ids");
        SaveUserSettings();
    }
}

}  // namespace

void SaveUserSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;

    std::wstring multi;
    for (const auto& id : g_settings.selectedDevices) {
        multi += id;
        multi += L'\0';
    }
    multi += L'\0';
    RegSetValueExW(key, L"SelectedDevices", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()),
                   (DWORD)(multi.size() * sizeof(wchar_t)));

    DWORD numeric = g_settings.numericDisplay ? 1 : 0;
    RegSetValueExW(key, L"NumericDisplay", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&numeric), sizeof(numeric));
    RegCloseKey(key);
}

std::wstring GetCachedDeviceName(const std::wstring& id) {
    wchar_t buf[256];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kNamesKey, id.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}

void SetCachedDeviceName(const std::wstring& id, const std::wstring& name) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kNamesKey, id.c_str(), REG_SZ, name.c_str(),
                    (DWORD)((name.size() + 1) * sizeof(wchar_t)));
}

bool GetAutostart() {
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) !=
        ERROR_SUCCESS)
        return false;
    // Disabled in Task Manager: the first byte of the record is odd (01/03); enabled is 02/06.
    BYTE approved[12]{};
    DWORD size = sizeof(approved);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, approved, &size) ==
            ERROR_SUCCESS &&
        size > 0 && (approved[0] & 1))
        return false;
    return true;
}

void SetAutostart(bool enabled) {
    if (enabled) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(exe) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, value.c_str(),
                        (DWORD)((value.size() + 1) * sizeof(wchar_t)));
        const BYTE approved[12] = {2};  // enabled, no "disabled at" time
        RegSetKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, REG_BINARY, approved, sizeof(approved));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue);
    }
}
