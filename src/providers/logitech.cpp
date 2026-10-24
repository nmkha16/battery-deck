#include "logitech.h"
#include "../settings.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>

namespace {

constexpr uint8_t SW_ID = 0x0A;
constexpr size_t MAX_QUEUED_RESPONSES = 5;

std::wstring Hex(const uint8_t* p, size_t n) {
    static const wchar_t* digits = L"0123456789ABCDEF";
    std::wstring s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 0xF];
    }
    return s;
}

// HID++ parameters start at byte 4.
uint8_t P(const std::vector<uint8_t>& r, size_t i) { return 4 + i < r.size() ? r[4 + i] : 0; }

const int kVoltageTable[] = {
    4186, 4156, 4143, 4133, 4122, 4113, 4103, 4094, 4086, 4075, 4067, 4059, 4051, 4043, 4035, 4027, 4019,
    4011, 4003, 3997, 3989, 3983, 3976, 3969, 3961, 3955, 3949, 3942, 3935, 3929, 3922, 3916, 3909, 3902,
    3896, 3890, 3883, 3877, 3870, 3865, 3859, 3853, 3848, 3842, 3837, 3833, 3828, 3824, 3819, 3815, 3811,
    3808, 3804, 3800, 3797, 3793, 3790, 3787, 3784, 3781, 3778, 3775, 3772, 3770, 3767, 3764, 3762, 3759,
    3757, 3754, 3751, 3748, 3744, 3741, 3737, 3734, 3730, 3726, 3724, 3720, 3717, 3714, 3710, 3706, 3702,
    3697, 3693, 3688, 3683, 3677, 3671, 3666, 3662, 3658, 3654, 3646, 3633, 3612, 3579, 3537};

