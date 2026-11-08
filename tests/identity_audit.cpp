#include "platform.h"
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace tt;
namespace {
std::string json(const std::string &text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"')
            result += '\\';
        if (c >= 32)
            result += static_cast<char>(c);
        else {
            char buffer[7]{};
            sprintf_s(buffer, "\\u%04x", c);
            result += buffer;
        }
    }
    return result + '"';
}
std::vector<AppInfo> readApps(const std::filesystem::path &path) {
    sqlite3 *db{};
    if (sqlite3_open_v2(utf8(path.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Cannot read database copy");
    }
    sqlite3_stmt *query{};
    if (sqlite3_prepare_v2(db, "SELECT id,name,path FROM apps ORDER BY id", -1, &query, nullptr) !=
        SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("Cannot read application catalog");
    }
    std::vector<AppInfo> apps;
    while (sqlite3_step(query) == SQLITE_ROW) {
        auto text = [&](int column) {
            auto p = sqlite3_column_text(query, column);
            return p ? std::string(reinterpret_cast<const char *>(p)) : std::string{};
        };
        apps.push_back({text(0), wide(text(1)), wide(text(2))});
    }
    sqlite3_finalize(query);
    sqlite3_close(db);
    return apps;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "identity_audit INPUT_COPY.db OUTPUT.json [--apply]\n";
        return 2;
    }
    auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result{};
    try {
        auto input = std::filesystem::absolute(argv[1]).lexically_normal();
        bool apply = argc == 4 && std::wstring(argv[3]) == L"--apply";
        if (argc == 4 && !apply)
            throw std::runtime_error("Unknown audit argument");
        if (apply && input.wstring().find(L"\\test-output\\") == std::wstring::npos)
            throw std::runtime_error("--apply requires a database copy under test-output");
        auto apps = readApps(input);
        AppResolver resolver;
        std::unique_ptr<Storage> storage;
        if (apply)
            storage = std::make_unique<Storage>(input);
        std::ofstream output(argv[2], std::ios::binary);
        if (!output)
            throw std::runtime_error("Cannot write audit output");
        output << "{\n\"apps\":[\n";
        size_t identified{};
        for (size_t i = 0; i < apps.size(); ++i) {
            auto match = resolver.identify(apps[i]);
            identified += !match.representative.id.empty();
            auto canonical =
                storage ? storage->registerApp(match.source, match.representative) : std::string{};
            output << (i ? ",\n" : "") << "{\"source\":" << json(apps[i].id)
                   << ",\"name\":" << json(utf8(apps[i].name))
                   << ",\"representative\":" << json(match.representative.id)
                   << ",\"display_name\":" << json(utf8(match.representative.name))
                   << ",\"evidence\":" << json(match.evidence)
                   << ",\"canonical\":" << json(canonical) << '}';
        }
        output << "\n],\n\"count\":" << apps.size() << ",\"identified\":" << identified << "\n}\n";
        std::cout << "Audited " << apps.size() << " sources, identified " << identified << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return result;
}
