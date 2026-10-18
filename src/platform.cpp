#include "platform.h"
#include <shlobj.h>
#include <algorithm>
#include <appmodel.h>
#include <cwctype>
#include <memory>
#include <shellapi.h>

#include <winver.h>

namespace tt {
std::filesystem::path executablePath() {
    std::wstring p(32768, 0);
    auto n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n);
    return p;
}
std::filesystem::path dataPath() {
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        throw std::runtime_error("LocalAppData is unavailable");
    std::filesystem::path result = std::filesystem::path(path) / L"TimeTrace";
    CoTaskMemFree(path);
    return result;
}
Sample captureSample(bool paused, bool locked, bool sleeping, Millis eventTick, HWND eventWindow) {
    Sample s;
    s.tick = static_cast<Millis>(GetTickCount64());
    s.utc = utcNow();
    LASTINPUTINFO input{sizeof(input)};
    GetLastInputInfo(&input);
    s.lastInput = s.tick - static_cast<DWORD>(static_cast<DWORD>(s.tick) - input.dwTime);
    if (eventTick >= 0) {
        s.utc -= s.tick - eventTick;
        s.tick = eventTick;
        s.lastInput = std::min(s.lastInput, s.tick);
    }
    s.window = eventWindow ? eventWindow : GetForegroundWindow();
    if (s.window) {
        GetWindowThreadProcessId(s.window, &s.pid);
        wchar_t cls[128]{};
        GetClassNameW(s.window, cls, 128);
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
            wcscmp(cls, L"Shell_TrayWnd") == 0) {
            s.window = nullptr;
            s.pid = 0;
        }
    }
    s.paused = paused;
    s.locked = locked;
    s.sleeping = sleeping;
    return s;
}
bool autoStartEnabled() {
    DWORD type{}, size{};
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"TimeTrace", RRF_RT_REG_SZ, &type, nullptr, &size) == ERROR_SUCCESS;
}
bool setAutoStart(bool enabled) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    auto command = L"\"" + executablePath().wstring() + L"\" --background";
    LSTATUS status =
        enabled ? RegSetValueExW(key, L"TimeTrace", 0, REG_SZ,
                                 reinterpret_cast<const BYTE *>(command.c_str()),
                                 static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                : RegDeleteValueW(key, L"TimeTrace");
    RegCloseKey(key);
    return status == ERROR_SUCCESS || (!enabled && status == ERROR_FILE_NOT_FOUND);
}
bool registerShortcut() {
    PWSTR programs{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        return false;
    auto path = std::filesystem::path(programs) / L"TimeTrace.lnk";
    CoTaskMemFree(programs);
    return createAppShortcut(executablePath(), path);
}
std::shared_ptr<void> loadAppIcon(const std::wstring &path) {
    if (path.empty())
        return {};
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) ||
        !info.hIcon)
        return {};
    return {info.hIcon, [](void *h) { DestroyIcon(static_cast<HICON>(h)); }};
}
struct ProcessEntry {
    HANDLE handle{};
    AppInfo app;
    ~ProcessEntry() {
        if (handle)
            CloseHandle(handle);
    }
};
static std::wstring description(const std::wstring &path) {
    DWORD ignored{}, size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size || size > 2 * 1024 * 1024)
        return {};
    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data()))
        return {};
    struct Translation {
        WORD language, codepage;
    };
    Translation *trans{};
    UINT length{};
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<void **>(&trans), &length) &&
        length >= sizeof(Translation)) {
        wchar_t key[128]{};
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", trans[0].language,
                   trans[0].codepage);
        wchar_t *text{};
        if (VerQueryValueW(buffer.data(), key, reinterpret_cast<void **>(&text), &length) && text &&
            length > 1)
            return std::wstring(text).substr(0, 160);
    }
    return {};
}
struct AppResolver::Impl {
    std::unordered_map<DWORD, std::unique_ptr<ProcessEntry>> cache_;
    std::string session_ = std::to_string(utcNow());

  public:
    AppInfo resolve(HWND hwnd, DWORD pid) {
        if (!pid)
            return {};
        wchar_t cls[128]{};
        if (hwnd)
            GetClassNameW(hwnd, cls, 128);
        if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
            struct Child {
                DWORD host, result;
            };
            Child child{pid, 0};
            EnumChildWindows(
                hwnd,
                [](HWND w, LPARAM lp) -> BOOL {
                    auto p = reinterpret_cast<Child *>(lp);
                    DWORD id{};
                    GetWindowThreadProcessId(w, &id);
                    wchar_t name[128]{};
                    GetClassNameW(w, name, 128);
                    if (id && id != p->host && wcscmp(name, L"Windows.UI.Core.CoreWindow") == 0) {
                        p->result = id;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&child));
            if (child.result)
                pid = child.result;
            else
                return {"unknown-frame:" + std::to_string(pid) + ":" +
                            std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + ":" + session_,
                        L"未识别的系统应用窗口",
                        {}};
        }
        auto found = cache_.find(pid);
        if (found != cache_.end()) {
            if (found->second->handle &&
                WaitForSingleObject(found->second->handle, 0) == WAIT_TIMEOUT)
                return found->second->app;
            cache_.erase(found);
        }
        if (cache_.size() >= 128)
            cache_.erase(cache_.begin());
        auto entry = std::make_unique<ProcessEntry>();
        entry->handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        std::wstring path(entry->handle ? 32768 : 0, 0);
        DWORD length = static_cast<DWORD>(path.size());
        if (entry->handle && QueryFullProcessImageNameW(entry->handle, 0, path.data(), &length)) {
            path.resize(length);
            auto filename = std::filesystem::path(path).filename().wstring();
            if (_wcsicmp(filename.c_str(), L"ApplicationFrameHost.exe") == 0)
                return {"unknown-frame:" + std::to_string(pid) + ":" +
                            std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + ":" + session_,
                        L"未识别的系统应用窗口",
                        {}};
            entry->app.path = path;
            auto normalized = path;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            entry->app.id = "exe:" + utf8(normalized);
            UINT32 size{};
            if (GetApplicationUserModelId(entry->handle, &size, nullptr) ==
                    ERROR_INSUFFICIENT_BUFFER &&
                size > 1 && size < 32768) {
                std::wstring aumid(size, 0);
                if (GetApplicationUserModelId(entry->handle, &size, aumid.data()) ==
                    ERROR_SUCCESS) {
                    aumid.resize(size - 1);
                    entry->app.id = "aumid:" + utf8(aumid);
                }
            }
            entry->app.name = description(path);
            if (entry->app.name.empty())
                entry->app.name = std::filesystem::path(path).stem().wstring();
        } else {
            FILETIME created{}, exit{}, kernel{}, user{};
            std::string birth = session_;
            if (entry->handle && GetProcessTimes(entry->handle, &created, &exit, &kernel, &user))
                birth = std::to_string((static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                                       created.dwLowDateTime);
            entry->app.id = "unknown:" + std::to_string(pid) + ":" + birth;
            entry->app.name = L"未识别应用 · PID " + std::to_wstring(pid);
        }
        auto app = entry->app;
        // An inaccessible process cannot be safely reused by PID. Do not cache it.
        if (entry->handle)
            cache_[pid] = std::move(entry);
        return app;
    }
};
AppResolver::AppResolver() : impl_(std::make_unique<Impl>()) {}
AppResolver::~AppResolver() = default;
AppInfo AppResolver::resolve(HWND window, DWORD pid) {
    return impl_->resolve(window, pid);
}
} // namespace tt
