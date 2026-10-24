#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class PowerStatus : uint8_t { Discharging, Charging, Full, NotCharging, Unknown };

enum class DeviceKind : uint8_t { Mouse, Keyboard, Headset, Gamepad, Other };

// Everything the tray UI needs to know about one device, whatever provider found it.
struct DeviceState {
    std::wstring id;         // "<provider>:<stable id>", e.g. "logi:1A2B3C4D5E6F7A8B9C0D1E"
    std::wstring name;
    DeviceKind kind = DeviceKind::Other;
    bool hasBattery = false;
    bool connected = false;
    double percent = -1;     // < 0 means unknown
    PowerStatus status = PowerStatus::Unknown;
    int millivolts = -1;
};

// Window messages posted from provider threads to the UI thread.
constexpr UINT WM_APP_DEVICES_CHANGED = WM_APP + 1;
constexpr UINT WM_APP_RESCAN = WM_APP + 2;
constexpr UINT WM_APP_TRAY = WM_APP + 3;

// Implemented in main.cpp.
void PublishDevice(const DeviceState& state);  // thread-safe
void RequestRescan();                           // thread-safe
void Log(const wchar_t* fmt, ...);              // only writes with --debug

std::wstring Utf8ToWide(const std::string& s);
