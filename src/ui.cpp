#include "ui.h"
#include "version.h"
#include "ui_canvas.h"
#include "ui_prompts.h"
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

namespace tt {
using Microsoft::WRL::ComPtr;
namespace {
using ui::Canvas;
using ui::color;
using ui::Prompt;
using ui::showPrompt;
namespace theme = ui::theme;
namespace size = ui::metrics;
} // namespace
Ui::Ui(Service *service, HWND host, std::filesystem::path directory)
    : service_(service), host_(host), directory_(std::move(directory)), day_(localDay(utcNow())) {
    auto icon =
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_SHARED);
    if (icon)
        appIcon_ = std::shared_ptr<void>(icon, [](void *) {});
}
Ui::~Ui() {
    close();
}
void Ui::factories() {
    if (!factory_)
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    if (!textFactory_)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown **>(textFactory_.GetAddressOf()));
}
void Ui::show(bool reminders) {
    if (reminders && page_ != 1) {
        page_ = 1;
        scroll_ = 0;
    }
    if (window_) {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        request();
        return;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"TimeTrace.Window";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(appIcon_.get());
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    auto dpi = GetDpiForSystem();
    window_ = CreateWindowExW(0, wc.lpszClassName, L"TimeTrace · 屏幕时间", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1140, dpi, 96),
                              MulDiv(810, dpi, 96), nullptr, nullptr, wc.hInstance, this);
    if (!window_)
        return;
    BOOL dark = FALSE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF border = RGB(229, 229, 234);
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(window_, &margins);
    // Recalculate after WM_NCCREATE installed our instance. The first sizing pass
    // happens earlier and otherwise leaves the standard caption's client inset.
    SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    request();
}
void Ui::close() {
    if (window_)
        DestroyWindow(window_);
}
void Ui::update(Snapshot s) {
    if (!window_ || (s.request && s.request != request_))
        return;
    if (!s.request && !s.error.empty()) {
        snapshot_.error = std::move(s.error);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    snapshot_ = std::move(s);
    std::erase_if(paintCache_.icons, [](const auto &item) { return item.first.use_count() == 1; });
    if (visible())
        InvalidateRect(window_, nullptr, FALSE);
}
void Ui::request() {
    if (!service_ || !window_)
        return;
    if (followToday_)
        day_ = localDay(utcNow());
    Command c;
    c.kind = Command::Query;
    c.request = ++request_;
    c.day = day_;
    c.weekly = weekly_;
    if (page_ == 3)
        c.app = selected_;
    service_->send(std::move(c));
}
void Ui::paint() {
    PAINTSTRUCT ps{};
    BeginPaint(window_, &ps);
    factories();
    if (!factory_ || !textFactory_) {
        EndPaint(window_, &ps);
        return;
    }
    RECT r{};
    GetClientRect(window_, &r);
    if (r.right > 0 && r.bottom > 0 && !IsIconic(window_)) {
        if (!target_) {
            auto properties = D2D1::RenderTargetProperties();
            properties.dpiX = properties.dpiY = static_cast<float>(GetDpiForWindow(window_));
            factory_->CreateHwndRenderTarget(
                properties,
                D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(r.right, r.bottom)),
                &target_);
        }
        if (target_) {
            auto extent = target_->GetSize();
            target_->BeginDraw();
            draw(target_.Get(), extent.width, extent.height);
            if (focus_ >= 0 && focus_ < static_cast<int>(hits_.size())) {
                ComPtr<ID2D1SolidColorBrush> brush;
                target_->CreateSolidColorBrush(color(theme::blue), &brush);
                target_->DrawRoundedRectangle(D2D1::RoundedRect(hits_[focus_].rect, 6, 6),
                                              brush.Get(), 2);
            }
            if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
                paintCache_.icons.clear();
                target_.Reset();
            }
        }
    }
    EndPaint(window_, &ps);
}
int Ui::hitAt(float x, float y) const {
    for (size_t i = 0; i < hits_.size(); ++i) {
        const auto &r = hits_[i].rect;
        if (hits_[i].action && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom)
            return static_cast<int>(i);
    }
    return -1;
}
void Ui::button(Canvas &c, D2D1_RECT_F r, const std::wstring &label, int action, std::string app,
                bool primary, bool danger) {
    bool hover = hover_ == static_cast<int>(hits_.size());
    if (primary)
        c.fill(r, hover ? 0x006BE0 : theme::blue, size::radius);
    else {
        if (hover)
            c.fill(r, danger ? theme::redTint : theme::hover, size::radius);
        c.outline(r, theme::border, size::radius);
    }
    c.text(label, r, size::body,
           primary  ? theme::surface
           : danger ? theme::red
                    : theme::text,
           400, DWRITE_TEXT_ALIGNMENT_CENTER);
    hits_.push_back({r, action, std::move(app)});
}
void Ui::segments(Canvas &c, D2D1_RECT_F r, const std::vector<std::wstring> &labels, int selected,
                  int firstAction) {
    c.fill(r, theme::track, size::radius);
    float step = (r.right - r.left) / labels.size();
    for (size_t i = 0; i < labels.size(); ++i) {
        D2D1_RECT_F item{r.left + step * i + 3, r.top + 3, r.left + step * (i + 1) - 3,
                         r.bottom - 3};
        if (static_cast<int>(i) == selected) {
            c.fill({item.left, item.top + 1, item.right, item.bottom + 1}, 0xDDDEE3, 6);
            c.fill(item, theme::surface, 6);
        }
        c.text(labels[i], item, size::body,
               static_cast<int>(i) == selected ? theme::blue : theme::secondary,
               static_cast<int>(i) == selected ? 600 : 400, DWRITE_TEXT_ALIGNMENT_CENTER);
        hits_.push_back({item, firstAction + static_cast<int>(i), {}});
    }
}
void Ui::linkButton(Canvas &c, D2D1_RECT_F r, const std::wstring &label, int action,
                    std::string app, bool danger, bool disclosure) {
    if (hover_ == static_cast<int>(hits_.size()))
        c.fill(r, danger ? theme::redTint : theme::hover, size::radius);
    auto labelRect = r;
    if (disclosure) {
        labelRect.right -= 20;
        c.symbol(L'\xE76C', {r.right - 20, r.top, r.right - 4, r.bottom}, size::body,
                 theme::tertiary);
    }
    c.text(label, labelRect, size::body, danger ? theme::red : theme::blue, 400,
           DWRITE_TEXT_ALIGNMENT_CENTER);
    hits_.push_back({r, action, std::move(app)});
}
void Ui::toggle(Canvas &c, D2D1_RECT_F r, bool enabled, int action) {
    c.fill(r, enabled ? theme::green : 0xD1D1D6, 12);
    c.circle(enabled ? r.right - 12 : r.left + 12, (r.top + r.bottom) / 2, 9, theme::surface);
    hits_.push_back({{r.left - 12, r.top - 6, r.right + 8, r.bottom + 6}, action, {}});
}
void Ui::applicationIcon(Canvas &c, const AppRow *row, D2D1_RECT_F r) {
    if (row && c.icon(row->icon, r))
        return;
    c.symbol(L'\xE8A5', r, size::title, theme::tertiary);
}
void Ui::drawSettings(Canvas &c, float x, float right) {
    c.text(L"设置", {x, 102, right, 138}, size::title, theme::text, 600);
    c.text(wide("TimeTrace " TIMETRACE_VERSION), {right - 180, 102, right, 138}, size::body,
           theme::tertiary, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
    c.text(L"常规", {x, 164, right, 196}, size::title, theme::text, 600);
    c.card({x, 208, right, 208 + 2 * size::preferenceRow});
    c.rowText(L"登录时启动", {}, x + size::inset, 208, right - 180);
    toggle(c, {right - 68, 236, right - 24, 260}, autoStartEnabled(), 203);
    c.line(x + size::inset, 288, right - size::inset, 288, theme::separator);
    c.rowText(L"时间统计", snapshot_.paused ? L"已暂停" : L"关闭窗口后继续统计", x + size::inset,
              288, right - 180);
    button(c, ui::controlRect(right - size::inset, 310),
           snapshot_.paused ? L"恢复统计" : L"暂停统计", 204, {}, snapshot_.paused);
    c.text(L"数据与隐私", {x, 400, right, 432}, size::title, theme::text, 600);
    c.card({x, 444, right, 444 + 2 * size::preferenceRow});
    c.rowText(L"使用记录", {}, x + size::inset, 444, right - 180);
    button(c, ui::controlRect(right - size::inset, 466), L"打开文件夹", 205);
    c.line(x + size::inset, 524, right - size::inset, 524, theme::separator);
    c.rowText(L"历史记录", L"保留提醒、名称和分组", x + size::inset, 524, right - 180);
    linkButton(c, ui::controlRect(right - size::inset, 546), L"清除历史…", 206, {}, true);
    c.text(L"仅存本机，不采集标题、网址、输入内容或截图。", {x + 4, 616, right, 644}, size::body,
           theme::secondary);
}
void Ui::draw(ID2D1RenderTarget *rt, float width, float height) {
    Canvas c(rt, textFactory_.Get(), paintCache_);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt->Clear(color(theme::surface));
    hits_.clear();
    c.icon(appIcon_, {28, 18, 60, 50});
    c.text(L"TimeTrace", {72, 16, 250, 52}, size::title, theme::text, 600,
           DWRITE_TEXT_ALIGNMENT_LEADING, 2);
    segments(c, {width / 2 - 126, 16, width / 2 + 126, 52}, {L"概览", L"提醒", L"设置"},
             page_ == 3 ? 0 : page_, 100);
    const wchar_t symbols[] = {L'\xE921', window_ && IsZoomed(window_) ? L'\xE923' : L'\xE922',
                               L'\xE8BB'};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r{width - 146 + i * 46.0f, 14, width - 100 + i * 46.0f, 50};
        bool hover = hover_ == static_cast<int>(hits_.size());
        if (hover)
            c.fill(r, i == 2 ? theme::red : theme::hover, 5);
        c.symbol(symbols[i], r, size::body, hover && i == 2 ? theme::surface : theme::text);
        hits_.push_back({r, 900 + i, {}});
    }
    float pad = std::clamp(width * .05f, 32.0f, 72.0f), x = pad, right = width - pad;
    if (!snapshot_.error.empty()) {
        c.fill({pad, 65, width - pad, 91}, theme::redTint, 6);
        c.text(snapshot_.error, {pad + 8, 65, width - pad - 8, 91}, size::body, theme::red);
    }
    if (page_ == 1 || page_ == 2) {
        x = std::max(pad, (width - size::contentWidth) / 2);
        right = width - x;
        if (page_ == 1)
            drawReminders(c, x, right, height);
        else
            drawSettings(c, x, right);
        return;
    }
    if (page_ == 3)
        drawDetail(c, x, right, height);
    else
        drawOverview(c, x, right, height);
}
void Ui::promptReminder(const std::string &app, bool device, bool chooseApp) {
    Prompt p;
    p.kind = chooseApp ? Prompt::AddReminder : Prompt::Reminder;
    p.device = device;
    const auto *view = reminder(device ? std::string{} : app);
    const bool enabled = !device || !view || view->rule.enabled;
    if (view)
        p.minutes = static_cast<int>(view->rule.threshold / 60000);
    if (device)
        p.appName = L"休息提醒";
    else
        for (const auto &candidate : snapshot_.catalog)
            if (candidate.id == app)
                p.appName = candidate.name;
    if (chooseApp) {
        for (const auto &candidate : snapshot_.catalog)
            if (!reminder(candidate.id))
                p.choices.push_back(candidate);
        if (p.choices.empty()) {
            MessageBoxW(window_, L"所有应用均已添加提醒，可直接修改时长。", L"应用提醒",
                        MB_ICONINFORMATION);
            return;
        }
    }
    if (!showPrompt(window_, p) || !service_)
        return;
    Command command;
    command.kind = Command::SetReminder;
    command.app = chooseApp ? p.target : device ? std::string{} : app;
    command.value = static_cast<Millis>(p.result) * 60000;
    command.enabled = enabled;
    service_->send(std::move(command));
    request();
}
void Ui::promptApp(const std::string &app, bool merge) {
    Prompt p;
    p.kind = merge ? Prompt::Merge : Prompt::Rename;
    for (const auto &row : snapshot_.apps)
        if (row.app.id == app)
            p.appName = row.app.name;
    if (merge) {
        for (const auto &candidate : snapshot_.catalog)
            if (candidate.id != app)
                p.choices.push_back(candidate);
        if (p.choices.empty())
            return;
    }
    if (!showPrompt(window_, p) || !service_)
        return;
    Command command;
    command.kind = merge ? Command::Merge : Command::Rename;
    command.app = app;
    command.name = p.text;
    command.target = p.target;
    service_->send(std::move(command));
    if (merge)
        selected_ = p.target;
    request();
}
void Ui::act(const Hit &h) {
    if (!h.action)
        return;
    hover_ = -1;
    focus_ = -1;
    if (h.action >= 100 && h.action <= 102) {
        page_ = h.action - 100;
        scroll_ = 0;
        selected_.clear();
        request();
    } else if (h.action >= 900 && h.action <= 902) {
        if (window_) {
            if (h.action == 900)
                ShowWindow(window_, SW_MINIMIZE);
            else if (h.action == 901)
                ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
            else
                PostMessageW(window_, WM_CLOSE, 0, 0);
        }
    } else if (h.action == 203) {
        if (!setAutoStart(!autoStartEnabled()))
            MessageBoxW(window_, L"无法修改开机启动设置。", L"TimeTrace", MB_ICONWARNING);
    } else if (h.action == 204) {
        PostMessageW(host_, WM_COMMAND, 2, 0);
    } else if (h.action == 205) {
        ShellExecuteW(window_, L"open", directory_.c_str(), nullptr, nullptr, SW_SHOW);
    } else if (h.action == 206) {
        if (MessageBoxW(window_, L"清除全部使用历史？\n提醒、名称和分组将保留。此操作无法撤销。",
                        L"清除使用历史", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES &&
            service_) {
            service_->send({Command::Clear});
            request();
        }
    } else if (h.action == 207) {
        promptReminder(h.app);
    } else if (h.action == 215 || h.action == 216) {
        promptReminder({}, h.action == 215, h.action == 216);
    } else if (h.action == 214) {
        if (service_)
            if (auto view = reminder({})) {
                Command command;
                command.kind = Command::SetReminder;
                command.value = view->rule.threshold;
                command.enabled = !view->rule.enabled;
                service_->send(std::move(command));
                request();
            }
    } else if (h.action >= 208 && h.action <= 210) {
        if (service_)
            if (auto view = reminder(h.app)) {
                Command command;
                command.kind = h.action == 208 ? Command::RemoveReminder : Command::ActOnReminder;
                command.value = view->rule.id;
                command.action = {h.action == 209 ? ReminderAction::Snooze
                                                  : ReminderAction::MuteToday,
                                  view->rule.id, view->state.token};
                service_->send(std::move(command));
                request();
            }
    } else if (h.action == 212 || h.action == 213) {
        promptApp(h.app, h.action == 213);
    } else if (h.action == 211) {
        selected_ = h.app;
        page_ = 3;
        scroll_ = 0;
        request();
    } else if (h.action == 301 || h.action == 302) {
        weekly_ = h.action == 302;
        scroll_ = 0;
        request();
    } else if (h.action == 303) {
        followToday_ = false;
        day_ = shiftDay(day_, weekly_ ? -7 : -1);
        scroll_ = 0;
        request();
    } else if (h.action == 304) {
        auto next = shiftDay(day_, weekly_ ? 7 : 1), today = localDay(utcNow());
        day_ = std::min(next, today);
        followToday_ = day_ == today;
        scroll_ = 0;
        request();
    } else if (h.action == 305) {
        followToday_ = true;
        day_ = localDay(utcNow());
        request();
    }
    if (window_)
        InvalidateRect(window_, nullptr, FALSE);
}
LRESULT CALLBACK Ui::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Ui *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Ui *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            if (IsZoomed(h)) {
                auto params = reinterpret_cast<NCCALCSIZE_PARAMS *>(l);
                MONITORINFO monitor{sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor))
                    params->rgrc[0] = monitor.rcWork;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &pt);
        RECT client{};
        GetClientRect(h, &client);
        float scale = GetDpiForWindow(h) / 96.0f;
        float x = pt.x / scale, y = pt.y / scale, width = client.right / scale,
              height = client.bottom / scale;
        if (!IsZoomed(h)) {
            bool left = x<6, right = x> width - 6, top = y<6, bottom = y> height - 6;
            if (top && left)
                return HTTOPLEFT;
            if (top && right)
                return HTTOPRIGHT;
            if (bottom && left)
                return HTBOTTOMLEFT;
            if (bottom && right)
                return HTBOTTOMRIGHT;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
            if (top)
                return HTTOP;
            if (bottom)
                return HTBOTTOM;
        }
        auto index = self->hitAt(x, y);
        if (index >= 0)
            return self->hits_[index].action == 901 ? HTMAXBUTTON : HTCLIENT;
        if (y < ui::metrics::toolbar)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
        if (w == HTMAXBUTTON)
            return 0;
        break;
    case WM_NCLBUTTONUP:
        if (w == HTMAXBUTTON) {
            self->act({{}, 901, {}});
            return 0;
        }
        break;
    case WM_NCMOUSEMOVE:
        if (w == HTMAXBUTTON) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &pt);
            float scale = GetDpiForWindow(h) / 96.0f;
            auto next = self->hitAt(pt.x / scale, pt.y / scale);
            if (self->hover_ != next) {
                self->hover_ = next;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE | TME_NONCLIENT, h, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_NCMOUSELEAVE:
        self->hover_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_PAINT:
        self->paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (w == SIZE_MINIMIZED)
            return 0;
        if (self->target_) {
            self->target_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        auto r = reinterpret_cast<RECT *>(l);
        self->paintCache_.icons.clear();
        self->target_.Reset();
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto p = reinterpret_cast<MINMAXINFO *>(l);
        UINT dpi = GetDpiForWindow(h);
        p->ptMinTrackSize = {MulDiv(920, dpi, 96), MulDiv(680, dpi, 96)};
        return 0;
    }
    case WM_LBUTTONUP: {
        float scale = GetDpiForWindow(h) / 96.0f, x = GET_X_LPARAM(l) / scale,
              y = GET_Y_LPARAM(l) / scale;
        for (const auto &hit : self->hits_)
            if (x >= hit.rect.left && x <= hit.rect.right && y >= hit.rect.top &&
                y <= hit.rect.bottom) {
                auto copy = hit;
                self->act(copy);
                break;
            }
        return 0;
    }
    case WM_MOUSEMOVE: {
        float scale = GetDpiForWindow(h) / 96.0f;
        float x = GET_X_LPARAM(l) / scale, y = GET_Y_LPARAM(l) / scale;
        int next = -1;
        for (size_t i = 0; i < self->hits_.size(); i++) {
            const auto &hit = self->hits_[i];
            if (hit.action && x >= hit.rect.left && x <= hit.rect.right && y >= hit.rect.top &&
                y <= hit.rect.bottom) {
                next = static_cast<int>(i);
                break;
            }
        }
        if (next != self->hover_) {
            self->hover_ = next;
            InvalidateRect(h, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, h, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hover_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && self->hover_ >= 0 &&
            self->hover_ < static_cast<int>(self->hits_.size())) {
            SetCursor(LoadCursorW(nullptr,
                                  self->hits_[self->hover_].action >= 900 ? IDC_ARROW : IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEWHEEL: {
        if (self->page_ != 0 && self->page_ != 1)
            return 0;
        int count = 0;
        if (self->page_ == 1) {
            for (const auto &view : self->snapshot_.reminders)
                if (view.rule.kind == ReminderKind::App)
                    ++count;
        } else {
            for (const auto &row : self->snapshot_.apps)
                if (row.used > 0)
                    ++count;
        }
        RECT client{};
        GetClientRect(h, &client);
        int height = MulDiv(client.bottom, 96, GetDpiForWindow(h));
        int visible =
            std::max(1, self->page_ == 1
                            ? static_cast<int>((height - ui::metrics::reminderListTop -
                                                ui::metrics::listFooter) /
                                               ui::metrics::reminderRow)
                            : static_cast<int>((height - ui::metrics::overviewBottom -
                                                (height * .60f + ui::metrics::overviewListGap)) /
                                               ui::metrics::row));
        self->scroll_ = std::clamp(self->scroll_ - (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1), 0,
                                   std::max(0, count - visible));
        self->hover_ = self->focus_ = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && w >= '1' && w <= '3') {
            self->act({{}, 100 + static_cast<int>(w - '1'), {}});
            return 0;
        }
        if ((w == VK_LEFT || w == VK_RIGHT) && self->focus_ >= 0 &&
            self->focus_ < static_cast<int>(self->hits_.size())) {
            auto action = self->hits_[self->focus_].action;
            if ((action >= 100 && action <= 102) || (action >= 301 && action <= 302)) {
                int first = action <= 102 ? 100 : 301, count = first == 100 ? 3 : 2;
                self->act(
                    {{}, first + (action - first + (w == VK_RIGHT ? 1 : count - 1)) % count, {}});
                return 0;
            }
        }
        if (w == VK_TAB && !self->hits_.empty()) {
            int count = static_cast<int>(self->hits_.size());
            bool reverse = GetKeyState(VK_SHIFT) < 0;
            if (self->focus_ < 0 && reverse)
                self->focus_ = 0;
            for (int i = 0; i < count; i++) {
                self->focus_ = (self->focus_ + (reverse ? count - 1 : 1)) % count;
                if (self->hits_[self->focus_].action)
                    break;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if ((w == VK_RETURN || w == VK_SPACE) && self->focus_ >= 0 &&
            self->focus_ < static_cast<int>(self->hits_.size())) {
            auto hit = self->hits_[self->focus_];
            self->act(hit);
            return 0;
        }
        if (w == VK_ESCAPE && self->page_ == 3) {
            self->act({{}, 100, {}});
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        self->window_ = nullptr;
        self->paintCache_ = {};
        self->snapshot_ = {};
        ++self->request_;
        if (self->service_)
            self->service_->send({Command::ReleaseView});
        self->target_.Reset();
        self->factory_.Reset();
        self->textFactory_.Reset();
        self->hits_.clear();
        self->hover_ = self->focus_ = -1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
bool Ui::renderPng(const std::filesystem::path &path, Snapshot snapshot, int page,
                   std::string selected, int width, int height) {
    page_ = page;
    selected_ = std::move(selected);
    snapshot_ = std::move(snapshot);
    day_ = snapshot_.day;
    weekly_ = snapshot_.weekly;
    factories();
    if (!factory_ || !textFactory_)
        return false;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                 &bitmap)))
        return false;
    ComPtr<ID2D1RenderTarget> rt;
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt)))
        return false;
    rt->BeginDraw();
    draw(rt.Get(), static_cast<float>(width), static_cast<float>(height));
    if (FAILED(rt->EndDraw()))
        return false;
    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height)))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()))
        return false;
    return SUCCEEDED(encoder->Commit());
}
} // namespace tt
