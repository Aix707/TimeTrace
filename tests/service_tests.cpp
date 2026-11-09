#include "platform.h"
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <iostream>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <stdexcept>

using namespace tt;
namespace {
int checks{};
std::vector<ReminderEvent> notices;
void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
void setWindowAppId(HWND window, const wchar_t *id) {
    IPropertyStore *properties{};
    require(SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties))),
            "Open isolated window identity properties");
    PROPVARIANT value{};
    auto status = id ? InitPropVariantFromString(id, &value) : S_OK;
    if (SUCCEEDED(status))
        status = properties->SetValue(PKEY_AppUserModel_ID, value);
    PropVariantClear(&value);
    properties->Release();
    require(SUCCEEDED(status), "Set isolated window identity");
}
Snapshot waitSnapshot(HWND host, uint64_t request, bool allowError = false) {
    auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, host, WM_SNAPSHOT, WM_NOTICE, PM_REMOVE)) {
            if (message.message == WM_NOTICE) {
                std::unique_ptr<ReminderEvent> event(
                    reinterpret_cast<ReminderEvent *>(message.lParam));
                notices.push_back(std::move(*event));
                continue;
            }
            std::unique_ptr<Snapshot> snapshot(reinterpret_cast<Snapshot *>(message.lParam));
            if (!snapshot->error.empty() && !allowError)
                throw std::runtime_error(utf8(snapshot->error));
            if (snapshot->request == request)
                return std::move(*snapshot);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_POSTMESSAGE);
    }
    throw std::runtime_error("Service did not return the latest query");
}
struct WriteLock {
    sqlite3 *db{};
    explicit WriteLock(const std::filesystem::path &file) {
        if (sqlite3_open(utf8(file.wstring()).c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("Open isolated lock connection");
        exec("BEGIN IMMEDIATE");
    }
    ~WriteLock() {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    void exec(const char *sql) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
};
void observe(Service &service, HWND host, Millis tick, Millis utc) {
    Command command;
    command.sample = {tick, utc, tick, host, GetCurrentProcessId()};
    service.send(command);
}
Snapshot query(Service &service, HWND host, const std::string &day, uint64_t request,
               const std::string &selected = {}) {
    Command command{Command::Query};
    command.day = day;
    command.request = request;
    command.app = selected;
    service.send(command);
    return waitSnapshot(host, request);
}
} // namespace
int wmain() {
    auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto dir = std::filesystem::temp_directory_path() /
               (L"TimeTrace-service-" + std::to_wstring(GetCurrentProcessId()));
    auto host = CreateWindowExW(0, L"STATIC", L"Service test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    int result{};
    try {
        require(SUCCEEDED(apartment), "Initialize isolated test apartment");
        require(host != nullptr, "Create isolated message target");
        auto utc = utcNow(), tick = static_cast<Millis>(GetTickCount64());
        // Keep the entire synthetic test on one local day.
        while (localDay(utc + 60000) != localDay(utc))
            utc -= 60000;
        auto day = localDay(utc);
        {
            AppResolver resolver;
            auto native = resolver.resolve(host, GetCurrentProcessId());
            auto repeated = resolver.resolve(host, GetCurrentProcessId());
            require(!native.source.path.empty() && repeated.source.id == native.source.id &&
                        repeated.representative.id == native.representative.id,
                    "Repeated live process resolution preserves cached application identity");
            setWindowAppId(host, L"TimeTrace.ServiceFixture.One");
            auto first = resolver.resolve(host, GetCurrentProcessId());
            setWindowAppId(host, L"TimeTrace.ServiceFixture.Two");
            auto second = resolver.resolve(host, GetCurrentProcessId());
            require(first.source.id == "aumid:TimeTrace.ServiceFixture.One" &&
                        second.source.id == "aumid:TimeTrace.ServiceFixture.Two" &&
                        first.representative.id != second.representative.id,
                    "A changed window AppID invalidates the process result cache");
            setWindowAppId(host, nullptr);
            require(resolver.resolve(host, GetCurrentProcessId()).source.id == native.source.id,
                    "Clearing a window AppID restores the process application identity");
            wchar_t systemDirectory[MAX_PATH]{};
            GetSystemDirectoryW(systemDirectory, MAX_PATH);
            auto executable = std::wstring(systemDirectory) + L"\\cmd.exe";
            auto commandLine = L"\"" + executable + L"\" /d /c exit";
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            require(CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
                                   FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                                   &startup, &process) != FALSE,
                    "Launch isolated short-lived identity fixture");
            auto live = resolver.resolve(nullptr, process.dwProcessId);
            ResumeThread(process.hThread);
            auto exited = WaitForSingleObject(process.hProcess, 10000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            auto expired = resolver.resolve(nullptr, process.dwProcessId);
            require(exited == WAIT_OBJECT_0 && !live.source.path.empty() &&
                        expired.source.id != live.source.id && expired.source.path.empty(),
                    "An exited process cannot reuse a cached executable identity");
        }
        {
            Service service(dir, host);
            for (int i = 0; i <= 100; ++i) {
                Command observation;
                observation.sample = {tick + i * 500LL, utc + i * 500LL, tick + i * 500LL, host,
                                      GetCurrentProcessId()};
                service.send(std::move(observation));
                for (int j = 0; j < 20; ++j) {
                    Command query{Command::Query};
                    query.day = day;
                    query.request = 1 + i * 20 + j;
                    query.weekly = j % 2 != 0;
                    service.send(std::move(query));
                }
            }
            service.send({Command::ReleaseView});
            Command final{Command::Query};
            final.day = day;
            final.request = 10001;
            service.send(final);
            auto snapshot = waitSnapshot(host, final.request);
            require(snapshot.total == 50000 && !snapshot.weekly,
                    "Query coalescing preserves every 500ms observation and latest selection");
            require(snapshot.apps.size() == 1 && snapshot.catalog.size() == 1,
                    "Metadata and icon cache survive release and reopen");
            Command clear{Command::Clear};
            service.send(clear);
            final.request = 10002;
            service.send(final);
            snapshot = waitSnapshot(host, final.request);
            require(snapshot.total == 0 && snapshot.catalog.empty(),
                    "Clear resets usage and hides classifications without records");
            Command paused;
            paused.sample = {tick + 60000, utc + 60000,           tick + 60000,
                             host,         GetCurrentProcessId(), true};
            service.send(paused);
            final.request = 10003;
            service.send(final);
            snapshot = waitSnapshot(host, final.request);
            require(snapshot.total == 0 && snapshot.paused,
                    "Paused observations do not accumulate after clear");
        }
        {
            Storage db(dir / L"data.db");
            require(db.getSetting("paused") == "1" && db.snapshot(day, false).total == 0,
                    "Service shutdown flushes state in the isolated profile");
        }
        {
            auto folder = dir / L"failed-start";
            std::filesystem::create_directories(folder / L"data.db");
            Service service(folder, host);
            auto failure = waitSnapshot(host, 0, true);
            require(!failure.error.empty() && !service.finish(),
                    "Fatal initialization publishes an unrequested error and cannot report a "
                    "successful final save");
        }
        {
            Service service(dir / L"registration", host);
            query(service, host, day, 20001);
            WriteLock lock(dir / L"registration" / L"data.db");
            observe(service, host, tick, utc);
            auto failure = waitSnapshot(host, 0, true);
            require(failure.error.find(L"locked") != std::wstring::npos,
                    "First application registration encounters injected write lock");
            lock.exec("ROLLBACK");
            observe(service, host, tick + 1000, utc + 1000);
            observe(service, host, tick + 2000, utc + 2000);
            auto recovered = query(service, host, day, 20002);
            require(recovered.total == 2000 && recovered.apps.size() == 1,
                    "Failed registration retries without losing captured activity or foreign keys");
            require(query(service, host, day, 20003).total == 2000,
                    "Repeated query after recovery does not double count");
        }
        {
            Millis lo = utc, hi = utc + 86400000;
            while (hi - lo > 1) {
                auto mid = lo + (hi - lo) / 2;
                if (localDay(mid) == day)
                    lo = mid;
                else
                    hi = mid;
            }
            Service service(dir / L"midnight", host);
            query(service, host, day, 30001);
            observe(service, host, tick, hi - 500);
            query(service, host, day, 30002);
            WriteLock lock(dir / L"midnight" / L"data.db");
            observe(service, host, tick + 1000, hi + 500);
            auto failure = waitSnapshot(host, 0, true);
            require(failure.error.find(L"locked") != std::wstring::npos,
                    "Midnight transition encounters injected write lock");
            lock.exec("ROLLBACK");
            observe(service, host, tick + 2000, hi + 1500);
            require(query(service, host, day, 30003).total == 500,
                    "Retry preserves exactly the 500ms before midnight");
            require(query(service, host, localDay(hi), 30004).total == 1500,
                    "Retry preserves exactly the 1500ms after midnight");
        }
        {
            auto folder = dir / L"reminder";
            {
                Storage db(folder / L"data.db");
                db.setReminder({}, 1000);
            }
            Service service(folder, host);
            query(service, host, day, 40001);
            observe(service, host, tick, utc);
            query(service, host, day, 40002);
            auto noticeCount = notices.size();
            WriteLock lock(folder / L"data.db");
            observe(service, host, tick + 1000, utc + 1000);
            auto failure = waitSnapshot(host, 0, true);
            require(failure.error.find(L"locked") != std::wstring::npos &&
                        notices.size() == noticeCount,
                    "Failed atomic reminder commit emits no notification");
            lock.exec("ROLLBACK");
            auto recovered = query(service, host, day, 40003);
            require(recovered.total == 1000 && notices.size() == noticeCount + 1,
                    "Recovered reminder emits once with captured time preserved");
            require(recovered.reminders.front().state.notified &&
                        recovered.reminders.front().state.token == notices.back().token,
                    "Published occurrence matches committed reminder state");
            query(service, host, day, 40004);
            require(notices.size() == noticeCount + 1,
                    "Snapshot queries cannot duplicate recovered notification");
            require(service.finish(), "Explicit finish confirms successful final save");
        }
        {
            auto folder = dir / L"mapping";
            {
                Storage db(folder / L"data.db");
                db.app({"fixture-source", L"Source", {}});
                db.app({"fixture-target", L"Target", {}});
                db.app({"fixture-other", L"Other", {}});
                db.save({{"fixture-source", utc - 90000, 30000},
                         {"fixture-target", utc - 60000, 35000},
                         {"fixture-other", utc - 25000, 15000}});
                db.setReminder("fixture-source", 60000);
            }
            Service service(folder, host);
            query(service, host, day, 50001);
            Command candidates{Command::Query};
            candidates.day = day;
            candidates.request = 50004;
            candidates.mergeFor = "fixture-target";
            service.send(candidates);
            auto selection = waitSnapshot(host, candidates.request);
            require(
                selection.mergeFor == "fixture-target" && selection.mergeCandidates.size() == 2 &&
                    selection.mergeCandidates[0].used + selection.mergeCandidates[1].used == 45000,
                "On-demand merge candidates retain identity and selected-period usage");
            Command merge{Command::Merge};
            merge.mergeApps = {"fixture-source", "fixture-other"};
            merge.target = "fixture-target";
            service.send(merge);
            auto merged = query(service, host, day, 50002);
            auto rule =
                std::find_if(merged.reminders.begin(), merged.reminders.end(),
                             [](const auto &view) { return view.rule.kind == ReminderKind::App; });
            require(rule != merged.reminders.end() && rule->rule.app == "fixture-target" &&
                        rule->used == 80000,
                    "Mapping reload transfers reminder to combined canonical usage");
            auto noticeCount = notices.size();
            Command desktop;
            desktop.sample = {tick, utc, tick, nullptr, 0};
            service.send(desktop);
            query(service, host, day, 50003);
            require(notices.size() == noticeCount + 1 &&
                        notices.back().rule.app == "fixture-target",
                    "Regrouped reminder evaluates against current application mapping");
            auto grouped = query(service, host, day, 50005, "fixture-target");
            require(grouped.members.size() == 3 && grouped.selectedTotal == 80000 &&
                        grouped.members.front().app.id == "fixture-target",
                    "Detail query exposes group members with the current primary first");
            Command primary{Command::SetPrimary};
            primary.app = "fixture-target";
            primary.target = "fixture-source";
            service.send(primary);
            auto promoted = query(service, host, day, 50006, "fixture-source");
            require(promoted.selected.app.id == "fixture-source" &&
                        promoted.selectedTotal == 80000 && promoted.members.size() == 3,
                    "Setting primary preserves group usage and refreshes detail identity");
            Command separate{Command::Unmerge};
            separate.app = "fixture-source";
            separate.target = "fixture-other";
            service.send(separate);
            auto separated = query(service, host, day, 50007, "fixture-source");
            require(separated.selectedTotal == 65000 && separated.members.size() == 2 &&
                        query(service, host, day, 50008, "fixture-other").selectedTotal == 15000 &&
                        separated.total == 80000,
                    "Removing one member restores independent usage without changing totals");
            require(notices.size() == noticeCount + 1,
                    "Editing group membership does not replay a delivered reminder");
            service.send({Command::Clear});
            auto cleared = query(service, host, day, 50009);
            require(cleared.total == 0 && cleared.catalog.empty() &&
                        std::all_of(cleared.reminders.begin(), cleared.reminders.end(),
                                    [](const auto &reminder) { return !reminder.name.empty(); }),
                    "Clearing usage hides empty classifications while retaining named reminder "
                    "settings");
        }
        {
            auto folder = dir / L"icons";
            auto path = executablePath().wstring();
            {
                Storage db(folder / L"data.db");
                db.app({"app:icon-primary", L"Icon primary", path});
                db.app({"app:icon-member", L"Icon member", path});
                db.save({{"app:icon-primary", utc - 2000, 1000},
                         {"app:icon-member", utc - 1000, 1000}});
                db.mergeApps("app:icon-member", "app:icon-primary");
                db.setReminder("app:icon-primary", 3600000);
            }
            Service service(folder, host);
            auto detail = query(service, host, day, 60001, "app:icon-primary");
            require(detail.selected.icon && !detail.memberIcons && detail.members.size() == 2 &&
                        std::none_of(detail.members.begin(), detail.members.end(),
                                     [](const auto &member) { return !!member.icon; }),
                    "Ordinary detail queries retain members without loading member icons");
            Command management{Command::Query};
            management.day = day;
            management.app = "app:icon-primary";
            management.request = 60002;
            management.memberIcons = true;
            service.send(management);
            auto members = waitSnapshot(host, management.request);
            require(members.memberIcons && members.members.size() == 2 &&
                        std::all_of(members.members.begin(), members.members.end(),
                                    [&](const auto &member) {
                                        return member.icon == members.selected.icon;
                                    }),
                    "Member management explicitly loads icons and shares identical paths");
            Command reminders{Command::Query};
            reminders.day = shiftDay(day, -1);
            reminders.request = 60003;
            reminders.iconApps = {"app:icon-primary"};
            service.send(reminders);
            auto historical = waitSnapshot(host, reminders.request);
            auto appReminder = std::find_if(
                historical.reminders.begin(), historical.reminders.end(),
                [](const auto &view) { return view.rule.kind == ReminderKind::App; });
            require(historical.apps.empty() && appReminder != historical.reminders.end() &&
                        appReminder->icon == detail.selected.icon,
                    "Reminder icons come from their application outside the selected period");
            {
                WriteLock lock(folder / L"data.db");
                lock.exec("UPDATE apps SET path='' WHERE id='app:icon-primary'");
                lock.exec("COMMIT");
            }
            Command rename{Command::Rename};
            rename.app = "app:icon-primary";
            rename.name = L"Icon primary updated";
            service.send(rename);
            auto updated = query(service, host, day, 60004, "app:icon-primary");
            require(updated.selected.app.path.empty() && !updated.selected.icon,
                    "A path change invalidates the icon without changing application identity");
            service.send({Command::ReleaseView});
            management.request = 60005;
            service.send(management);
            auto reopened = waitSnapshot(host, management.request);
            require(reopened.memberIcons && !reopened.selected.icon &&
                        reopened.members.size() == 2 && reopened.members[1].icon,
                    "Reopening member management reloads icons after releasing view resources");
            Command primary{Command::SetPrimary};
            primary.app = "app:icon-primary";
            primary.target = "app:icon-member";
            service.send(primary);
            service.send({Command::Clear});
            reminders.request = 60006;
            reminders.iconApps = {"app:icon-member"};
            service.send(reminders);
            auto cleared = waitSnapshot(host, reminders.request);
            appReminder = std::find_if(
                cleared.reminders.begin(), cleared.reminders.end(),
                [](const auto &view) { return view.rule.kind == ReminderKind::App; });
            require(cleared.total == 0 && cleared.catalog.empty() &&
                        appReminder != cleared.reminders.end() && appReminder->icon &&
                        appReminder->name == L"Icon member",
                    "Clearing history retains reminder names and icons without empty "
                    "application classifications");
        }
        std::cout << "PASS: " << checks << " service checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n';
        result = 1;
    }
    MSG message{};
    while (PeekMessageW(&message, host, WM_SNAPSHOT, WM_NOTICE, PM_REMOVE)) {
        if (message.message == WM_SNAPSHOT)
            delete reinterpret_cast<Snapshot *>(message.lParam);
        else
            delete reinterpret_cast<ReminderEvent *>(message.lParam);
    }
    DestroyWindow(host);
    for (auto child : {L"", L"registration", L"midnight", L"reminder", L"mapping",
                       L"failed-start", L"icons"}) {
        auto folder = dir / child;
        for (auto name : {L"data.db", L"data.db-wal", L"data.db-shm"})
            std::filesystem::remove(folder / name);
        if (*child)
            std::filesystem::remove(folder);
    }
    std::filesystem::remove(dir);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return result;
}
