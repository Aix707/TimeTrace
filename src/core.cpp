#include "core.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <windows.h>

namespace tt {
void Timeline::observe(const Observation &n) {
    if (initialized_ && n.tick < previous_.tick)
        return;
    if (initialized_ && previous_.available) {
        const auto &p = previous_;
        // Large unexplained gaps are not evidence of active use (e.g. lost resume event).
        Millis end = n.tick - p.tick > 5000 ? p.tick : n.tick;
        Millis a = p.tick, b = std::clamp(p.lastInput + kIdleMs, a, end);
        if (b > a)
            sink_({p.app, p.utc, b - a});
        // A new input can resume an idle interval between two heartbeats.
        if (n.lastInput > p.lastInput && n.lastInput <= end) {
            if (interrupted_ && n.lastInput >= p.lastInput + kIdleMs)
                interrupted_();
            Millis c = std::max({b, a, n.lastInput});
            if (end > c)
                sink_({p.app, p.utc + c - a, end - c});
        }
    }
    if (interrupted_ && (!n.available || n.tick - n.lastInput >= kIdleMs ||
                         (initialized_ && n.tick - previous_.tick > 5000)))
        interrupted_();
    Millis anchoredUtc = n.utc;
    if (initialized_) {
        auto expected = previous_.utc + n.tick - previous_.tick;
        if (std::abs(expected - n.utc) < 50)
            anchoredUtc = expected;
    }
    previous_ = n;
    previous_.utc = anchoredUtc;
    previous_.lastInput = std::min(n.lastInput, n.tick);
    initialized_ = true;
}
static constexpr uint64_t kEpoch = 116444736000000000ULL;
Millis utcNow() {
    FILETIME f{};
    GetSystemTimePreciseAsFileTime(&f);
    ULARGE_INTEGER u{};
    u.LowPart = f.dwLowDateTime;
    u.HighPart = f.dwHighDateTime;
    return static_cast<Millis>((u.QuadPart - kEpoch) / 10000);
}
static SYSTEMTIME systemFromUtc(Millis utc) {
    ULARGE_INTEGER u{};
    u.QuadPart = static_cast<uint64_t>(utc) * 10000 + kEpoch;
    FILETIME f{u.LowPart, u.HighPart};
    SYSTEMTIME s{};
    FileTimeToSystemTime(&f, &s);
    return s;
}
static Millis utcFromSystem(const SYSTEMTIME &s) {
    FILETIME f{};
    SystemTimeToFileTime(&s, &f);
    ULARGE_INTEGER u{};
    u.LowPart = f.dwLowDateTime;
    u.HighPart = f.dwHighDateTime;
    return static_cast<Millis>((u.QuadPart - kEpoch) / 10000);
}
static std::string dayText(const SYSTEMTIME &s) {
    char b[16];
    sprintf_s(b, "%04u-%02u-%02u", s.wYear, s.wMonth, s.wDay);
    return b;
}
std::string localDay(Millis utc) {
    SYSTEMTIME u = systemFromUtc(utc), l{};
    SystemTimeToTzSpecificLocalTime(nullptr, &u, &l);
    return dayText(l);
}
std::vector<LocalPart> splitLocal(const Usage &usage) {
    std::vector<LocalPart> out;
    Millis utc = usage.startUtc, left = usage.duration;
    while (left > 0) {
        SYSTEMTIME u = systemFromUtc(utc), l{};
        SystemTimeToTzSpecificLocalTime(nullptr, &u, &l);
        // Split at the next UTC minute or local hour, including DST discontinuities.
        Millis step =
            std::min(left, static_cast<Millis>((60 - l.wSecond) * 1000 - l.wMilliseconds));
        if (step <= 0)
            step = std::min<Millis>(left, 1000);
        auto day = dayText(l);
        if (!out.empty() && out.back().day == day && out.back().hour == l.wHour &&
            out.back().startUtc + out.back().duration == utc)
            out.back().duration += step;
        else
            out.push_back({day, l.wHour, utc, step});
        utc += step;
        left -= step;
    }
    return out;
}
std::string shiftDay(const std::string &day, int days) {
    int y{}, m{}, d{};
    if (sscanf_s(day.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
        throw std::runtime_error("Invalid date");
    SYSTEMTIME s{};
    s.wYear = static_cast<WORD>(y);
    s.wMonth = static_cast<WORD>(m);
    s.wDay = static_cast<WORD>(d);
    s.wHour = 12;
    return dayText(systemFromUtc(utcFromSystem(s) + static_cast<Millis>(days) * 86400000));
}
std::string weekStart(const std::string &day) {
    int y{}, m{}, d{};
    sscanf_s(day.c_str(), "%d-%d-%d", &y, &m, &d);
    SYSTEMTIME s{};
    s.wYear = static_cast<WORD>(y);
    s.wMonth = static_cast<WORD>(m);
    s.wDay = static_cast<WORD>(d);
    s.wHour = 12;
    auto date = systemFromUtc(utcFromSystem(s));
    return shiftDay(day, -((date.wDayOfWeek + 6) % 7));
}
std::wstring formatDuration(Millis ms) {
    ms = std::max<Millis>(0, ms);
    auto seconds = ms / 1000, minutes = seconds / 60, hours = minutes / 60;
    if (hours)
        return std::to_wstring(hours) + L"小时" +
               (minutes % 60 ? std::to_wstring(minutes % 60) + L"分钟" : L"");
    if (minutes)
        return std::to_wstring(minutes) + L"分钟";
    return std::to_wstring(seconds) + L"秒";
}
std::string utf8(const std::wstring &v) {
    if (v.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}
std::wstring wide(const std::string &v) {
    if (v.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v.data(), static_cast<int>(v.size()),
                                nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v.data(), static_cast<int>(v.size()),
                        out.data(), n);
    return out;
}
} // namespace tt