double PercentFromMillivolts(int mv) {
    const int n = (int)(sizeof(kVoltageTable) / sizeof(kVoltageTable[0]));
    for (int i = 0; i < n; ++i)
        if (mv > kVoltageTable[i]) return n - i;
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Receiver lifetime

Receiver::Receiver(const HidInterfaceInfo& shortIf, const HidInterfaceInfo& longIf)
    : shortIf_(shortIf), longIf_(longIf) {}

Receiver::~Receiver() { Stop(); }

bool Receiver::Start() {
    auto open = [](const std::wstring& path) {
        return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    };
    hShort_ = open(shortIf_.path);
    hLong_ = open(longIf_.path);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (hShort_ == INVALID_HANDLE_VALUE || hLong_ == INVALID_HANDLE_VALUE || !stopEvent_) {
        Log(L"Failed to open %s (error %lu)", shortIf_.path.c_str(), GetLastError());
        Stop();
        return false;
    }

    Log(L"Opened receiver %s", shortIf_.path.c_str());
    tasks_.push_back({GetTickCount64(), TaskKind::Setup, 0});
    started_ = true;
    reader_ = std::thread(&Receiver::ReaderLoop, this);
    worker_ = std::thread(&Receiver::WorkerLoop, this);
    return true;
}

void Receiver::Stop() {
    if (started_) {
        {
            std::lock_guard lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        SetEvent(stopEvent_);
        if (reader_.joinable()) reader_.join();
        if (worker_.joinable()) worker_.join();
        started_ = false;

        for (auto& [idx, d] : devices_) {
            if (d.initialized && d.state.connected) {
                d.state.connected = false;
                PublishDevice(d.state);
            }
        }
    }

    for (HANDLE* h : {&hShort_, &hLong_}) {
        if (*h != INVALID_HANDLE_VALUE) CloseHandle(*h);
        *h = INVALID_HANDLE_VALUE;
    }
    if (stopEvent_) CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
}

void Receiver::PollSoon() {
    {
        std::lock_guard lk(mu_);
        pollSoon_ = true;
    }
    cv_.notify_all();
}

bool Receiver::IsStopping() {
    std::lock_guard lk(mu_);
    return stopping_;
}

void Receiver::SleepInterruptible(DWORD ms) { WaitForSingleObject(stopEvent_, ms); }

// ---------------------------------------------------------------------------
// Reader thread

void Receiver::ReaderLoop() {
    struct Pipe {
        HANDLE h;
        std::vector<uint8_t> buf;
        OVERLAPPED ov{};
        bool pending = false;
    };
    Pipe pipes[2] = {
        {hShort_, std::vector<uint8_t>(std::max<USHORT>(shortIf_.inputLen, 7))},
        {hLong_, std::vector<uint8_t>(std::max<USHORT>(longIf_.inputLen, 20))},
    };

    auto startRead = [](Pipe& p) {
        ResetEvent(p.ov.hEvent);
        if (!ReadFile(p.h, p.buf.data(), (DWORD)p.buf.size(), nullptr, &p.ov) && GetLastError() != ERROR_IO_PENDING) {
            p.pending = false;
            return false;
        }
        p.pending = true;
        return true;
    };

    for (auto& p : pipes) p.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    bool failed = !startRead(pipes[0]) || !startRead(pipes[1]);
    HANDLE waits[3] = {stopEvent_, pipes[0].ov.hEvent, pipes[1].ov.hEvent};

    while (!failed) {
        DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1 && w != WAIT_OBJECT_0 + 2) {
            failed = true;
            break;
        }
        Pipe& p = pipes[w - WAIT_OBJECT_0 - 1];
        DWORD n = 0;
        p.pending = false;
        if (!GetOverlappedResult(p.h, &p.ov, &n, FALSE)) {
            failed = true;
            break;
        }
        if (n > 0) OnReport(p.buf.data(), n);
        if (!startRead(p)) failed = true;
    }

    for (auto& p : pipes) {
        if (p.pending) {
            DWORD n = 0;
            CancelIoEx(p.h, &p.ov);
            GetOverlappedResult(p.h, &p.ov, &n, TRUE);
        }
        CloseHandle(p.ov.hEvent);
    }

    if (failed && !IsStopping()) {
        Log(L"Receiver %s went away (error %lu)", shortIf_.path.c_str(), GetLastError());
        dead_ = true;
        RequestRescan();
    }
}

void Receiver::OnReport(const uint8_t* data, DWORD len) {
    // HID++ 1.0 "device connection" notification with the link-established bit:
    // a paired device was switched on or woke up.
    if (len >= 5 && data[0] == 0x10 && data[2] == 0x41 && (data[4] & 0x40) == 0) {
        uint8_t idx = data[1];
        Log(L"Device %u connected", idx);
        {
            std::lock_guard lk(mu_);
            announced_.insert(idx);
        }
        QueueInit(idx, GetTickCount64() + 1000);
        return;
    }

    {
        std::lock_guard lk(mu_);
        responses_.emplace_back(data, data + len);
        while (responses_.size() > MAX_QUEUED_RESPONSES) responses_.pop_front();
    }
    cv_.notify_all();
}

void Receiver::QueueInit(uint8_t idx, ULONGLONG due) {
    {
        std::lock_guard lk(mu_);
        auto it = std::find_if(tasks_.begin(), tasks_.end(),
                               [&](const Task& t) { return t.kind == TaskKind::Init && t.idx == idx; });
        if (it != tasks_.end())
            it->due = due;  // coalesce repeated announcements
        else
            tasks_.push_back({due, TaskKind::Init, idx});
    }
    cv_.notify_all();
}

// ---------------------------------------------------------------------------
// Worker thread

void Receiver::WorkerLoop() {
    std::unique_lock lk(mu_);
    while (!stopping_) {
        ULONGLONG now = GetTickCount64();

        if (pollSoon_) {
            pollSoon_ = false;
            for (auto& [idx, d] : devices_)
                if (d.nextPoll) d.nextPoll = now + 3000;
        }

        // Due task?
        auto task = std::min_element(tasks_.begin(), tasks_.end(),
                                     [](const Task& a, const Task& b) { return a.due < b.due; });
        if (task != tasks_.end() && task->due <= now) {
            Task t = *task;
            tasks_.erase(task);
            lk.unlock();
            if (t.kind == TaskKind::Setup)
                DoSetup();
            else
                DoInit(t.idx);
            lk.lock();
            continue;
        }

        // Due battery poll?
        ULONGLONG next = ULLONG_MAX;
        Device* due = nullptr;
        for (auto& [idx, d] : devices_) {
            if (!d.nextPoll) continue;
            if (d.nextPoll <= now) {
                due = &d;
                break;
            }
            next = std::min(next, d.nextPoll);
        }
        if (due) {
            lk.unlock();
            PollBattery(*due);
            lk.lock();
            continue;
        }

        if (task != tasks_.end()) next = std::min(next, task->due);
        if (next == ULLONG_MAX)
            cv_.wait(lk);
        else
            cv_.wait_for(lk, std::chrono::milliseconds(next - now));
    }
}

void Receiver::DoSetup() {
    // Ask the receiver how many devices are paired.
    Report r = WriteRead10({0x10, 0xFF, 0x81, 0x02, 0x00, 0x00, 0x00}, 1000);
    int paired = 0;
    if (r.size() >= 6 && r[2] == 0x81 && r[3] == 0x02) paired = r[5];
    Log(L"Receiver reports %d paired device(s)", paired);

    // Make every connected device announce itself (handled in OnReport).
    if (paired > 0) WriteRead10({0x10, 0xFF, 0x80, 0x02, 0x02, 0x00, 0x00}, 1000);

    SleepInterruptible(500);
    if (IsStopping()) return;

    bool anyAnnounced;
    {
        std::lock_guard lk(mu_);
        anyAnnounced = !announced_.empty();
    }
    if (anyAnnounced) return;

    // Nothing announced: probe device slots directly (0xFF = directly connected device).
    static const uint8_t kSlots[] = {1, 2, 3, 4, 5, 6, 0xFF};
    for (uint8_t idx : kSlots) {
        if (IsStopping()) return;
        if (Ping20(idx, 100, false)) DoInit(idx);
    }
}

void Receiver::DoInit(uint8_t idx) {
    Device& d = devices_[idx];
    d.idx = idx;
    d.nextPoll = 0;

    // If this re-init fails part way, keep polling a device that worked before.
    bool success = false;
    struct RestorePolling {
        Device& d;
        bool& ok;
        ~RestorePolling() {
            if (!ok && d.initialized && d.battery != BatteryFeature::None && !d.nextPoll)
                d.nextPoll = GetTickCount64() + (ULONGLONG)GetSettings().logitech.retryTime * 1000;
        }
    } restore{d, success};

    // Wait for a stable link: 3 pings in a row.
    int ok = 0;
    for (int i = 0; i < 10 && ok < 3; ++i) {
        if (IsStopping()) return;
        ok = Ping20(idx, 100, true) ? ok + 1 : 0;
    }
    if (ok < 3) {
        Log(L"Device %u did not answer pings", idx);
        return;
    }

    // Feature table (built separately so a failed re-read keeps the old one).
    std::map<uint16_t, uint8_t> features;
    Report r = WriteRead20({0x10, idx, 0x00, 0x00 | SW_ID, 0x00, 0x01, 0x00});
    uint8_t featureSet = P(r, 0);
    if (r.empty() || featureSet == 0) return;
    features[0x0001] = featureSet;

    r = WriteRead20({0x10, idx, featureSet, 0x00 | SW_ID, 0x00, 0x00, 0x00});
    if (r.empty()) return;
    int count = P(r, 0);
    for (int i = 0; i <= count; ++i) {
        if (IsStopping()) return;
        r = WriteRead20({0x10, idx, featureSet, 0x10 | SW_ID, (uint8_t)i, 0x00, 0x00});
        if (r.empty()) continue;
        features[(uint16_t)((P(r, 0) << 8) | P(r, 1))] = (uint8_t)i;
    }
    if (!features.count(0x0005)) return;  // devices without a name are ignored (as before)
    d.features = std::move(features);

    auto feature = [&](uint16_t id) -> std::optional<uint8_t> {
        auto it = d.features.find(id);
        if (it == d.features.end()) return std::nullopt;
        return it->second;
    };

    // 0x0005 device name & type. Devices without it are ignored (same as the original).
    auto nameFeature = feature(0x0005);
    if (!nameFeature) return;
    r = WriteRead20({0x10, idx, *nameFeature, 0x00 | SW_ID, 0x00, 0x00, 0x00});
    if (r.empty()) return;
    size_t nameLen = P(r, 0);
    std::string name;
    while (name.size() < nameLen) {
        r = WriteRead20({0x10, idx, *nameFeature, 0x10 | SW_ID, (uint8_t)name.size(), 0x00, 0x00});
        if (r.size() <= 4) return;
        size_t take = std::min(r.size() - 4, nameLen - name.size());
        name.append(reinterpret_cast<const char*>(&r[4]), take);
    }
    name = name.substr(0, name.find('\0'));
    std::wstring wname = Utf8ToWide(name);

    for (const auto& tag : GetSettings().logitech.disabledDevices) {
        if (!tag.empty() && wname.find(tag) != std::wstring::npos) {
            Log(L"%s is marked as disabled", wname.c_str());
            return;
        }
    }

    r = WriteRead20({0x10, idx, *nameFeature, 0x20 | SW_ID, 0x00, 0x00, 0x00});
    int type = r.empty() ? 3 : P(r, 0);

    // 0x0003 device information -> stable identifier (same format as the .NET app,
    // so saved device selections carry over).
    std::wstring id;
    if (auto infoFeature = feature(0x0003)) {
        r = WriteRead20({0x10, idx, *infoFeature, 0x00 | SW_ID, 0x00, 0x00, 0x00});
        if (r.size() >= 4 + 15) {
            std::wstring unitId = Hex(&r[4 + 1], 4);
            std::wstring modelId = Hex(&r[4 + 7], 5);
            bool serialSupported = (P(r, 14) & 0x1) != 0;
            if (serialSupported) {
                Report s = WriteRead20({0x10, idx, *infoFeature, 0x20 | SW_ID, 0x00, 0x00, 0x00});
                if (s.size() >= 4 + 11) id = Hex(&s[4], 11);
            }
            if (id.empty()) id = unitId + L"-" + modelId;
        }
    }
    if (id.empty()) {
        // No serial: stable hash of the name (FNV-1a).
        uint32_t h = 2166136261u;
        for (wchar_t c : wname) h = (h ^ (uint32_t)c) * 16777619u;
        wchar_t buf[16];
        swprintf_s(buf, L"%08X", h);
        id = buf;
    }

    if (feature(0x1000))
        d.battery = BatteryFeature::F1000;
    else if (feature(0x1001))
        d.battery = BatteryFeature::F1001;
    else if (feature(0x1004))
        d.battery = BatteryFeature::F1004;
    else
        d.battery = BatteryFeature::None;

    id = L"logi:" + id;
    bool sameDevice = d.initialized && d.state.id == id;
    d.state.id = id;
    d.state.name = wname;
    // HID++ device types: 0 keyboard, 8 headset, everything else drawn as a mouse.
    d.state.kind = type == 0 ? DeviceKind::Keyboard : type == 8 ? DeviceKind::Headset : DeviceKind::Mouse;
    d.state.hasBattery = d.battery != BatteryFeature::None;
    d.state.connected = true;
    if (!sameDevice) {
        d.state.percent = -1;
        d.state.status = PowerStatus::Unknown;
    }
    d.initialized = true;
    d.haveReading = false;  // always report the first reading after (re)connect
    Log(L"Device %u ready: %s (%s), battery feature %d", idx, wname.c_str(), id.c_str(), (int)d.battery);
    PublishDevice(d.state);

    // Poll right away, then on schedule. One schedule entry per device, so repeated
    // reconnects can never stack up extra polling.
    if (d.battery != BatteryFeature::None) d.nextPoll = GetTickCount64() + 1000;
    success = true;
}

std::optional<Receiver::Reading> Receiver::ReadBattery(const Device& d) {
    auto it = d.features.end();
    switch (d.battery) {
        case BatteryFeature::F1000: it = d.features.find(0x1000); break;
        case BatteryFeature::F1001: it = d.features.find(0x1001); break;
        case BatteryFeature::F1004: it = d.features.find(0x1004); break;
        default: return std::nullopt;
    }
    if (it == d.features.end()) return std::nullopt;
    uint8_t f = it->second;

    if (d.battery == BatteryFeature::F1000) {
        Report r = WriteRead20({0x10, d.idx, f, 0x00 | SW_ID, 0x00, 0x00, 0x00});
        if (r.empty()) return std::nullopt;
        PowerStatus s;
        switch (P(r, 2)) {
            case 0: s = PowerStatus::Discharging; break;
            case 1: case 2: case 4: s = PowerStatus::Charging; break;
            case 3: s = PowerStatus::Full; break;
            default: s = PowerStatus::NotCharging; break;
        }
        return Reading{(double)P(r, 0), s, -1};
    }

    if (d.battery == BatteryFeature::F1001) {
        Report r = WriteRead20({0x10, d.idx, f, 0x00 | SW_ID, 0x00, 0x00, 0x00});
        if (r.empty()) return std::nullopt;
        int mv = (P(r, 0) << 8) | P(r, 1);
        uint8_t flags = P(r, 2);
        PowerStatus s = PowerStatus::Discharging;
        if (flags & 0x80) {
            switch (flags & 0x07) {
                case 0: s = PowerStatus::Charging; break;
                case 1: s = PowerStatus::Full; break;
                case 2: s = PowerStatus::NotCharging; break;
                default: s = PowerStatus::Unknown; break;
            }
        }
        return Reading{PercentFromMillivolts(mv), s, mv};
    }

    // 0x1004 Unified Battery: GetStatus (function 1)
    Report r = WriteRead20({0x10, d.idx, f, 0x10 | SW_ID, 0x00, 0x00, 0x00});
    if (r.empty()) return std::nullopt;
    PowerStatus s;
    switch (P(r, 2)) {
        case 0: s = PowerStatus::Discharging; break;
        case 1: case 2: s = PowerStatus::Charging; break;
        case 3: s = PowerStatus::Full; break;
        default: s = PowerStatus::NotCharging; break;
    }
    return Reading{(double)P(r, 0), s, -1};
}

void Receiver::PollBattery(Device& d) {
    const auto& cfg = GetSettings().logitech;
    auto reading = ReadBattery(d);
    ULONGLONG now = GetTickCount64();
    if (!reading) {
        d.nextPoll = now + (ULONGLONG)cfg.retryTime * 1000;
        Log(L"%s: battery read failed, retry in %d s", d.state.name.c_str(), cfg.retryTime);
        return;
    }
    d.nextPoll = now + (ULONGLONG)cfg.pollPeriod * 1000;

    bool changed = !d.haveReading || reading->percent != d.state.percent || reading->status != d.state.status ||
                   reading->millivolts != d.state.millivolts || !d.state.connected;
    if (!changed) return;  // don't report if nothing changed

    d.haveReading = true;
    d.state.percent = reading->percent;
    d.state.status = reading->status;
    d.state.millivolts = reading->millivolts;
    d.state.connected = true;
    Log(L"%s: %.0f%% status %d", d.state.name.c_str(), d.state.percent, (int)d.state.status);
    PublishDevice(d.state);
}

// ---------------------------------------------------------------------------
// HID++ transactions (worker thread only)

bool Receiver::Write(const uint8_t* msg, size_t len) {
    std::vector<uint8_t> buf(std::max<size_t>(shortIf_.outputLen, len), 0);
    memcpy(buf.data(), msg, len);
    return HidWrite(hShort_, buf.data(), (DWORD)buf.size(), 1000, stopEvent_);
}

Receiver::Report Receiver::Transact(const uint8_t* msg, size_t len, int timeoutMs,
                                    const std::function<int(const Report&)>& match) {
    {
        std::lock_guard lk(mu_);
        if (stopping_) return {};
        responses_.clear();
    }
    if (!Write(msg, len)) return {};

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock lk(mu_);
    for (;;) {
        while (!responses_.empty()) {
            Report r = std::move(responses_.front());
            responses_.pop_front();
            int m = match(r);
            if (m > 0) return r;
            if (m < 0) return {};
        }
        if (stopping_) return {};
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout && responses_.empty()) return {};
    }
}

