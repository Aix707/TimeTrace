#pragma once
#include "storage.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace tt {
constexpr UINT WM_SNAPSHOT = WM_APP + 20, WM_NOTICE = WM_APP + 21, WM_SHOW = WM_APP + 22,
               WM_TRAY = WM_APP + 23, WM_STOP = WM_APP + 24;
struct Sample {
    Millis tick{}, utc{}, lastInput{};
    HWND window{};
    DWORD pid{};
    bool paused{}, locked{}, sleeping{};
};
struct Command {
    enum Kind {
        Observe,
        Query,
        ReleaseView,
        SetReminder,
        RemoveReminder,
        ActOnReminder,
        Clear,
        Stop,
        Rename,
        Merge
    } kind{Observe};
    Sample sample;
    std::string app, day;
    Millis value{};
    bool weekly{};
    std::wstring name;
    std::string target;
    bool enabled{true};
    ReminderAction action;
    uint64_t request{};
};
class AppResolver {
  public:
    AppResolver();
    ~AppResolver();
    AppInfo resolve(HWND window, DWORD pid);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Service {
  public:
    Service(std::filesystem::path directory, HWND target);
    ~Service();
    void send(Command command);

  private:
    std::filesystem::path directory_;
    HWND target_{};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Command> queue_;
    std::atomic<bool> alive_{true};
    std::thread thread_;
    void run();
};
std::filesystem::path executablePath();
std::filesystem::path dataPath();
Sample captureSample(bool paused, bool locked, bool sleeping, Millis eventTick = -1,
                     HWND eventWindow = nullptr);
bool autoStartEnabled();
bool setAutoStart(bool enabled);
bool showToast(const ReminderEvent &event);
bool registerShortcut();
std::shared_ptr<void> loadAppIcon(const std::wstring &path);
bool createAppShortcut(const std::filesystem::path &exe, const std::filesystem::path &shortcut);
bool repairInstallation(const std::filesystem::path &exe,
                        const std::filesystem::path &shortcut = {},
                        HKEY userKey = HKEY_CURRENT_USER);
} // namespace tt
