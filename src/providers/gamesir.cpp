#include "gamesir.h"
#include "../settings.h"

#include <hidsdi.h>

#include <algorithm>

namespace {

constexpr USHORT kVendorId = 0x3537;
constexpr USHORT kVendorPage = 0xFFF0;
constexpr uint8_t kStateReportId = 0x12;
constexpr DWORD kReadTimeoutMs = 600;

struct Model {
    USHORT pid;
    const wchar_t* name;
};
// Product ids from GameSir Connect's Cyclone 2 entry (wired, wireless dongle, ADC variants).
const Model kModels[] = {
    {4107, L"GameSir Cyclone 2"},  // 0x100B wireless (dongle)
    {4138, L"GameSir Cyclone 2"},  // 0x102A wireless
    {4125, L"GameSir Cyclone 2"},  // 0x101D wired
    {4179, L"GameSir Cyclone 2"},  // 0x1053 wired (ADC)
};

const Model* FindModel(USHORT pid) {
    for (const auto& m : kModels)
        if (m.pid == pid) return &m;
    return nullptr;
}

}  // namespace

GameSirProvider::GameSirProvider() { stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

GameSirProvider::~GameSirProvider() {
    Stop();
    if (stopEvent_) CloseHandle(stopEvent_);
}

void GameSirProvider::Rescan() {
    std::vector<Target> found;
    for (const auto& i : EnumerateHidInterfaces(kVendorId)) {
        if (i.usagePage != kVendorPage) continue;
        const Model* model = FindModel(i.productId);
        if (!model) continue;

        Target t;
        t.path = i.path;
        t.name = model->name;
        t.outputLen = i.outputLen ? i.outputLen : 64;
        t.inputLen = i.inputLen ? i.inputLen : 64;

        // Stable id: USB serial if the device has one, otherwise the model.
        std::wstring serial;
        HANDLE h = CreateFileW(i.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t buf[128] = L"";
            if (HidD_GetSerialNumberString(h, buf, sizeof(buf))) serial = buf;
            CloseHandle(h);
        }
        t.id = L"gamesir:c2" + (serial.empty() ? std::wstring() : L"-" + serial);

        if (std::none_of(found.begin(), found.end(), [&](const Target& x) { return x.id == t.id; }))
            found.push_back(std::move(t));
    }

    {
        std::lock_guard lk(mu_);
        targets_ = std::move(found);
    }

    if (!worker_.Running()) {
        ResetEvent(stopEvent_);
        worker_.Start([this] { return PollAll(); });
    } else {
        worker_.Wake(500);
    }
}

void GameSirProvider::Stop() {
    SetEvent(stopEvent_);
    worker_.Stop();
    std::lock_guard lk(mu_);
    targets_.clear();
}

void GameSirProvider::PollSoon() { worker_.Wake(3000); }

DWORD GameSirProvider::PollAll() {
    const auto& cfg = GetSettings().gamesir;
    std::vector<Target> targets;
    {
        std::lock_guard lk(mu_);
        targets = targets_;
    }

    bool anyFailed = false;
    for (const auto& t : targets) {
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return INFINITE;

        DeviceState& s = known_[t.id];
        bool first = s.id.empty();
        s.id = t.id;
        s.name = t.name;
        s.kind = DeviceKind::Gamepad;
        s.hasBattery = true;

        Reading r{};
        if (!ReadBattery(t, r)) {
            anyFailed = true;
            Log(L"%s: battery read failed", t.name.c_str());
            if (first) PublishDevice(s);  // show it in the menu even before the first reading
            continue;
        }
        if (first || !s.connected || r.percent != s.percent || r.status != s.status) {
            s.connected = true;
            s.percent = r.percent;
            s.status = r.status;
            Log(L"%s (%s): %.0f%% status %d", t.name.c_str(), t.id.c_str(), r.percent, (int)r.status);
            PublishDevice(s);
        }
    }

    // Controllers that went away (switched off: the dongle changes product id) keep
    // their last reading but are marked disconnected.
    for (auto& [id, s] : known_) {
        bool present = std::any_of(targets.begin(), targets.end(), [&](const Target& t) { return t.id == id; });
        if (!present && s.connected) {
            s.connected = false;
            PublishDevice(s);
        }
    }

    return (DWORD)(anyFailed ? cfg.retryTime : cfg.pollPeriod) * 1000;
}

bool GameSirProvider::ReadBattery(const Target& t, Reading& out) {
    HANDLE h = OpenHidReadWrite(t.path);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = false;
    std::vector<uint8_t> heartbeat(t.outputLen, 0);
    heartbeat[0] = 0x0F;
    heartbeat[1] = 0xF2;
    heartbeat[2] = 0x00;

    if (HidWrite(h, heartbeat.data(), (DWORD)heartbeat.size(), 500, stopEvent_)) {
        std::vector<uint8_t> buf(std::max<USHORT>(t.inputLen, 64));
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ULONGLONG deadline = GetTickCount64() + kReadTimeoutMs;

        while (!ok) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            ResetEvent(ov.hEvent);
            DWORD n = 0;
            if (!ReadFile(h, buf.data(), (DWORD)buf.size(), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            HANDLE waits[2] = {ov.hEvent, stopEvent_};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, (DWORD)(deadline - now));
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &n, TRUE);
                break;
            }
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) break;

            // A live state report has centred sticks (0x80); an idle dongle sends all zeros.
            if (n > 36 && buf[0] == kStateReportId && (buf[1] | buf[2] | buf[3] | buf[4]) != 0) {
                uint8_t charge = buf[35];
                uint8_t level = buf[36];
                out.percent = std::min<int>(level, 100);
                out.status = charge == 0   ? PowerStatus::Discharging
                             : charge == 2 ? PowerStatus::Full
                                           : PowerStatus::Charging;
                ok = true;
            }
        }
        CloseHandle(ov.hEvent);
    }

    CloseHandle(h);
    return ok;
}
