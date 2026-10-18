#include "ui.h"
#include "notification_protocol.h"
#include "reminders.h"
#include <commctrl.h>
#include <memory>
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <shellapi.h>
#include <shobjidl.h>
#include <winrt/base.h>
#include <wtsapi32.h>

namespace {
bool sessionLocked() {
    LPWSTR buffer{};
    DWORD size{};
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                    WTSSessionInfoEx, &buffer, &size)) {
        auto info = reinterpret_cast<WTSINFOEXW *>(buffer);
        bool locked = size >= sizeof(WTSINFOEXW) && info->Level == 1 &&
                      info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(buffer);
        return locked;
    }
    auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop)
        return true;
    CloseDesktop(desktop);
    return false;
}
struct Application {
    HWND host{};
    std::unique_ptr<tt::Service> service;
    std::unique_ptr<tt::Ui> ui;
    std::filesystem::path directory;
    NOTIFYICONDATAW tray{};
    HWINEVENTHOOK hook{};
    bool paused{}, locked{}, sleeping{};
    unsigned heartbeat{};
    void observe(tt::Millis tick = -1, HWND window = nullptr) {
        if (service) {
            tt::Command c;
            c.sample = tt::captureSample(paused, locked, sleeping, tick, window);
            service->send(std::move(c));
        }
    }
    void addTray() {
        tray = {sizeof(tray)};
        tray.hWnd = host;
        tray.uID = 1;
        tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray.uCallbackMessage = tt::WM_TRAY;
        tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wcscpy_s(tray.szTip, L"TimeTrace · 屏幕时间");
        Shell_NotifyIconW(NIM_ADD, &tray);
        tray.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray);
    }
    void updateTray() {
        tray.uFlags = NIF_TIP;
        wcscpy_s(tray.szTip, paused ? L"TimeTrace · 已暂停" : L"TimeTrace · 正在统计");
        Shell_NotifyIconW(NIM_MODIFY, &tray);
    }
    void toggle() {
        paused = !paused;
        observe();
        updateTray();
        if (ui->visible())
            ui->request();
    }
    void menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"打开 TimeTrace");
        AppendMenuW(menu, MF_STRING, 2, paused ? L"恢复统计" : L"暂停统计");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"退出 TimeTrace");
        POINT p{};
        GetCursorPos(&p);
        SetForegroundWindow(host);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, host, nullptr);
        PostMessageW(host, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }
} *application{};
void CALLBACK foreground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD eventTime) {
    if (!application)
        return;
    auto now = static_cast<tt::Millis>(GetTickCount64());
    auto tick = now - static_cast<DWORD>(static_cast<DWORD>(now) - eventTime);
    application->observe(tick, hwnd);
}
LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto a = application;
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (!a)
        return DefWindowProcW(h, m, w, l);
    if (m == taskbarCreated) {
        a->addTray();
        return 0;
    }
    switch (m) {
    case WM_TIMER:
        if (w == 1) {
            a->observe();
            if (++a->heartbeat % 5 == 0 && a->ui->visible())
                a->ui->request();
        }
        return 0;
    case tt::WM_SHOW:
        if (a->ui)
            a->ui->show(w == 1);
        return 0;
    case WM_COPYDATA: {
        auto data = reinterpret_cast<COPYDATASTRUCT *>(l);
        if (!data || !a->service || data->dwData != tt::kReminderCopyData ||
            data->cbData != sizeof(tt::ReminderActionWire) || !data->lpData)
            return FALSE;
        tt::ReminderActionWire wire{};
        memcpy(&wire, data->lpData, sizeof(wire));
        auto action = tt::parseReminderWire(wire);
        if (!action)
            return FALSE;
        tt::Command command;
        command.kind = tt::Command::ActOnReminder;
        command.action = *action;
        a->service->send(std::move(command));
        if (a->ui && a->ui->visible())
            a->ui->request();
        return TRUE;
    }
    case tt::WM_STOP:
        DestroyWindow(h);
        return 0;
    case tt::WM_TRAY:
        switch (LOWORD(l)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            a->ui->show();
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            a->menu();
            break;
        case NIN_BALLOONUSERCLICK:
            a->ui->show(true);
            break;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == 1)
            a->ui->show();
        else if (LOWORD(w) == 2)
            a->toggle();
        else if (LOWORD(w) == 3)
            DestroyWindow(h);
        else if (LOWORD(w) == 4 && !a->paused)
            a->toggle();
        else if (LOWORD(w) == 5 && a->paused)
            a->toggle();
        return 0;
    case tt::WM_SNAPSHOT: {
        std::unique_ptr<tt::Snapshot> snapshot(reinterpret_cast<tt::Snapshot *>(l));
        if (a->ui)
            a->ui->update(std::move(*snapshot));
        return 0;
    }
    case tt::WM_NOTICE: {
        std::unique_ptr<tt::ReminderEvent> event(reinterpret_cast<tt::ReminderEvent *>(l));
        if (!tt::showToast(*event)) {
            a->tray.uFlags = NIF_INFO;
            auto title = tt::reminderTitle(*event);
            wcsncpy_s(a->tray.szInfoTitle, title.c_str(), _TRUNCATE);
            auto body = tt::reminderBody(*event);
            wcsncpy_s(a->tray.szInfo, body.c_str(), _TRUNCATE);
            a->tray.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &a->tray);
        }
        return 0;
    }
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK)
            a->locked = true;
        else if (w == WTS_SESSION_UNLOCK)
            a->locked = false;
        a->observe();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMSUSPEND)
            a->sleeping = true;
        else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND)
            a->sleeping = false;
        a->observe();
        return TRUE;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w)
            DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        if (a->hook) {
            UnhookWinEvent(a->hook);
            a->hook = nullptr;
        }
        WTSUnRegisterSessionNotification(h);
        a->observe();
        if (a->ui)
            a->ui->close();
        a->service.reset();
        Shell_NotifyIconW(NIM_DELETE, &a->tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    SetCurrentProcessExplicitAppUserModelID(L"TimeTrace.Desktop");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool background = false, reminders = false, stop = false, pause = false, resume = false,
         weekly = false, shortcut = false, install = false;
    std::filesystem::path directory, render;
    std::string date, selected;
    int renderPage = 0, renderWidth = 1140, renderHeight = 810;
    std::optional<tt::ReminderAction> action;
    bool invalidUri = false;
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--data-dir" && i + 1 < argc)
            directory = argv[++i];
        else if (arg == L"--background")
            background = true;
        else if (arg == L"--limits" || arg == L"--reminders")
            reminders = true;
        else if (_wcsnicmp(arg.c_str(), L"timetrace:", 10) == 0) {
            auto request = tt::parseReminderUri(arg);
            if (!request)
                invalidUri = true;
            else if (request->action) {
                action = request->action;
                background = true;
            } else
                reminders = true;
        } else if (arg == L"--stop")
            stop = true;
        else if (arg == L"--pause")
            pause = true;
        else if (arg == L"--resume")
            resume = true;
        else if (arg == L"--register-shortcut")
            shortcut = true;
        else if (arg == L"--register-install")
            install = true;
        else if (arg == L"--render" && i + 1 < argc)
            render = argv[++i];
        else if (arg == L"--render-width" && i + 1 < argc)
            renderWidth = _wtoi(argv[++i]);
        else if (arg == L"--render-height" && i + 1 < argc)
            renderHeight = _wtoi(argv[++i]);
        else if (arg == L"--date" && i + 1 < argc)
            date = tt::utf8(argv[++i]);
        else if (arg == L"--week")
            weekly = true;
        else if (arg == L"--page" && i + 1 < argc) {
            std::wstring page = argv[++i];
            renderPage = page == L"settings"                         ? 2
                         : page == L"limits" || page == L"reminders" ? 1
                         : page == L"detail"                         ? 3
                                                                     : 0;
        } else if (arg == L"--app-id" && i + 1 < argc)
            selected = tt::utf8(argv[++i]);
    }
    LocalFree(argv);
    if (renderWidth < 920 || renderWidth > 3840 || renderHeight < 680 || renderHeight > 2160)
        return 2;
    if (invalidUri ||
        (action && (stop || pause || resume || !render.empty() || install || shortcut)))
        return 2;
    try {
        if (shortcut)
            return tt::registerShortcut() ? 0 : 4;
        if (install)
            return tt::repairInstallation(tt::executablePath()) ? 0 : 4;
        // Only an installed, normal profile repairs global integration. Isolated test and
        // preview profiles must never take ownership of the user's shortcuts or protocols.
        if (directory.empty() && render.empty() && !stop && !pause && !resume) {
            std::error_code error;
            if (std::filesystem::is_regular_file(
                    tt::executablePath().parent_path() / L"Uninstall.exe", error))
                tt::repairInstallation(tt::executablePath());
        }
        if (directory.empty())
            directory = tt::dataPath();
        directory = std::filesystem::absolute(directory).lexically_normal();
        std::filesystem::create_directories(directory);
        if (!render.empty()) {
            tt::Storage db(directory / L"data.db");
            tt::Ui ui(nullptr, nullptr, directory);
            auto s =
                db.snapshot(date.empty() ? tt::localDay(tt::utcNow()) : date, weekly, selected);
            tt::ReminderEngine engine("preview");
            auto today = tt::localDay(tt::utcNow());
            engine.load(db.reminderRules(), db.reminderStates(today), db.dailyUsage(today), today,
                        true);
            s.reminders = engine.views();
            for (auto &view : s.reminders)
                for (const auto &app : s.catalog)
                    if (app.id == view.rule.app)
                        view.name = app.name;
            for (auto &row : s.apps)
                row.icon = tt::loadAppIcon(row.app.path);
            return ui.renderPng(render, std::move(s), renderPage, selected, renderWidth,
                                renderHeight)
                       ? 0
                       : 3;
        }
        auto normalizedDirectory = std::filesystem::weakly_canonical(directory).wstring();
        std::transform(normalizedDirectory.begin(), normalizedDirectory.end(),
                       normalizedDirectory.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        auto identity = std::to_wstring(std::hash<std::wstring>{}(normalizedDirectory));
        auto hostName = L"TimeTrace.Host." + identity;
        auto mutexName = L"Local\\TimeTrace." + identity;
        HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
        if (existing || stop || pause || resume) {
            HWND other = FindWindowW(L"TimeTrace.Host", hostName.c_str());
            for (int i = 0; existing && !other && i < 20; i++) {
                Sleep(50);
                other = FindWindowW(L"TimeTrace.Host", hostName.c_str());
            }
            if (other && action) {
                auto wire = tt::reminderWire(*action);
                COPYDATASTRUCT data{tt::kReminderCopyData, sizeof(wire), &wire};
                DWORD_PTR delivered{};
                LRESULT ok{};
                // The host window can become visible to FindWindow just before its service
                // is created. Retrying is safe: occurrence tokens are consumed only once.
                for (int retry = 0; retry < 20 && !delivered; ++retry) {
                    ok = SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &delivered);
                    if (!delivered)
                        Sleep(50);
                }
                if (mutex)
                    CloseHandle(mutex);
                return ok && delivered ? 0 : 5;
            }
            if (other && !(background && !stop && !pause && !resume && !reminders)) {
                DWORD otherPid{};
                GetWindowThreadProcessId(other, &otherPid);
                HANDLE exiting = stop ? OpenProcess(SYNCHRONIZE, FALSE, otherPid) : nullptr;
                AllowSetForegroundWindow(otherPid);
                PostMessageW(other,
                             stop              ? tt::WM_STOP
                             : pause || resume ? WM_COMMAND
                                               : tt::WM_SHOW,
                             pause       ? 4
                             : resume    ? 5
                             : reminders ? 1
                                         : 0,
                             0);
                if (exiting) {
                    auto result = WaitForSingleObject(exiting, 10000);
                    CloseHandle(exiting);
                    if (result != WAIT_OBJECT_0) {
                        if (mutex)
                            CloseHandle(mutex);
                        return 5;
                    }
                }
            }
            if (mutex)
                CloseHandle(mutex);
            return other ? 0 : 2;
        }
        Application state;
        application = &state;
        state.directory = directory;
        state.locked = sessionLocked();
        {
            tt::Storage db(directory / L"data.db");
            state.paused = db.getSetting("paused", "0") == "1";
        }
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = hostProc;
        wc.lpszClassName = L"TimeTrace.Host";
        RegisterClassW(&wc);
        state.host = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, hostName.c_str(), WS_POPUP,
                                     0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!state.host)
            throw std::runtime_error("Cannot create native message window");
        state.service = std::make_unique<tt::Service>(directory, state.host);
        state.ui = std::make_unique<tt::Ui>(state.service.get(), state.host, directory);
        state.addTray();
        state.updateTray();
        WTSRegisterSessionNotification(state.host, NOTIFY_FOR_THIS_SESSION);
        state.hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                     foreground, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!state.hook)
            throw std::runtime_error("Cannot subscribe to foreground events");
        SetTimer(state.host, 1, 1000, nullptr);
        state.observe();
        if (action) {
            tt::Command command;
            command.kind = tt::Command::ActOnReminder;
            command.action = *action;
            state.service->send(std::move(command));
        }
        if (!background)
            state.ui->show(reminders);
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, nullptr, tt::WM_SNAPSHOT, tt::WM_NOTICE, PM_REMOVE)) {
            if (msg.message == tt::WM_SNAPSHOT)
                delete reinterpret_cast<tt::Snapshot *>(msg.lParam);
            else if (msg.message == tt::WM_NOTICE)
                delete reinterpret_cast<tt::ReminderEvent *>(msg.lParam);
        }
        application = nullptr;
        CloseHandle(mutex);
    } catch (const std::exception &e) {
        MessageBoxW(nullptr,
                    (L"TimeTrace 无法启动。\n\n" + tt::wide(e.what()) +
                     L"\n\n请检查数据目录权限。原始数据不会被覆盖。")
                        .c_str(),
                    L"TimeTrace", MB_ICONERROR);
        return 1;
    }
    winrt::uninit_apartment();
    return 0;
}
