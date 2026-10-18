#include <windows.h>
#include <fstream>
#include <shellapi.h>
#include <string>
#include <vector>
namespace {
constexpr UINT kActivate = WM_APP + 1, kAllow = WM_APP + 2;
LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kAllow) {
        AllowSetForegroundWindow(ASFW_ANY);
        return 0;
    }
    if (m == kActivate) {
        SetForegroundWindow(h);
        SetActiveWindow(h);
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT p;
        auto dc = BeginPaint(h, &p);
        TextOutW(dc, 20, 25, L"TimeTrace foreground verification", 33);
        EndPaint(h, &p);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
HWND windowFor(DWORD pid) {
    struct Find {
        DWORD pid;
        HWND window;
    };
    Find f{pid, nullptr};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto f = reinterpret_cast<Find *>(l);
            DWORD p{};
            GetWindowThreadProcessId(h, &p);
            wchar_t cls[100]{};
            GetClassNameW(h, cls, 100);
            if (p == f->pid && wcscmp(cls, L"TimeTrace.Probe") == 0) {
                f->window = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.window;
}
void focus(HWND h) {
    auto old = GetForegroundWindow();
    wchar_t cls[100]{};
    if (old)
        GetClassNameW(old, cls, 100);
    if (wcscmp(cls, L"TimeTrace.Probe") == 0)
        SendMessageTimeoutW(old, kAllow, 0, 0, SMTO_ABORTIFHUNG, 50, nullptr);
    DWORD thread = old ? GetWindowThreadProcessId(old, nullptr) : 0, own = GetCurrentThreadId();
    bool attached = thread && thread != own && AttachThreadInput(own, thread, TRUE);
    SetForegroundWindow(h);
    SendMessageW(h, kActivate, 0, 0);
    if (attached)
        AttachThreadInput(own, thread, FALSE);
}
int64_t utc() {
    FILETIME f;
    GetSystemTimePreciseAsFileTime(&f);
    return static_cast<int64_t>(
        ((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000 -
        11644473600000ULL);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; i++)
        args.emplace_back(argv[i]);
    LocalFree(argv);
    if (argc > 1 && args[1] == L"--child") {
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpfnWndProc = proc;
        wc.lpszClassName = L"TimeTrace.Probe";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        auto h = CreateWindowW(wc.lpszClassName, L"TimeTrace · 自动计时验证", WS_OVERLAPPEDWINDOW,
                               80, 80, 440, 140, nullptr, nullptr, instance, nullptr);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (argc != 7 || args[1] != L"--pair")
        return 2;
    HWND original = GetForegroundWindow();
    PROCESS_INFORMATION processes[2]{};
    HWND windows[2]{};
    for (int i = 0; i < 2; i++) {
        STARTUPINFOW si{sizeof(si)};
        auto command = L"\"" + args[5 + i] + L"\" --child";
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                            &si, &processes[i]))
            return 3;
        for (int attempt = 0; attempt < 100 && !windows[i]; attempt++) {
            Sleep(50);
            windows[i] = windowFor(processes[i].dwProcessId);
        }
        if (!windows[i])
            return 4;
    }
    int iterations = _wtoi(args[3].c_str()), interval = _wtoi(args[4].c_str());
    std::ofstream log(args[2]), observed(args[2] + L".observed.csv");
    log << "iteration,tick,utc_ms,expected_pid,actual_pid,focused\n";
    observed << "utc_ms,pid\n";
    DWORD lastPid = MAXDWORD;
    auto record = [&] {
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid != lastPid) {
            observed << utc() << "," << pid << "\n";
            observed.flush();
            lastPid = pid;
        }
    };
    record();
    for (int attempt = 0; attempt < 20 && GetForegroundWindow() != windows[0]; attempt++) {
        INPUT initial{};
        initial.type = INPUT_MOUSE;
        initial.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &initial, sizeof(initial));
        focus(windows[0]);
        record();
        Sleep(25);
    }
    int success{};
    for (int i = 0; i < iterations; i++) {
        int n = i % 2;
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &input, sizeof(input));
        focus(windows[n]);
        record();
        auto start = GetTickCount64();
        DWORD actual{};
        GetWindowThreadProcessId(GetForegroundWindow(), &actual);
        bool correct = GetForegroundWindow() == windows[n];
        success += correct;
        log << i << "," << start << "," << utc() << "," << processes[n].dwProcessId << "," << actual
            << "," << correct << "\n";
        log.flush();
        while (GetTickCount64() - start < static_cast<ULONGLONG>(interval)) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            record();
            Sleep(2);
        }
    }
    log << iterations << "," << GetTickCount64() << "," << utc() << ",0,0,1\n";
    log.close();
    observed << utc() << ",0\n";
    observed.close();
    for (int i = 0; i < 2; i++) {
        PostMessageW(windows[i], WM_CLOSE, 0, 0);
        WaitForSingleObject(processes[i].hProcess, 5000);
        CloseHandle(processes[i].hThread);
        CloseHandle(processes[i].hProcess);
    }
    if (original && IsWindow(original))
        SetForegroundWindow(original);
    return success == iterations ? 0 : 5;
}
