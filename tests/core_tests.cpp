#include "storage.h"
#include "reminders.h"
#include "notification_protocol.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace tt;
namespace {
int checks{};
void require(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
Millis sum(const std::vector<Usage> &entries) {
    Millis n{};
    for (auto &u : entries)
        n += u.duration;
    return n;
}
void rawSql(const std::filesystem::path &path, const std::string &sql) {
    sqlite3 *db{};
    if (sqlite3_open(utf8(path.wstring()).c_str(), &db) != SQLITE_OK)
        throw std::runtime_error("open fixture");
    char *err{};
    auto result = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    std::string message = err ? err : "";
    sqlite3_free(err);
    sqlite3_close(db);
    if (result != SQLITE_OK)
        throw std::runtime_error(message);
}
Millis scalar(const std::filesystem::path &path, const char *sql) {
    sqlite3 *db{};
    sqlite3_open_v2(utf8(path.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *s{};
    sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    if (sqlite3_step(s) != SQLITE_ROW)
        throw std::runtime_error("fixture query");
    auto result = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(db);
    return result;
}
ReminderView view(const ReminderEngine &e, int64_t id) {
    for (auto &v : e.views())
        if (v.rule.id == id)
            return v;
    throw std::runtime_error("missing rule");
}
ReminderAction action(const ReminderEvent &e, ReminderAction::Kind kind = ReminderAction::Snooze) {
    return {kind, e.rule.id, e.token};
}
void fixture(const std::filesystem::path &dir) {
    Storage db(dir / L"data.db");
    auto now = utcNow(), hi = now, lo = now - 86400000;
    auto day = localDay(now);
    while (hi - lo > 1) {
        auto mid = (lo + hi) / 2;
        if (localDay(mid) == day)
            hi = mid;
        else
            lo = mid;
    }
    auto base = hi + 18 * 3600000;
    for (const auto &a :
         std::vector<AppInfo>{{"chrome", L"Google Chrome", L"C:\\Apps\\Chrome\\chrome.exe"},
                              {"code", L"Visual Studio Code", L"C:\\Apps\\Code.exe"},
                              {"wechat", L"微信", L"C:\\Apps\\WeChat.exe"},
                              {"explorer", L"文件资源管理器", L"C:\\Windows\\explorer.exe"}})
        db.app(a);
    db.app({"chrome-helper", L"Chrome Helper", L"C:\\Apps\\Chrome\\chrome-helper.exe"});
    db.app({"chrome-old", L"Chrome Old", L"C:\\Apps\\Chrome\\chrome-old.exe"});
    db.save({{"chrome", base - 5 * 3600000, 4860000},
             {"code", base - 3 * 3600000, 3000000},
             {"wechat", base - 2 * 3600000, 1560000},
             {"explorer", base - 1200000, 660000},
             {"chrome-helper", base - 100000, 5000},
             {"chrome-old", base - 90000, 7000}});
    db.setReminder("chrome", 7200000);
    db.setReminder("wechat", 1800000);
}
void timelineTests() {
    std::vector<Usage> entries;
    int resets{};
    Timeline t([&](const auto &u) { entries.push_back(u); }, [&] { ++resets; });
    const Millis utc = 1700000000000;
    t.observe({1000, utc, 1000, "a", true});
    t.observe({1500, utc + 500, 1400, "b", true});
    t.observe({2000, utc + 1000, 1800, "a", true});
    require(entries.size() == 2 && entries[0].app == "a" && entries[1].app == "b",
            "Foreground attribution");
    require(sum(entries) == 1000 && !resets, "Switch does not reset continuous device use");
    t.observe({3000, utc + 2000, 2800, "a", false});
    t.observe({4000, utc + 3000, 3800, "b", false});
    t.observe({5000, utc + 4000, 4800, "b", true});
    t.observe({6000, utc + 5000, 5800, "b", true});
    require(sum(entries) == 3000 && resets == 2, "Pause lock sleep suspend and reset");
    entries.clear();
    t.reset();
    for (int i = 0; i <= 100; ++i)
        t.observe({i * 500, utc + i * 500, i * 500, i % 2 ? "b" : "a", true});
    require(entries.size() == 100 && sum(entries) == 50000, "100 rapid foreground intervals exact");
    entries.clear();
    t.reset();
    for (int i = 0; i <= 1800; ++i)
        t.observe({i * 1000, utc + i * 1000, i * 1000, "a", true});
    require(sum(entries) == 1800000, "Synthetic thirty minute duration exact");
    entries.clear();
    t.reset();
    resets = 0;
    for (int i = 0; i <= 299; ++i)
        t.observe({i * 1000, utc + i * 1000, 0, "a", true});
    require(sum(entries) == 299000 && !resets, "Idle below five minutes continues");
    t.observe({300000, utc + 300000, 0, "a", true});
    require(sum(entries) == 300000 && resets == 1,
            "Idle resets at five minute boundary including initial grace");
    t.observe({301000, utc + 301000, 300500, "a", true});
    require(sum(entries) == 300500, "Resume counts only after new input");
    entries.clear();
    t.reset();
    resets = 0;
    t.observe({0, utc, 0, "", true});
    t.observe({1000, utc + 1000, 1000, "a", true});
    t.observe({2000, utc + 2000, 2000, "", true});
    require(entries.size() == 2 && entries[0].app.empty() && sum(entries) == 2000 && !resets,
            "Desktop contributes to device use with unchanged app attribution");
    t.observe({1900, utc + 1900, 1900, "b", true});
    t.observe({3000, utc + 3000, 3000, "", true});
    require(entries.back().app.empty() && sum(entries) == 3000,
            "Late event does not replace current foreground");
    entries.clear();
    t.reset();
    resets = 0;
    t.observe({0, utc, 0, "a", true});
    t.observe({3600000, utc + 3600000, 3600000, "a", true});
    require(entries.empty() && resets > 0, "Unexplained suspend gap not counted");
    t.observe({3601000, utc - 86400000, 3601000, "b", true});
    t.observe({3602000, utc + 86400000, 3602000, "b", true});
    require(sum(entries) == 2000, "System time changes do not inflate monotonic duration");
    require(shiftDay("2024-02-28", 1) == "2024-02-29", "Leap date");
    require(shiftDay("2026-12-31", 1) == "2027-01-01", "Year boundary");
    require(weekStart("2026-10-18") == "2026-10-12", "Monday week boundary");
}
void engineTests() {
    const std::string day = "2026-10-17", next = "2026-10-18";
    std::vector<ReminderRule> rules = {{1, ReminderKind::Device, "", 60000, true},
                                       {2, ReminderKind::App, "a", 60000, true},
                                       {3, ReminderKind::App, "b", 120000, true}};
    ReminderEngine e("test");
    e.load(rules, {}, {}, day, true);
    e.add("a", 30000);
    e.add("b", 29999);
    require(!e.due(true), "Due check is false below the boundary");
    require(e.evaluate(true).empty(), "Below thresholds");
    e.add("", 1);
    require(e.due(true) && !e.due(false), "Due check follows availability and exact boundary");
    auto events = e.evaluate(true);
    require(events.size() == 1 && events[0].rule.kind == ReminderKind::Device,
            "Cross app and desktop reach device threshold");
    require(e.evaluate(true).empty(), "No duplicate on query or save");
    require(!e.due(true), "Due check does not repeat a consumed occurrence");
    require(e.apply(action(events[0])), "Device snooze accepted");
    require(!e.apply(action(events[0])), "Duplicate action ignored");
    e.add("b", 299999);
    require(e.evaluate(false).empty(), "Unavailable state does not emit");
    auto a = e.evaluate(true); // b now reaches its own threshold, device still one millisecond away
    require(a.size() == 1 && a[0].rule.id == 3, "Other reminder unaffected by device snooze");
    e.add("", 1);
    auto snoozed = e.evaluate(true);
    require(snoozed.size() == 1 && snoozed[0].rule.id == 1, "Device snooze counts actual use");
    require(view(e, 1).state.next == e.views()[0].used + 60000,
            "Device normal interval resumes after snooze");
    e.add("a", 30000);
    auto app = e.evaluate(true);
    require(app.size() == 1 && app[0].rule.id == 2, "App daily threshold cumulative");
    require(e.apply(action(app[0])), "App snooze accepted");
    e.add("b", 600000);
    auto other = e.evaluate(true);
    require(std::none_of(other.begin(), other.end(), [](auto &v) { return v.rule.id == 2; }),
            "Other apps do not advance app snooze");
    e.add("a", 299999);
    auto more = e.evaluate(true);
    require(std::none_of(more.begin(), more.end(), [](auto &v) { return v.rule.id == 2; }),
            "App snooze five minutes minus one");
    e.add("a", 1);
    more = e.evaluate(true);
    auto found = std::find_if(more.begin(), more.end(), [](auto &v) { return v.rule.id == 2; });
    require(found != more.end(), "App snooze fires at exact actual use boundary");
    require(!e.apply(action(app[0], ReminderAction::MuteToday)), "Old occurrence token ignored");
    require(e.apply(action(*found, ReminderAction::MuteToday)), "Mute current occurrence");
    e.add("a", 600000);
    more = e.evaluate(true);
    require(std::none_of(more.begin(), more.end(), [](auto &v) { return v.rule.id == 2; }),
            "Mute suppresses only own rule");
    auto states = e.states();
    auto usedDevice = view(e, 1).used;
    ReminderEngine restart("restart");
    restart.load(rules, states, {{"a", 960000}, {"b", 1000000}}, day, true);
    require(view(restart, 1).used == 0 && view(restart, 1).state.token.empty(),
            "Restart starts fresh device round");
    require(view(restart, 2).state.muted && restart.evaluate(true).empty(),
            "Mute and app daily state persist on restart");
    e.changeDay({}, {}, next);
    require(view(e, 1).used == usedDevice && view(e, 2).used == 0 && !view(e, 2).state.muted,
            "Midnight resets apps and mute but keeps device round");
    require(!e.apply(action(*found)), "Previous day action ignored");
    e.add("a", 60000);
    more = e.evaluate(true);
    require(std::any_of(more.begin(), more.end(), [](auto &v) { return v.rule.id == 2; }),
            "Next day app notifies again");
    e.resetDevice();
    require(view(e, 1).used == 0 && view(e, 1).state.token.empty(),
            "Interrupt clears device occurrence");
    // A muted device must not immediately fire at midnight due to elapsed muted intervals.
    ReminderEngine d("d");
    d.load({rules[0]}, {}, {}, day, true);
    d.add("", 60000);
    auto dev = d.evaluate(true).front();
    require(d.apply(action(dev, ReminderAction::MuteToday)), "Device mute accepted");
    d.add("", 1000000);
    require(d.evaluate(true).empty(), "Muted device quiet");
    auto elapsed = view(d, 1).used;
    d.changeDay({}, {}, next);
    require(d.evaluate(true).empty() && view(d, 1).used == elapsed,
            "Muted periodic reminder avoids midnight backlog");
    d.add("", 60000);
    require(d.evaluate(true).size() == 1, "Device mute expires next day");
    auto saved = d.states();
    d.load({rules[0]}, saved, {}, next);
    require(view(d, 1).used == elapsed + 60000, "Unrelated reload preserves device session");
    auto changed = rules[0];
    changed.threshold = 120000;
    d.load({changed}, {}, {}, next);
    require(view(d, 1).used == 0, "Changed device config resets round");
    d.load({}, {}, {}, next);
    require(!d.apply(action(dev)), "Deleted rule action ignored");
    auto disabled = rules[0];
    disabled.enabled = false;
    d.load({disabled}, {}, {}, day, true);
    d.add("a", 1000000);
    require(d.evaluate(true).empty() && view(d, 1).used == 0,
            "Disabled device does not accumulate");
    // Shared chain integration: no activity on an idle, locked or paused period.
    ReminderEngine chain("chain");
    chain.load({rules[0]}, {}, {}, day, true);
    Timeline t([&](auto &u) { chain.add(u.app, u.duration); }, [&] { chain.resetDevice(); });
    for (int i = 0; i <= 60; ++i)
        t.observe({i * 1000, 100000 + i * 1000, i * 1000, i % 2 ? "a" : "", true});
    require(chain.evaluate(true).size() == 1, "Shared chain cross app and desktop reminder");
    t.observe({61000, 161000, 61000, "a", false});
    require(view(chain, 1).used == 0, "Lock resets shared chain immediately");
    t.observe({62000, 162000, 62000, "a", false});
    t.observe({63000, 163000, 63000, "a", true});
    t.observe({64000, 164000, 64000, "a", true});
    require(view(chain, 1).used == 1000, "Resume starts new round without backfill");
}
void boundaryTests(const std::filesystem::path &dir) {
    auto now = utcNow(), hi = now, lo = now - 86400000;
    const auto day = localDay(now), yesterday = shiftDay(day, -1);
    while (hi - lo > 1) {
        auto mid = (lo + hi) / 2;
        if (localDay(mid) == day)
            hi = mid;
        else
            lo = mid;
    }
    auto parts = splitLocal({"a", hi - 500, 1000});
    require(parts.size() == 2 && parts[0].day == yesterday && parts[1].day == day &&
                parts[0].duration == 500 && parts[1].duration == 500,
            "Exact local midnight split");
    Storage db(dir / L"midnight.db");
    db.app({"a", L"a", {}});
    db.save({{"a", hi - 500, 1000}});
    require(db.snapshot(day, false).total == 500 && db.snapshot(yesterday, false).total == 500,
            "Midnight storage attribution");
    std::vector<ReminderRule> rules = {{1, ReminderKind::Device, "", 1000, true},
                                       {2, ReminderKind::App, "a", 500, true}};
    ReminderEngine e("boundary");
    e.load(rules, {}, {{"a", 500}}, yesterday, true);
    e.add("", 500);
    auto event = e.evaluate(true).front();
    require(event.rule.id == 2, "Previous day app reminder");
    auto old = e.states();
    e.changeDay({}, {{"a", 500}}, day);
    e.add("", 500);
    auto due = e.evaluate(true);
    require(due.size() == 2, "Continuous device crosses midnight and app uses new day total");
    require(!e.apply(action(event)), "Midnight occurrence expired");
    auto todayStates = e.states();
    e.changeDay(old, {{"a", 500}}, yesterday);
    require(e.evaluate(true).empty() && view(e, 1).used == 1000,
            "Clock backward restores daily dedup without resetting device");
    e.changeDay(todayStates, {{"a", 500}}, day);
    require(e.evaluate(true).empty(), "Clock forward restores already sent daily reminder");
    auto device = view(e, 1).used;
    e.load(rules, {}, {{"a", 500}}, shiftDay(day, 1));
    require(view(e, 1).used == device && view(e, 1).state.day == shiftDay(day, 1) &&
                view(e, 1).state.token.empty(),
            "Midnight config reload keeps device duration with fresh daily state");
    auto state = e.states();
    e.load(rules, state, {{"a", 500}}, shiftDay(day, 1));
    require(view(e, 1).used == device, "Same config reload preserves round");
    std::vector<Usage> entries;
    int resets{};
    Timeline t([&](const Usage &u) { entries.push_back(u); }, [&] { ++resets; });
    t.observe({299500, now, 0, "a", true});
    t.observe({300500, now + 1000, 300250, "a", true});
    require(resets == 1 && sum(entries) == 750,
            "Idle boundary between heartbeats excludes idle gap and resets before resume");
    t.observe({306500, now + 7000, 306500, "a", true});
    require(resets == 2 && sum(entries) == 750,
            "Missed short suspend event resets round without filling time");
}
void protocolTests() {
    require(parseReminderUri(L"timetrace://reminders").has_value(), "New body URI");
    require(parseReminderUri(L"TIMETRACE://limits/").has_value(),
            "Legacy body URI case insensitive");
    ReminderAction a{ReminderAction::Snooze, 123, "abc-DEF-123"};
    auto p = parseReminderUri(reminderActionUri(a));
    require(p && p->action && p->action->token == a.token && p->action->rule == 123,
            "Snooze URI roundtrip");
    a.kind = ReminderAction::MuteToday;
    p = parseReminderUri(reminderActionUri(a));
    require(p && p->action->kind == ReminderAction::MuteToday, "Mute URI roundtrip");
    for (const auto *uri :
         {L"timetrace://reminders?", L"timetrace://reminders?action=snooze&rule=1&token=a&",
          L"timetrace://limits?action=mute&rule=1&token=a",
          L"timetrace://reminders?action=mute&rule=0&token=a",
          L"timetrace://reminders?action=mute&rule=1x&token=a",
          L"timetrace://reminders?action=mute&rule=9223372036854775808&token=a",
          L"timetrace://reminders?action=mute&rule=-1&token=a",
          L"timetrace://reminders?action=bad&rule=1&token=a",
          L"timetrace://reminders?action=mute&rule=1&token=a&rule=2",
          L"timetrace://reminders?action=mute&rule=1&token=a&x=1",
          L"timetrace://reminders?action=mute&rule=1&token=%20",
          L"timetrace://reminders?action=mute&rule=1&token=", L"timetrace://reminders#x",
          L"timetrace://other", L"timetrace://reminders?action=mute&rule=1&token=中文"})
        require(!parseReminderUri(uri), "Malformed URI rejected");
    require(!parseReminderUri(L"timetrace://reminders?action=mute&rule=1&token=" +
                              std::wstring(81, L'a')),
            "Long token rejected");
    auto w = reminderWire(a);
    require(parseReminderWire(w).has_value(), "IPC wire roundtrip");
    w.version = 2;
    require(!parseReminderWire(w), "Wrong wire version rejected");
    w.version = 1;
    w.kind = 2;
    require(!parseReminderWire(w), "Wrong wire action rejected");
    w.kind = 0;
    std::fill(std::begin(w.token), std::end(w.token), 'a');
    require(!parseReminderWire(w), "Unterminated IPC token rejected");
    ReminderEvent e{{2, ReminderKind::App, "a", 60000, true}, "xml-1", 60000, L"A & <B>\""};
    auto xml = reminderToastXml(e);
    require(xml.find(L"A &amp; &lt;B&gt;&quot;") != xml.npos, "Toast escapes application name");
    require(xml.find(L"action=snooze&amp;rule=2&amp;token=xml-1") != xml.npos &&
                xml.find(L"action=mute&amp;") != xml.npos,
            "Toast direct actions encoded");
    require(xml.find(L"launch=\"timetrace://reminders\"") != xml.npos,
            "Toast body opens reminder page");
}
void storageTests(const std::filesystem::path &dir) {
    auto file = dir / L"storage.db";
    auto now = utcNow();
    auto today = localDay(now);
    // Keep synthetic records away from local midnight.
    while (localDay(now + 1000000) != today)
        now -= 1000000;
    Storage db(file);
    db.app({"a", L"应用甲", L"a.exe"});
    db.app({"b", L"应用乙", L"b.exe"});
    db.app({"c", L"应用丙", L"c.exe"});
    auto rules = db.reminderRules();
    require(rules.size() == 1 && !rules[0].enabled && rules[0].threshold == 3600000,
            "Device defaults disabled sixty minutes");
    db.save({{"a", now, 1000}, {"a", now + 1000, 2000}, {"b", now + 3000, 500}});
    auto s = db.snapshot(today, false);
    require(s.total == 3500 && s.apps.size() == 2, "SQLite aggregate");
    require(std::accumulate(s.buckets.begin(), s.buckets.end(), Millis{}) == s.total,
            "Chart equals totals");
    require(db.snapshot(today, false, "a").selectedTotal == 3000, "Selected app totals");
    require(db.setReminder("a", 2000), "New app configuration");
    ReminderEngine e("storage");
    auto load = [&] {
        e.load(db.reminderRules(), db.reminderStates(today), db.dailyUsage(today), today);
    };
    load();
    auto event = e.evaluate(true).front();
    db.commit({}, e.states());
    e.saved();
    require(!db.setReminder("a", 2000), "Saving identical configuration is no-op");
    load();
    require(e.evaluate(true).empty(), "Same config does not repeat");
    require(e.apply(action(event)), "Storage snooze");
    db.commit({}, e.states());
    ReminderEngine restarted("new");
    restarted.load(db.reminderRules(), db.reminderStates(today), db.dailyUsage(today), today, true);
    require(restarted.evaluate(true).empty() && view(restarted, event.rule.id).state.next == 303000,
            "App snooze survives restart");
    require(db.setReminder("a", 1000), "Changed app configuration");
    load();
    require(e.evaluate(true).size() == 1, "Changed config re-evaluates today");
    db.commit({}, e.states());
    auto before = db.snapshot(today, false).total;
    bool rejected = false;
    try {
        db.commit({{"a", now + 4000, 1234}}, {{99999, today, 0, true, false, "invalid"}});
    } catch (...) {
        rejected = true;
    }
    require(rejected && db.snapshot(today, false).total == before,
            "Usage rolls back if reminder state cannot persist atomically");
    require(db.reminderStates(today).size() == 2, "Failed transaction preserves prior state only");
    auto v = e.views();
    auto a = std::find_if(v.begin(), v.end(), [](auto &r) { return r.rule.app == "a"; })->rule.id;
    db.removeReminder(a);
    require(db.reminderStates(today).size() == 1, "Deleting rule cascades state");
    db.setReminder("a", 2000);
    db.setSetting("paused", "1");
    db.renameApp("a", L" 自定义名称 ");
    db.app({"a", L"refresh", L"a.exe"});
    auto catalog = db.applications();
    require(std::any_of(catalog.begin(), catalog.end(),
                        [](auto &a) { return a.name == L"自定义名称"; }),
            "Custom label survives metadata refresh");
    db.mergeApps("a", "b");
    s = db.snapshot(today, false, "a");
    require(s.total == 3500 && s.selectedTotal == 3500 && s.apps.size() == 1 &&
                db.reminderRules().back().threshold == 2000,
            "Merge inherits source rule without changing raw usage");
    require(scalar(file, "SELECT COUNT(*) FROM segments WHERE app='a'") == 1,
            "Original interval identity preserved");
    db.clear();
    s = db.snapshot(today, false);
    require(s.total == 0 && s.apps.empty() && db.reminderRules().back().threshold == 2000,
            "Clear retains reminder configuration");
    require(db.reminderStates(today).empty() && db.getSetting("paused") == "1" &&
                db.appMappings()["a"] == "b",
            "Clear resets states and retains groups and settings");
    rejected = false;
    try {
        db.setReminder("b", 0);
    } catch (...) {
        rejected = true;
    }
    require(rejected, "Reject invalid duration");
    rejected = false;
    try {
        db.mergeApps("b", "missing");
    } catch (...) {
        rejected = true;
    }
    require(rejected, "Reject missing merge target");
    rejected = false;
    try {
        db.renameApp("b", L" \t ");
    } catch (...) {
        rejected = true;
    }
    require(rejected, "Reject empty name");
}
void mergeTests(const std::filesystem::path &dir) {
    auto file = dir / L"merge.db";
    Storage db(file);
    auto now = utcNow();
    auto day = localDay(now);
    for (auto id : {"a", "b", "c", "d"})
        db.app({id, wide(id), {}});
    db.save({{"a", now - 100000, 1000}, {"b", now - 99000, 2000}, {"c", now - 97000, 3000}});
    day = localDay(now - 100000);
    db.setReminder("a", 1000);
    db.setReminder("b", 5000);
    auto rules = db.reminderRules();
    auto ruleId = [&](const std::string &app) {
        for (auto &r : db.reminderRules())
            if (r.app == app)
                return r.id;
        return int64_t{};
    };
    auto source = ruleId("a"), target = ruleId("b");
    db.commit({}, {{source, day, 301000, false, false, "pending"},
                   {target, day, 0, false, true, "target"}});
    db.mergeApps("a", "b");
    auto states = db.reminderStates(day);
    require(ruleId("a") == 0 && ruleId("b") == target, "Target rule wins merge");
    require(states.size() == 1 && states[0].muted && states[0].next == 303000 &&
                states[0].token.empty(),
            "Merge inherits silence and remaining snooze, invalidates old token");
    db.mergeApps("b", "c");
    states = db.reminderStates(day);
    require(ruleId("c") == target && db.reminderRules().back().threshold == 5000,
            "Unconfigured target inherits rule");
    require(states[0].next == 306000 && states[0].muted,
            "Second merge preserves remaining actual use");
    require(db.snapshot(day, false, "a").selectedTotal == 6000 && db.appMappings()["a"] == "c",
            "Merged groups stay flat");
    db.mergeApps("c", "a");
    require(db.appMappings()["a"] == "c", "Same group merge no cycle");
}
void batchMergeTests(const std::filesystem::path &dir) {
    auto file = dir / L"batch-merge.db";
    Storage db(file);
    constexpr Millis base = 1700000000000;
    auto day = localDay(base);
    for (auto id : {"target", "a", "alias", "b", "c", "x", "y"})
        db.app({id, wide(id), {}});
    db.save({{"target", base, 1000},
             {"a", base + 1000, 2000},
             {"b", base + 3000, 3000},
             {"c", base + 6000, 4000},
             {"alias", base + 10000, 5000}});
    db.renameApp("target", L"Current application");
    db.mergeApps("alias", "a");
    db.setReminder("a", 1000);
    db.setReminder("target", 5000);
    db.setReminder("c", 9000);
    auto ruleId = [&](const std::string &app) {
        for (const auto &rule : db.reminderRules())
            if (rule.app == app)
                return rule.id;
        return int64_t{};
    };
    auto targetRule = ruleId("target");
    db.commit({}, {{ruleId("a"), day, 307000, false, false, "source-token"},
                   {targetRule, day, 0, false, true, "target-token"},
                   {ruleId("c"), day, 104000, false, false, "other-token"}});
    db.mergeApps(std::vector<std::string>{"a", "alias", "b", "c", "a", "target"}, "target");
    auto snapshot = db.snapshot(day, false, "target");
    require(snapshot.total == 15000 && snapshot.selectedTotal == 15000 &&
                snapshot.apps.size() == 1 && snapshot.selected.app.name == L"Current application",
            "Batch merge deduplicates aliases and keeps the current application name and total");
    auto mapping = db.appMappings();
    require(mapping["a"] == "target" && mapping["alias"] == "target" && mapping["b"] == "target" &&
                mapping["c"] == "target" &&
                scalar(file, "SELECT COUNT(*) FROM app_links WHERE manual<>1") == 0,
            "Batch merge flattens every selected group and records manual ownership");
    auto states = db.reminderStates(day);
    require(ruleId("target") == targetRule && db.reminderRules().back().threshold == 5000 &&
                states.size() == 1 && states[0].rule == targetRule && states[0].muted &&
                states[0].next == 315000 && states[0].token.empty(),
            "Batch merge preserves the target rule, silence, and longest remaining snooze");
    require(scalar(file, "SELECT COUNT(*)=5 AND SUM(duration)=15000 AND "
                         "SUM(CASE WHEN app='target' THEN duration ELSE 0 END)=1000 AND "
                         "SUM(CASE WHEN app='a' THEN duration ELSE 0 END)=2000 AND "
                         "SUM(CASE WHEN app='b' THEN duration ELSE 0 END)=3000 AND "
                         "SUM(CASE WHEN app='c' THEN duration ELSE 0 END)=4000 AND "
                         "SUM(CASE WHEN app='alias' THEN duration ELSE 0 END)=5000 "
                         "FROM segments") == 1,
            "Batch grouping preserves every original interval and source identity");
    db.mergeApps(std::vector<std::string>{"a", "alias", "b", "target"}, "a");
    db.mergeApps(std::vector<std::string>{}, "target");
    states = db.reminderStates(day);
    require(db.appMappings() == mapping && states.size() == 1 && states[0].next == 315000 &&
                states[0].muted && states[0].token.empty(),
            "Retrying a completed batch or an empty selection leaves state unchanged");
    bool rejected = false;
    try {
        db.mergeApps(std::vector<std::string>{"x", "missing", "y"}, "target");
    } catch (...) {
        rejected = true;
    }
    require(rejected && db.appMappings() == mapping,
            "A missing batch member rejects the entire selection without partial grouping");
    rejected = false;
    try {
        db.mergeApps(std::vector<std::string>{"x", "y"}, "missing");
    } catch (...) {
        rejected = true;
    }
    require(rejected && db.appMappings() == mapping,
            "A missing batch target leaves every source intact");
    db.setReminder("x", 8000);
    auto xRule = ruleId("x");
    db.commit({}, {{xRule, day, 300000, false, false, "x-token"}});
    auto catalogSize = db.applications().size();
    rawSql(file, "CREATE TRIGGER fail_batch BEFORE INSERT ON app_links "
                 "WHEN NEW.app='y' BEGIN SELECT RAISE(ABORT,'injected batch failure'); END;");
    rejected = false;
    try {
        db.mergeApps(std::vector<std::string>{"x", "y"}, "target");
    } catch (...) {
        rejected = true;
    }
    require(rejected && db.appMappings() == mapping && db.applications().size() == catalogSize &&
                ruleId("x") == xRule,
            "A later database failure rolls back earlier mappings, reminders, and catalog changes");
    states = db.reminderStates(day);
    auto xState = std::find_if(states.begin(), states.end(),
                               [&](const auto &state) { return state.rule == xRule; });
    require(xState != states.end() && xState->token == "x-token" && xState->next == 300000 &&
                !xState->muted && db.snapshot(day, false, "target").selectedTotal == 15000,
            "Failed batch restores source occurrence tokens and the target's original usage");
    rawSql(file, "DROP TRIGGER fail_batch;");
    db.mergeApps(std::vector<std::string>{"x", "y"}, "target");
    require(db.appMappings()["x"] == "target" && db.appMappings()["y"] == "target" &&
                ruleId("x") == 0 && ruleId("target") == targetRule,
            "Retry after a rolled-back batch merges every requested source exactly once");
}
void groupManagementTests(const std::filesystem::path &dir) {
    auto file = dir / L"group-management.db";
    constexpr Millis base = 1700000000000;
    auto day = localDay(base), previous = localDay(base - 86400000);
    int64_t reminder{};
    {
        Storage db(file);
        for (auto id : {"target", "a", "b", "outside"})
            db.app({id, wide(id), wide(id) + L".exe"});
        db.save({{"target", base, 1000},
                 {"a", base + 1000, 2000},
                 {"b", base + 3000, 3000},
                 {"outside", base + 6000, 4000},
                 {"a", base - 86400000, 5000}});
        db.renameApp("target", L"Current group");
        db.renameApp("b", L"Named helper");
        db.mergeApps(std::vector<std::string>{"a", "b"}, "target");
        db.setReminder("target", 60000);
        reminder = db.reminderRules().back().id;
        db.commit({}, {{reminder, day, 306000, false, true, "before-detach"},
                       {reminder, previous, 500, true, false, "previous-day"}});
        auto snapshot = db.snapshot(day, false, "target");
        require(snapshot.members.size() == 3 && snapshot.members.front().app.id == "target" &&
                    std::any_of(snapshot.members.begin(), snapshot.members.end(),
                                [](const auto &member) {
                                    return member.app.id == "b" &&
                                           member.app.name == L"Named helper";
                                }) &&
                    std::accumulate(snapshot.members.begin(), snapshot.members.end(), Millis{},
                                    [](Millis total, const auto &member) {
                                        return total + member.used;
                                    }) == snapshot.selectedTotal,
                "Details list each original member, labels, and selected-period usage with primary "
                "first");
        db.unmergeApp("target", "a");
        require(
            db.appMappings()["a"] == "a" &&
                scalar(file, "SELECT separate_from='target' FROM apps WHERE id='a'") == 1,
            "Removing a member restores its independent identity and remembers the former group");
        auto state = db.reminderStates(day).front();
        require(db.snapshot(day, false, "target").selectedTotal == 4000 &&
                    db.snapshot(day, false, "a").selectedTotal == 2000 &&
                    db.snapshot(day, false).total == 10000 && state.rule == reminder &&
                    state.next == 304000 && state.muted && !state.notified && state.token.empty(),
                "Removing a member preserves total time, group reminder, silence, and remaining "
                "snooze");
        auto oldState = db.reminderStates(previous).front();
        require(oldState.next == 1 && oldState.notified && !oldState.muted &&
                    oldState.token.empty(),
                "Removing historical usage adjusts each day without turning a positive threshold "
                "into zero");
        db.unmergeApp("target", "a");
        require(db.reminderStates(day).front().next == 304000,
                "Retrying member removal does not subtract usage twice");
        bool rejected = false;
        try {
            db.unmergeApp("target", "target");
        } catch (...) {
            rejected = true;
        }
        require(rejected && db.appMappings()["target"] == "target",
                "The current primary cannot be removed from its own group");
        rejected = false;
        try {
            db.unmergeApp("outside", "a");
        } catch (...) {
            rejected = true;
        }
        require(rejected && db.appMappings()["a"] == "a",
                "An independent member cannot be removed again through an unrelated group");
        require(db.registerApp({"a", L"Refreshed metadata", L"a.exe"},
                               {"target", L"Automatic group", L"target.exe"}) == "a",
                "Automatic identity refresh respects an explicit separation");
        db.commit({}, {{reminder, day, 304000, false, true, "primary-token"}});
        require(db.setPrimary("target", "b") == "b",
                "Selecting an existing member changes the group's primary identity");
        snapshot = db.snapshot(day, false, "target");
        state = db.reminderStates(day).front();
        require(snapshot.selected.app.id == "b" && snapshot.selected.app.name == L"Current group" &&
                    snapshot.selected.app.path == L"b.exe" && snapshot.selectedTotal == 4000 &&
                    snapshot.members.size() == 2 && snapshot.members.front().app.id == "b" &&
                    db.appMappings()["target"] == "b",
                "Primary selection preserves the displayed group name, member usage, and flat "
                "mappings");
        require(
            db.reminderRules().back().id == reminder && db.reminderRules().back().app == "b" &&
                state.rule == reminder && state.next == 304000 && state.muted &&
                state.token == "primary-token",
            "Primary selection moves the same reminder rule without changing its occurrence state");
        db.setPrimary("target", "b");
        db.unmergeApp("target", "a");
        require(db.reminderStates(day).front().token == "primary-token" &&
                    db.reminderStates(day).front().next == 304000,
                "Primary selection and removal retries remain idempotent after the root changes");
        auto mapping = db.appMappings();
        rejected = false;
        try {
            db.setPrimary("b", "outside");
        } catch (...) {
            rejected = true;
        }
        require(rejected && db.appMappings() == mapping,
                "Primary selection rejects applications outside the current group");
        rawSql(file,
               "CREATE TRIGGER fail_primary BEFORE UPDATE ON reminder_rules "
               "WHEN NEW.app='target' BEGIN SELECT RAISE(ABORT,'injected primary failure'); END;");
        rejected = false;
        try {
            db.setPrimary("b", "target");
        } catch (...) {
            rejected = true;
        }
        require(
            rejected && db.appMappings() == mapping && db.reminderRules().back().app == "b" &&
                db.reminderStates(day).front().token == "primary-token" &&
                db.snapshot(day, false, "b").selected.app.name == L"Current group",
            "Failed primary selection rolls back labels, mappings, and reminder identity together");
        rawSql(file, "DROP TRIGGER fail_primary;");
        db.unmergeApp("b", "target");
        require(db.snapshot(day, false, "b").selectedTotal == 3000 &&
                    db.snapshot(day, false, "target").selectedTotal == 1000 &&
                    db.reminderStates(day).front().next == 303000,
                "After choosing a new primary the former primary can be separated independently");
        db.mergeApps("a", "b");
        require(scalar(file, "SELECT separate_from='' FROM apps WHERE id='a'") == 1 &&
                    db.snapshot(day, false, "b").selectedTotal == 5000,
                "An explicit new merge clears the member's previous separation choice");
        db.commit({}, {{reminder, day, 305000, false, true, "rollback-token"}});
        rawSql(file, "CREATE TRIGGER fail_unmerge BEFORE DELETE ON app_links "
                     "WHEN OLD.app='a' BEGIN SELECT RAISE(ABORT,'injected removal failure'); END;");
        rejected = false;
        try {
            db.unmergeApp("b", "a");
        } catch (...) {
            rejected = true;
        }
        state = db.reminderStates(day).front();
        require(rejected && db.appMappings()["a"] == "b" && state.next == 305000 &&
                    state.token == "rollback-token" &&
                    scalar(file, "SELECT separate_from='' FROM apps WHERE id='a'") == 1,
                "Failed member removal restores the reminder token, threshold, and original "
                "membership");
        rawSql(file, "DROP TRIGGER fail_unmerge;");
        require(
            scalar(file,
                   "SELECT COUNT(*)=5 AND SUM(duration)=15000 AND "
                   "SUM(CASE WHEN app='target' THEN duration ELSE 0 END)=1000 AND "
                   "SUM(CASE WHEN app='a' THEN duration ELSE 0 END)=7000 AND "
                   "SUM(CASE WHEN app='b' THEN duration ELSE 0 END)=3000 AND "
                   "SUM(CASE WHEN app='outside' THEN duration ELSE 0 END)=4000 FROM segments") == 1,
            "All group-management operations preserve original intervals and source totals");
    }
    {
        Storage db(file);
        require(db.registerApp({"target", L"Refreshed target", L"target.exe"},
                               {"b", L"Automatic grouping", L"b.exe"}) == "target",
                "Independent-member choice survives closing and reopening the database");
        db.unmergeApp("b", "target");
        require(db.reminderStates(day).front().token == "rollback-token",
                "Removal retries after restart do not invalidate a later reminder occurrence");
        db.clear();
        auto snapshot = db.snapshot(day, false, "b");
        require(snapshot.members.size() == 2 && snapshot.members[0].app.id == "b" &&
                    snapshot.members[1].app.id == "a" && snapshot.members[0].used == 0 &&
                    snapshot.members[1].used == 0 &&
                    db.registerApp({"target", L"Target", L"target.exe"},
                                   {"b", L"Automatic grouping", L"b.exe"}) == "target",
                "Clearing history retains real members and independent-member choices");
        db.app({"app:combined", L"Combined", {}});
        db.mergeApps("b", "app:combined");
        db.setPrimary("app:combined", "a");
        snapshot = db.snapshot(day, false, "a");
        require(snapshot.members.size() == 2 && snapshot.members[0].app.id == "a" &&
                    snapshot.members[1].app.id == "b",
                "Member details omit synthetic aliases that never directly recorded usage");
    }
    file = dir / L"schema5.db";
    {
        Storage db(file);
        db.app({"source", L"Source", L"source.exe"});
        db.app({"group", L"Group", L"group.exe"});
        db.save({{"source", base, 1234}});
        db.mergeApps("source", "group");
        db.renameApp("group", L"Custom group");
    }
    rawSql(file, "ALTER TABLE apps DROP COLUMN separate_from;PRAGMA user_version=5;");
    {
        Storage db(file);
        require(
            scalar(file, "PRAGMA user_version") == 6 &&
                scalar(file, "SELECT COUNT(*) FROM apps WHERE separate_from<>''") == 0 &&
                db.appMappings()["source"] == "group" &&
                db.snapshot(day, false, "group").selectedTotal == 1234 &&
                db.snapshot(day, false, "group").selected.app.name == L"Custom group",
            "Schema five upgrades to six with existing names, mappings, and raw usage preserved");
    }
}
void migrationTests(const std::filesystem::path &dir) {
    for (int version : {1, 2}) {
        auto file = dir / (L"v" + std::to_wstring(version) + L".db");
        {
            Storage db(file);
            for (auto id : {"a", "b", "c"})
                db.app({id, wide(id), {}});
            db.save({{"a", 1700000000000, 1000}, {"b", 1700000001000, 2000}});
        }
        rawSql(file,
               "DROP TABLE reminder_states;DROP TABLE reminder_rules;"
               "CREATE TABLE limits(app TEXT PRIMARY KEY,duration INTEGER NOT NULL);"
               "CREATE TABLE notices(app TEXT,day TEXT,next INTEGER,muted INTEGER,PRIMARY "
               "KEY(app,day));"
               "INSERT INTO limits "
               "VALUES('a',60000),('b',120000);INSERT INTO notices "
               "VALUES('a','2026-10-17',0,1),('b','2026-10-17',420000,0);PRAGMA user_version=" +
                   std::to_string(version) + ";");
        if (version == 1)
            rawSql(file, "DROP TABLE app_labels;DROP TABLE app_links;");
        else
            rawSql(file, "INSERT INTO app_labels VALUES('b','custom');INSERT INTO app_links "
                         "(app,target) VALUES('c','b');");
        Storage db(file);
        require(scalar(file, "PRAGMA user_version") == 6, "Schema one and two upgrade to six");
        require(
            scalar(file, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('limits','notices')") ==
                0,
            "Legacy tables removed after conversion");
        require(scalar(file, "SELECT COUNT(*) FROM segments") == 2 &&
                    scalar(file, "SELECT SUM(duration) FROM segments") == 3000,
                "Upgrade retains every raw interval and total");
        auto rules = db.reminderRules();
        require(rules.size() == 3 && rules[1].threshold == 60000 && rules[2].threshold == 120000 &&
                    !rules[0].enabled,
                "Legacy durations migrate and device stays off");
        auto states = db.reminderStates("2026-10-17");
        require(states.size() == 2 && states[0].muted && states[1].next == 420000,
                "Legacy daily mute and snooze retained");
        ReminderEngine e("migrate");
        e.load(rules, states, {{"a", 600000}, {"b", 419999}}, "2026-10-17", true);
        require(e.evaluate(true).empty(), "Migration does not repeat reminder");
        e.add("b", 1);
        require(e.evaluate(true).size() == 1, "Migrated snooze reaches exact threshold");
        if (version == 2)
            require(db.appMappings()["c"] == "b" && db.applications()[1].name == L"custom",
                    "Schema two labels and groups preserved");
    }
    auto grouped = dir / L"legacy-grouped.db";
    rawSql(grouped,
           "CREATE TABLE apps(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT NULL);"
           "CREATE TABLE segments(id INTEGER PRIMARY KEY,app TEXT,start INTEGER,end INTEGER,"
           "duration INTEGER,day TEXT,hour INTEGER);"
           "CREATE TABLE app_links(app TEXT PRIMARY KEY,target TEXT NOT NULL);"
           "CREATE TABLE limits(app TEXT PRIMARY KEY,duration INTEGER NOT NULL);"
           "CREATE TABLE notices(app TEXT,day TEXT,next INTEGER,muted INTEGER,"
           "PRIMARY KEY(app,day));"
           "INSERT INTO apps VALUES('alias','Alias',''),('target','Target',''),"
           "('other','Other',''),('other-target','Other Target','');"
           "INSERT INTO app_links VALUES('alias','target'),('other','other-target');"
           "INSERT INTO segments VALUES(1,'alias',0,20000,20000,'2026-10-17',12),"
           "(2,'target',20000,60000,40000,'2026-10-17',12),"
           "(3,'other',60000,90000,30000,'2026-10-17',12),"
           "(4,'other-target',90000,100000,10000,'2026-10-17',12),"
           "(5,'alias',100000,110000,10000,'2026-10-19',12),"
           "(6,'target',110000,140000,30000,'2026-10-19',12);"
           "INSERT INTO limits VALUES('alias',120000),('target',60000),('other',90000);"
           "INSERT INTO notices VALUES('alias','2026-10-17',320000,0),"
           "('target','2026-10-17',200000,0),('other','2026-10-17',330000,0),"
           "('alias','2026-10-18',0,1),('target','2026-10-18',200000,0),"
           "('target','2026-10-19',340000,0);"
           "PRAGMA user_version=2;");
    {
        Storage db(grouped);
        require(
            scalar(grouped, "SELECT duration FROM reminder_rules WHERE app='target'") == 60000 &&
                scalar(grouped, "SELECT duration FROM reminder_rules WHERE app='other-target'") ==
                    90000,
            "Grouped legacy migration prioritizes target settings and inherits source-only rules");
        auto states = db.reminderStates("2026-10-17");
        auto ruleFor = [&](const std::string &app) {
            auto rules = db.reminderRules();
            auto found = std::find_if(rules.begin(), rules.end(),
                                      [&](const auto &rule) { return rule.app == app; });
            return found == rules.end() ? int64_t{} : found->id;
        };
        auto targetRule = ruleFor("target"), otherRule = ruleFor("other-target");
        auto stateFor = [&](int64_t rule) {
            auto found = std::find_if(states.begin(), states.end(),
                                      [&](const auto &state) { return state.rule == rule; });
            return found == states.end() ? ReminderState{} : *found;
        };
        require(stateFor(targetRule).next == 360000 && stateFor(otherRule).next == 340000,
                "Grouped legacy snoozes retain their remaining actual-use time");
        auto nextDay = db.reminderStates("2026-10-18");
        require(nextDay.size() == 1 && nextDay.front().rule == targetRule && nextDay.front().muted,
                "Alias silence is retained when target and source both have legacy notices");
        auto targetOnly = db.reminderStates("2026-10-19");
        require(targetOnly.size() == 1 && targetOnly.front().next == 340000,
                "Canonical legacy snooze thresholds are not extended by existing aliases");
        require(scalar(grouped, "SELECT COUNT(*) FROM segments") == 6 &&
                    scalar(grouped, "SELECT SUM(duration) FROM segments") == 140000,
                "Grouped legacy state migration preserves every original interval");
        ReminderEngine engine("legacy-grouped");
        engine.load(db.reminderRules(), states, db.dailyUsage("2026-10-17"), "2026-10-17", true);
        engine.add("target", 299999);
        require(engine.evaluate(true).empty(),
                "Grouped legacy reminder waits for the preserved snooze remainder");
        engine.add("target", 1);
        auto events = engine.evaluate(true);
        require(events.size() == 1 && events.front().rule.id == targetRule,
                "Grouped legacy snooze fires at its exact combined-use boundary");
    }
    std::filesystem::remove(grouped);
    auto file = dir / L"corrupt.db";
    {
        std::ofstream f(file);
        f << "this is not a SQLite database";
    }
    bool rejected = false;
    try {
        Storage db(file);
    } catch (...) {
        rejected = true;
    }
    require(rejected, "Corrupt database rejected");
    std::ifstream f(file);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    f.close();
    require(content == "this is not a SQLite database", "Corrupt data unchanged");
    require(std::filesystem::remove(file), "Corrupt handle released");
}
void optimizationTests(const std::filesystem::path &dir) {
    auto file = dir / L"optimized.db";
    SYSTEMTIME local{}, utc{};
    local.wYear = 2026;
    local.wMonth = 10;
    local.wDay = 12;
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME fileTime{};
    SystemTimeToFileTime(&utc, &fileTime);
    const Millis monday = static_cast<Millis>(
        ((static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime) / 10000 -
        11644473600000ULL);
    auto day = localDay(monday);
    {
        Storage db(file);
        require(
            scalar(file, "SELECT COUNT(*) FROM sqlite_master WHERE name IN ('limits','notices')") ==
                0,
            "New profiles never create legacy tables");
        require(db.applications().empty(), "Empty catalog is cached");
        db.app({"a", L"A", L"a.exe"});
        require(db.applications().size() == 1, "New metadata invalidates catalog");
        db.app({"a", L"Updated", L"new.exe"});
        require(db.applications()[0].name == L"Updated" && db.applications()[0].path == L"new.exe",
                "Metadata changes invalidate cached names and paths");
        db.renameApp("a", L"Custom");
        require(db.applications()[0].name == L"Custom", "Rename invalidates catalog");
        db.app({"b", L"B", {}});
        db.setReminder("b", 60000);
        require(db.snapshot(day, false).apps.empty() && db.applications().size() == 2,
                "Zero-use reminder applications stay in the catalog outside the ranking");
        bool failed = false;
        try {
            db.save({{"a", monday, 1000}, {"missing", monday + 1000, 1000}});
        } catch (...) {
            failed = true;
        }
        require(failed && db.snapshot(day, false).total == 0,
                "Batched writes roll back after a later invalid entry");
        std::vector<Usage> entries;
        for (int d = 0; d < 7; ++d)
            for (int h = 0; h < 24; ++h)
                entries.push_back(
                    {h % 2 ? "a" : "b", monday + d * 86400000LL + h * 3600000LL, 1000});
        db.save(entries);
        auto weekly = db.snapshot(day, true);
        require(weekly.total == 168000 && weekly.buckets == std::vector<Millis>(7, 24000),
                "Weekly buckets equal all daily records after a rollback");
        require(db.snapshot(day, true, "a").selectedTotal == 84000,
                "Selected weekly buckets preserve totals");
        db.mergeApps("a", "b");
        require(db.applications().size() == 1 && db.applications()[0].id == "b",
                "Merge invalidates catalog");
        db.commit({}, {{1, day, 12345, true, true, "keep-token"}});
    }
    // A schema-3 profile must keep every current record and pending notification action.
    rawSql(file, "CREATE TABLE limits(app TEXT PRIMARY KEY,duration INTEGER);"
                 "CREATE TABLE notices(app TEXT,day TEXT,next INTEGER,muted INTEGER);PRAGMA "
                 "user_version=3;");
    {
        Storage db(file);
        require(scalar(file, "PRAGMA user_version") == 6 && db.snapshot(day, true).total == 168000,
                "Schema three upgrades without discarding history");
        auto state = db.reminderStates(day).front();
        require(state.token == "keep-token" && state.muted && state.next == 12345,
                "Schema three preserves reminder state and token");
        require(db.appMappings()["a"] == "b", "Upgrade preserves application groups");
        db.clear();
        require(db.snapshot(day, true).total == 0 && db.reminderRules().size() == 2 &&
                    db.applications().size() == 1 && db.reminderStates(day).empty(),
                "Clear resets usage and states while retaining cached catalog and rules");
    }
    rawSql(file, "PRAGMA user_version=99;");
    bool rejected = false;
    try {
        Storage db(file);
    } catch (...) {
        rejected = true;
    }
    require(rejected && scalar(file, "PRAGMA user_version") == 99,
            "Newer schemas fail without resetting data");
    std::filesystem::remove(file);
    std::filesystem::remove(file.wstring() + L"-wal");
    std::filesystem::remove(file.wstring() + L"-shm");
}
void identityStorageTests(const std::filesystem::path &dir) {
    auto file = dir / L"identities.db";
    const Millis start = 1700000000000;
    const auto day = localDay(start);
    const AppInfo group{"app:sample", L"Sample", L"sample.exe"};
    {
        Storage db(file);
        db.app({"sample-helper.exe", L"Sample Helper", L"sample-helper.exe"});
        require(!db.applications()[0].hasUsage, "Catalog metadata alone does not count as use");
        db.save({{"sample-helper.exe", start, 400}});
        require(db.registerApp({"sample-helper.exe", L"Sample Helper", L"sample-helper.exe"},
                               group) == group.id,
                "Confident identity registration returns the display application");
        require(db.applications().size() == 1 && db.applications()[0].hasUsage &&
                    db.snapshot(day, false).total == 400,
                "Automatic grouping preserves subsecond use and propagates activity");
        require(scalar(file, "SELECT COUNT(*) FROM segments WHERE app='sample-helper.exe'") == 1,
                "Automatic grouping retains original interval identity");
        auto refreshed = group;
        refreshed.name = L"Sample Updated";
        db.registerApp({"sample-helper.exe", L"Sample Helper", L"sample-helper.exe"}, refreshed);
        require(db.applications()[0].name == L"Sample Updated",
                "Repeated registration refreshes representative metadata");
        require(db.identitySources().size() == 2,
                "Startup identity sources include aliases and representatives");
        auto history = db.snapshot(shiftDay(day, -1), false, "sample-helper.exe");
        require(history.apps.empty() && history.selected.app.id == group.id &&
                    history.selected.used == 0 && history.selectedTotal == 0,
                "Historical zero-use details retain selected application metadata");
        db.app({"new-helper", L"New Helper", L"new-helper.exe"});
        const AppInfo newGroup{"app:new", L"New", L"new.exe"};
        db.registerApp({"new-helper", L"New Helper", L"new-helper.exe"}, newGroup);
        require(!db.applications()[0].hasUsage, "Unused representative remains distinguishable");
        db.save({{"new-helper", start + 400, 1000}});
        auto fresh = db.applications();
        require(std::all_of(fresh.begin(), fresh.end(), [](const auto &a) { return a.hasUsage; }),
                "First committed interval invalidates catalog and marks its representative used");

        db.app({"manual", L"Manual", {}});
        db.renameApp("manual", L"User Name");
        require(db.registerApp({"manual", L"Metadata", {}}, group) == "manual",
                "Automatic grouping respects explicit user names");
        db.app({"child", L"Child", {}});
        db.mergeApps("child", "manual");
        require(db.registerApp({"child", L"Child", {}}, group) == "manual" &&
                    db.registerApp({"manual", L"Metadata", {}}, group) == "manual",
                "Automatic grouping respects both members and roots of manual groups");

        db.app({"conflicting", L"Conflicting", {}});
        db.setReminder("conflicting", 60000);
        db.setReminder(group.id, 120000);
        require(db.registerApp({"conflicting", L"Conflicting", {}}, group) == "conflicting",
                "Automatic grouping retains distinct user reminder configurations");
        auto rule = db.reminderRules().back().id;
        db.commit({}, {{rule, day, 300400, false, false, "same-occurrence"}});
        db.app({"second-helper", L"Second Helper", {}});
        db.save({{"second-helper", start + 1400, 2000}});
        db.registerApp({"second-helper", L"Second Helper", {}}, group);
        auto states = db.reminderStates(day);
        require(states[0].next == 302400 && states[0].token == "same-occurrence",
                "Automatic grouping retains the existing token and remaining snooze");
        db.registerApp({"second-helper", L"Second Helper", {}}, group);
        require(db.reminderStates(day)[0].next == 302400,
                "Repeated automatic grouping does not extend snooze again");
        require(db.registerApp({"sample-helper.exe", L"Sample Helper", L"sample-helper.exe"},
                               newGroup) == group.id &&
                    db.appMappings()["sample-helper.exe"] == group.id,
                "Changed identity evidence cannot split an established automatic group");
        require(db.reminderStates(day)[0].next == 302400 &&
                    db.reminderStates(day)[0].token == "same-occurrence" &&
                    db.dailyUsage(day)[group.id] == 2400,
                "Skipped regrouping preserves configured reminder state and cumulative use");

        db.registerApp({"unknown:10", L"Unknown 10", {}});
        db.registerApp({"unknown-frame:20", L"Unknown 20", {}});
        auto mappings = db.appMappings();
        require(mappings["unknown:10"] == "app:unidentified" &&
                    mappings["unknown-frame:20"] == "app:unidentified",
                "Unidentified process instances share one stable display identity");
        db.clear();
        require(std::none_of(db.applications().begin(), db.applications().end(),
                             [](const auto &a) { return a.hasUsage; }),
                "Clear resets activity eligibility while preserving applications");
    }
    std::filesystem::remove(file);

    file = dir / L"schema4.db";
    rawSql(file,
           "CREATE TABLE apps(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT NULL);"
           "CREATE TABLE segments(id INTEGER PRIMARY KEY,app TEXT,start INTEGER,end INTEGER,"
           "duration INTEGER,day TEXT,hour INTEGER);"
           "CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
           "CREATE TABLE app_labels(app TEXT PRIMARY KEY,name TEXT NOT NULL);"
           "CREATE TABLE app_links(app TEXT PRIMARY KEY,target TEXT NOT NULL);"
           "CREATE TABLE reminder_rules(id INTEGER PRIMARY KEY AUTOINCREMENT,kind INTEGER,"
           "app TEXT UNIQUE,duration INTEGER,enabled INTEGER);"
           "CREATE TABLE reminder_states(rule INTEGER,day TEXT,next INTEGER,notified INTEGER,"
           "muted INTEGER,token TEXT,PRIMARY KEY(rule,day));"
           "INSERT INTO apps VALUES('a','A','a.exe'),('b','B','b.exe'),"
           "('unknown:1','Unknown 1',''),('unknown:2','Unknown 2',''),('empty','Empty','');"
           "INSERT INTO segments VALUES(1,'a',1000,2000,1000,'2026-10-17',12),"
           "(2,'unknown:1',2000,2300,300,'2026-10-17',12),"
           "(3,'unknown:2',2300,2500,200,'2026-10-17',12);"
           "INSERT INTO app_links VALUES('a','b');INSERT INTO app_labels VALUES('b','Custom');"
           "INSERT INTO reminder_rules VALUES(1,0,NULL,3600000,0),(2,1,'b',60000,1);"
           "INSERT INTO reminder_states VALUES(2,'2026-10-17',300000,0,1,'retain-token');"
           "PRAGMA user_version=4;");
    {
        Storage db(file);
        require(scalar(file, "PRAGMA user_version") == 6 &&
                    scalar(file, "SELECT COUNT(*) FROM segments") == 3 &&
                    scalar(file, "SELECT SUM(duration) FROM segments") == 1500,
                "Schema four upgrade retains every interval and total");
        require(db.appMappings()["a"] == "b" &&
                    scalar(file, "SELECT manual FROM app_links WHERE app='a'") == 1,
                "Schema four existing groups retain explicit ownership");
        auto snapshot = db.snapshot("2026-10-17", false);
        require(snapshot.total == 1500 && snapshot.apps.size() == 2 &&
                    snapshot.apps[0].app.name == L"Custom" && snapshot.apps[0].app.hasUsage,
                "Migration consolidates unidentified rows and derives catalog activity");
        require(db.reminderStates("2026-10-17")[0].token == "retain-token" &&
                    db.reminderStates("2026-10-17")[0].muted,
                "Schema four migration keeps reminder actions and silence state");
    }
    std::filesystem::remove(file);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--fixture") {
            fixture(argv[2]);
            return 0;
        }
        auto dir = std::filesystem::temp_directory_path() /
                   (L"TimeTrace-test-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(dir);
        timelineTests();
        require(formatDuration(0) == L"0秒", "Zero duration remains zero");
        require(formatDuration(1) == L"不足1秒" && formatDuration(999) == L"不足1秒",
                "Positive subsecond duration is never displayed as zero");
        require(formatDuration(1000) == L"1秒", "Whole seconds keep existing formatting");
        engineTests();
        protocolTests();
        boundaryTests(dir);
        storageTests(dir);
        mergeTests(dir);
        batchMergeTests(dir);
        groupManagementTests(dir);
        migrationTests(dir);
        optimizationTests(dir);
        identityStorageTests(dir);
        // Files are all fixed names in this process's test directory; never touch a user profile.
        for (auto name : {L"storage.db", L"merge.db", L"batch-merge.db", L"group-management.db",
                          L"schema5.db", L"v1.db", L"v2.db", L"midnight.db"})
            std::filesystem::remove(dir / name);
        std::filesystem::remove(dir);
        std::cout << "PASS: " << checks << " checks\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
