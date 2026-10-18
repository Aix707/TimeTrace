#pragma once
#include "core.h"
#include <filesystem>
#include <map>
#include <optional>
#include <sqlite3.h>

namespace tt {
class Storage {
  public:
    explicit Storage(const std::filesystem::path &path);
    ~Storage();
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
    void app(const AppInfo &info);
    void save(const std::vector<Usage> &entries);
    void commit(const std::vector<Usage> &entries, const std::vector<ReminderState> &states);
    Snapshot snapshot(const std::string &day, bool weekly, const std::string &selected = {});
    const std::vector<AppInfo> &applications();
    void renameApp(const std::string &app, const std::wstring &name);
    void mergeApps(const std::string &source, const std::string &target);
    bool setReminder(const std::string &app, Millis duration, bool enabled = true);
    void removeReminder(int64_t rule);
    std::vector<ReminderRule> reminderRules();
    std::vector<ReminderState> reminderStates(const std::string &day);
    std::map<std::string, Millis> dailyUsage(const std::string &day);
    std::map<std::string, std::string> appMappings();
    std::string getSetting(const std::string &key, const std::string &fallback = {});
    void setSetting(const std::string &key, const std::string &value);
    void clear();

  private:
    sqlite3 *db_{};
    std::optional<std::vector<AppInfo>> catalog_;
    std::string canonicalId(const std::string &app);
    Millis usedOnDay(const std::string &app, const std::string &day);
    void writeUsage(const std::vector<Usage> &entries);
    void writeStates(const std::vector<ReminderState> &states);
    void mergeReminders(const std::string &from, const std::string &to);
};
} // namespace tt
