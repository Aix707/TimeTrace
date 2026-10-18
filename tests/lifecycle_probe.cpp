#include <windows.h>
#include <fstream>
#include <algorithm>
#include <psapi.h>
#include <shellapi.h>
#include <sqlite3.h>
#include <string>
#include <vector>
namespace {
HWND findWindow(DWORD pid, const wchar_t *cls) {
    struct Find {
        DWORD pid;
        const wchar_t *cls;
        HWND window;
    };
    Find f{pid, cls, nullptr};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto p = reinterpret_cast<Find *>(l);
            DWORD id{};
            GetWindowThreadProcessId(h, &id);
            wchar_t name[100]{};
            GetClassNameW(h, name, 100);
            if (id == p->pid && wcscmp(name, p->cls) == 0) {
                p->window = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.window;
}
PROCESS_INFORMATION launch(const std::wstring &exe, const std::wstring &args) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION p{};
    std::wstring command = L"\"" + exe + L"\" " + args;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &p))
        return {};
    return p;
}
void wait(PROCESS_INFORMATION p) {
    if (p.hProcess) {
        WaitForSingleObject(p.hProcess, 10000);
        CloseHandle(p.hProcess);
        CloseHandle(p.hThread);
    }
}
void click(HWND h, int x, int y) {
    UINT dpi = GetDpiForWindow(h);
    PostMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(MulDiv(x, dpi, 96), MulDiv(y, dpi, 96)));
    Sleep(250);
}
void finishDialog(HWND h, int command) {
    auto owner = GetWindow(h, GW_OWNER);
    // Match a real button click: a sent command may destroy the dialog while its
    // GetMessage is still waiting, allowing the following action to run nested.
    PostMessageW(h, WM_COMMAND, command, 0);
    for (int i = 0; i < 40 && (IsWindow(h) || (owner && !IsWindowEnabled(owner))); ++i)
        Sleep(25);
    Sleep(100);
}
void capture(HWND h, const std::wstring &path) {
    SetForegroundWindow(h);
    UpdateWindow(h);
    Sleep(100);
    RECT rect{};
    GetClientRect(h, &rect);
    HDC windowDc = GetDC(h), memory = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, rect.right, rect.bottom);
    auto previous = SelectObject(memory, bitmap);
    if (!PrintWindow(h, memory, 3))
        BitBlt(memory, 0, 0, rect.right, rect.bottom, windowDc, 0, 0, SRCCOPY);
    SelectObject(memory, previous);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    std::vector<char> pixels(static_cast<size_t>(rect.right) * rect.bottom * 4);
    GetDIBits(memory, bitmap, 0, rect.bottom, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    out.write(pixels.data(), pixels.size());
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(h, windowDc);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc != 4)
        return 2;
    std::wstring exe = argv[1], data = argv[2], output = argv[3],
                 args = L"--data-dir \"" + data + L"\"";
    auto app = launch(exe, args + L" --background");
    if (!app.hProcess)
        return 3;
    Sleep(500);
    auto host = findWindow(app.dwProcessId, L"TimeTrace.Host");
    if (!host)
        return 4;
    DWORD baseGdi = GetGuiResources(app.hProcess, GR_GDIOBJECTS),
          baseUser = GetGuiResources(app.hProcess, GR_USEROBJECTS);
    std::vector<std::pair<DWORD, DWORD>> handles;
    int cycles{}, failures{};
    bool promptMetricsPassed = false;
    bool framePassed = true, keyboardPassed = false, directEditOpened = false,
         clearCancelPassed = false, reminderScrollPassed = false;
    bool reminderSaved = false, renamed = false, merged = false, settingsVisited = false,
         deviceSaved = false, addedSaved = false;
    constexpr int warmupCycles = 7, measuredCycles = 12;
    for (int i = 0; i < warmupCycles + measuredCycles; i++) {
        wait(launch(exe, args));
        HWND window{};
        for (int j = 0; j < 30 && !window; j++) {
            Sleep(50);
            window = findWindow(app.dwProcessId, L"TimeTrace.Window");
        }
        if (!window) {
            failures++;
            continue;
        }
        Sleep(300);
        RECT client{};
        GetClientRect(window, &client);
        auto dpi = GetDpiForWindow(window);
        int width = MulDiv(client.right, 96, dpi), height = MulDiv(client.bottom, 96, dpi);
        int edge = static_cast<int>(width * .05f),
            groupRight = width - std::max(edge, (width - 820) / 2);
        auto screenHit = [&](int x, int y) {
            POINT point{MulDiv(x, dpi, 96), MulDiv(y, dpi, 96)};
            ClientToScreen(window, &point);
            return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        framePassed = framePassed && screenHit(width - 77, 32) == HTMAXBUTTON &&
                      screenHit(260, 32) == HTCAPTION && screenHit(width / 2, 32) == HTCLIENT;
        if (i == 0) {
            capture(window, output + L".overview.bmp");
            click(window, 200, static_cast<int>(height * .60f + 74));
            capture(window, output + L".detail.bmp");
            click(window, width - edge - 196, static_cast<int>(height * .60f + 80));
            auto dialog = findWindow(app.dwProcessId, L"TimeTrace.Reminder");
            if (dialog) {
                auto edit = GetDlgItem(dialog, 100), save = GetDlgItem(dialog, IDOK);
                LOGFONTW font{};
                GetObjectW(reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)),
                           sizeof(font), &font);
                RECT field{}, action{};
                GetWindowRect(edit, &field);
                GetClientRect(save, &action);
                promptMetricsPassed =
                    font.lfHeight == -MulDiv(16, GetDpiForWindow(dialog), 96) &&
                    field.bottom - field.top == MulDiv(36, GetDpiForWindow(dialog), 96) &&
                    action.bottom == MulDiv(36, GetDpiForWindow(dialog), 96);
                capture(dialog, output + L".reminder-dialog.bmp");
                SendMessageW(GetDlgItem(dialog, 100), WM_SETTEXT, 0,
                             reinterpret_cast<LPARAM>(L"1"));
                finishDialog(dialog, IDOK);
                reminderSaved = true;
                Sleep(300);
            }
        }
        if (i == 1) {
            click(window, width / 2 + 84, 34);
            capture(window, output + L".settings.bmp");
            // The Run key belongs to the user's real installation; leave it untouched.
            click(window, groupRight - 84, 328);
            capture(window, output + L".settings-paused.bmp");
            click(window, groupRight - 84, 564);
            auto confirmation = findWindow(app.dwProcessId, L"#32770");
            clearCancelPassed = confirmation != nullptr;
            if (confirmation) {
                finishDialog(confirmation, IDNO);
                Sleep(100);
            }
            click(window, groupRight - 84, 328);
            settingsVisited = true;
        }
        if (i == 2) {
            click(window, width / 2 - 84, 34);
            click(window, 200, static_cast<int>(height * .60f + 74));
            click(window, width - edge - 192, height - 38);
            auto dialog = findWindow(app.dwProcessId, L"TimeTrace.Rename");
            if (dialog) {
                capture(dialog, output + L".rename-dialog.bmp");
                SendMessageW(GetDlgItem(dialog, 100), WM_SETTEXT, 0,
                             reinterpret_cast<LPARAM>(L"演示应用"));
                finishDialog(dialog, IDOK);
                renamed = true;
                Sleep(300);
            }
            click(window, width - edge - 60, height - 38);
            dialog = findWindow(app.dwProcessId, L"TimeTrace.Merge");
            if (dialog) {
                capture(dialog, output + L".merge-dialog.bmp");
                auto combo = GetDlgItem(dialog, 100);
                auto item = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                         reinterpret_cast<LPARAM>(L"Visual Studio Code"));
                if (item != CB_ERR) {
                    SendMessageW(combo, CB_SETCURSEL, item, 0);
                    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(100, CBN_SELCHANGE),
                                 reinterpret_cast<LPARAM>(combo));
                    finishDialog(dialog, IDOK);
                    merged = true;
                    Sleep(300);
                } else
                    SendMessageW(dialog, WM_CLOSE, 0, 0);
            }
        }
        if (i == 3) {
            click(window, width / 2, 34);
            click(window, groupRight - 46, 204);
            click(window, groupRight - 104, 284);
            auto dialog = findWindow(app.dwProcessId, L"TimeTrace.Reminder");
            if (dialog) {
                SendMessageW(GetDlgItem(dialog, 100), WM_SETTEXT, 0,
                             reinterpret_cast<LPARAM>(L"45"));
                finishDialog(dialog, IDOK);
                deviceSaved = true;
                Sleep(200);
            }
            click(window, groupRight - 60, 412);
            dialog = findWindow(app.dwProcessId, L"TimeTrace.AddReminder");
            if (dialog) {
                capture(dialog, output + L".add-dialog.bmp");
                auto combo = GetDlgItem(dialog, 101);
                auto item = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                         reinterpret_cast<LPARAM>(L"文件资源管理器"));
                if (item != CB_ERR) {
                    SendMessageW(combo, CB_SETCURSEL, item, 0);
                    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(101, CBN_SELCHANGE),
                                 reinterpret_cast<LPARAM>(combo));
                    SendMessageW(GetDlgItem(dialog, 100), WM_SETTEXT, 0,
                                 reinterpret_cast<LPARAM>(L"30"));
                    finishDialog(dialog, IDOK);
                    addedSaved = true;
                    Sleep(200);
                } else
                    SendMessageW(dialog, WM_CLOSE, 0, 0);
            }
            capture(window, output + L".reminders.bmp");
            click(window, groupRight - 196, 506);
            dialog = findWindow(app.dwProcessId, L"TimeTrace.Reminder");
            directEditOpened = dialog != nullptr;
            if (dialog)
                finishDialog(dialog, IDCANCEL);
        }
        if (i == 4) {
            // Tab/arrow navigation uses the same interactive segmented controls.
            click(window, width / 2 - 84, 34);
            SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
            SendMessageW(window, WM_KEYDOWN, VK_RIGHT, 0);
            Sleep(100);
            click(window, groupRight - 104, 284);
            auto dialog = findWindow(app.dwProcessId, L"TimeTrace.Reminder");
            keyboardPassed = dialog != nullptr;
            if (dialog)
                finishDialog(dialog, IDCANCEL);
        }
        if (i == 5) {
            SetWindowPos(window, nullptr, 0, 0, MulDiv(920, dpi, 96), MulDiv(680, dpi, 96),
                         SWP_NOMOVE | SWP_NOZORDER);
            Sleep(200);
            RECT minimum{};
            GetClientRect(window, &minimum);
            framePassed = framePassed && minimum.right >= MulDiv(920, dpi, 96) &&
                          minimum.bottom >= MulDiv(680, dpi, 96);
            PostMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
            Sleep(150);
            capture(window, output + L".reminders-scrolled.bmp");
            int minimumRight = MulDiv(minimum.right, 96, dpi) - 50;
            click(window, minimumRight - 196, 586);
            auto dialog = findWindow(app.dwProcessId, L"TimeTrace.Reminder");
            if (dialog) {
                wchar_t label[100]{};
                GetWindowTextW(FindWindowExW(dialog, nullptr, L"STATIC", nullptr), label, 100);
                reminderScrollPassed = wcscmp(label, L"文件资源管理器") == 0;
                finishDialog(dialog, IDCANCEL);
            }
        }
        if (i == 6) {
            SendMessageW(window, WM_NCLBUTTONUP, HTMAXBUTTON, 0);
            Sleep(200);
            framePassed = framePassed && IsZoomed(window);
            RECT maximized{};
            GetClientRect(window, &maximized);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            framePassed = framePassed &&
                          maximized.right == monitor.rcWork.right - monitor.rcWork.left &&
                          maximized.bottom == monitor.rcWork.bottom - monitor.rcWork.top;
            SendMessageW(window, WM_NCLBUTTONUP, HTMAXBUTTON, 0);
            Sleep(100);
            framePassed = framePassed && !IsZoomed(window);
            click(window, width - 123, 32);
            framePassed = framePassed && IsIconic(window);
            wait(launch(exe, args));
            Sleep(200);
            framePassed = framePassed && !IsIconic(window);
        }
        RECT closing{};
        GetClientRect(window, &closing);
        click(window, MulDiv(closing.right, 96, GetDpiForWindow(window)) - 31, 32);

        Sleep(100);
        if (findWindow(app.dwProcessId, L"TimeTrace.Window"))
            failures++;
        else
            cycles++;
        handles.emplace_back(GetGuiResources(app.hProcess, GR_GDIOBJECTS),
                             GetGuiResources(app.hProcess, GR_USEROBJECTS));
        if (i == warmupCycles - 1) {
            baseGdi = handles.back().first;
            baseUser = handles.back().second;
        }
    }
    Sleep(500);
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(app.hProcess, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                         sizeof(memory));
    DWORD afterGdi = GetGuiResources(app.hProcess, GR_GDIOBJECTS),
          afterUser = GetGuiResources(app.hProcess, GR_USEROBJECTS);
    bool alive = WaitForSingleObject(app.hProcess, 0) == WAIT_TIMEOUT;
    PostMessageW(host, WM_APP + 24, 0, 0);
    wait(app);
    // Validate the results in the isolated fixture, rather than just the dialog closing.
    std::wstring filePath = data + L"\\data.db";
    int size = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8Path(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), -1, utf8Path.data(), size, nullptr, nullptr);
    sqlite3 *db{};
    bool persisted = false;
    if (sqlite3_open_v2(utf8Path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *s{};
        if (sqlite3_prepare_v2(
                db,
                "SELECT (SELECT name FROM app_labels WHERE app='chrome')='演示应用',"
                "(SELECT target FROM app_links WHERE app='chrome')='code',"
                "(SELECT duration FROM reminder_rules WHERE app='code')=60000 AND "
                "(SELECT duration FROM reminder_rules WHERE kind=0)=2700000 AND "
                "(SELECT enabled FROM reminder_rules WHERE kind=0)=1 AND "
                "(SELECT duration FROM reminder_rules WHERE app='explorer')=1800000 "
                "AND (SELECT SUM(duration) FROM segments WHERE app='chrome')=4860000",
                -1, &s, nullptr) == SQLITE_OK) {
            persisted = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) &&
                        sqlite3_column_int(s, 1) && sqlite3_column_int(s, 2);
            sqlite3_finalize(s);
        }
    }
    if (db)
        sqlite3_close(db);
    std::ofstream file(output);
    bool passed = !failures && cycles == warmupCycles + measuredCycles && alive && reminderSaved &&
                  renamed && merged && persisted && settingsVisited && deviceSaved && addedSaved &&
                  framePassed && promptMetricsPassed && keyboardPassed && directEditOpened &&
                  clearCancelPassed && reminderScrollPassed && afterGdi <= baseGdi + 5 &&
                  afterUser <= baseUser + 5 && memory.PrivateWorkingSetSize <= 50000000;
    file << "{\n  \"cycles\": " << cycles << ",\n  \"failures\": " << failures
         << ",\n  \"native_prompt_metrics\": " << (promptMetricsPassed ? "true" : "false")
         << ",\n  \"native_window_frame\": " << (framePassed ? "true" : "false")
         << ",\n  \"native_keyboard_navigation\": " << (keyboardPassed ? "true" : "false")
         << ",\n  \"native_reminder_dialog_saved\": " << (reminderSaved ? "true" : "false")
         << ",\n  \"native_rename_saved\": " << (renamed && persisted ? "true" : "false")
         << ",\n  \"native_merge_saved\": " << (merged && persisted ? "true" : "false")
         << ",\n  \"native_device_and_add_saved\": "
         << (deviceSaved && addedSaved && persisted ? "true" : "false")
         << ",\n  \"native_inline_reminder_edit\": " << (directEditOpened ? "true" : "false")
         << ",\n  \"native_clear_history_cancel\": "
         << (clearCancelPassed && persisted ? "true" : "false")
         << ",\n  \"native_reminder_scroll\": " << (reminderScrollPassed ? "true" : "false")
         << ",\n  \"autostart_test_skipped\": true"
         << ",\n  \"backend_survived_window_close\": " << (alive ? "true" : "false")
         << ",\n  \"baseline_gdi_handles\": " << baseGdi
         << ",\n  \"final_gdi_handles\": " << afterGdi
         << ",\n  \"baseline_user_handles\": " << baseUser
         << ",\n  \"final_user_handles\": " << afterUser
         << ",\n  \"private_working_set_after_cycles\": " << memory.PrivateWorkingSetSize
         << ",\n  \"warmup_cycles\": " << warmupCycles
         << ",\n  \"measured_cycles\": " << measuredCycles << ",\n  \"handle_samples\": [";
    for (size_t i = 0; i < handles.size(); i++) {
        if (i)
            file << ",";
        file << "[" << handles[i].first << "," << handles[i].second << "]";
    }
    file << "],\n  \"passed\": " << (passed ? "true" : "false") << "\n}\n";
    return passed ? 0 : 5;
}
