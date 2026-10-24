#pragma once
#include "common.h"

// One HID top-level collection.
struct HidInterfaceInfo {
    std::wstring path;
    std::wstring groupKey;   // same for all collections of one physical device
    USHORT vendorId = 0;
    USHORT productId = 0;
    USHORT usagePage = 0;
    USHORT usage = 0;
    USHORT inputLen = 0;
    USHORT outputLen = 0;
};

// Device interface class GUID for HID (for plug/unplug notifications).
GUID HidInterfaceGuid();

// Lists present HID collections of one vendor.
std::vector<HidInterfaceInfo> EnumerateHidInterfaces(USHORT vendorId);

// Overlapped HID helpers with a timeout. `stopEvent` (optional) aborts the wait.
HANDLE OpenHidReadWrite(const std::wstring& path);
bool HidWrite(HANDLE h, const void* data, DWORD len, DWORD timeoutMs, HANDLE stopEvent = nullptr);
