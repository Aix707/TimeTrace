#pragma once
#include <cstdint>
#include <string>

namespace tt {
enum class ReminderKind : int { Device = 0, App = 1 };
struct ReminderRule {
    int64_t id{};
    ReminderKind kind{ReminderKind::App};
    std::string app;
    int64_t threshold{3600000};
    bool enabled{true};
};
struct ReminderState {
    int64_t rule{};
    std::string day;
    int64_t next{};
    bool notified{}, muted{};
    std::string token;
};
struct ReminderEvent {
    ReminderRule rule;
    std::string token;
    int64_t used{};
    std::wstring name;
};
struct ReminderAction {
    enum Kind : int { Snooze = 0, MuteToday = 1 } kind{Snooze};
    int64_t rule{};
    std::string token;
};
struct ReminderView {
    ReminderRule rule;
    ReminderState state;
    int64_t used{};
    std::wstring name;
};
} // namespace tt
