#include "storage.h"
#include "sqlite_statement.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace tt {
namespace {
constexpr const char *unknownId = "app:unidentified";
bool hasColumn(sqlite3 *db, const char *table, const char *column) {
    auto sql = std::string("PRAGMA table_info(") + table + ")";
    Statement fields(db, sql.c_str());
    while (fields.step())
        if (fields.text(1) == column)
            return true;
    return false;
}
} // namespace
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
            if (schema > 7)
                throw std::runtime_error("This database requires a newer TimeTrace version");
        }
        execSql(db_, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA foreign_keys=ON;"
                     "PRAGMA cache_size=-1024;");
        if (schema == 7)
            return;
        Transaction transaction(db_);
        execSql(
            db_,
            "CREATE TABLE IF NOT EXISTS apps(id TEXT PRIMARY KEY,name TEXT NOT NULL,path TEXT NOT "
            "NULL,has_usage INTEGER NOT NULL DEFAULT 0 CHECK(has_usage IN (0,1)),"
            "separate_from TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE IF NOT EXISTS segments(id INTEGER PRIMARY KEY,app TEXT NOT NULL "
            "REFERENCES "
            "apps(id),start INTEGER NOT NULL,end INTEGER NOT NULL,duration INTEGER NOT NULL "
            "CHECK(duration>0),day TEXT NOT NULL,hour INTEGER NOT NULL);"
            "CREATE INDEX IF NOT EXISTS segments_day ON segments(day,app);"
            "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS app_labels(app TEXT PRIMARY KEY REFERENCES apps(id),"
            "name TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS app_links(app TEXT PRIMARY KEY REFERENCES apps(id),"
            "target TEXT NOT NULL REFERENCES apps(id),manual INTEGER NOT NULL DEFAULT 1 "
            "CHECK(manual IN (0,1)),CHECK(app<>target));"
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
        // Alias notices use the source's cumulative clock. Target notices already use
        // the combined clock, so only alias thresholds need the other members' usage.
        if (schema == 1 || schema == 2)
            execSql(db_,
                    "INSERT OR IGNORE INTO reminder_rules(kind,app,duration,enabled) "
                    "SELECT 1,COALESCE(m.target,l.app),l.duration,1 FROM limits l LEFT JOIN "
                    "app_links m ON m.app=l.app ORDER BY m.target IS NOT NULL,l.app;"
                    "WITH raw_usage AS (SELECT app,day,SUM(duration) used FROM segments "
                    "GROUP BY app,day),group_usage AS (SELECT COALESCE(m.target,s.app) app,"
                    "s.day,SUM(s.duration) used FROM segments s LEFT JOIN app_links m "
                    "ON m.app=s.app GROUP BY COALESCE(m.target,s.app),s.day),states AS ("
                    "SELECT r.id rule,n.day,MAX(CASE WHEN n.next>0 THEN n.next+CASE WHEN "
                    "m.target IS NOT NULL THEN COALESCE(g.used,0)-COALESCE(u.used,0) "
                    "ELSE 0 END ELSE 0 END) next,MAX(n.muted) muted "
                    "FROM notices n LEFT JOIN app_links m ON m.app=n.app JOIN reminder_rules r "
                    "ON r.app=COALESCE(m.target,n.app) LEFT JOIN raw_usage u "
                    "ON u.app=n.app AND u.day=n.day LEFT JOIN group_usage g "
                    "ON g.app=r.app AND g.day=n.day GROUP BY r.id,n.day) "
                    "INSERT OR REPLACE INTO reminder_states(rule,day,next,notified,muted) "
                    "SELECT rule,day,next,muted,muted FROM states;");
        if (!hasColumn(db_, "apps", "has_usage"))
            execSql(db_, "ALTER TABLE apps ADD COLUMN has_usage INTEGER NOT NULL DEFAULT 0 "
                         "CHECK(has_usage IN (0,1));");
        if (!hasColumn(db_, "apps", "separate_from"))
            execSql(db_, "ALTER TABLE apps ADD COLUMN separate_from TEXT NOT NULL DEFAULT '';");
        if (!hasColumn(db_, "app_links", "manual"))
            execSql(db_, "ALTER TABLE app_links ADD COLUMN manual INTEGER NOT NULL DEFAULT 1 "
                         "CHECK(manual IN (0,1));");
        execSql(db_, "UPDATE apps SET has_usage=CASE WHEN EXISTS(SELECT 1 FROM segments s WHERE "
                     "s.app=apps.id) OR EXISTS(SELECT 1 FROM app_links m JOIN segments s ON "
                     "s.app=m.app WHERE m.target=apps.id) THEN 1 ELSE 0 END;"
                     "DROP TABLE IF EXISTS notices;DROP TABLE IF EXISTS limits;");
        if (!hasColumn(db_, "apps", "resolved_name")) {
            execSql(db_, "ALTER TABLE apps ADD COLUMN resolved_name TEXT NOT NULL DEFAULT '';"
                         "ALTER TABLE apps ADD COLUMN name_key TEXT NOT NULL DEFAULT '';"
                         "ALTER TABLE apps ADD COLUMN identified INTEGER NOT NULL DEFAULT 1;"
                         "UPDATE apps SET resolved_name=name;"
                         "UPDATE apps SET resolved_name=COALESCE((SELECT t.name FROM app_links m "
                         "JOIN apps t ON t.id=m.target WHERE m.app=apps.id AND m.manual=0),name);"
                         "UPDATE apps SET identified=0 WHERE id LIKE 'unknown:%' OR "
                         "id LIKE 'unknown-frame:%' OR id='app:unidentified';");
        }
        execSql(db_, "CREATE INDEX IF NOT EXISTS apps_name_key ON apps(name_key);");
        {
            Statement names(db_, "SELECT id,resolved_name FROM apps");
            Statement key(db_, "UPDATE apps SET name_key=?1 WHERE id=?2");
            while (names.step()) {
                key.reset();
                key.text(1, applicationNameKey(wide(names.text(1))));
                key.text(2, names.text(0));
                key.step();
            }
        }
        std::vector<std::string> unknown;
        {
            Statement sources(db_, "SELECT id FROM apps WHERE id LIKE 'unknown:%' OR "
                                   "id LIKE 'unknown-frame:%'");
            while (sources.step())
                unknown.push_back(sources.text(0));
        }
        if (!unknown.empty() && canonicalId(unknownId).empty())
            upsertApp({unknownId, L"未识别用时", {}});
        for (const auto &id : unknown)
            if (canonicalId(id) == id)
                mergeRoots(id, unknownId, false);
        regroupNames();
        execSql(db_, "PRAGMA user_version=7;");
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
    struct Tail {
        int64_t id{};
        std::string app;
        Millis end{};
        std::string day;
        int hour{};
    };
    std::optional<Tail> tail;
    {
        Statement last(db_, "SELECT id,app,end,day,hour FROM segments ORDER BY id DESC LIMIT 1");
        if (last.step())
            tail = Tail{last.integer(0), last.text(1), last.integer(2), last.text(3),
                        static_cast<int>(last.integer(4))};
    }
    Statement update(db_, "UPDATE segments SET end=?1,duration=duration+?2 WHERE id=?3");
    Statement insert(db_, "INSERT INTO segments(app,start,end,duration,day,hour) "
                          "VALUES(?1,?2,?3,?4,?5,?6)");
    Statement used(db_, "UPDATE apps SET has_usage=1 WHERE has_usage=0 AND "
                        "(id=?1 OR id=(SELECT target FROM app_links WHERE app=?1))");
    for (const auto &u : entries) {
        auto parts = splitLocal(u);
        for (const auto &p : parts) {
            bool join = tail && tail->app == u.app && tail->end == p.startUtc &&
                        tail->day == p.day && tail->hour == p.hour;
            if (join) {
                update.reset();
                update.integer(1, p.startUtc + p.duration);
                update.integer(2, p.duration);
                update.integer(3, tail->id);
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
                tail = Tail{sqlite3_last_insert_rowid(db_), u.app, p.startUtc, p.day, p.hour};
            }
            tail->end = p.startUtc + p.duration;
        }
        if (!parts.empty()) {
            used.reset();
            used.text(1, u.app);
            used.step();
            if (sqlite3_changes(db_))
                catalog_.reset();
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
                "SELECT a.id,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name),a.path,a.has_usage "
                "FROM apps a "
                "LEFT JOIN app_labels n ON n.app=a.id WHERE a.has_usage=1 AND a.identified=1 AND "
                "NOT EXISTS(SELECT 1 "
                "FROM app_links m WHERE m.app=a.id) "
                "ORDER BY COALESCE(n.name,NULLIF(a.resolved_name,''),a.name) COLLATE NOCASE,a.id");
    while (s.step())
        out.push_back({s.text(0), wide(s.text(1)), wide(s.text(2)), s.integer(3) != 0});
    catalog_ = std::move(out);
    return *catalog_;
}
std::vector<AppInfo> Storage::identitySources() {
    std::vector<AppInfo> result;
    Statement sources(
        db_, "SELECT id,name,path,has_usage,resolved_name,identified FROM apps ORDER BY id");
    while (sources.step())
        result.push_back({sources.text(0), wide(sources.text(1)), wide(sources.text(2)),
                          sources.integer(3) != 0, wide(sources.text(4)), sources.integer(5) != 0});
    return result;
}
void Storage::mergeApps(const std::string &source, const std::string &target) {
    mergeApps(std::vector<std::string>{source}, target);
}
void Storage::mergeApps(const std::vector<std::string> &sources, const std::string &target) {
    Transaction transaction(db_);
    auto to = canonicalId(target);
    if (to.empty())
        throw std::runtime_error("Application does not exist");
    std::vector<std::string> roots;
    for (const auto &source : sources) {
        auto from = canonicalId(source);
        if (from.empty())
            throw std::runtime_error("Application does not exist");
        if (from != to && std::find(roots.begin(), roots.end(), from) == roots.end())
            roots.push_back(std::move(from));
    }
    if (roots.empty()) {
        transaction.commit();
        return;
    }
    for (const auto &from : roots)
        mergeRoots(from, to, true);
    transaction.commit();
    catalog_.reset();
}
void Storage::detachMember(const std::string &group, const std::string &member) {
    auto root = canonicalId(group), current = canonicalId(member);
    if (root.empty() || current.empty())
        throw std::runtime_error("Application does not exist");
    if (member == root)
        throw std::runtime_error("Choose another primary application before removing this member");
    if (current != root) {
        Statement previous(db_, "SELECT separate_from FROM apps WHERE id=?1");
        previous.text(1, member);
        if (previous.step() && current == member && !previous.text(0).empty() &&
            canonicalId(previous.text(0)) == root) {
            return;
        }
        throw std::runtime_error("Application is not a member of this group");
    }
    Statement state(db_, "UPDATE reminder_states SET next=CASE WHEN next>0 THEN MAX(1,next-"
                         "COALESCE((SELECT SUM(duration) FROM segments WHERE app=?1 AND "
                         "day=reminder_states.day),0)) ELSE next END,token='' WHERE rule IN "
                         "(SELECT id FROM reminder_rules WHERE kind=1 AND app=?2)");
    state.text(1, member);
    state.text(2, root);
    state.step();
    Statement remove(db_, "DELETE FROM app_links WHERE app=?1 AND target=?2");
    remove.text(1, member);
    remove.text(2, root);
    remove.step();
    Statement separate(db_, "UPDATE apps SET separate_from=?1 WHERE id=?2");
    separate.text(1, root);
    separate.text(2, member);
    separate.step();
    Statement usage(db_, "UPDATE apps SET has_usage=EXISTS(SELECT 1 FROM segments WHERE "
                         "app=apps.id) OR EXISTS(SELECT 1 FROM app_links m JOIN segments s "
                         "ON s.app=m.app WHERE m.target=apps.id) WHERE id=?1 OR id=?2");
    usage.text(1, root);
    usage.text(2, member);
    usage.step();
    catalog_.reset();
}
std::string Storage::setPrimary(const std::string &group, const std::string &member, bool combine) {
    Transaction transaction(db_);
    auto root = canonicalId(group);
    if (root.empty() || canonicalId(member) != root)
        throw std::runtime_error("Application is not a member of this group");
    if (member == root) {
        transaction.commit();
        return root;
    }
    checkName(root, displayName(member), combine);
    Statement remove(db_, "DELETE FROM app_links WHERE app=?1 AND target=?2");
    remove.text(1, member);
    remove.text(2, root);
    remove.step();
    Statement members(db_, "UPDATE app_links SET target=?1 WHERE target=?2");
    members.text(1, member);
    members.text(2, root);
    members.step();
    Statement former(db_, "INSERT INTO app_links(app,target,manual) VALUES(?1,?2,1)");
    former.text(1, root);
    former.text(2, member);
    former.step();
    Statement rule(db_, "UPDATE reminder_rules SET app=?1 WHERE kind=1 AND app=?2");
    rule.text(1, member);
    rule.text(2, root);
    rule.step();
    Statement usage(db_, "UPDATE apps SET has_usage=1 WHERE id=?1 AND "
                         "EXISTS(SELECT 1 FROM apps WHERE id=?2 AND has_usage=1)");
    usage.text(1, member);
    usage.text(2, root);
    usage.step();
    regroupNames(member);
    transaction.commit();
    catalog_.reset();
    return member;
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
        "SELECT "
        "a.id,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name),a.path,COALESCE(u.used,0),a.has_"
        "usage,a.identified "
        "FROM apps a LEFT JOIN app_labels n ON n.app=a.id LEFT JOIN usage u ON u.app=a.id "
        "WHERE NOT EXISTS(SELECT 1 FROM app_links m WHERE m.app=a.id) "
        "AND u.used>0 ORDER BY u.used DESC,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name)");
    rows.text(1, from);
    rows.text(2, to);
    while (rows.step()) {
        AppRow r{{rows.text(0), wide(rows.text(1)), wide(rows.text(2)), rows.integer(4) != 0},
                 rows.integer(3)};
        out.total += r.used;
        if (rows.integer(5))
            out.apps.push_back(std::move(r));
        else
            out.unidentified += r.used;
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
    if (!selection.empty()) {
        auto chosen = std::find_if(out.catalog.begin(), out.catalog.end(),
                                   [&](const AppInfo &app) { return app.id == selection; });
        if (chosen != out.catalog.end())
            out.selected = {*chosen, out.selectedTotal};
        if (chosen == out.catalog.end()) {
            Statement metadata(db_, "SELECT path FROM apps WHERE id=?1");
            metadata.text(1, selection);
            if (metadata.step())
                out.selected = {{selection, displayName(selection), wide(metadata.text(0)), false},
                                out.selectedTotal};
        }
        Statement members(
            db_,
            "WITH member_ids AS MATERIALIZED (SELECT ?3 id,1 manual UNION ALL "
            "SELECT app,manual FROM app_links WHERE target=?3),usage AS ("
            "SELECT app,SUM(duration) used FROM segments WHERE day>=?1 AND day<?2 "
            "AND app IN (SELECT id FROM member_ids) GROUP BY app),members AS MATERIALIZED ("
            "SELECT a.id,a.name,a.resolved_name,a.path,m.manual,COALESCE(u.used,0) used,"
            "CASE WHEN u.app IS NOT NULL THEN 1 ELSE "
            "EXISTS(SELECT 1 FROM segments WHERE app=a.id) END recorded "
            "FROM member_ids m JOIN apps a ON a.id=m.id LEFT JOIN usage u ON u.app=a.id) "
            "SELECT "
            "a.id,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name),a.path,a.used,a.recorded "
            "FROM members a LEFT JOIN app_labels n ON n.app=a.id "
            "WHERE a.id=?3 OR a.id NOT LIKE 'app:%' OR a.recorded=1 OR n.app IS NOT NULL "
            "OR (a.manual=1 AND a.path<>'') "
            "ORDER BY a.id<>?3,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name) COLLATE "
            "NOCASE,a.id");
        members.text(1, from);
        members.text(2, to);
        members.text(3, selection);
        while (members.step())
            out.members.push_back({{members.text(0), wide(members.text(1)), wide(members.text(2)),
                                    members.integer(4) != 0},
                                   members.integer(3)});
    }
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
    execSql(db_, "DELETE FROM segments;DELETE FROM reminder_states;UPDATE apps SET has_usage=0;");
    transaction.commit();
    catalog_.reset();
    // Clearing is complete once committed. Optional compaction cannot undo it.
    sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);VACUUM;", nullptr, nullptr, nullptr);
}
} // namespace tt
