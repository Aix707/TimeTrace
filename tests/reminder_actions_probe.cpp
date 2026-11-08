#include "platform.h"
#include "notification_protocol.h"
#include <windows.h>
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
HWND find(DWORD pid, const wchar_t *cls) {
    struct State {
        DWORD pid;
        const wchar_t *cls;
        HWND h{};
    } s{pid, cls};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto &s = *reinterpret_cast<State *>(l);
            DWORD pid{};
            GetWindowThreadProcessId(h, &pid);
            wchar_t cls[80]{};
            GetClassNameW(h, cls, 80);
            if (pid == s.pid && wcscmp(cls, s.cls) == 0) {
                s.h = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&s));
    return s.h;
}
struct Process {
    PROCESS_INFORMATION p{};
    Process(const std::wstring &exe, const std::wstring &args) {
        STARTUPINFOW si{sizeof(si)};
        auto command = L"\"" + exe + L"\" " + args;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &p))
            throw std::runtime_error("launch");
    }
    ~Process() {
        if (p.hProcess) {
            if (WaitForSingleObject(p.hProcess, 0) == WAIT_TIMEOUT) {
                if (auto h = find(p.dwProcessId, L"TimeTrace.Host"))
                    PostMessageW(h, tt::WM_STOP, 0, 0);
                WaitForSingleObject(p.hProcess, 10000);
            }
            CloseHandle(p.hProcess);
            CloseHandle(p.hThread);
        }
    }
    DWORD exit() {
        if (WaitForSingleObject(p.hProcess, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("sender did not exit");
        DWORD code{};
        GetExitCodeProcess(p.hProcess, &code);
        return code;
    }
    template <class Predicate> bool waitUntil(Predicate predicate) const {
        auto deadline = GetTickCount64() + 10000;
        do {
            if (WaitForSingleObject(p.hProcess, 0) != WAIT_TIMEOUT)
                throw std::runtime_error("application exited before expected state");
            if (predicate())
                return true;
            Sleep(25);
        } while (GetTickCount64() < deadline);
        return false;
    }
    HWND window(const wchar_t *cls) const {
        HWND result{};
        if (!waitUntil([&] {
                result = find(p.dwProcessId, cls);
                return result != nullptr;
            }))
            throw std::runtime_error("application window deadline exceeded");
        return result;
    }
    HWND host() const {
        return window(L"TimeTrace.Host");
    }
    DWORD stop() {
        if (auto h = find(p.dwProcessId, L"TimeTrace.Host")) {
            if (!PostMessageW(h, tt::WM_STOP, 0, 0))
                throw std::runtime_error("stop message failed");
        }
        return exit();
    }
};
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 3)
        return 2;
    std::ofstream out(argv[2]);
    ULONGLONG startupWait{};
    try {
        auto root = std::filesystem::temp_directory_path() /
                    (L"TimeTrace-actions-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        auto path = root / L"data.db";
        auto today = tt::localDay(tt::utcNow());
        int64_t appRule{};
        {
            tt::Storage db(path);
            db.app({"a", L"action fixture", {}});
            db.setReminder("a", 60000);
            db.setSetting("paused", "1");
            db.save({{"a", tt::utcNow() - 120000, 60000}});
            for (auto &r : db.reminderRules())
                if (r.kind == tt::ReminderKind::App)
                    appRule = r.id;
            db.commit({}, {{appRule, today, 0, true, false, "startup-token"}});
        }
        auto args = L"--data-dir \"" + root.wstring() + L"\" ";
        auto state = [&]() {
            tt::Storage db(path);
            for (auto &s : db.reminderStates(today))
                if (s.rule == appRule)
                    return s;
            return tt::ReminderState{};
        };
        {
            Process app(argv[1], args + tt::reminderActionUri({tt::ReminderAction::Snooze, appRule,
                                                               "startup-token"}));
            auto host = app.host();
            auto actionStarted = GetTickCount64();
            bool snoozed = app.waitUntil([&] {
                auto saved = state();
                return saved.next == 360000 && saved.token.empty();
            });
            startupWait = GetTickCount64() - actionStarted;
            require(!find(app.p.dwProcessId, L"TimeTrace.Window"),
                    "Action starts stopped app in tray");
            require(snoozed, "Stopped app accepts persisted snooze occurrence");
            Process duplicate(argv[1], args + tt::reminderActionUri({tt::ReminderAction::MuteToday,
                                                                     appRule, "startup-token"}));
            require(duplicate.exit() == 0, "Existing instance sender acknowledged");
            require(!find(app.p.dwProcessId, L"TimeTrace.Window"),
                    "Action forwarding does not open UI");
            tt::ReminderActionWire wire{};
            wire.version = 999;
            COPYDATASTRUCT data{tt::kReminderCopyData, sizeof(wire), &wire};
            require(!SendMessageW(host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)),
                    "Invalid IPC rejected");
            data.cbData = 1;
            require(!SendMessageW(host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)),
                    "Truncated IPC rejected");
            Process body(argv[1], args + L"timetrace://reminders");
            require(body.exit() == 0, "Body URI delivered");
            auto ui = app.window(L"TimeTrace.Window");
            require(ui != nullptr, "Body click opens window");
            SendMessageW(ui, WM_CLOSE, 0, 0);
            Process old(argv[1], args + L"--limits");
            require(old.exit() == 0, "Legacy command accepted");
            ui = app.window(L"TimeTrace.Window");
            require(ui != nullptr, "Legacy command opens reminder page");
            SendMessageW(ui, WM_CLOSE, 0, 0);
            Process legacyUri(argv[1], args + L"timetrace://limits");
            require(legacyUri.exit() == 0, "Legacy URI accepted");
            ui = app.window(L"TimeTrace.Window");
            require(ui != nullptr, "Legacy URI opens window");
            SendMessageW(ui, WM_CLOSE, 0, 0);
            require(app.stop() == 0, "Clean isolated stop");
            auto saved = state();
            require(!saved.muted && saved.next == 360000, "Replay ignored across instances");
        }
        {
            tt::Storage db(path);
            db.commit({}, {{appRule, today, 0, true, false, "mute-token"}});
        }
        {
            Process app(argv[1], args + tt::reminderActionUri({tt::ReminderAction::MuteToday,
                                                               appRule, "mute-token"}));
            app.host();
            require(app.waitUntil([&] {
                auto saved = state();
                return saved.muted && saved.token.empty();
            }),
                    "Stopped app direct mute persists");
            require(app.stop() == 0, "Direct mute finishes cleanly");
        }
        {
            Process app(argv[1], args + L"--background");
            app.host();
            Process expired(argv[1], args + tt::reminderActionUri({tt::ReminderAction::Snooze,
                                                                   appRule, "expired-token"}));
            require(expired.exit() == 0, "Stale sender exits normally");
            // Stop is queued after the action. Its completed flush is a processing barrier
            // for actions that deliberately leave no new state to poll.
            require(app.stop() == 0, "Restarted app drains queued actions cleanly");
            require(state().muted, "Today's silence survives normal restart");
            require(state().muted, "Expired occurrence ignored");
        }
        {
            tt::Storage db(path);
            db.removeReminder(appRule);
        }
        {
            Process app(argv[1], args + tt::reminderActionUri(
                                            {tt::ReminderAction::Snooze, appRule, "mute-token"}));
            app.host();
            require(!find(app.p.dwProcessId, L"TimeTrace.Window"),
                    "Deleted rule starts without UI");
            require(app.stop() == 0 && state().rule == 0, "Deleted rule ignored without UI");
        }
        auto invalid = root / L"invalid";
        {
            Process bad(argv[1], L"--data-dir \"" + invalid.wstring() +
                                     L"\" timetrace://reminders?action=bad&rule=1&token=a");
            require(bad.exit() == 2 && !std::filesystem::exists(invalid),
                    "Invalid URI exits before data access");
        }
        std::filesystem::remove(path);
        std::filesystem::remove(root);
        out << "{\"startup_snooze_wait_ms\":" << startupWait << ",\"checks\":" << checks
            << ",\"passed\":true,\"actual_shell_toast_click_tested\":false}\n";
        std::cout << "PASS: " << checks << " notification action checks\n";
        return 0;
    } catch (const std::exception &e) {
        out.seekp(0);
        out << "{\"checks\":" << checks << ",\"passed\":false}\n";
        std::cerr << e.what() << "\n";
        return 1;
    }
}
