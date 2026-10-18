#include "storage.h"
#include "sqlite_statement.h"
#include <algorithm>

namespace tt {
void Storage::writeStates(const std::vector<ReminderState> &states) {
    if (states.empty())
        return;
    Statement s(db_, "INSERT INTO reminder_states VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(rule,day) "
                     "DO UPDATE SET next=excluded.next,notified=excluded.notified,"
                     "muted=excluded.muted,token=excluded.token");
    for (const auto &state : states) {
        s.reset();
        s.integer(1, state.rule);
        s.text(2, state.day);
        s.integer(3, state.next);
        s.integer(4, state.notified);
        s.integer(5, state.muted);
        s.text(6, state.token);
        s.step();
    }
}
std::vector<ReminderRule> Storage::reminderRules() {
    std::vector<ReminderRule> result;
    Statement s(db_,
                "SELECT id,kind,COALESCE(app,''),duration,enabled FROM reminder_rules ORDER BY id");
    while (s.step())
        result.push_back({s.integer(0), static_cast<ReminderKind>(s.integer(1)), s.text(2),
                          s.integer(3), s.integer(4) != 0});
    return result;
}
std::vector<ReminderState> Storage::reminderStates(const std::string &day) {
    std::vector<ReminderState> result;
    Statement s(db_, "SELECT rule,day,next,notified,muted,token FROM reminder_states WHERE day=?1");
    s.text(1, day);
    while (s.step())
        result.push_back({s.integer(0), s.text(1), s.integer(2), s.integer(3) != 0,
                          s.integer(4) != 0, s.text(5)});
    return result;
}
std::map<std::string, Millis> Storage::dailyUsage(const std::string &day) {
    std::map<std::string, Millis> result;
    Statement s(
        db_,
        "SELECT COALESCE(m.target,s.app),SUM(s.duration) FROM segments s LEFT JOIN app_links m "
        "ON m.app=s.app WHERE s.day=?1 GROUP BY COALESCE(m.target,s.app)");
    s.text(1, day);
    while (s.step())
        result[s.text(0)] = s.integer(1);
    return result;
}
std::map<std::string, std::string> Storage::appMappings() {
    std::map<std::string, std::string> result;
    Statement s(
        db_, "SELECT a.id,COALESCE(m.target,a.id) FROM apps a LEFT JOIN app_links m ON m.app=a.id");
    while (s.step())
        result[s.text(0)] = s.text(1);
    return result;
}
bool Storage::setReminder(const std::string &app, Millis duration, bool enabled) {
    if (duration <= 0 || duration > 86400000)
        throw std::runtime_error("Invalid reminder duration");
    auto id = app.empty() ? std::string{} : canonicalId(app);
    if (!app.empty() && id.empty())
        throw std::runtime_error("Application does not exist");
    Statement existing(db_, "SELECT id,duration,enabled FROM reminder_rules WHERE "
                            "(?1='' AND kind=0) OR (kind=1 AND app=?1)");
    existing.text(1, id);
    int64_t rule{};
    if (existing.step()) {
        rule = existing.integer(0);
        if (duration == existing.integer(1) && enabled == (existing.integer(2) != 0))
            return false;
    }
    Transaction transaction(db_);
    if (rule) {
        Statement s(db_, "UPDATE reminder_rules SET duration=?1,enabled=?2 WHERE id=?3");
        s.integer(1, duration);
        s.integer(2, enabled);
        s.integer(3, rule);
        s.step();
        Statement clear(db_, "DELETE FROM reminder_states WHERE rule=?1");
        clear.integer(1, rule);
        clear.step();
    } else {
        Statement s(db_,
                    "INSERT INTO reminder_rules(kind,app,duration,enabled) VALUES(1,?1,?2,?3)");
        s.text(1, id);
        s.integer(2, duration);
        s.integer(3, enabled);
        s.step();
    }
    transaction.commit();
    return true;
}
void Storage::removeReminder(int64_t rule) {
    Statement s(db_, "DELETE FROM reminder_rules WHERE id=?1 AND kind=1");
    s.integer(1, rule);
    s.step();
}
void Storage::mergeReminders(const std::string &from, const std::string &to) {
    int64_t source{}, target{};
    for (const auto &r : reminderRules()) {
        if (r.app == from)
            source = r.id;
        if (r.app == to)
            target = r.id;
    }
    if (!source && !target)
        return;
    auto resultId = target ? target : source;
    struct State {
        Millis next{};
        bool notified{}, muted{};
    };
    std::map<std::string, State> combined;
    Statement old(
        db_, "SELECT rule,day,next,notified,muted FROM reminder_states WHERE rule=?1 OR rule=?2");
    old.integer(1, source);
    old.integer(2, target);
    while (old.step()) {
        auto day = old.text(1);
        auto &n = combined[day];
        if (old.integer(2) > 0)
            n.next = std::max(n.next, old.integer(2) +
                                          usedOnDay(old.integer(0) == source ? to : from, day));
        n.notified = n.notified || old.integer(3) != 0;
        n.muted = n.muted || old.integer(4) != 0;
    }
    Statement clear(db_, "DELETE FROM reminder_states WHERE rule=?1 OR rule=?2");
    clear.integer(1, source);
    clear.integer(2, target);
    clear.step();
    if (source && target) {
        Statement remove(db_, "DELETE FROM reminder_rules WHERE id=?1");
        remove.integer(1, source);
        remove.step();
    } else if (source) {
        Statement move(db_, "UPDATE reminder_rules SET app=?1 WHERE id=?2");
        move.text(1, to);
        move.integer(2, source);
        move.step();
    }
    std::vector<ReminderState> states;
    for (const auto &[day, n] : combined)
        states.push_back({resultId, day, n.next, n.notified && !n.next, n.muted, {}});
    writeStates(states);
}
} // namespace tt
