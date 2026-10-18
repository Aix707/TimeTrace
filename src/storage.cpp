#include "storage.h"
#include "sqlite_statement.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace tt {
Storage::Storage(const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    auto file = utf8(path.wstring());
    if (sqlite3_open_v2(file.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string e = db_ ? sqlite3_errmsg(db_) : "Cannot open database";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(e);
    }
    sqlite3_busy_timeout(db_, 3000);
    try {
        int schema = 0;
        {
            Statement version(db_, "PRAGMA user_version");
            if (version.step())
                schema = static_cast<int>(version.integer(0));
            if (schema > 4)
                throw std::runtime_error("This database requires a newer TimeTrace version");
        }
        execSql(db_, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
                     "PRAGMA cache_size=-1024;");
        if (schema == 4)
            return;
        Transaction transaction(db_);
        execSql(
            db_,
            "CREATE TABLE IF NOT EXISTS apps(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT "
            "NULL);"
            "CREATE TABLE IF NOT EXISTS segments(id INTEGER PRIMARY KEY,app TEXT NOT NULL "
            "REFERENCES "
            "apps(id),start INTEGER NOT NULL,end INTEGER NOT NULL,duration INTEGER NOT NULL "
            "CHECK(duration>0),day TEXT NOT NULL,hour INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS segments_day ON segments(day,app);"
            "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS app_labels(app TEXT PRIMARY KEY REFERENCES apps(id),"
            "name TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS app_links(app TEXT PRIMARY KEY REFERENCES apps(id),"
            "target TEXT NOT NULL REFERENCES apps(id),CHECK(app<>target));"
            "CREATE INDEX IF NOT EXISTS app_links_target ON app_links(target);"
            "CREATE TABLE IF NOT EXISTS reminder_rules(id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "kind INTEGER NOT NULL CHECK(kind IN (0,1)),app TEXT UNIQUE REFERENCES apps(id),"
            "duration INTEGER NOT NULL CHECK(duration>0),enabled INTEGER NOT NULL "
            "CHECK(enabled IN (0,1)),CHECK((kind=0 AND app IS NULL) OR (kind=1 AND app IS NOT "
            "NULL)));"
            "CREATE UNIQUE INDEX IF NOT EXISTS reminder_device ON reminder_rules(kind) WHERE "
            "kind=0;"
            "CREATE TABLE IF NOT EXISTS reminder_states(rule INTEGER NOT NULL REFERENCES "
            "reminder_rules(id) ON DELETE CASCADE,day TEXT NOT NULL,next INTEGER NOT NULL DEFAULT "
            "0,"
            "notified INTEGER NOT NULL DEFAULT 0,muted INTEGER NOT NULL DEFAULT 0,"
            "token TEXT NOT NULL DEFAULT '',PRIMARY KEY(rule,day));"
            "INSERT INTO reminder_rules(kind,app,duration,enabled) SELECT 0,NULL,3600000,0 "
            "WHERE NOT EXISTS(SELECT 1 FROM reminder_rules WHERE kind=0);");
        // Only old profiles need this one-time conversion. New profiles have no legacy tables.
        if (schema == 1 || schema == 2)
            execSql(db_,
                    "INSERT OR IGNORE INTO reminder_rules(kind,app,duration,enabled) "
                    "SELECT 1,COALESCE(m.target,l.app),l.duration,1 FROM limits l LEFT JOIN "
                    "app_links m ON m.app=l.app ORDER BY m.target IS NOT NULL;"
                    "INSERT OR REPLACE INTO reminder_states(rule,day,next,notified,muted) "
                    "SELECT r.id,n.day,n.next,n.muted,n.muted FROM notices n JOIN reminder_rules "
                    "r ON r.app=n.app;");
        execSql(db_,
                "DROP TABLE IF EXISTS notices;DROP TABLE IF EXISTS limits;PRAGMA user_version=4;");
        transaction.commit();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
Storage::~Storage() {
    if (db_)
        sqlite3_close(db_);
}
void Storage::app(const AppInfo &a) {
    Statement s(db_, "INSERT INTO apps VALUES(?1,?2,?3) ON CONFLICT(id) DO UPDATE SET "
                     "name=excluded.name,path=excluded.path WHERE name<>excluded.name OR "
                     "path<>excluded.path");
    s.text(1, a.id);
    s.text(2, utf8(a.name));
    s.text(3, utf8(a.path));
    s.step();
    if (sqlite3_changes(db_))
        catalog_.reset();
}
void Storage::save(const std::vector<Usage> &entries) {
    commit(entries, {});
}
void Storage::commit(const std::vector<Usage> &entries, const std::vector<ReminderState> &states) {
    if (entries.empty() && states.empty())
        return;
    Transaction transaction(db_);
    writeUsage(entries);
    writeStates(states);
    transaction.commit();
}
void Storage::writeUsage(const std::vector<Usage> &entries) {
    if (entries.empty())
        return;
    Statement last(db_, "SELECT id,app,end,day,hour FROM segments ORDER BY id DESC LIMIT 1");
    Statement update(db_, "UPDATE segments SET end=?1,duration=duration+?2 WHERE id=?3");
    Statement insert(db_, "INSERT INTO segments(app,start,end,duration,day,hour) "
                          "VALUES(?1,?2,?3,?4,?5,?6)");
    for (const auto &u : entries)
        for (const auto &p : splitLocal(u)) {
            last.reset();
            bool join = last.step() && last.text(1) == u.app && last.integer(2) == p.startUtc &&
                        last.text(3) == p.day && last.integer(4) == p.hour;
            if (join) {
                update.reset();
                update.integer(1, p.startUtc + p.duration);
                update.integer(2, p.duration);
                update.integer(3, last.integer(0));
                update.step();
            } else {
                insert.reset();
                insert.text(1, u.app);
                insert.integer(2, p.startUtc);
                insert.integer(3, p.startUtc + p.duration);
                insert.integer(4, p.duration);
                insert.text(5, p.day);
                insert.integer(6, p.hour);
                insert.step();
            }
        }
}
std::string Storage::canonicalId(const std::string &app) {
    Statement s(db_, "SELECT COALESCE(m.target,a.id) FROM apps a LEFT JOIN app_links m ON "
                     "m.app=a.id WHERE a.id=?1");
    s.text(1, app);
    return s.step() ? s.text(0) : std::string{};
}
Millis Storage::usedOnDay(const std::string &app, const std::string &day) {
    Statement s(db_, "SELECT COALESCE(SUM(s.duration),0) FROM segments s LEFT JOIN app_links m "
                     "ON m.app=s.app WHERE COALESCE(m.target,s.app)=?1 AND s.day=?2");
    s.text(1, app);
    s.text(2, day);
    return s.step() ? s.integer(0) : 0;
}
const std::vector<AppInfo> &Storage::applications() {
    if (catalog_)
        return *catalog_;
    std::vector<AppInfo> out;
    Statement s(db_,
                "SELECT a.id,COALESCE(n.name,a.name),a.path FROM apps a LEFT JOIN app_labels n "
                "ON n.app=a.id WHERE NOT EXISTS(SELECT 1 FROM app_links m WHERE m.app=a.id) "
                "ORDER BY COALESCE(n.name,a.name) COLLATE NOCASE,a.id");
    while (s.step())
        out.push_back({s.text(0), wide(s.text(1)), wide(s.text(2))});
    catalog_ = std::move(out);
    return *catalog_;
}
void Storage::renameApp(const std::string &app, const std::wstring &value) {
    auto first =
        std::find_if_not(value.begin(), value.end(), [](wchar_t c) { return iswspace(c); });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t c) {
                    return iswspace(c);
                }).base();
    if (first >= last || std::distance(first, last) > 40)
        throw std::runtime_error("Application name must contain 1 to 40 characters");
    auto id = canonicalId(app);
    if (id.empty())
        throw std::runtime_error("Application does not exist");
    Statement s(
        db_,
        "INSERT INTO app_labels VALUES(?1,?2) ON CONFLICT(app) DO UPDATE SET name=excluded.name");
    s.text(1, id);
    s.text(2, utf8(std::wstring(first, last)));
    s.step();
    catalog_.reset();
}
void Storage::mergeApps(const std::string &source, const std::string &target) {
    auto from = canonicalId(source), to = canonicalId(target);
    if (from.empty() || to.empty())
        throw std::runtime_error("Application does not exist");
    if (from == to)
        return;
    Transaction transaction(db_);
    mergeReminders(from, to);
    // Flatten groups, so aggregation never needs recursive traversal in the background.
    Statement members(db_, "UPDATE app_links SET target=?1 WHERE target=?2");
    members.text(1, to);
    members.text(2, from);
    members.step();
    Statement link(db_, "INSERT INTO app_links VALUES(?1,?2)");
    link.text(1, from);
    link.text(2, to);
    link.step();
    transaction.commit();
    catalog_.reset();
}
Snapshot Storage::snapshot(const std::string &day, bool weekly, const std::string &selected) {
    Snapshot out;
    out.day = day;
    out.weekly = weekly;
    out.buckets.resize(weekly ? 7 : 24);
    auto from = weekly ? weekStart(day) : day, to = shiftDay(from, weekly ? 7 : 1);
    auto selection = selected.empty() ? std::string{} : canonicalId(selected);
    Statement rows(
        db_,
        "WITH usage AS (SELECT COALESCE(m.target,s.app) app,SUM(s.duration) used "
        "FROM segments s LEFT JOIN app_links m ON m.app=s.app WHERE s.day>=?1 AND s.day<?2 "
        "GROUP BY COALESCE(m.target,s.app)) "
        "SELECT a.id,COALESCE(n.name,a.name),a.path,COALESCE(u.used,0) "
        "FROM apps a LEFT JOIN app_labels n ON n.app=a.id LEFT JOIN usage u ON u.app=a.id "
        "LEFT JOIN reminder_rules l ON l.app=a.id AND l.kind=1 AND l.enabled=1 WHERE NOT "
        "EXISTS(SELECT 1 FROM app_links m WHERE "
        "m.app=a.id) "
        "AND (u.used>0 OR l.duration>0) ORDER BY COALESCE(u.used,0) DESC,COALESCE(n.name,a.name)");
    rows.text(1, from);
    rows.text(2, to);
    while (rows.step()) {
        AppRow r{{rows.text(0), wide(rows.text(1)), wide(rows.text(2))}, rows.integer(3)};
        out.total += r.used;
        out.apps.push_back(std::move(r));
    }
    Statement buckets(
        db_, weekly ? "SELECT s.day,0,SUM(s.duration) FROM segments s LEFT JOIN app_links m "
                      "ON m.app=s.app WHERE s.day>=?1 AND s.day<?2 AND (?3='' OR "
                      "COALESCE(m.target,s.app)=?3) "
                      "GROUP BY s.day"
                    : "SELECT s.day,s.hour,SUM(s.duration) FROM segments s LEFT JOIN app_links m "
                      "ON m.app=s.app WHERE s.day>=?1 AND s.day<?2 AND (?3='' OR "
                      "COALESCE(m.target,s.app)=?3) "
                      "GROUP BY s.day,s.hour");
    buckets.text(1, from);
    buckets.text(2, to);
    buckets.text(3, selected.empty() ? "" : selection.empty() ? selected : selection);
    std::vector<std::string> days;
    if (weekly)
        for (int i = 0; i < 7; ++i)
            days.push_back(shiftDay(from, i));
    while (buckets.step()) {
        int n = static_cast<int>(buckets.integer(1));
        if (weekly) {
            n = static_cast<int>(std::lower_bound(days.begin(), days.end(), buckets.text(0)) -
                                 days.begin());
        }
        if (n >= 0 && n < static_cast<int>(out.buckets.size()))
            out.buckets[n] += buckets.integer(2);
        out.selectedTotal += buckets.integer(2);
    }
    out.catalog = applications();
    return out;
}
std::string Storage::getSetting(const std::string &key, const std::string &fallback) {
    Statement s(db_, "SELECT value FROM settings WHERE key=?1");
    s.text(1, key);
    return s.step() ? s.text(0) : fallback;
}
void Storage::setSetting(const std::string &key, const std::string &value) {
    Statement s(
        db_,
        "INSERT INTO settings VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    s.text(1, key);
    s.text(2, value);
    s.step();
}
void Storage::clear() {
    Transaction transaction(db_);
    execSql(db_, "DELETE FROM segments;DELETE FROM reminder_states;");
    transaction.commit();
    // Clearing is complete once committed. Optional compaction cannot undo it.
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);VACUUM;", nullptr, nullptr, nullptr);
}
} // namespace tt
