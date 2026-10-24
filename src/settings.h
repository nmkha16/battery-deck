#pragma once
#include "common.h"

struct Settings {
    // From appsettings.toml (read once at startup). [Native] is the old name for [Logitech].
    struct {
        bool enabled = true;
        int retryTime = 10;     // seconds until a failed poll is retried
        int pollPeriod = 600;   // seconds between successful polls
        std::vector<std::wstring> disabledDevices;
    } logitech;

    struct {
        bool enabled = true;
        int retryTime = 30;
        int pollPeriod = 600;
    } gamesir;

    struct {
        bool enabled = true;
        int pollPeriod = 120;   // only reads a value Windows already keeps, so this is cheap
    } bluetooth;

    // Per-user choices (HKCU\Software\LGSTray). Changed on the UI thread only.
    std::vector<std::wstring> selectedDevices;
    bool numericDisplay = false;
};

const Settings& GetSettings();
Settings& MutableSettings();

void LoadSettings(const std::wstring& exeDir);
void SaveUserSettings();

// Remembers device names so icons of devices that are off still have a name.
std::wstring GetCachedDeviceName(const std::wstring& id);
void SetCachedDeviceName(const std::wstring& id, const std::wstring& name);

bool GetAutostart();
void SetAutostart(bool enabled);