Receiver::Report Receiver::WriteRead10(std::initializer_list<uint8_t> msg, int timeoutMs) {
    std::vector<uint8_t> m(msg);
    return Transact(m.data(), m.size(), timeoutMs, [&](const Report& r) {
        return (r.size() >= 3 && (r[2] == 0x8F || r[2] == m[2])) ? 1 : 0;
    });
}

Receiver::Report Receiver::WriteRead20(std::initializer_list<uint8_t> msg, int timeoutMs, bool ignoreHidpp10) {
    std::vector<uint8_t> m(msg);
    return Transact(m.data(), m.size(), timeoutMs, [&](const Report& r) {
        if (r.size() < 4 || r[1] != m[1]) return 0;
        if (r[2] == 0x8F && !ignoreHidpp10) return -1;          // HID++ 1.0 error
        if (r[2] == 0xFF && r[3] == m[2]) return -1;            // HID++ 2.0 error for our request
        if (r[2] == m[2] && (r[3] & 0x0F) == SW_ID) return 1;
        return 0;
    });
}

bool Receiver::Ping20(uint8_t idx, int timeoutMs, bool ignoreHidpp10) {
    uint8_t payload = ++pingPayload_;
    Report r = WriteRead20({0x10, idx, 0x00, 0x10 | SW_ID, 0x00, 0x00, payload}, timeoutMs, ignoreHidpp10);
    return r.size() >= 7 && r[2] == 0x00 && (r[3] & 0x0F) == SW_ID && r[6] == payload;
}

