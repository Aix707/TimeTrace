#include "platform.h"
#include "version.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <optional>
#include <propkey.h>
#include <propvarutil.h>
#include <string_view>
#include <wrl/client.h>

namespace tt {
namespace {
using Microsoft::WRL::ComPtr;
std::optional<std::wstring> readString(HKEY root, const wchar_t *key, const wchar_t *name) {
    DWORD size{};
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size > 65536)
        return {};
    std::wstring result(size / sizeof(wchar_t), 0);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, result.data(), &size) !=
        ERROR_SUCCESS)
        return {};
    if (!result.empty() && result.back() == 0)
        result.pop_back();
    return result;
}
bool putString(HKEY root, const wchar_t *key, const wchar_t *name, const std::wstring &value) {
    auto old = readString(root, key, name);
    if (old && *old == value)
        return true;
    HKEY h{};
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) !=
        ERROR_SUCCESS)
        return false;
    auto result = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return result == ERROR_SUCCESS;
}
bool putDword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD value) {
    DWORD old{}, size = sizeof(old);
    if (RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &old, &size) == ERROR_SUCCESS &&
        old == value)
        return true;
    HKEY h{};
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) !=
        ERROR_SUCCESS)
        return false;
    auto result = RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value),
                                 sizeof(value));
    RegCloseKey(h);
    return result == ERROR_SUCCESS;
}
} // namespace
bool createAppShortcut(const std::filesystem::path &exe, const std::filesystem::path &shortcut) {
    ComPtr<IShellLinkW> link;
    if (FAILED(
            CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;
    ComPtr<IPersistFile> file;
    ComPtr<IPropertyStore> properties;
    if (FAILED(link.As(&file)) || FAILED(link.As(&properties)))
        return false;
    if (SUCCEEDED(file->Load(shortcut.c_str(), STGM_READ))) {
        wchar_t target[32768]{};
        wchar_t arguments[2]{};
        PROPVARIANT identity{};
        bool valid = SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)) &&
                     _wcsicmp(target, exe.c_str()) == 0 &&
                     SUCCEEDED(link->GetArguments(arguments, 2)) && arguments[0] == 0 &&
                     SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &identity)) &&
                     identity.vt == VT_LPWSTR && identity.pwszVal &&
                     wcscmp(identity.pwszVal, L"TimeTrace.Desktop") == 0;
        PropVariantClear(&identity);
        if (valid)
            return true;
    }
    if (FAILED(link->SetPath(exe.c_str())) || FAILED(link->SetArguments(L"")) ||
        FAILED(link->SetWorkingDirectory(exe.parent_path().c_str())) ||
        FAILED(link->SetDescription(L"TimeTrace · 屏幕时间")) ||
        FAILED(link->SetIconLocation(exe.c_str(), 0)))
        return false;
    PROPVARIANT value{};
    if (FAILED(InitPropVariantFromString(L"TimeTrace.Desktop", &value)))
        return false;
    auto result = properties->SetValue(PKEY_AppUserModel_ID, value);
    PropVariantClear(&value);
    return SUCCEEDED(result) && SUCCEEDED(properties->Commit()) &&
           SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
}
bool repairInstallation(const std::filesystem::path &exe, const std::filesystem::path &shortcut,
                        HKEY userKey) {
    std::error_code error;
    auto uninstaller = exe.parent_path() / L"Uninstall.exe";
    if (!std::filesystem::is_regular_file(exe, error) ||
        !std::filesystem::is_regular_file(uninstaller, error))
        return false;
    auto link = shortcut;
    if (link.empty()) {
        PWSTR programs{};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
            return false;
        link = std::filesystem::path(programs) / L"TimeTrace.lnk";
        CoTaskMemFree(programs);
    }
    constexpr auto identity = L"Software\\Classes\\AppUserModelId\\TimeTrace.Desktop";
    constexpr auto protocol = L"Software\\Classes\\timetrace";
    constexpr auto command = L"Software\\Classes\\timetrace\\shell\\open\\command";
    constexpr auto uninstall =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeTrace";
    std::wstring version;
    for (char c : std::string_view(TIMETRACE_VERSION))
        version += static_cast<wchar_t>(c);
    bool ok = true;
    auto string = [&](const wchar_t *key, const wchar_t *name, const std::wstring &value) {
        ok = putString(userKey, key, name, value) && ok;
    };
    string(identity, L"DisplayName", L"TimeTrace");
    string(identity, L"IconUri", exe.wstring());
    ok = putDword(userKey, identity, L"ShowInSettings", 1) && ok;
    string(protocol, nullptr, L"URL:TimeTrace");
    string(protocol, L"URL Protocol", L"");
    string(command, nullptr, L"\"" + exe.wstring() + L"\" \"%1\"");
    string(uninstall, L"DisplayName", L"TimeTrace");
    string(uninstall, L"DisplayVersion", version);
    string(uninstall, L"Publisher", L"TimeTrace");
    string(uninstall, L"InstallLocation", exe.parent_path().wstring());
    string(uninstall, L"DisplayIcon", exe.wstring());
    string(uninstall, L"UninstallString", L"\"" + uninstaller.wstring() + L"\"");
    string(uninstall, L"QuietUninstallString", L"\"" + uninstaller.wstring() + L"\" /S");
    constexpr auto run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    auto startup = readString(userKey, run, L"TimeTrace");
    if (startup && !startup->empty())
        string(run, L"TimeTrace", L"\"" + exe.wstring() + L"\" --background");
    ok = putDword(userKey, uninstall, L"NoModify", 1) && ok;
    ok = putDword(userKey, uninstall, L"NoRepair", 1) && ok;
    return createAppShortcut(exe, link) && ok;
}
} // namespace tt
