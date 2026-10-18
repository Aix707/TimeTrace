#include "reminders.h"
#include <algorithm>

namespace tt {
void ReminderEngine::load(const std::vector<ReminderRule> &rules,
                          const std::vector<ReminderState> &states,
                          const std::map<std::string, Millis> &usage, const std::string &day,
                          bool reset) {
    auto old = std::move(entries_);
    entries_.clear();
    day_ = day;
    for (const auto &rule : rules) {
        Entry e;
        e.rule = rule;
        e.state.rule = rule.id;
        e.state.day = day;
        for (const auto &s : states)
            if (s.rule == rule.id && s.day == day)
                e.state = s;
        if (rule.kind == ReminderKind::App) {
            auto found = usage.find(rule.app);
            if (found != usage.end())
                e.used = found->second;
        } else {
            auto previous = old.find(rule.id);
            if (!reset && previous != old.end() &&
                previous->second.rule.threshold == rule.threshold &&
                previous->second.rule.enabled == rule.enabled) {
                e.used = previous->second.used;
                if (previous->second.state.day == day)
                    e.state = previous->second.state;
                else {
                    e.state.next = previous->second.state.next;
                    e.state.token.clear();
                    dirty_ = true;
                }
            } else {
                e.state.next = rule.threshold;
                e.state.notified = false;
                e.state.token.clear();
                dirty_ = true;
            }
        }
        entries_[rule.id] = std::move(e);
    }
}
void ReminderEngine::changeDay(const std::vector<ReminderState> &states,
                               const std::map<std::string, Millis> &usage, const std::string &day) {
    if (day == day_)
        return;
    day_ = day;
    for (auto &[id, e] : entries_) {
        auto next = e.state.next;
        e.state = {id, day};
        for (const auto &s : states)
            if (s.rule == id && s.day == day)
                e.state = s;
        if (e.rule.kind == ReminderKind::Device) {
            e.state.next = next;
            e.state.token.clear();
        } else {
            auto value = usage.find(e.rule.app);
            e.used = value == usage.end() ? 0 : value->second;
        }
    }
    dirty_ = true;
}
void ReminderEngine::add(const std::string &app, Millis duration) {
    if (duration <= 0)
        return;
    for (auto &[id, e] : entries_) {
        if (!e.rule.enabled)
            continue;
        if (e.rule.kind == ReminderKind::App && (app.empty() || app != e.rule.app))
            continue;
        e.used += duration;
        if (e.rule.kind == ReminderKind::Device && e.state.muted && e.used >= e.state.next) {
            e.state.next = e.used + e.rule.threshold;
            dirty_ = true;
        }
    }
}
void ReminderEngine::resetDevice() {
    for (auto &[id, e] : entries_)
        if (e.rule.kind == ReminderKind::Device) {
            if (e.used || e.state.notified || !e.state.token.empty())
                dirty_ = true;
            e.used = 0;
            e.state.next = e.rule.threshold;
            e.state.notified = false;
            e.state.token.clear();
        }
}
bool ReminderEngine::isDue(const Entry &e) {
    return e.rule.enabled && !e.state.muted &&
           !(e.rule.kind == ReminderKind::App && e.state.notified && !e.state.next) &&
           e.used >= (e.state.next ? e.state.next : e.rule.threshold);
}
bool ReminderEngine::due(bool available) const {
    return available && std::any_of(entries_.begin(), entries_.end(),
                                    [](const auto &item) { return isDue(item.second); });
}
std::vector<ReminderEvent> ReminderEngine::evaluate(bool available) {
    std::vector<ReminderEvent> result;
    if (!available)
        return result;
    for (auto &[id, e] : entries_) {
        if (!isDue(e))
            continue;
        e.state.notified = true;
        e.state.token = session_ + "-" + std::to_string(++serial_);
        e.state.next = e.rule.kind == ReminderKind::Device ? e.used + e.rule.threshold : 0;
        result.push_back({e.rule, e.state.token, e.used});
        dirty_ = true;
    }
    return result;
}
bool ReminderEngine::apply(const ReminderAction &action) {
    auto found = entries_.find(action.rule);
    if (found == entries_.end() || action.token.empty())
        return false;
    auto &e = found->second;
    if (!e.rule.enabled || e.state.day != day_ || e.state.token != action.token)
        return false;
    if (action.kind != ReminderAction::Snooze && action.kind != ReminderAction::MuteToday)
        return false;
    e.state.token.clear();
    if (action.kind == ReminderAction::MuteToday) {
        e.state.muted = true;
    } else {
        e.state.muted = false;
        e.state.notified = false;
        e.state.next = std::max(e.used, e.rule.threshold) + kIdleMs;
    }
    dirty_ = true;
    return true;
}
std::vector<ReminderView> ReminderEngine::views() const {
    std::vector<ReminderView> result;
    for (const auto &[id, e] : entries_)
        result.push_back({e.rule, e.state, e.used});
    return result;
}
std::vector<ReminderState> ReminderEngine::states() const {
    std::vector<ReminderState> result;
    for (const auto &[id, e] : entries_)
        result.push_back(e.state);
    return result;
}
} // namespace tt
