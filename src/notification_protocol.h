#pragma once
#include "reminder_types.h"
#include <optional>
#include <string_view>

namespace tt {
struct NotificationRequest {
    std::optional<ReminderAction> action;
};
struct ReminderActionWire {
    uint32_t version{1}, kind{};
    int64_t rule{};
    char token[81]{};
};
constexpr uintptr_t kReminderCopyData = 0x5454524d;
std::optional<NotificationRequest> parseReminderUri(std::wstring_view uri);
std::wstring reminderActionUri(const ReminderAction &action);
ReminderActionWire reminderWire(const ReminderAction &action);
std::optional<ReminderAction> parseReminderWire(const ReminderActionWire &wire);
std::wstring reminderTitle(const ReminderEvent &event);
std::wstring reminderBody(const ReminderEvent &event);
std::wstring reminderToastXml(const ReminderEvent &event);
} // namespace tt
