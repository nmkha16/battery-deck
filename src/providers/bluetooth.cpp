#include "bluetooth.h"
#include "../settings.h"

#include <cfgmgr32.h>
#include <devpropdef.h>

#include <algorithm>
#include <cwctype>
#include <set>

namespace {

// Battery % Windows keeps for Bluetooth devices (BYTE). Not in the public SDK headers.
const DEVPROPKEY kBatteryLevel = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
const DEVPROPKEY kFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
const DEVPROPKEY kContainerId = {{0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};
// Device container properties (System.Devices.Connected / System.Devices.Category).
const DEVPROPKEY kContainerConnected = {{0x78C34FC8, 0x104A, 0x4ACA, {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}}, 55};
const DEVPROPKEY kContainerCategory = {{0x78C34FC8, 0x104A, 0x4ACA, {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}}, 90};

// DevQuery (cfgmgr32.dll, Windows 10 1809+). Declared here because the MinGW headers lack it.
struct DevPropCompKey_ {
    DEVPROPKEY Key;
    int Store;  // DEVPROP_STORE_SYSTEM = 0
    PCWSTR LocaleName;
};
struct DevProperty_ {
    DevPropCompKey_ CompKey;
    DEVPROPTYPE Type;
    ULONG BufferSize;
    PVOID Buffer;
};
struct DevObject_ {
    int ObjectType;
    PCWSTR pszObjectId;
    ULONG cPropertyCount;
    const DevProperty_* pProperties;
};
constexpr int kDevObjectTypeDeviceContainer = 2;
using DevGetObjectsFn = HRESULT(WINAPI*)(int, ULONG, ULONG, const DevPropCompKey_*, ULONG, const void*, PULONG,
                                         const DevObject_**);
using DevFreeObjectsFn = void(WINAPI*)(ULONG, const DevObject_*);

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring GuidString(const GUID& g) {
    wchar_t s[64];
    StringFromGUID2(g, s, 64);
    return Lower(s);  // "{...}"
}

struct ContainerInfo {
    bool connected = false;
    std::wstring category;  // e.g. "Input.Gaming.Gamepad;Input.Keyboard"
};

// Connected state + category for every device container, keyed by "{guid}" (lower case).
std::map<std::wstring, ContainerInfo> QueryContainers() {
    std::map<std::wstring, ContainerInfo> out;
    static HMODULE mod = LoadLibraryW(L"cfgmgr32.dll");
    static auto get = mod ? (DevGetObjectsFn)GetProcAddress(mod, "DevGetObjects") : nullptr;
    static auto release = mod ? (DevFreeObjectsFn)GetProcAddress(mod, "DevFreeObjects") : nullptr;
    if (!get || !release) return out;

    DevPropCompKey_ keys[2] = {{kContainerConnected, 0, nullptr}, {kContainerCategory, 0, nullptr}};
    ULONG count = 0;
    const DevObject_* objects = nullptr;
    if (FAILED(get(kDevObjectTypeDeviceContainer, 0, 2, keys, 0, nullptr, &count, &objects))) return out;

    for (ULONG i = 0; i < count; ++i) {
        ContainerInfo info;
        for (ULONG p = 0; p < objects[i].cPropertyCount; ++p) {
            const DevProperty_& pr = objects[i].pProperties[p];
            if (!pr.Buffer) continue;
            if (pr.CompKey.Key.pid == 55 && pr.Type == DEVPROP_TYPE_BOOLEAN)
                info.connected = *static_cast<const DEVPROP_BOOLEAN*>(pr.Buffer) != DEVPROP_FALSE;
            if (pr.CompKey.Key.pid == 90 && pr.Type == DEVPROP_TYPE_STRING_LIST)
                for (auto* s = static_cast<const wchar_t*>(pr.Buffer); *s; s += wcslen(s) + 1) {
                    info.category += s;
                    info.category += L';';
                }
        }
        out[Lower(objects[i].pszObjectId)] = info;
    }
    release(count, objects);
    return out;
}

std::vector<std::wstring> ListDeviceIds(const wchar_t* enumerator) {
    std::vector<std::wstring> ids;
    ULONG len = 0;
    ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    if (CM_Get_Device_ID_List_SizeW(&len, enumerator, flags) != CR_SUCCESS || len == 0) return ids;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(enumerator, buf.data(), len, flags) != CR_SUCCESS) return ids;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) ids.emplace_back(p);
    return ids;
}