// ---------------------------------------------------------------------------
// LogitechProvider

void LogitechProvider::Rescan() {
    // HID++ short (usage 1) and long (usage 2) collections on a vendor usage page.
    std::vector<HidInterfaceInfo> list;
    for (auto& i : EnumerateHidInterfaces(0x046D))
        if ((i.usagePage & 0xFF00) == 0xFF00 && (i.usage == 1 || i.usage == 2)) list.push_back(std::move(i));

    std::erase_if(receivers_, [&](const std::unique_ptr<Receiver>& r) {
        bool present = std::any_of(list.begin(), list.end(), [&](const HidInterfaceInfo& i) {
            return _wcsicmp(i.path.c_str(), r->ShortPath().c_str()) == 0;
        });
        return r->IsDead() || !present;
    });

    std::map<std::wstring, std::pair<const HidInterfaceInfo*, const HidInterfaceInfo*>> groups;
    for (const auto& i : list) {
        auto& g = groups[i.groupKey];
        if (i.usage == 1 && !g.first) g.first = &i;
        if (i.usage == 2 && !g.second) g.second = &i;
    }

    for (const auto& [key, g] : groups) {
        if (!g.first || !g.second) continue;
        bool known = std::any_of(receivers_.begin(), receivers_.end(), [&](const std::unique_ptr<Receiver>& r) {
            return _wcsicmp(r->ShortPath().c_str(), g.first->path.c_str()) == 0;
        });
        if (known) continue;
        auto r = std::make_unique<Receiver>(*g.first, *g.second);
        if (r->Start()) receivers_.push_back(std::move(r));
    }
}

void LogitechProvider::Stop() { receivers_.clear(); }

void LogitechProvider::PollSoon() {
    for (auto& r : receivers_) r->PollSoon();
}
