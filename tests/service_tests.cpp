#include "platform.h"
#include <iostream>
#include <stdexcept>

using namespace tt;
namespace {
int checks{};
void require(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
Snapshot waitSnapshot(HWND host, uint64_t request) {
    auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, host, WM_SNAPSHOT, WM_NOTICE, PM_REMOVE)) {
            if (message.message == WM_NOTICE) {
                delete reinterpret_cast<ReminderEvent *>(message.lParam);
                continue;
            }
            std::unique_ptr<Snapshot> snapshot(reinterpret_cast<Snapshot *>(message.lParam));
            if (!snapshot->error.empty())
                throw std::runtime_error(utf8(snapshot->error));
            if (snapshot->request == request)
                return std::move(*snapshot);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_POSTMESSAGE);
    }
    throw std::runtime_error("Service did not return the latest query");
}
} // namespace
int wmain() {
    auto dir = std::filesystem::temp_directory_path() /
               (L"TimeTrace-service-" + std::to_wstring(GetCurrentProcessId()));
    auto host = CreateWindowExW(0, L"STATIC", L"Service test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    int result{};
    try {
        require(host != nullptr, "Create isolated message target");
        auto utc = utcNow(), tick = static_cast<Millis>(GetTickCount64());
        // Keep the entire synthetic test on one local day.
        while (localDay(utc + 60000) != localDay(utc))
            utc -= 60000;
        auto day = localDay(utc);
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
            require(snapshot.total == 0 && snapshot.catalog.size() == 1,
                    "Clear resets usage and keeps application metadata");
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
        std::cout << "PASS: " << checks << " service checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
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
    for (auto name : {L"data.db", L"data.db-wal", L"data.db-shm"})
        std::filesystem::remove(dir / name);
    std::filesystem::remove(dir);
    return result;
}
