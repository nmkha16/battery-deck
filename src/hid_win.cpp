#include "hid_win.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <initguid.h>
#include <devpkey.h>

#include <algorithm>
#include <cwctype>

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

// Fallback grouping when Windows reports no container ID: strip "&col0N" from the path.
std::wstring PathWithoutCollection(const std::wstring& lowerPath) {
    std::wstring s = lowerPath;
    size_t pos = s.find(L"&col");
    if (pos != std::wstring::npos) {
        size_t end = s.find(L'#', pos);
        s.erase(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
    }
    return s;
}

}  // namespace

GUID HidInterfaceGuid() {
    GUID g;
    HidD_GetHidGuid(&g);
    return g;
}

std::vector<HidInterfaceInfo> EnumerateHidInterfaces(USHORT vendorId) {
    std::vector<HidInterfaceInfo> out;

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    wchar_t vidTag[16];
    swprintf_s(vidTag, L"vid_%04x", vendorId);

    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, &dev)) continue;

        std::wstring path = detail->DevicePath;
        std::wstring lower = Lower(path);
        if (lower.find(vidTag) == std::wstring::npos) continue;

        // Query caps without read/write access (works even for collections owned by the system).
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        HIDP_CAPS caps{};
        bool ok = HidD_GetAttributes(h, &attr) != FALSE;
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (ok && HidD_GetPreparsedData(h, &pp)) {
            ok = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
            HidD_FreePreparsedData(pp);
        } else {
            ok = false;
        }
        CloseHandle(h);
        if (!ok || attr.VendorID != vendorId) continue;

        HidInterfaceInfo info;
        info.path = path;
        info.vendorId = attr.VendorID;
        info.productId = attr.ProductID;
        info.usagePage = caps.UsagePage;
        info.usage = caps.Usage;
        info.inputLen = caps.InputReportByteLength;
        info.outputLen = caps.OutputReportByteLength;

        GUID container{};
        DEVPROPTYPE type = 0;
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_ContainerId, &type,
                                      reinterpret_cast<PBYTE>(&container), sizeof(container), nullptr, 0) &&
            container != GUID{}) {
            wchar_t s[64];
            StringFromGUID2(container, s, 64);
            info.groupKey = s;
        } else {
            info.groupKey = PathWithoutCollection(lower);
        }

        out.push_back(std::move(info));
    }

    SetupDiDestroyDeviceInfoList(set);
    return out;
}

HANDLE OpenHidReadWrite(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

bool HidWrite(HANDLE h, const void* data, DWORD len, DWORD timeoutMs, HANDLE stopEvent) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    bool ok = false;
    DWORD n = 0;
    if (WriteFile(h, data, len, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[2] = {ov.hEvent, stopEvent};
        DWORD w = WaitForMultipleObjects(stopEvent ? 2 : 1, waits, FALSE, timeoutMs);
        if (w == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(h, &ov, &n, FALSE) && n > 0;
        } else {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &n, TRUE);
        }
    }
    CloseHandle(ov.hEvent);
    return ok;
}
