#pragma once
#include "../common.h"
#include "../hid_win.h"
#include "../provider.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

// One Logitech receiver (or directly connected device) with up to 6 paired devices.
//
// Threads:
//  - reader: blocks on overlapped reads of the short + long HID collections. It sleeps
//    in the kernel until a report arrives, so it costs nothing while idle.
//  - worker: runs every HID++ transaction (setup, device init, battery polls) one at a
//    time and sleeps until the next scheduled poll.
class Receiver {
public:
    Receiver(const HidInterfaceInfo& shortIf, const HidInterfaceInfo& longIf);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool Start();
    void Stop();
    void PollSoon();  // e.g. after the PC resumes from sleep

    bool IsDead() const { return dead_; }
    const std::wstring& ShortPath() const { return shortIf_.path; }

private:
    using Report = std::vector<uint8_t>;

    enum class BatteryFeature { None, F1000, F1001, F1004 };

    struct Device {
        uint8_t idx = 0;
        std::map<uint16_t, uint8_t> features;  // feature id -> feature index
        BatteryFeature battery = BatteryFeature::None;
        DeviceState state;
        bool initialized = false;
        bool haveReading = false;
        ULONGLONG nextPoll = 0;  // tick count; 0 = not polling
    };

    struct Reading {
        double percent;
        PowerStatus status;
        int millivolts;
    };

    enum class TaskKind { Setup, Init };
    struct Task {
        ULONGLONG due;
        TaskKind kind;
        uint8_t idx;
    };

    void ReaderLoop();
    void WorkerLoop();
    void OnReport(const uint8_t* data, DWORD len);
    void QueueInit(uint8_t idx, ULONGLONG due);

    void DoSetup();
    void DoInit(uint8_t idx);
    void PollBattery(Device& d);
    std::optional<Reading> ReadBattery(const Device& d);

    bool Write(const uint8_t* msg, size_t len);
    // match: 1 = accept, -1 = give up now, 0 = keep waiting
    Report Transact(const uint8_t* msg, size_t len, int timeoutMs, const std::function<int(const Report&)>& match);
    Report WriteRead10(std::initializer_list<uint8_t> msg, int timeoutMs);
    Report WriteRead20(std::initializer_list<uint8_t> msg, int timeoutMs = 100, bool ignoreHidpp10 = true);
    bool Ping20(uint8_t idx, int timeoutMs, bool ignoreHidpp10);

    bool IsStopping();
    void SleepInterruptible(DWORD ms);

    HidInterfaceInfo shortIf_;
    HidInterfaceInfo longIf_;
    HANDLE hShort_ = INVALID_HANDLE_VALUE;
    HANDLE hLong_ = INVALID_HANDLE_VALUE;
    HANDLE stopEvent_ = nullptr;

    std::thread reader_;
    std::thread worker_;
    bool started_ = false;
    std::atomic<bool> dead_{false};

    std::mutex mu_;                 // guards everything below
    std::condition_variable cv_;
    bool stopping_ = false;
    bool pollSoon_ = false;
    std::deque<Report> responses_;
    std::vector<Task> tasks_;
    std::set<uint8_t> announced_;

    std::map<uint8_t, Device> devices_;  // worker thread only (and Stop() after join)
    uint8_t pingPayload_ = 0x55;
};

// Logitech HID++ devices (Unifying / Lightspeed / Bolt receivers, wired and BT HID++).
class LogitechProvider : public Provider {
public:
    const wchar_t* Name() const override { return L"Logitech"; }
    void Rescan() override;  // pick up new receivers, drop removed ones
    void Stop() override;
    void PollSoon() override;

private:
    std::vector<std::unique_ptr<Receiver>> receivers_;
};
