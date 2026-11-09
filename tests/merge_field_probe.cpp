#include "platform.h"
#include "sqlite_statement.h"
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <tuple>

using namespace tt;
namespace {
struct Connection {
    sqlite3 *db{};
    Connection(const std::filesystem::path &path, int flags) {
        if (sqlite3_open_v2(utf8(path.wstring()).c_str(), &db, flags, nullptr) != SQLITE_OK) {
            std::string error = db ? sqlite3_errmsg(db) : "Cannot open test database";
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error(error);
        }
    }
    ~Connection() {
        sqlite3_close(db);
    }
};
using RawRow = std::tuple<int64_t, std::string, Millis, Millis, Millis, std::string, int64_t>;
std::vector<RawRow> rawRows(const std::filesystem::path &path) {
    Connection connection(path, SQLITE_OPEN_READONLY);
    Statement query(connection.db,
                    "SELECT id,app,start,end,duration,day,hour FROM segments ORDER BY id");
    std::vector<RawRow> rows;
    while (query.step())
        rows.emplace_back(query.integer(0), query.text(1), query.integer(2), query.integer(3),
                          query.integer(4), query.text(5), query.integer(6));
    return rows;
}
std::map<std::string, Millis> totals(const Snapshot &snapshot) {
    std::map<std::string, Millis> result;
    for (const auto &row : snapshot.apps)
        result[row.app.id] = row.used;
    return result;
}
std::string json(const std::string &value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '\\' || c == '"')
            result += '\\';
        if (c >= 32)
            result += static_cast<char>(c);
        else {
            char escaped[7]{};
            sprintf_s(escaped, "\\u%04x", c);
            result += escaped;
        }
    }
    return result + '"';
}
std::filesystem::path testOutputRoot(std::filesystem::path path) {
    while (path.has_relative_path()) {
        if (_wcsicmp(path.filename().c_str(), L"test-output") == 0)
            return path;
        path = path.parent_path();
    }
    throw std::runtime_error("Probe requires a database copy under test-output");
}
int probeNamedGroup(const std::filesystem::path &working, const std::filesystem::path &output,
                    const std::string &day, const std::vector<RawRow> &original,
                    const std::wstring &name) {
    auto storage = std::make_unique<Storage>(working);
    auto catalog = storage->applications();
    auto target = std::find_if(catalog.begin(), catalog.end(), [&](const auto &app) {
        return applicationNameKey(app.name) == applicationNameKey(name);
    });
    if (target == catalog.end())
        throw std::runtime_error("Requested classification is absent from the field copy");
    std::map<std::string, Millis> sourceUsage;
    for (const auto &row : original)
        sourceUsage[std::get<1>(row)] += std::get<4>(row);
    auto lifetimeTotals = [&] {
        auto mappings = storage->appMappings();
        std::map<std::string, Millis> result;
        for (const auto &[source, used] : sourceUsage)
            result[mappings.at(source)] += used;
        return result;
    };
    auto memberMetadata = [](const Snapshot &snapshot) {
        std::map<std::string, std::pair<std::wstring, std::wstring>> result;
        for (const auto &member : snapshot.members)
            result[member.app.id] = {member.app.name, member.app.path};
        return result;
    };
    auto before = storage->snapshot(day, true, target->id);
    auto beforeTotals = lifetimeTotals();
    auto beforeMappings = storage->appMappings();
    auto beforeMembers = memberMetadata(before);
    auto beforeWeek = totals(before);
    size_t recordedMembers{};
    const AppRow *secondary{};
    for (const auto &member : before.members) {
        auto usage = sourceUsage.find(member.app.id);
        if (usage == sourceUsage.end() || usage->second <= 0)
            continue;
        ++recordedMembers;
        bool collision = std::any_of(catalog.begin(), catalog.end(), [&](const auto &app) {
            return app.id != target->id &&
                   applicationNameKey(app.name) == applicationNameKey(member.app.name);
        });
        if (member.app.id != target->id && !collision &&
            (!secondary || usage->second > sourceUsage.at(secondary->app.id)))
            secondary = &member;
    }
    auto receiver = std::find_if(catalog.begin(), catalog.end(), [&](const auto &app) {
        auto usage = beforeTotals.find(app.id);
        return app.id != target->id && usage != beforeTotals.end() && usage->second > 0;
    });
    std::string reason;
    if (recordedMembers < 2)
        reason = "Fewer than two members have recorded usage";
    else if (!secondary)
        reason = "No recorded secondary source has an available display name";
    else if (receiver == catalog.end())
        reason = "No other recorded classification is available for a source move";
    bool complete = reason.empty();
    bool promotedCorrect{}, primaryRoundtrip{}, separatedCorrect{}, remergedCorrect{};
    bool promotedRestart{}, finalRestart{};
    if (complete) {
        auto expectedPromoted = beforeTotals;
        expectedPromoted[secondary->app.id] = expectedPromoted.at(target->id);
        expectedPromoted.erase(target->id);
        auto primary = storage->setPrimary(target->id, secondary->app.id);
        auto promoted = storage->snapshot(day, true, primary);
        promotedCorrect = promoted.selected.app.name == secondary->app.name &&
                          promoted.selected.app.path == secondary->app.path &&
                          promoted.selectedTotal == before.selectedTotal &&
                          memberMetadata(promoted) == beforeMembers &&
                          lifetimeTotals() == expectedPromoted && original == rawRows(working);
        storage.reset();
        storage = std::make_unique<Storage>(working);
        auto reopened = storage->snapshot(day, true, target->id);
        promotedRestart = reopened.selected.app.id == primary &&
                          reopened.selected.app.name == secondary->app.name &&
                          reopened.selected.app.path == secondary->app.path &&
                          memberMetadata(reopened) == beforeMembers &&
                          lifetimeTotals() == expectedPromoted;
        storage->setPrimary(primary, target->id);
        auto restored = storage->snapshot(day, true, target->id);
        primaryRoundtrip = restored.selected.app.name == target->name &&
                           restored.selected.app.path == target->path &&
                           memberMetadata(restored) == beforeMembers &&
                           storage->appMappings() == beforeMappings &&
                           lifetimeTotals() == beforeTotals;
        // Move between recorded groups to preserve the source's saved name.
        auto expectedSeparated = beforeTotals;
        expectedSeparated.at(target->id) -= sourceUsage.at(secondary->app.id);
        expectedSeparated.at(receiver->id) += sourceUsage.at(secondary->app.id);
        storage->moveMember(target->id, secondary->app.id, receiver->id);
        separatedCorrect = lifetimeTotals() == expectedSeparated &&
                           storage->appMappings().at(secondary->app.id) == receiver->id &&
                           original == rawRows(working);
        storage->moveMember(receiver->id, secondary->app.id, target->id);
        remergedCorrect = lifetimeTotals() == beforeTotals &&
                          storage->appMappings() == beforeMappings &&
                          memberMetadata(storage->snapshot(day, true, target->id)) == beforeMembers;
    }
    storage.reset();
    storage = std::make_unique<Storage>(working);
    auto final = storage->snapshot(day, true, target->id);
    auto rawUnchanged = original == rawRows(working);
    auto allTotalsRestored = lifetimeTotals() == beforeTotals;
    auto weekRestored = totals(final) == beforeWeek && final.total == before.total &&
                        final.selectedTotal == before.selectedTotal;
    finalRestart = final.selected.app.id == target->id && final.selected.app.name == target->name &&
                   final.selected.app.path == target->path &&
                   memberMetadata(final) == beforeMembers &&
                   storage->appMappings() == beforeMappings && allTotalsRestored && weekRestored;
    bool passed = complete && promotedCorrect && promotedRestart && primaryRoundtrip &&
                  separatedCorrect && remergedCorrect && rawUnchanged && finalRestart;
    std::ofstream report(output, std::ios::binary);
    if (!report)
        throw std::runtime_error("Cannot write named classification probe report");
    auto checked = [complete](bool value) {
        if (!complete)
            return "null";
        return value ? "true" : "false";
    };
    report << "{\n  \"target_name\": " << json(utf8(target->name))
           << ",\n  \"target_id\": " << json(target->id)
           << ",\n  \"week_containing\": " << json(day)
           << ",\n  \"intervals\": " << original.size()
           << ",\n  \"recorded_members\": " << recordedMembers
           << ",\n  \"target_lifetime_ms\": " << beforeTotals.at(target->id)
           << ",\n  \"coverage_complete\": " << (complete ? "true" : "false")
           << ",\n  \"not_covered_reason\": " << json(reason)
           << ",\n  \"primary_uses_source_name_and_icon\": "
           << checked(promotedCorrect)
           << ",\n  \"promoted_primary_survives_restart\": "
           << checked(promotedRestart)
           << ",\n  \"primary_roundtrip_restores_source\": "
           << checked(primaryRoundtrip)
           << ",\n  \"source_move_preserves_usage\": "
           << checked(separatedCorrect)
           << ",\n  \"move_back_restores_members_and_totals\": "
           << checked(remergedCorrect)
           << ",\n  \"raw_intervals_unchanged\": " << (rawUnchanged ? "true" : "false")
           << ",\n  \"all_lifetime_group_totals_restored\": "
           << (allTotalsRestored ? "true" : "false")
           << ",\n  \"weekly_group_totals_restored\": " << (weekRestored ? "true" : "false")
           << ",\n  \"restart_restores_final_state\": " << (finalRestart ? "true" : "false")
           << ",\n  \"passed\": " << (passed ? "true" : "false") << "\n}\n";
    report.close();
    if (!report)
        throw std::runtime_error("Cannot finish named classification probe report");
    std::cout << (complete ? "Checked named classification on an isolated copy\n"
                          : "Named classification coverage is incomplete\n");
    if (!complete)
        return 3;
    return passed ? 0 : 1;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "merge_field_probe INPUT_COPY.db OUTPUT.json [CLASSIFICATION_NAME]\n";
        return 2;
    }
    auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result{};
    try {
        if (FAILED(apartment))
            throw std::runtime_error("Cannot initialize probe apartment");
        auto input = std::filesystem::weakly_canonical(argv[1]);
        auto output = std::filesystem::weakly_canonical(argv[2]);
        auto testRoot = testOutputRoot(input.parent_path());
        auto outputRoot = testOutputRoot(output.parent_path());
        if (testRoot != outputRoot || !std::filesystem::is_regular_file(input))
            throw std::runtime_error(
                "Input and output must be inside the same test-output directory");
        auto working = output.parent_path() / (output.stem().wstring() + L"-" +
                                               std::to_wstring(GetCurrentProcessId()) + L".db");
        if (std::filesystem::exists(working))
            throw std::runtime_error("Probe working database already exists");
        {
            Connection source(input, SQLITE_OPEN_READONLY);
            Connection target(working, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            auto backup = sqlite3_backup_init(target.db, "main", source.db, "main");
            if (!backup)
                throw std::runtime_error("Cannot create isolated working copy");
            auto copied = sqlite3_backup_step(backup, -1);
            auto finished = sqlite3_backup_finish(backup);
            if (copied != SQLITE_DONE || finished != SQLITE_OK)
                throw std::runtime_error("Cannot finish isolated working copy");
        }
        auto original = rawRows(working);
        std::string day;
        {
            Connection connection(working, SQLITE_OPEN_READONLY);
            Statement latest(connection.db, "SELECT MAX(day) FROM segments");
            if (latest.step())
                day = latest.text(0);
        }
        if (day.empty())
            throw std::runtime_error("Field copy has no recorded intervals");
        if (argc == 4) {
            result = probeNamedGroup(working, output, day, original, argv[3]);
        } else {
            Storage storage(working);
            AppResolver resolver;
            auto catalog = storage.applications();
            auto before = totals(storage.snapshot(day, true));
            AppInfo target;
            std::vector<std::string> sources;
            Millis greatestRelatedUse{};
            for (const auto &app : catalog) {

                auto candidates = resolver.mergeCandidates(catalog, app.id);
                std::vector<std::string> related;
                Millis used{};
                for (const auto &candidate : candidates)
                    if (candidate.related) {
                        related.push_back(candidate.app.id);
                        if (auto found = before.find(candidate.app.id); found != before.end())
                            used += found->second;
                    }
                auto details = storage.snapshot(day, true, app.id);
                bool existingMembers = details.members.size() > 1;
                used += existingMembers ? details.selectedTotal : 0;
                if ((!related.empty() || existingMembers) && used > greatestRelatedUse) {
                    target = app;
                    sources = std::move(related);
                    greatestRelatedUse = used;
                }
            }
            if (target.id.empty())
                throw std::runtime_error(
                    "No installed application has related usage in the field copy");
            auto expected = before;
            auto oldTarget = expected[target.id];
            for (const auto &source : sources) {
                expected[target.id] += expected[source];
                expected.erase(source);
            }
            storage.mergeApps(sources, target.id);
            auto after = storage.snapshot(day, true, target.id);
            auto rowsUnchanged = original == rawRows(working);
            auto totalsCorrect = totals(after) == expected;
            auto namePreserved =
                after.selected.app.id == target.id && after.selected.app.name == target.name;
            storage.mergeApps(sources, target.id);
            auto repeated = totals(storage.snapshot(day, true, target.id)) == expected &&
                            original == rawRows(working);
            auto primary = after.members.end();
            for (auto member = after.members.begin(); member != after.members.end(); ++member) {
                bool collision = std::any_of(catalog.begin(), catalog.end(), [&](const auto &app) {
                    return app.id != target.id &&
                           applicationNameKey(app.name) == applicationNameKey(member->app.name);
                });
                if (member->app.id != target.id && member->used > 0 && !collision &&
                    (primary == after.members.end() || member->used > primary->used))
                    primary = member;
            }
            if (primary == after.members.end())
                throw std::runtime_error(
                    "Field copy has no source with a distinct available display name");
            auto primaryId = storage.setPrimary(target.id, primary->app.id);
            auto promoted = storage.snapshot(day, true, primaryId);
            auto expectedPromoted = expected;
            expectedPromoted[primaryId] = expectedPromoted[target.id];
            expectedPromoted.erase(target.id);
            bool primaryPreserved = totals(promoted) == expectedPromoted &&
                                    promoted.selected.app.name == primary->app.name &&
                                    promoted.selected.app.path == primary->app.path &&
                                    original == rawRows(working);
            auto member = std::find_if(promoted.members.begin(), promoted.members.end(),
                                       [&](const auto &row) {
                                           return row.app.id != primaryId && row.used > 0;
                                       });
            if (member == promoted.members.end())
                throw std::runtime_error("Field copy has no non-primary member to remove");
            auto removedId = member->app.id;
            auto removedUsed = member->used;
            storage.unmergeApp(primaryId, removedId,
                               applicationNameKey(member->app.name) ==
                                       applicationNameKey(primary->app.name)
                                   ? L"独立来源"
                                   : std::wstring{});
            auto separated = storage.snapshot(day, true, primaryId);
            bool separatedCorrect =
                separated.selectedTotal == promoted.selectedTotal - removedUsed &&
                storage.snapshot(day, true, removedId).selectedTotal == removedUsed &&
                separated.total == promoted.total && original == rawRows(working);
            storage.mergeApps(removedId, primaryId);
            bool restored = totals(storage.snapshot(day, true, primaryId)) == expectedPromoted &&
                            original == rawRows(working);
            bool passed = rowsUnchanged && totalsCorrect && namePreserved && repeated &&
                          primaryPreserved && separatedCorrect && restored;
            std::ofstream report(output, std::ios::binary);
            if (!report)
                throw std::runtime_error("Cannot write field probe report");
            report << "{\n  \"target_name\": " << json(utf8(target.name))
                   << ",\n  \"week_containing\": " << json(day)
                   << ",\n  \"source_count\": " << sources.size()
                   << ",\n  \"intervals\": " << original.size()
                   << ",\n  \"target_before_ms\": " << oldTarget
                   << ",\n  \"target_expected_ms\": " << expected[target.id]
                   << ",\n  \"target_after_ms\": " << after.selectedTotal
                   << ",\n  \"raw_intervals_unchanged\": " << (rowsUnchanged ? "true" : "false")
                   << ",\n  \"all_group_totals_correct\": " << (totalsCorrect ? "true" : "false")
                   << ",\n  \"target_identity_preserved\": " << (namePreserved ? "true" : "false")
                   << ",\n  \"repeat_is_noop\": " << (repeated ? "true" : "false")
                   << ",\n  \"primary_uses_source_name_and_icon\": "
                   << (primaryPreserved ? "true" : "false")
                   << ",\n  \"separation_restores_member_usage\": "
                   << (separatedCorrect ? "true" : "false")
                   << ",\n  \"remerge_preserves_totals\": " << (restored ? "true" : "false")
                   << ",\n  \"passed\": " << (passed ? "true" : "false") << "\n}\n";
            std::cout << "Merged " << sources.size()
                      << " directory-related records on an isolated copy\n";
            result = passed ? 0 : 1;
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return result;
}
