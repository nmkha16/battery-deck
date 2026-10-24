#pragma once
#include "../hid_win.h"
#include "../provider.h"

#include <map>
#include <mutex>

// GameSir controllers that speak the "C2" protocol (Cyclone 2), wired or on their dongle.
//
// Battery read: send the heartbeat output report (0F F2 00) on the vendor collection
// (usage page 0xFFF0). The controller then streams input report 0x12 for ~2 seconds;
// byte 35 = charge state, byte 36 = battery %. We read one report and close the handle,
// so nothing runs between polls.
class GameSirProvider : public Provider {
public:
    GameSirProvider();
    ~GameSirProvider() override;

    const wchar_t* Name() const override { return L"GameSir"; }
    void Rescan() override;
    void Stop() override;
    void PollSoon() override;

private:
    struct Target {
        std::wstring path;
        std::wstring id;
        std::wstring name;
        USHORT outputLen = 64;
        USHORT inputLen = 64;
    };
    struct Reading {
        double percent;
        PowerStatus status;
    };

    DWORD PollAll();  // worker thread
    bool ReadBattery(const Target& t, Reading& out);

    HANDLE stopEvent_ = nullptr;
    PeriodicWorker worker_;

    std::mutex mu_;
    std::vector<Target> targets_;  // set by Rescan (UI thread), read by the worker

    std::map<std::wstring, DeviceState> known_;  // worker thread only
};