template <typename T>
bool GetProp(DEVINST dn, const DEVPROPKEY& key, DEVPROPTYPE expected, T& out) {
    DEVPROPTYPE type = 0;
    ULONG size = sizeof(T);
    return CM_Get_DevNode_PropertyW(dn, &key, &type, reinterpret_cast<PBYTE>(&out), &size, 0) == CR_SUCCESS &&
           type == expected;
}

std::wstring GetStringProp(DEVINST dn, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CM_Get_DevNode_PropertyW(dn, &key, &type, nullptr, &size, 0);
    if (size == 0) return L"";
    std::vector<BYTE> buf(size + 2, 0);
    if (CM_Get_DevNode_PropertyW(dn, &key, &type, buf.data(), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING)
        return L"";
    return reinterpret_cast<const wchar_t*>(buf.data());
}

DeviceKind KindFrom(const std::wstring& category, const std::wstring& name) {
    std::wstring c = Lower(category + L";" + name);
    if (c.find(L"gamepad") != std::wstring::npos || c.find(L"gaming") != std::wstring::npos ||
        c.find(L"controller") != std::wstring::npos)
        return DeviceKind::Gamepad;
    if (c.find(L"keyboard") != std::wstring::npos) return DeviceKind::Keyboard;
    if (c.find(L"mouse") != std::wstring::npos) return DeviceKind::Mouse;
    if (c.find(L"headset") != std::wstring::npos || c.find(L"headphone") != std::wstring::npos ||
        c.find(L"audio") != std::wstring::npos || c.find(L"buds") != std::wstring::npos)
        return DeviceKind::Headset;
    return DeviceKind::Other;
}

}  // namespace

void BluetoothProvider::Rescan() {
    if (!worker_.Running())
        worker_.Start([this] { return Poll(); });
    else
        worker_.Wake(2000);  // give Windows a moment to update the values after a (dis)connect
}

DWORD BluetoothProvider::Poll() {
    struct Found {
        std::wstring name;
        int battery = -1;
    };
    std::map<std::wstring, Found> byContainer;

    // The device-level node ("...\DEV_<address>") carries the name; the battery value can
    // sit on it (BLE) or on one of its profile nodes (classic BT hands-free), so check all.
    for (const wchar_t* enumerator : {L"BTHLE", L"BTHENUM"}) {
        for (const auto& id : ListDeviceIds(enumerator)) {
            DEVINST dn = 0;
            if (CM_Locate_DevNodeW(&dn, const_cast<DEVINSTID_W>(id.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
                continue;
            GUID container{};
            if (!GetProp(dn, kContainerId, DEVPROP_TYPE_GUID, container) || container == GUID{}) continue;
            Found& f = byContainer[GuidString(container)];

            BYTE level = 0;
            if (GetProp(dn, kBatteryLevel, DEVPROP_TYPE_BYTE, level) && level <= 100) f.battery = level;

            std::wstring upper = id;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
            if (upper.rfind(std::wstring(enumerator) + L"\\DEV_", 0) == 0) {
                std::wstring name = GetStringProp(dn, kFriendlyName);
                if (!name.empty()) f.name = name;
            }
        }
    }

    auto containers = QueryContainers();

    for (const auto& [container, f] : byContainer) {
        if (f.battery < 0 || f.name.empty()) continue;  // Windows doesn't know its battery
        auto info = containers.find(container);
        bool connected = info != containers.end() && info->second.connected;

        DeviceState s;
        s.id = L"bt:" + container.substr(1, container.size() - 2);
        s.name = f.name;
        s.kind = KindFrom(info != containers.end() ? info->second.category : L"", f.name);
        s.hasBattery = true;
        s.connected = connected;
        s.percent = f.battery;
        s.status = PowerStatus::Unknown;  // Windows only exposes the level

        auto old = published_.find(s.id);
        if (old == published_.end() || old->second.percent != s.percent || old->second.connected != s.connected ||
            old->second.name != s.name || old->second.kind != s.kind) {
            Log(L"Bluetooth %s (%s): %d%% %s", s.name.c_str(), s.id.c_str(), f.battery,
                connected ? L"connected" : L"not connected");
            published_[s.id] = s;
            PublishDevice(s);
        }
    }

    return (DWORD)GetSettings().bluetooth.pollPeriod * 1000;
}
