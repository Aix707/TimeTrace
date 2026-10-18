#include "notification_protocol.h"
#include "core.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <map>

namespace tt {
namespace {
bool validToken(std::string_view token) {
    return !token.empty() && token.size() <= 80 &&
           std::all_of(token.begin(), token.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      c == '-';
           });
}
std::wstring escape(const std::wstring &s) {
    std::wstring r;
    for (auto c : s) {
        if (c == L'&')
            r += L"&amp;";
        else if (c == L'<')
            r += L"&lt;";
        else if (c == L'>')
            r += L"&gt;";
        else if (c == L'\"')
            r += L"&quot;";
        else
            r += c;
    }
    return r;
}
} // namespace
std::optional<NotificationRequest> parseReminderUri(std::wstring_view input) {
    if (input.size() > 512)
        return {};
    std::string uri;
    for (auto c : input) {
        if (c > 127 || c < 32)
            return {};
        uri += static_cast<char>(c);
    }
    auto question = uri.find('?');
    auto base = uri.substr(0, question);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    if (!base.empty() && base.back() == '/')
        base.pop_back();
    if (base != "timetrace://reminders" && base != "timetrace://limits")
        return {};
    if (question == std::string::npos)
        return NotificationRequest{};
    if (uri.back() == '&')
        return {};
    if (base != "timetrace://reminders")
        return {};
    std::map<std::string, std::string> args;
    size_t start = question + 1;
    while (start < uri.size()) {
        auto end = uri.find('&', start);
        if (end == std::string::npos)
            end = uri.size();
        auto equal = uri.find('=', start);
        if (equal == std::string::npos || equal >= end || equal == start)
            return {};
        if (!args.emplace(uri.substr(start, equal - start), uri.substr(equal + 1, end - equal - 1))
                 .second)
            return {};
        start = end + 1;
    }
    if (args.size() != 3 || !args.contains("action") || !args.contains("rule") ||
        !args.contains("token"))
        return {};
    ReminderAction action;
    if (args["action"] == "snooze")
        action.kind = ReminderAction::Snooze;
    else if (args["action"] == "mute")
        action.kind = ReminderAction::MuteToday;
    else
        return {};
    const auto &id = args["rule"];
    auto parsed = std::from_chars(id.data(), id.data() + id.size(), action.rule);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || action.rule <= 0 ||
        !validToken(args["token"]))
        return {};
    action.token = args["token"];
    return NotificationRequest{action};
}
std::wstring reminderActionUri(const ReminderAction &action) {
    return L"timetrace://reminders?action=" +
           std::wstring(action.kind == ReminderAction::Snooze ? L"snooze" : L"mute") + L"&rule=" +
           std::to_wstring(action.rule) + L"&token=" + wide(action.token);
}
ReminderActionWire reminderWire(const ReminderAction &action) {
    ReminderActionWire wire;
    wire.kind = static_cast<uint32_t>(action.kind);
    wire.rule = action.rule;
    if (validToken(action.token))
        std::memcpy(wire.token, action.token.data(), action.token.size());
    return wire;
}
std::optional<ReminderAction> parseReminderWire(const ReminderActionWire &wire) {
    if (wire.version != 1 || wire.kind > 1 || wire.rule <= 0 ||
        !std::memchr(wire.token, 0, sizeof(wire.token)))
        return {};
    std::string token = wire.token;
    if (!validToken(token))
        return {};
    return ReminderAction{static_cast<ReminderAction::Kind>(wire.kind), wire.rule, token};
}
std::wstring reminderTitle(const ReminderEvent &event) {
    return event.rule.kind == ReminderKind::Device ? L"休息提醒" : L"应用提醒";
}
std::wstring reminderBody(const ReminderEvent &event) {
    return event.rule.kind == ReminderKind::Device
               ? L"已连续使用 " + formatDuration(event.used)
               : event.name + L" 今日已用 " + formatDuration(event.used);
}
std::wstring reminderToastXml(const ReminderEvent &event) {
    auto action = [&](ReminderAction::Kind kind, const wchar_t *label) {
        return L"<action activationType=\"protocol\" content=\"" + escape(label) +
               L"\" arguments=\"" + escape(reminderActionUri({kind, event.rule.id, event.token})) +
               L"\"/>";
    };
    return L"<toast activationType=\"protocol\" launch=\"timetrace://reminders\"><visual><binding "
           L"template=\"ToastGeneric\"><text>" +
           escape(reminderTitle(event)) + L"</text><text>" + escape(reminderBody(event)) +
           L"</text></binding></visual><actions>" +
           action(ReminderAction::Snooze, L"再使用 5 分钟后提醒") +
           action(ReminderAction::MuteToday, L"今日不再提醒") + L"</actions></toast>";
}
} // namespace tt
