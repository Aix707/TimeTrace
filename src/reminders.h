#pragma once
#include "core.h"
#include <map>

namespace tt {
class ReminderEngine {
  public:
    explicit ReminderEngine(std::string session) : session_(std::move(session)) {}
    void load(const std::vector<ReminderRule> &rules, const std::vector<ReminderState> &states,
              const std::map<std::string, Millis> &usage, const std::string &day,
              bool resetDevice = false);
    void changeDay(const std::vector<ReminderState> &states,
                   const std::map<std::string, Millis> &usage, const std::string &day);
    void add(const std::string &app, Millis duration);
    void resetDevice();
    bool due(bool available) const;
    std::vector<ReminderEvent> evaluate(bool available);
    bool apply(const ReminderAction &action);
    std::vector<ReminderView> views() const;
    std::vector<ReminderState> states() const;
    const std::string &day() const {
        return day_;
    }
    bool dirty() const {
        return dirty_;
    }
    void saved() {
        dirty_ = false;
    }

  private:
    struct Entry {
        ReminderRule rule;
        ReminderState state;
        Millis used{};
    };
    static bool isDue(const Entry &entry);
    std::map<int64_t, Entry> entries_;
    std::string session_, day_;
    uint64_t serial_{};
    bool dirty_{};
};
} // namespace tt
