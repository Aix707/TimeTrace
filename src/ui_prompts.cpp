#include "ui_prompts.h"
#include "ui_metrics.h"
#include <commctrl.h>
#include <algorithm>
#include <cwctype>

namespace tt::ui {
namespace {
constexpr int sourceListTop = 104, sourceListHeight = metrics::preferenceRow * 3 + 2;
constexpr int sourceFooterTop = sourceListTop + sourceListHeight + metrics::inset;
constexpr int sourceDialogHeight = sourceFooterTop + metrics::control + metrics::inset;
bool beginPromptDraw(Prompt &p, HDC dc, const RECT &rect) {
    if (!p.drawFactory)
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, p.drawFactory.GetAddressOf());
    if (!p.textFactory)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(p.textFactory.GetAddressOf()));
    if (!p.drawTarget && p.drawFactory) {
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(p.dpi), static_cast<float>(p.dpi));
        p.drawFactory->CreateDCRenderTarget(&properties, &p.drawTarget);
    }
    if (!p.drawTarget || !p.textFactory || FAILED(p.drawTarget->BindDC(dc, &rect)))
        return false;
    p.drawTarget->BeginDraw();
    return true;
}
void endPromptDraw(Prompt &p) {
    if (p.drawTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
        p.drawTarget.Reset();
        p.paintCache.icons.clear();
    }
}
LRESULT CALLBACK sourceButtonProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                                  DWORD_PTR data) {
    auto p = reinterpret_cast<Prompt *>(data);
    if (message == WM_MOUSEMOVE && p->hotButton != GetDlgCtrlID(window)) {
        p->hotButton = GetDlgCtrlID(window);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        p->hotButton = 0;
        InvalidateRect(window, nullptr, FALSE);
    }
    return DefSubclassProc(window, message, w, l);
}
std::wstring folded(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}
int selectedMember(Prompt &p) {
    auto row = ListView_GetNextItem(p.list, -1, LVNI_SELECTED);
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (row < 0 || !ListView_GetItem(p.list, &item) || item.lParam < 0 ||
        static_cast<size_t>(item.lParam) >= p.members.size())
        return -1;
    return static_cast<int>(item.lParam);
}
template <typename Row>
void listImages(Prompt &prompt, const std::vector<Row> &rows, int height, UINT dpi) {
    int icon = MulDiv(metrics::icon, dpi, 96), row = MulDiv(height, dpi, 96) - 2;
    prompt.images =
        ImageList_Create(icon, row, ILC_COLOR24 | ILC_MASK, static_cast<int>(rows.size()), 1);
    auto screen = GetDC(nullptr);
    auto dc = CreateCompatibleDC(screen);
    auto bitmap = CreateCompatibleBitmap(screen, icon, row);
    ReleaseDC(nullptr, screen);
    auto previous = SelectObject(dc, bitmap);
    for (const auto &entry : rows) {
        PatBlt(dc, 0, 0, icon, row, WHITENESS);
        auto handle =
            entry.icon ? static_cast<HICON>(entry.icon.get()) : LoadIconW(nullptr, IDI_APPLICATION);
        DrawIconEx(dc, 0, (row - icon) / 2, handle, icon, icon, 0, nullptr, DI_NORMAL);
        ImageList_AddMasked(prompt.images, bitmap, RGB(255, 255, 255));
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ListView_SetImageList(prompt.list, prompt.images, LVSIL_SMALL);
}
void updateMergeSelection(HWND dialog, Prompt &p) {
    auto selected = std::count(p.checked.begin(), p.checked.end(), true);
    auto total = p.baseUsed;
    for (size_t i = 0; i < p.mergeCandidates.size(); ++i)
        if (p.checked[i])
            total += p.mergeCandidates[i].used;
    auto text = L"已选 " + std::to_wstring(selected) + L" 项\n合并后用时 " + formatDuration(total);
    SetWindowTextW(p.count, text.c_str());
    text = selected ? L"合并 " + std::to_wstring(selected) + L" 项" : L"合并";
    SetWindowTextW(GetDlgItem(dialog, IDOK), text.c_str());
    EnableWindow(GetDlgItem(dialog, IDOK), selected != 0);
    bool anyRelated = false, allRelated = true;
    for (size_t i = 0; i < p.mergeCandidates.size(); ++i)
        if (p.mergeCandidates[i].related) {
            anyRelated = true;
            allRelated = allRelated && p.checked[i];
        }
    EnableWindow(p.related, anyRelated);
    SendMessageW(p.related, BM_SETCHECK, anyRelated && allRelated ? BST_CHECKED : BST_UNCHECKED, 0);
}
void populateMergeList(HWND dialog, Prompt &p) {
    auto length = GetWindowTextLengthW(p.edit);
    std::wstring query(length + 1, 0);
    GetWindowTextW(p.edit, query.data(), length + 1);
    query.resize(length);
    query = folded(std::move(query));
    p.updatingList = true;
    SendMessageW(p.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(p.list);
    for (size_t i = 0; i < p.mergeCandidates.size(); ++i) {
        const auto &candidate = p.mergeCandidates[i];
        auto searchable = candidate.app.name;
        if (candidate.related)
            searchable += L" " + p.appName;
        if (folded(std::move(searchable)).find(query) == std::wstring::npos)
            continue;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID | LVIF_IMAGE;
        item.iImage = static_cast<int>(i);
        item.iItem = ListView_GetItemCount(p.list);
        item.pszText = const_cast<wchar_t *>(candidate.app.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        item.iGroupId = candidate.related ? 0 : 1;
        auto row = ListView_InsertItem(p.list, &item);
        auto duration = formatDuration(candidate.used);
        ListView_SetItemText(p.list, row, 1, duration.data());
        ListView_SetCheckState(p.list, row, p.checked[i]);
    }
    SendMessageW(p.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p.list, nullptr, TRUE);
    p.updatingList = false;
    SetWindowTextW(p.detail, ListView_GetItemCount(p.list) ? L"" : L"未找到匹配记录");
    updateMergeSelection(dialog, p);
}
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
        p->dpi = dpi;
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
        if (p->kind == Prompt::GroupMembers) {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
            InitCommonControlsEx(&controls);
            p->list = control(WC_LISTVIEWW, L"",
                              WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                  LVS_NOCOLUMNHEADER | LVS_OWNERDRAWFIXED,
                              25, sourceListTop + 1, 590, sourceListHeight - 2, 100);
            ListView_SetExtendedListViewStyle(p->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                           LVS_EX_LABELTIP);
            LVCOLUMNW column{};
            column.mask = LVCF_WIDTH | LVCF_TEXT;
            const wchar_t *labels[]{L"来源名称", L"所选时段用时", L"程序路径"};
            const int widths[]{180, 120, 262};
            for (int i = 0; i < 3; ++i) {
                column.cx = s(widths[i]);
                column.pszText = const_cast<wchar_t *>(labels[i]);
                ListView_InsertColumn(p->list, i, &column);
            }
            p->updatingList = true;
            for (size_t i = 0; i < p->members.size(); ++i) {
                auto name = p->members[i].app.name + (i == 0 ? L" · 主应用" : L"");
                LVITEMW item{};
                item.mask = LVIF_TEXT | LVIF_PARAM;
                item.iItem = static_cast<int>(i);
                item.pszText = name.data();
                item.lParam = static_cast<LPARAM>(i);
                auto row = ListView_InsertItem(p->list, &item);
                auto duration = formatDuration(p->members[i].used);
                ListView_SetItemText(p->list, row, 1, duration.data());
                ListView_SetItemText(p->list, row, 2, p->members[i].app.path.data());
            }
            auto remove = control(L"BUTTON", L"更改归属", WS_TABSTOP | BS_OWNERDRAW, 24,
                                  sourceFooterTop, metrics::actionWidth, metrics::control, 107);
            auto primary = control(L"BUTTON", L"设为主应用", WS_TABSTOP | BS_OWNERDRAW, 156,
                                   sourceFooterTop, metrics::valueWidth, metrics::control, 108);
            auto done = control(L"BUTTON", L"完成", WS_TABSTOP | BS_OWNERDRAW, 496, sourceFooterTop,
                                metrics::actionWidth, metrics::control, IDCANCEL);
            for (auto button : {remove, primary, done})
                SetWindowSubclass(button, sourceButtonProc, 1, reinterpret_cast<DWORD_PTR>(p));
            EnableWindow(remove, FALSE);
            EnableWindow(primary, FALSE);
            p->updatingList = false;
            if (!p->members.empty())
                ListView_SetItemState(p->list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
            return 0;
        }
        if (p->kind == Prompt::Merge) {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
            InitCommonControlsEx(&controls);
            auto title = L"并入 " + p->appName;
            control(L"STATIC", title.c_str(), SS_ENDELLIPSIS | SS_NOPREFIX, 24, 18, 592, 26);
            control(L"STATIC", L"选择要并入的应用", SS_NOPREFIX, 24, 46, 592, 26, 106);
            p->edit = control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 24, 82, 592,
                              metrics::control, 102, WS_EX_CLIENTEDGE);
            SendMessageW(p->edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索应用名称"));
            SendMessageW(p->edit, EM_SETLIMITTEXT, 256, 0);
            p->related = control(L"BUTTON", L"全选相关应用", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 132,
                                 244, 28, 103);
            p->count = control(L"STATIC", L"", SS_RIGHT, 284, 126, 332, 46, 104);
            p->list = control(WC_LISTVIEWW, L"",
                              WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 24, 180,
                              592, 228, 100, WS_EX_CLIENTEDGE);
            ListView_SetExtendedListViewStyle(p->list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                           LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            LVCOLUMNW column{};
            column.mask = LVCF_WIDTH | LVCF_TEXT;
            column.cx = s(382);
            column.pszText = const_cast<wchar_t *>(L"应用");
            ListView_InsertColumn(p->list, 0, &column);
            column.cx = s(180);
            column.pszText = const_cast<wchar_t *>(L"所选时段用时");
            ListView_InsertColumn(p->list, 1, &column);
            auto relatedHeader = p->appName + L" · 相关应用";
            LVGROUP group{sizeof(group)};
            group.mask = LVGF_HEADER | LVGF_GROUPID;
            group.iGroupId = 0;
            group.pszHeader = relatedHeader.data();
            ListView_InsertGroup(p->list, -1, &group);
            group.iGroupId = 1;
            group.pszHeader = const_cast<wchar_t *>(L"其他应用");
            ListView_InsertGroup(p->list, -1, &group);
            ListView_EnableGroupView(p->list, TRUE);
            p->detail =
                control(L"STATIC", L"", SS_EDITCONTROL | SS_NOPREFIX, 24, 416, 592, 38, 105);
            control(L"BUTTON", L"合并", WS_TABSTOP | BS_DEFPUSHBUTTON, 364, 460,
                    metrics::actionWidth, metrics::control, IDOK);
            control(L"BUTTON", L"取消", WS_TABSTOP, 496, 460, metrics::actionWidth,
                    metrics::control, IDCANCEL);
            p->checked.assign(p->mergeCandidates.size(), false);
            listImages(*p, p->mergeCandidates, metrics::row, dpi);
            populateMergeList(h, *p);
            return 0;
        }
        control(L"STATIC",
                (p->kind == Prompt::AddReminder || p->kind == Prompt::MoveMember)
                    ? L"选择应用"
                    : p->appName.c_str(),
                SS_ENDELLIPSIS, 24, 18, 332, 26);
        const bool choice = p->kind == Prompt::AddReminder || p->kind == Prompt::MoveMember;
        if (choice) {
            p->combo = control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 24, 50,
                               332, 240, 101);
            for (const auto &app : p->choices) {
                auto label = app.name;
                SendMessageW(p->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(p->combo, CB_SETMINVISIBLE, 8, 0);
            SendMessageW(p->combo, CB_SETDROPPEDWIDTH, s(540), 0);
            SendMessageW(p->combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                         s(metrics::control - 6));
            SendMessageW(p->combo, CB_SETITEMHEIGHT, 0, s(metrics::body + 12));
        }
        {
            auto caption = (p->kind == Prompt::Rename || p->kind == Prompt::MoveMember)
                               ? L"应用名称"
                           : p->device ? L"连续时长（分钟）"
                                       : L"每日时长（分钟）";
            int top = choice ? 94 : 54;
            control(L"STATIC", caption, 0, 24, top, 332, 26);
            bool naming = p->kind == Prompt::Rename || p->kind == Prompt::MoveMember;
            auto initial = naming ? p->appName : std::to_wstring(p->minutes);
            p->edit = control(L"EDIT", initial.c_str(),
                              WS_TABSTOP | ES_AUTOHSCROLL | (naming ? 0 : ES_NUMBER), 24, top + 33,
                              332, metrics::control, 100, WS_EX_CLIENTEDGE);
            SendMessageW(p->edit, EM_SETLIMITTEXT, naming ? 40 : 4, 0);
        }
        int buttons = choice ? 194 : 150;
        auto ok = control(L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, 104, buttons,
                          metrics::actionWidth, metrics::control, IDOK);
        control(L"BUTTON", L"取消", WS_TABSTOP, 236, buttons, metrics::actionWidth,
                metrics::control, IDCANCEL);
        if (p->kind == Prompt::MoveMember) {
            SendMessageW(p->combo, CB_SETCURSEL, 0, 0);
        } else if (choice)
            EnableWindow(ok, FALSE);
        else
            SendMessageW(p->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_MEASUREITEM && p->kind == Prompt::GroupMembers) {
        reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight =
            MulDiv(metrics::preferenceRow, p->dpi, 96);
        return TRUE;
    }
    if (message == WM_PAINT && p->kind == Prompt::GroupMembers) {
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(h, &paint);
        RECT rect{};
        GetClientRect(h, &rect);
        if (beginPromptDraw(*p, dc, rect)) {
            Canvas c(p->drawTarget.Get(), p->textFactory.Get(), p->paintCache);
            c.fill({0, 0, 640, sourceDialogHeight}, theme::background);
            c.text(p->appName, {24, 24, 616, 56}, metrics::title, theme::text, 600);
            c.text(std::to_wstring(p->members.size()) + L" 个来源", {24, 64, 284, 92},
                   metrics::body, theme::secondary);
            c.text(L"合计 " + formatDuration(p->baseUsed), {300, 64, 616, 92}, metrics::body,
                   theme::secondary, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
            c.card({24, sourceListTop, 616, sourceListTop + sourceListHeight});
            endPromptDraw(*p);
        }
        EndPaint(h, &paint);
        return 0;
    }
    if (message == WM_DRAWITEM && p->kind == Prompt::GroupMembers) {
        auto draw = reinterpret_cast<DRAWITEMSTRUCT *>(l);
        auto bounds = draw->rcItem;
        if (draw->CtlType == ODT_LISTVIEW) {
            RECT client{};
            GetClientRect(draw->hwndItem, &client);
            bounds.right = client.right;
        }
        if (!beginPromptDraw(*p, draw->hDC, bounds))
            return TRUE;
        Canvas c(p->drawTarget.Get(), p->textFactory.Get(), p->paintCache);
        float width = (bounds.right - bounds.left) * 96.0f / p->dpi;
        float height = (bounds.bottom - bounds.top) * 96.0f / p->dpi;
        c.fill({0, 0, width, height}, theme::background);
        if (draw->CtlType == ODT_LISTVIEW && draw->itemID < p->members.size()) {
            const auto &member = p->members[draw->itemID];
            float inset = metrics::inset - 1.0f;
            float nameLeft = inset + metrics::icon + metrics::inset;
            float right = width - inset;
            if (draw->itemState & ODS_SELECTED)
                c.fill({4, 4, width - 4, height - 4}, theme::hover, metrics::radius);
            if (!c.icon(member.icon, {inset, 24, inset + metrics::icon, 56}))
                c.symbol(L'\xE8A5', {inset, 24, inset + metrics::icon, 56}, metrics::title,
                         theme::tertiary);
            c.text(member.app.name, {nameLeft, 10, right - metrics::valueWidth, 38});
            c.text(formatDuration(member.used), {right - metrics::valueWidth, 10, right, 38},
                   metrics::body, theme::secondary, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
            c.text(member.app.path.empty() ? L"路径不可用" : member.app.path,
                   {nameLeft, 42,
                    right - (draw->itemID == 0 ? metrics::compactWidth + metrics::gap : 0), 70},
                   metrics::body, theme::secondary);
            if (draw->itemID == 0) {
                c.fill({right - metrics::compactWidth, 44, right, 68}, theme::blueTint,
                       metrics::radius);
                c.text(L"主应用", {right - metrics::compactWidth, 44, right, 68}, metrics::body,
                       theme::blue, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            if (draw->itemID + 1 < p->members.size())
                c.line(nameLeft, height - 1, right, height - 1, theme::separator);
        } else if (draw->CtlType == ODT_BUTTON) {
            bool primary = draw->CtlID == IDCANCEL, disabled = draw->itemState & ODS_DISABLED;
            bool active =
                p->hotButton == static_cast<int>(draw->CtlID) || (draw->itemState & ODS_SELECTED);
            D2D1_RECT_F rect{1, 1, width - 1, height - 1};
            if (primary)
                c.fill(rect, active ? 0x006BE0 : theme::blue, metrics::radius);
            else {
                c.fill(rect, active && !disabled ? theme::hover : theme::surface, metrics::radius);
                c.outline(rect, theme::border, metrics::radius);
            }
            wchar_t text[32]{};
            GetWindowTextW(draw->hwndItem, text, 32);
            c.text(text, rect, metrics::body,
                   disabled  ? theme::tertiary
                   : primary ? theme::surface
                             : theme::text,
                   400, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (draw->itemState & ODS_FOCUS)
            c.outline({4, 4, width - 4, height - 4}, theme::blue, metrics::radius);
        endPromptDraw(*p);
        return TRUE;
    }
    if (message == WM_CTLCOLORSTATIC) {
        auto dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, RGB(29, 29, 31));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    if (message == WM_COMMAND) {
        if (p->kind == Prompt::GroupMembers && LOWORD(w) == IDOK) {
            DestroyWindow(h);
            return 0;
        }
        if (p->kind == Prompt::GroupMembers && (LOWORD(w) == 107 || LOWORD(w) == 108)) {
            auto member = selectedMember(*p);
            if (member <= 0)
                return 0;
            p->target = p->members[member].app.id;
            p->result = LOWORD(w);
            p->accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (p->kind == Prompt::Merge && LOWORD(w) == 102 && HIWORD(w) == EN_CHANGE && p->list) {
            populateMergeList(h, *p);
            return 0;
        }
        if (p->kind == Prompt::Merge && LOWORD(w) == 103 && HIWORD(w) == BN_CLICKED) {
            bool checked = SendMessageW(p->related, BM_GETCHECK, 0, 0) == BST_CHECKED;
            for (size_t i = 0; i < p->mergeCandidates.size(); ++i)
                if (p->mergeCandidates[i].related)
                    p->checked[i] = checked;
            populateMergeList(h, *p);
            return 0;
        }
        if (p->kind == Prompt::MoveMember && HIWORD(w) == CBN_SELCHANGE) {
            auto index = SendMessageW(p->combo, CB_GETCURSEL, 0, 0);
            EnableWindow(p->edit, index == 0);
        }
        if (HIWORD(w) == CBN_SELCHANGE && (LOWORD(w) == 100 || LOWORD(w) == 101)) {
            EnableWindow(GetDlgItem(h, IDOK), SendMessageW(p->combo, CB_GETCURSEL, 0, 0) != CB_ERR);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDOK) {
            if (p->kind == Prompt::Merge) {
                p->targets.clear();
                for (size_t i = 0; i < p->mergeCandidates.size(); ++i)
                    if (p->checked[i])
                        p->targets.push_back(p->mergeCandidates[i].app.id);
                if (p->targets.empty())
                    return 0;
            }
            if (p->combo) {
                auto index = SendMessageW(p->combo, CB_GETCURSEL, 0, 0);
                if (index == CB_ERR || static_cast<size_t>(index) >= p->choices.size())
                    return 0;
                p->target = p->choices[static_cast<size_t>(index)].id;
            }
            if (p->kind != Prompt::Merge) {
                wchar_t value[80]{};
                GetWindowTextW(p->edit, value, 80);
                if (p->kind == Prompt::Rename || p->kind == Prompt::MoveMember) {
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
    if (message == WM_NOTIFY && p->kind == Prompt::GroupMembers && !p->updatingList) {
        auto header = reinterpret_cast<NMHDR *>(l);
        if (header->hwndFrom == p->list && header->code == LVN_ITEMCHANGED) {
            auto member = selectedMember(*p);
            EnableWindow(GetDlgItem(h, 107), member > 0);
            EnableWindow(GetDlgItem(h, 108), member > 0);
            return 0;
        }
    }
    if (message == WM_NOTIFY && p->kind == Prompt::Merge && !p->updatingList) {
        auto header = reinterpret_cast<NMHDR *>(l);
        if (header->hwndFrom == p->list && header->code == LVN_ITEMCHANGED) {
            auto change = reinterpret_cast<NMLISTVIEW *>(l);
            if (change->iItem < 0)
                return 0;
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = change->iItem;
            if (!ListView_GetItem(p->list, &item) || item.lParam < 0 ||
                static_cast<size_t>(item.lParam) >= p->mergeCandidates.size())
                return 0;
            if ((change->uChanged & LVIF_STATE) &&
                ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) {
                p->checked[static_cast<size_t>(item.lParam)] =
                    (change->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
                updateMergeSelection(h, *p);
            }
            if (change->uNewState & LVIS_SELECTED)
                SetWindowTextW(
                    p->detail,
                    p->mergeCandidates[static_cast<size_t>(item.lParam)].app.name.c_str());
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
    p.accepted = false;
    p.target.clear();
    p.edit = p.combo = p.list = nullptr;
    WNDCLASSW wc{};
    wc.lpfnWndProc = promptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = p.kind == Prompt::Reminder       ? L"TimeTrace.Reminder"
                       : p.kind == Prompt::Rename       ? L"TimeTrace.Rename"
                       : p.kind == Prompt::Merge        ? L"TimeTrace.Merge"
                       : p.kind == Prompt::GroupMembers ? L"TimeTrace.GroupMembers"
                       : p.kind == Prompt::MoveMember   ? L"TimeTrace.MoveMember"
                                                        : L"TimeTrace.AddReminder";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{};
    GetWindowRect(owner, &r);
    auto dpi = GetDpiForWindow(owner);
    bool tall = p.kind == Prompt::AddReminder || p.kind == Prompt::MoveMember;
    int width = MulDiv(396, dpi, 96), height = MulDiv(tall ? 286 : 242, dpi, 96);
    if (p.kind == Prompt::Merge || p.kind == Prompt::GroupMembers) {
        RECT client{0, 0, MulDiv(640, dpi, 96),
                    MulDiv(p.kind == Prompt::Merge ? 520 : sourceDialogHeight, dpi, 96)};
        AdjustWindowRectExForDpi(&client, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
        width = client.right - client.left;
        height = client.bottom - client.top;
    }
    const wchar_t *title = p.kind == Prompt::Reminder       ? (p.device ? L"休息提醒" : L"应用提醒")
                           : p.kind == Prompt::Rename       ? L"重命名"
                           : p.kind == Prompt::Merge        ? L"合并记录"
                           : p.kind == Prompt::GroupMembers ? L"应用来源"
                           : p.kind == Prompt::MoveMember   ? L"更改归属"
                                                            : L"添加应用提醒";
    auto dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, title,
                                  WS_CAPTION | WS_SYSMENU |
                                      (p.kind == Prompt::GroupMembers ? WS_CLIPCHILDREN : 0),
                                  (r.left + r.right - width) / 2, (r.top + r.bottom - height) / 2,
                                  width, height, owner, nullptr, wc.hInstance, &p);
    if (!dialog)
        return false;
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(p.kind == Prompt::GroupMembers ? p.list : p.combo ? p.combo : p.edit);
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
    if (p.images) {
        ImageList_Destroy(p.images);
        p.images = nullptr;
    }
    p.paintCache = {};
    p.drawTarget.Reset();
    p.drawFactory.Reset();
    p.textFactory.Reset();
    return p.accepted;
}
} // namespace tt::ui
