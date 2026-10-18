#pragma once
#include "reminder_types.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tt {
using Millis = int64_t;
constexpr Millis kIdleMs = 300000;
struct Usage {
    std::string app;
    Millis startUtc{}, duration{};
};
struct Observation {
    Millis tick{}, utc{}, lastInput{};
    std::string app;
    bool available{true};
};
class Timeline {
  public:
    explicit Timeline(std::function<void(const Usage &)> sink,
                      std::function<void()> interrupted = {})
        : sink_(std::move(sink)), interrupted_(std::move(interrupted)) {}
    void observe(const Observation &next);
    void reset() {
        initialized_ = false;
    }

  private:
    Observation previous_;
    bool initialized_{};
    std::function<void(const Usage &)> sink_;
    std::function<void()> interrupted_;
};
struct LocalPart {
    std::string day;
    int hour{};
    Millis startUtc{}, duration{};
};
std::vector<LocalPart> splitLocal(const Usage &usage);
std::string localDay(Millis utc);
std::string shiftDay(const std::string &day, int days);
std::string weekStart(const std::string &day);
Millis utcNow();
struct AppInfo {
    std::string id;
    std::wstring name, path;
};
struct AppRow {
    AppInfo app;
    Millis used{};
    std::shared_ptr<void> icon;
};
struct Snapshot {
    std::string day;
    bool weekly{}, paused{};
    uint64_t request{};
    Millis total{}, selectedTotal{};
    std::vector<Millis> buckets;
    std::vector<AppRow> apps;
    std::vector<AppInfo> catalog;
    std::vector<ReminderView> reminders;
    std::wstring error;
};
std::wstring formatDuration(Millis ms);
std::string utf8(const std::wstring &value);
std::wstring wide(const std::string &value);
} // namespace tt
