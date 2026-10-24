#pragma once
#include "../provider.h"

#include <map>

// Bluetooth devices whose battery level Windows already tracks (the value shown in
// Settings > Bluetooth & devices): Xbox controllers over BT, earbuds, headsets, mice, ...
//
// Reads a device property and Windows' "connected" state for each device container, so
// it never talks to the devices itself and costs next to nothing.
class BluetoothProvider : public Provider {
public:
    ~BluetoothProvider() override { Stop(); }

    const wchar_t* Name() const override { return L"Bluetooth"; }
    void Rescan() override;
    void Stop() override { worker_.Stop(); }
    void PollSoon() override { worker_.Wake(3000); }

private:
    DWORD Poll();  // worker thread

    PeriodicWorker worker_;
    std::map<std::wstring, DeviceState> published_;  // worker thread only
};
