#include "storage.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <windows.h>

using namespace tt;
using Clock = std::chrono::steady_clock;
template <class F> double measure(int count, F operation) {
    auto start = Clock::now();
    for (int i = 0; i < count; ++i)
        operation(i);
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count() / count;
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    auto dir = std::filesystem::temp_directory_path() /
               (L"TimeTrace-benchmark-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    try {
        double seedMs{}, writeMs{}, dayMs{}, weekMs{};
        Millis check{};
        constexpr Millis start =
            1788220800000; // Fixed synthetic fixture, independent of live data.
        {
            Storage db(dir / L"data.db");
            for (int i = 0; i < 48; ++i)
                db.app({"app" + std::to_string(i), L"App " + std::to_wstring(i), L"test.exe"});
            std::vector<Usage> entries;
            for (int d = 0; d < 60; ++d)
                for (int i = 0; i < 1200; ++i)
                    entries.push_back({"app" + std::to_string(i % 48),
                                       start + d * 86400000LL + i * 60000LL, 5000});
            seedMs = measure(1, [&](int) { db.save(entries); });
            auto day = localDay(start + 59 * 86400000LL);
            // Warm all query paths before timing. Each read must produce the same total.
            db.snapshot(day, false);
            db.snapshot(day, true, "app3");
            dayMs = measure(200, [&](int) { check += db.snapshot(day, false).total; });
            weekMs =
                measure(200, [&](int) { check += db.snapshot(day, true, "app3").selectedTotal; });
            writeMs = measure(240, [&](int i) {
                auto tick = start + 60 * 86400000LL + i * 5000LL;
                db.commit({{"app0", tick, 2000}, {"app1", tick + 2000, 3000}},
                          {{1, localDay(tick), 3600000, false, false, {}}});
            });
        }
        std::ofstream out(std::filesystem::path(argv[1]), std::ios::trunc);
        out << "{\"intervals\":72000,\"applications\":48,\"seed_ms\":" << seedMs
            << ",\"commit_ms\":" << writeMs << ",\"daily_query_ms\":" << dayMs
            << ",\"weekly_query_ms\":" << weekMs << ",\"checksum\":" << check << "}\n";
        if (!out)
            throw std::runtime_error("Cannot write benchmark output");
        std::filesystem::remove(dir / L"data.db");
        std::filesystem::remove(dir);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
