#pragma once
#include "common.h"

#include <chrono>
#include <climits>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

// A source of battery readings (Logitech receivers, GameSir controllers, Bluetooth, ...).
// All methods are called on the UI thread. Providers report devices through PublishDevice()
// using ids prefixed with their own short name, e.g. "logi:...".
class Provider {
public:
    virtual ~Provider() = default;
    virtual const wchar_t* Name() const = 0;

    virtual void Rescan() = 0;      // startup, device plugged/unplugged
    virtual void Stop() = 0;        // drop everything (exit, "Rediscover Devices")
    virtual void PollSoon() {}      // e.g. after the PC resumes from sleep
};

// Background thread that runs `work` whenever it is due and otherwise sleeps.
// `work` returns how many milliseconds to wait before the next run.
class PeriodicWorker {
public:
    ~PeriodicWorker() { Stop(); }

    void Start(std::function<DWORD()> work) {
        Stop();
        stopping_ = false;
        due_ = 0;
        work_ = std::move(work);
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        {
            std::lock_guard lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // Run again within `delayMs` (sooner if already scheduled sooner).
    void Wake(DWORD delayMs = 0) {
        {
            std::lock_guard lk(mu_);
            ULONGLONG at = GetTickCount64() + delayMs;
            if (at < due_) due_ = at;
        }
        cv_.notify_all();
    }

    bool Running() const { return thread_.joinable(); }

private:
    void Run() {
        std::unique_lock lk(mu_);
        while (!stopping_) {
            ULONGLONG now = GetTickCount64();
            if (due_ <= now) {
                due_ = ULLONG_MAX;
                lk.unlock();
                DWORD next = work_();
                lk.lock();
                ULONGLONG at = GetTickCount64() + next;
                if (at < due_) due_ = at;
                continue;
            }
            if (due_ == ULLONG_MAX)
                cv_.wait(lk);
            else
                cv_.wait_for(lk, std::chrono::milliseconds(due_ - now));
        }
    }

    std::function<DWORD()> work_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    ULONGLONG due_ = 0;
};
