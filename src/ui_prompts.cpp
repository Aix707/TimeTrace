#include "ui_prompts.h"
#include "ui_metrics.h"
#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace tt::ui {
namespace {
LRESULT CALLBACK promptProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    auto p = reinterpret_cast<Prompt *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        p = static_cast<Prompt *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
    }
    if (!p)
        return DefWindowProcW(h, message, w, l);
    if (message == WM_CREATE) {
        auto dpi = GetDpiForWindow(h);
        auto s = [&](int value) { return MulDiv(value, dpi, 96); };
        p->font = CreateFontW(-s(metrics::body), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        auto control = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y,
                           int width, int height, int id = 0, DWORD ex = 0) {
            auto child = CreateWindowExW(
                ex, cls, text, WS_CHILD | WS_VISIBLE | style, s(x), s(y), s(width), s(height), h,
                reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), nullptr, nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(p->font), TRUE);
            return child;
        };
        control(L"STATIC", p->kind == Prompt::AddReminder ? L"选择应用" : p->appName.c_str(),
                SS_ENDELLIPSIS, 24, 18, 332, 26);
        const bool choice = p->kind == Prompt::Merge || p->kind == Prompt::AddReminder;
        if (choice) {
            if (p->kind == Prompt::Merge)
                control(L"STATIC", L"合并到", 0, 24, 54, 332, 26);
            p->combo = control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 24,
                               p->kind == Prompt::Merge ? 87 : 50, 332, 240,
                               p->kind == Prompt::Merge ? 100 : 101);
            for (const auto &app : p->choices) {
                auto label = app.name;
                if (std::count_if(p->choices.begin(), p->choices.end(),
                                  [&](const auto &a) { return a.name == app.name; }) > 1 &&
                    !app.path.empty()) {
                    auto path = std::filesystem::path(app.path);
                    label += L" · " + path.parent_path().filename().wstring() + L" / " +
                             path.filename().wstring();
                }
                SendMessageW(p->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(p->combo, CB_SETMINVISIBLE, 8, 0);
            SendMessageW(p->combo, CB_SETDROPPEDWIDTH, s(540), 0);
            SendMessageW(p->combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                         s(metrics::control - 6));
            SendMessageW(p->combo, CB_SETITEMHEIGHT, 0, s(metrics::body + 12));
        }
        if (p->kind == Prompt::Merge) {
            p->edit = p->combo;
            control(L"STATIC", L"合并历史与今后统计。\n优先保留所选应用的名称和提醒。", 0, 24, 133,
                    332, 48);
        } else {
            auto caption = p->kind == Prompt::Rename ? L"名称（最多 40 字）"
                           : p->device               ? L"连续时长（分钟）"
                                                     : L"每日时长（分钟）";
            int top = p->kind == Prompt::AddReminder ? 94 : 54;
            control(L"STATIC", caption, 0, 24, top, 332, 26);
            auto initial = p->kind == Prompt::Rename ? p->appName : std::to_wstring(p->minutes);
            p->edit =
                control(L"EDIT", initial.c_str(),
                        WS_TABSTOP | ES_AUTOHSCROLL | (p->kind == Prompt::Rename ? 0 : ES_NUMBER),
                        24, top + 33, 332, metrics::control, 100, WS_EX_CLIENTEDGE);
            SendMessageW(p->edit, EM_SETLIMITTEXT, p->kind == Prompt::Rename ? 40 : 4, 0);
        }
        int buttons = choice ? 194 : 150;
        auto ok = control(L"BUTTON", p->kind == Prompt::Merge ? L"合并" : L"保存",
                          WS_TABSTOP | BS_DEFPUSHBUTTON, 104, buttons, metrics::actionWidth,
                          metrics::control, IDOK);
        control(L"BUTTON", L"取消", WS_TABSTOP, 236, buttons, metrics::actionWidth,
                metrics::control, IDCANCEL);
        if (choice)
            EnableWindow(ok, FALSE);
        else
            SendMessageW(p->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_CTLCOLORSTATIC) {
        auto dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, RGB(29, 29, 31));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    if (message == WM_COMMAND) {
        if (HIWORD(w) == CBN_SELCHANGE && (LOWORD(w) == 100 || LOWORD(w) == 101)) {
            EnableWindow(GetDlgItem(h, IDOK), SendMessageW(p->combo, CB_GETCURSEL, 0, 0) != CB_ERR);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDOK) {
            if (p->combo) {
                auto index = SendMessageW(p->combo, CB_GETCURSEL, 0, 0);
                if (index == CB_ERR || static_cast<size_t>(index) >= p->choices.size())
                    return 0;
                p->target = p->choices[static_cast<size_t>(index)].id;
            }
            if (p->kind != Prompt::Merge) {
                wchar_t value[80]{};
                GetWindowTextW(p->edit, value, 80);
                if (p->kind == Prompt::Rename) {
                    p->text = value;
                    if (p->text.empty() || std::all_of(p->text.begin(), p->text.end(),
                                                       [](wchar_t c) { return iswspace(c); })) {
                        MessageBoxW(h, L"名称不能为空。", L"重命名", MB_ICONINFORMATION);
                        return 0;
                    }
                } else {
                    wchar_t *end{};
                    p->result = static_cast<int>(wcstol(value, &end, 10));
                    if (end == value || *end)
                        p->result = -1;
                    if (p->result < 1 || p->result > 1440) {
                        p->result = -1;
                        MessageBoxW(h, L"请输入 1–1440 分钟。", L"提醒", MB_ICONINFORMATION);
                        return 0;
                    }
                }
            }
            p->accepted = true;
            DestroyWindow(h);
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, message, w, l);
}
} // namespace
bool showPrompt(HWND owner, Prompt &p) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = promptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = p.kind == Prompt::Reminder ? L"TimeTrace.Reminder"
                       : p.kind == Prompt::Rename ? L"TimeTrace.Rename"
                       : p.kind == Prompt::Merge  ? L"TimeTrace.Merge"
                                                  : L"TimeTrace.AddReminder";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{};
    GetWindowRect(owner, &r);
    auto dpi = GetDpiForWindow(owner);
    bool tall = p.kind == Prompt::Merge || p.kind == Prompt::AddReminder;
    int width = MulDiv(396, dpi, 96), height = MulDiv(tall ? 286 : 242, dpi, 96);
    const wchar_t *title = p.kind == Prompt::Reminder ? (p.device ? L"休息提醒" : L"应用提醒")
                           : p.kind == Prompt::Rename ? L"重命名"
                           : p.kind == Prompt::Merge  ? L"合并应用"
                                                      : L"添加应用提醒";
    auto dialog =
        CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title, WS_CAPTION | WS_SYSMENU,
                        (r.left + r.right - width) / 2, (r.top + r.bottom - height) / 2, width,
                        height, owner, nullptr, wc.hInstance, &p);
    if (!dialog)
        return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(p.combo ? p.combo : p.edit);
    MSG msg{};
    while (IsWindow(dialog)) {
        int result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            DestroyWindow(dialog);
            if (!result)
                PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (p.font)
        DeleteObject(p.font);
    return p.accepted;
}
} // namespace tt::ui
