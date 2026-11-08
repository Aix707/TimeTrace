#include "storage.h"
#include "sqlite_statement.h"
#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace tt {
namespace {
std::wstring checkedName(const std::wstring &value) {
    auto first =
        std::find_if_not(value.begin(), value.end(), [](wchar_t c) { return iswspace(c); });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t c) {
                    return iswspace(c);
                }).base();
    if (first >= last || std::distance(first, last) > 40)
        throw std::runtime_error("Application name must contain 1 to 40 characters");
    return {first, last};
}
} // namespace
void Storage::upsertApp(const AppInfo &app) {
    if (app.id.empty())
        throw std::runtime_error("Application identity is empty");
    bool identified = app.identified && app.id != "app:unidentified" &&
                      !app.id.starts_with("unknown:") && !app.id.starts_with("unknown-frame:");
    auto name = app.applicationName.empty() ? app.name : app.applicationName;
    Statement row(db_, "INSERT INTO apps(id,name,path,resolved_name,name_key,identified) "
                       "VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(id) DO UPDATE SET "
                       "name=excluded.name,path=excluded.path,resolved_name=excluded.resolved_name,"
                       "name_key=excluded.name_key,identified=excluded.identified WHERE "
                       "name<>excluded.name OR path<>excluded.path OR "
                       "resolved_name<>excluded.resolved_name OR identified<>excluded.identified");
    row.text(1, app.id);
    row.text(2, utf8(app.name));
    row.text(3, utf8(app.path));
    row.text(4, utf8(name));
    row.text(5, applicationNameKey(name));
    row.integer(6, identified);
    row.step();
    if (sqlite3_changes(db_))
        catalog_.reset();
}
void Storage::app(const AppInfo &app) {
    registerApp(app);
}
std::string Storage::registerApp(const AppInfo &source, const AppInfo &representative) {
    Transaction transaction(db_);
    auto info = source;
    if (!representative.id.empty())
        info.applicationName = representative.name;
    info.identified =
        info.identified &&
        !applicationNameKey(info.applicationName.empty() ? info.name : info.applicationName)
             .empty();
    upsertApp(info);
    auto current = canonicalId(source.id);
    if (!info.identified || source.id.starts_with("unknown:") ||
        source.id.starts_with("unknown-frame:")) {
        if (current == source.id && source.id != "app:unidentified") {
            if (canonicalId("app:unidentified").empty())
                upsertApp({"app:unidentified", L"未识别用时", {}});
            mergeRoots(source.id, "app:unidentified", false);
        }
    } else {
        // The name of any existing member also identifies its renamed classification.
        // Keep explicit independence and source-specific custom names authoritative.
        Statement protectedApp(db_,
                               "SELECT 1 FROM apps WHERE id=?1 AND (separate_from<>'' OR "
                               "EXISTS(SELECT 1 FROM app_labels WHERE app=?1) OR "
                               "EXISTS(SELECT 1 FROM app_links WHERE target=?1 AND manual=1))");
        protectedApp.text(1, current);
        if (!protectedApp.step()) {
            Statement matching(
                db_, "SELECT DISTINCT COALESCE(m.target,a.id) FROM apps a "
                     "LEFT JOIN app_links m ON m.app=a.id "
                     "JOIN apps root ON root.id=COALESCE(m.target,a.id) "
                     "WHERE a.name_key=?1 AND a.identified=1 AND a.separate_from='' "
                     "AND COALESCE(m.target,a.id)<>?2 ORDER BY root.has_usage DESC,root.id");
            matching.text(1, applicationNameKey(
                                 info.applicationName.empty() ? info.name : info.applicationName));
            matching.text(2, current);
            if (matching.step())
                mergeRoots(current, matching.text(0), false);
        }
    }
    regroupNames();
    auto result = canonicalId(source.id);
    transaction.commit();
    return result;
}
std::wstring Storage::displayName(const std::string &app) {
    Statement name(db_, "SELECT COALESCE(n.name,NULLIF(a.resolved_name,''),a.name) FROM apps a "
                        "LEFT JOIN app_labels n ON n.app=a.id WHERE a.id=?1");
    name.text(1, app);
    return name.step() ? wide(name.text(0)) : std::wstring{};
}
void Storage::checkName(const std::string &group, const std::wstring &name, bool combine) {
    if (combine)
        return;
    auto key = applicationNameKey(name);
    for (const auto &app : applications())
        if (app.id != group && applicationNameKey(app.name) == key)
            throw std::runtime_error("已存在同名应用，请确认合计用时后合并。");
}
void Storage::mergeRoots(const std::string &from, const std::string &to, bool manual) {
    if (from == to)
        return;
    mergeReminders(from, to, !manual);
    Statement joined(db_, "UPDATE apps SET separate_from='' WHERE id=?1 OR id IN "
                          "(SELECT app FROM app_links WHERE target=?1)");
    joined.text(1, from);
    joined.step();
    Statement members(db_, "UPDATE app_links SET target=?1,manual=MAX(manual,?3) WHERE target=?2");
    members.text(1, to);
    members.text(2, from);
    members.integer(3, manual);
    members.step();
    Statement link(db_, "INSERT INTO app_links(app,target,manual) VALUES(?1,?2,?3)");
    link.text(1, from);
    link.text(2, to);
    link.integer(3, manual);
    link.step();
    Statement usage(db_, "UPDATE apps SET has_usage=1 WHERE id=?1 AND "
                         "EXISTS(SELECT 1 FROM apps WHERE id=?2 AND has_usage=1)");
    usage.text(1, to);
    usage.text(2, from);
    usage.step();
    catalog_.reset();
}
void Storage::regroupNames(const std::string &preferred) {
    std::map<std::string, std::string> groups;
    std::vector<std::pair<std::string, std::string>> names;
    Statement roots(db_,
                    "SELECT a.id,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name) "
                    "FROM apps a LEFT JOIN app_labels n ON n.app=a.id "
                    "WHERE a.identified=1 AND NOT EXISTS(SELECT 1 FROM app_links WHERE app=a.id) "
                    "ORDER BY a.id<>?1,a.has_usage DESC,n.app IS NULL,"
                    "EXISTS(SELECT 1 FROM app_links WHERE target=a.id AND manual=1) DESC,a.id");
    roots.text(1, preferred);
    while (roots.step())
        names.emplace_back(roots.text(0), applicationNameKey(wide(roots.text(1))));
    for (const auto &[id, key] : names) {
        if (key.empty())
            continue;
        auto [group, inserted] = groups.try_emplace(key, id);
        if (!inserted)
            mergeRoots(id, group->second, false);
    }
}
void Storage::renameApp(const std::string &app, const std::wstring &value, bool combine) {
    auto name = checkedName(value);
    Transaction transaction(db_);
    auto root = canonicalId(app);
    if (root.empty())
        throw std::runtime_error("Application does not exist");
    checkName(root, name, combine);
    Statement label(db_, "INSERT INTO app_labels(app,name) VALUES(?1,?2) "
                         "ON CONFLICT(app) DO UPDATE SET name=excluded.name");
    label.text(1, root);
    label.text(2, utf8(name));
    label.step();
    catalog_.reset();
    regroupNames(root);
    transaction.commit();
}
void Storage::unmergeApp(const std::string &group, const std::string &member,
                         const std::wstring &name) {
    moveMember(group, member, {}, name);
}
void Storage::moveMember(const std::string &group, const std::string &member,
                         const std::string &target, const std::wstring &value) {
    Transaction transaction(db_);
    auto root = canonicalId(group);
    auto to = target.empty() ? std::string{} : canonicalId(target);
    if (!target.empty() && (to.empty() || to == root || to == member))
        throw std::runtime_error("请选择其他应用。");
    auto name = value.empty() ? displayName(member) : checkedName(value);
    if (to.empty()) {
        if (applicationNameKey(name) == applicationNameKey(displayName(root)))
            throw std::runtime_error("请为独立应用填写其他名称。");
        checkName(member, name, false);
    }
    detachMember(group, member);
    if (!value.empty()) {
        Statement label(db_, "INSERT INTO app_labels(app,name) VALUES(?1,?2) "
                             "ON CONFLICT(app) DO UPDATE SET name=excluded.name");
        label.text(1, member);
        label.text(2, utf8(name));
        label.step();
    }
    if (!to.empty())
        mergeRoots(member, to, true);
    transaction.commit();
    catalog_.reset();
}
} // namespace tt
