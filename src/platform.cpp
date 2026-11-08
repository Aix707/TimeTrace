#include "platform.h"
#include <shlobj.h>
#include <algorithm>
#include <appmodel.h>
#include <cwctype>
#include <memory>
#include <propkey.h>
#include <propvarutil.h>
#include <propsys.h>
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
    ExecutableMetadata metadata;
    std::wstring appId;
    ~ProcessEntry() {
        if (handle)
            CloseHandle(handle);
    }
};
static std::wstring windowAppId(HWND window) {
    if (!window)
        return {};
    IPropertyStore *properties{};
    std::wstring result;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties)))) {
        PROPVARIANT value{};
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value))) {
            PWSTR text{};
            if (SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text) {
                result = text;
                CoTaskMemFree(text);
            }
        }
        PropVariantClear(&value);
        properties->Release();
    }
    return result.size() <= 512 ? result : L"";
}
struct AppResolver::Impl {
    std::unordered_map<DWORD, std::unique_ptr<ProcessEntry>> cache_;
    std::unordered_map<std::wstring, ExecutableMetadata> metadata_;
    IdentityCatalog catalog_;
    std::string session_ = std::to_string(utcNow());

    ExecutableMetadata metadata(const std::wstring &path) {
        auto key = identityPath(path);
        auto found = metadata_.find(key);
        if (found != metadata_.end())
            return found->second;
        auto info = executableMetadata(path);
        // Missing or newly installed paths can become accessible on a later observation.
        if (info.exists) {
            if (metadata_.size() >= 256)
                metadata_.erase(metadata_.begin());
            metadata_[key] = info;
        }
        return info;
    }
    ResolvedApp frame(HWND hwnd, DWORD pid, const std::wstring &appId) {
        AppInfo source{"unknown-frame:" + std::to_string(pid) + ":" +
                           std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + ":" + session_,
                       L"未识别的系统应用窗口",
                       {}};
        if (!appId.empty())
            source.id = "aumid:" + utf8(appId);
        return catalog_.identify(source, {{}, appId, !appId.empty()});
    }

  public:
    ResolvedApp identify(const AppInfo &source) {
        return catalog_.identify(source, {metadata(source.path), {}, false});
    }
    ResolvedApp resolve(HWND hwnd, DWORD pid) {
        if (!pid)
            return {};
        DWORD windowPid{};
        if (hwnd && (!GetWindowThreadProcessId(hwnd, &windowPid) || windowPid != pid))
            hwnd = nullptr;
        auto appId = windowAppId(hwnd);
        if (appId.empty() && hwnd) {
            auto owner = GetAncestor(hwnd, GA_ROOTOWNER);
            DWORD ownerPid{};
            GetWindowThreadProcessId(owner, &ownerPid);
            if (owner != hwnd && ownerPid == pid)
                appId = windowAppId(owner);
        }
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
                return frame(hwnd, pid, appId);
        }
        auto found = cache_.find(pid);
        if (found != cache_.end()) {
            if (!found->second->handle ||
                WaitForSingleObject(found->second->handle, 0) != WAIT_TIMEOUT) {
                cache_.erase(found);
                found = cache_.end();
            }
        }
        std::unique_ptr<ProcessEntry> fresh;
        ProcessEntry *entry{};
        if (found != cache_.end()) {
            entry = found->second.get();
        } else {
            fresh = std::make_unique<ProcessEntry>();
            entry = fresh.get();
            entry->handle =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            std::wstring path(entry->handle ? 32768 : 0, 0);
            DWORD length = static_cast<DWORD>(path.size());
            if (entry->handle &&
                QueryFullProcessImageNameW(entry->handle, 0, path.data(), &length)) {
                path.resize(length);
                auto filename = std::filesystem::path(path).filename().wstring();
                if (_wcsicmp(filename.c_str(), L"ApplicationFrameHost.exe") == 0)
                    return frame(hwnd, pid, appId);
                entry->app.path = path;
                entry->app.id = "exe:" + utf8(identityPath(path));
                if (entry->app.id == "exe:") {
                    auto lowered = path;
                    std::transform(lowered.begin(), lowered.end(), lowered.begin(), towlower);
                    entry->app.id += utf8(lowered);
                }
                UINT32 size{};
                if (GetApplicationUserModelId(entry->handle, &size, nullptr) ==
                        ERROR_INSUFFICIENT_BUFFER &&
                    size > 1 && size <= 512) {
                    std::wstring id(size, 0);
                    if (GetApplicationUserModelId(entry->handle, &size, id.data()) ==
                        ERROR_SUCCESS) {
                        id.resize(size - 1);
                        entry->appId = id;
                        entry->app.id = "aumid:" + utf8(id);
                    }
                }
                entry->metadata = metadata(path);
                entry->app.name = executableDisplayName(entry->metadata, path);
            } else {
                FILETIME created{}, exit{}, kernel{}, user{};
                std::string birth = session_;
                if (entry->handle &&
                    GetProcessTimes(entry->handle, &created, &exit, &kernel, &user))
                    birth = std::to_string((static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                                           created.dwLowDateTime);
                entry->app.id = "unknown:" + std::to_string(pid) + ":" + birth;
                entry->app.name = L"未识别应用";
            }
        }
        auto app = entry->app;
        if (appId.empty())
            appId = entry->appId;
        if (!appId.empty())
            app.id = "aumid:" + utf8(appId);
        auto result = catalog_.identify(app, {entry->metadata, appId, !appId.empty()});
        if (fresh && entry->handle && !entry->app.path.empty()) {
            if (cache_.size() >= 128)
                cache_.erase(cache_.begin());
            cache_[pid] = std::move(fresh);
        }
        return result;
    }
};
AppResolver::AppResolver() : impl_(std::make_unique<Impl>()) {}
AppResolver::~AppResolver() = default;
ResolvedApp AppResolver::resolve(HWND window, DWORD pid) {
    return impl_->resolve(window, pid);
}
ResolvedApp AppResolver::identify(const AppInfo &app) {
    return impl_->identify(app);
}
} // namespace tt
