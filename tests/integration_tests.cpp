#include "platform.h"
#include "version.h"
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
int checks{};
void require(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
std::wstring read(HKEY root, const wchar_t *key, const wchar_t *name) {
    wchar_t value[4096]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return {};
    return value;
}
FILETIME modified(HKEY root, const wchar_t *key) {
    HKEY h{};
    FILETIME time{};
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, &time);
        RegCloseKey(h);
    }
    return time;
}
} // namespace
int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto key = L"Software\\TimeTrace.Test." + std::to_wstring(GetCurrentProcessId());
    HKEY root{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                        &root, nullptr) != ERROR_SUCCESS)
        return 2;
    int result{};
    auto directory = std::filesystem::temp_directory_path() /
                     (L"TimeTrace integration 空格 " + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    auto exe = directory / L"TimeTrace.exe", uninstaller = directory / L"Uninstall.exe",
         shortcut = directory / L"TimeTrace.lnk";
    try {
        std::ofstream(exe) << "test";
        require(!tt::repairInstallation(exe, shortcut, root),
                "Portable executable does not create installation registration");
        std::ofstream(uninstaller) << "test";
        require(tt::repairInstallation(exe, shortcut, root),
                "Install registration succeeds in an isolated registry root");
        constexpr auto protocol = L"Software\\Classes\\timetrace\\shell\\open\\command";
        constexpr auto identity = L"Software\\Classes\\AppUserModelId\\TimeTrace.Desktop";
        constexpr auto uninstall =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeTrace";
        require(read(root, protocol, nullptr) == L"\"" + exe.wstring() + L"\" \"%1\"",
                "Unicode and spaced protocol path is quoted correctly");
        require(read(root, identity, L"IconUri") == exe.wstring() &&
                    std::filesystem::exists(shortcut),
                "Notification identity and shortcut are registered");
        require(read(root, uninstall, L"InstallLocation") == directory.wstring() &&
                    read(root, uninstall, L"UninstallString") ==
                        L"\"" + uninstaller.wstring() + L"\"",
                "Uninstaller resolves to the owning installation");
        constexpr auto run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        require(read(root, run, L"TimeTrace").empty(),
                "Installation keeps startup disabled by default");
        HKEY runKey{};
        RegCreateKeyExW(root, run, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &runKey, nullptr);
        const wchar_t stale[] = L"\"C:\\old location\\TimeTrace.exe\" --background";
        RegSetValueExW(runKey, L"TimeTrace", 0, REG_SZ, reinterpret_cast<const BYTE *>(stale),
                       sizeof(stale));
        RegCloseKey(runKey);
        require(tt::repairInstallation(exe, shortcut, root) &&
                    read(root, run, L"TimeTrace") == L"\"" + exe.wstring() + L"\" --background",
                "An enabled startup entry follows an installation move");
        auto registryTime = modified(root, uninstall);
        auto linkTime = std::filesystem::last_write_time(shortcut);
        Sleep(20);
        require(tt::repairInstallation(exe, shortcut, root), "Repeated startup check succeeds");
        auto after = modified(root, uninstall);
        require(CompareFileTime(&registryTime, &after) == 0 &&
                    linkTime == std::filesystem::last_write_time(shortcut),
                "Valid registration and shortcut are not rewritten");
        RegDeleteTreeW(root, L"Software\\Classes\\timetrace");
        RegDeleteTreeW(root, identity);
        RegDeleteTreeW(root, uninstall);
        std::filesystem::remove(shortcut);
        require(
            tt::repairInstallation(exe, shortcut, root) &&
                read(root, protocol, nullptr) == L"\"" + exe.wstring() + L"\" \"%1\"" &&
                read(root, identity, L"DisplayName") == L"TimeTrace" &&
                read(root, uninstall, L"DisplayVersion") == tt::wide(TIMETRACE_VERSION),
            "Missing protocol, identity, uninstaller registration and shortcut recover together");
        std::cout << "PASS: " << checks << " integration checks (isolated registry)\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        result = 1;
    }
    RegCloseKey(root);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    for (const auto &file : {exe, uninstaller, shortcut})
        std::filesystem::remove(file);
    std::filesystem::remove(directory);
    CoUninitialize();
    return result;
}
