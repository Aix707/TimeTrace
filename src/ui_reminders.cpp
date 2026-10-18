#include "ui.h"
#include <algorithm>

namespace tt {
namespace {
namespace theme = ui::theme;
namespace size = ui::metrics;
std::wstring appState(const ReminderView &view) {
    if (view.state.muted)
        return L"今日静默";
    if (view.state.notified)
        return L"已提醒";
    return view.state.next > 0 ? L"已延后" : L"";
}
} // namespace
const ReminderView *Ui::reminder(const std::string &app) const {
    for (const auto &view : snapshot_.reminders)
        if (app.empty() ? view.rule.kind == ReminderKind::Device
                        : view.rule.kind == ReminderKind::App && view.rule.app == app)
            return &view;
    return nullptr;
}
void Ui::reminderActions(ui::Canvas &c, float right, float top, const std::string &app) {
    button(c, ui::controlRect(right - size::actionWidth - size::gap, top, size::wideWidth),
           L"再使用 5 分钟后提醒", 209, app);
    linkButton(c, ui::controlRect(right, top), L"今日不再提醒", 210, app);
}
void Ui::appReminderRow(ui::Canvas &c, const ReminderView &view, float x, float right, float top,
                        bool showApp) {
    auto used = L"今日已用 " + ui::timeText(view.used);
    auto state = appState(view);
    if (showApp) {
        D2D1_RECT_F identity{x + 8, top + 4, right - 300, top + size::preferenceRow - 4};
        if (hover_ == static_cast<int>(hits_.size()))
            c.fill(identity, theme::hover, size::radius);
        const AppRow *row = nullptr;
        for (const auto &app : snapshot_.apps)
            if (app.app.id == view.rule.app)
                row = &app;
        applicationIcon(c, row,
                        {x + size::inset, top + 24, x + size::inset + size::icon, top + 56});
        c.rowText(view.name, used + (state.empty() ? L"" : L" · " + state), x + 80, top,
                  right - 300);
        hits_.push_back({identity, 211, view.rule.app});
    } else {
        if (!view.state.muted && !view.state.notified && view.state.next > 0)
            state = L"再用 " + ui::timeText(std::max<Millis>(0, view.state.next - view.used)) +
                    L"后提醒";
        c.rowText(used, state.empty() ? L"未达到提醒时长" : state, x + size::inset, top,
                  right - 300);
    }
    linkButton(c, ui::reminderValueRect(right, top + 22), ui::timeText(view.rule.threshold), 207,
               view.rule.app, false, true);
    linkButton(c, ui::controlRect(right - size::inset, top + 22, size::compactWidth), L"删除", 208,
               view.rule.app, true);
}
void Ui::drawReminders(ui::Canvas &c, float x, float right, float height) {
    c.text(L"提醒", {x, 102, right, 138}, size::title, theme::text, 600);
    const auto *device = reminder({});
    bool enabled = device && device->rule.enabled;
    auto threshold = device ? device->rule.threshold : 3600000;
    c.card({x, 164, right, 164 + 2 * size::preferenceRow});
    c.fill({x + size::inset, 188, x + size::inset + size::icon, 220}, theme::blueTint,
           size::radius);
    c.symbol(L'\xE916', {x + size::inset, 188, x + size::inset + size::icon, 220}, size::title,
             theme::blue);
    c.text(L"休息提醒", {x + 72, 176, right - 116, 208}, size::title, theme::text, 600);
    std::wstring state = enabled ? L"本轮已用 " + ui::timeText(device->used) : L"未开启";
    if (device && device->state.muted)
        state = L"今日静默";
    else if (device && enabled && !device->state.notified && device->state.next > threshold)
        state = L"再用 " + ui::timeText(std::max<Millis>(0, device->state.next - device->used)) +
                L"后提醒";
    c.text(state, {x + 72, 210, right - 116, 238}, size::body, theme::secondary);
    toggle(c, {right - 68, 192, right - 24, 216}, enabled, 214);
    c.line(x + size::inset, 244, right - size::inset, 244, theme::separator);
    c.rowText(L"提醒间隔", {}, x + size::inset, 244, right - 220);
    linkButton(c, ui::controlRect(right - size::inset, 266, size::valueWidth),
               ui::timeText(threshold), 215, {}, false, true);
    if (device && enabled && !device->state.token.empty())
        reminderActions(c, right - size::inset, 336);
    else
        c.text(L"空闲 5 分钟、锁屏、睡眠或暂停后重计。", {x + 4, 336, right, 372}, size::body,
               theme::secondary);
    c.text(L"应用提醒", {x, 396, right, 428}, size::title, theme::text, 600);
    button(c, ui::controlRect(right, 394), L"添加应用", 216, {}, true);
    c.text(L"按当天累计用时提醒", {x, 434, right - 300, 462}, size::body, theme::secondary);
    int total = 0;
    for (const auto &view : snapshot_.reminders)
        if (view.rule.kind == ReminderKind::App)
            ++total;
    int capacity =
        std::max(1, static_cast<int>((height - size::reminderListTop - size::listFooter) /
                                     size::reminderRow));
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - capacity));
    if (total) {
        auto column = ui::reminderValueRect(right, 434);
        column.right -= 20;
        column.bottom = 462;
        c.text(L"每日时长", column, size::body, theme::secondary, 400,
               DWRITE_TEXT_ALIGNMENT_CENTER);
        c.card({x, size::reminderListTop, right,
                size::reminderListTop +
                    static_cast<float>(size::reminderRow * std::min(total, capacity))});
    }
    int index = 0, visible = 0;
    float top = size::reminderListTop;
    for (const auto &view : snapshot_.reminders) {
        if (view.rule.kind != ReminderKind::App || index++ < scroll_)
            continue;
        if (visible >= capacity)
            break;
        appReminderRow(c, view, x, right, top, true);
        if (visible + 1 < std::min(total - scroll_, capacity))
            c.line(x + 80, top + size::reminderRow, right - size::inset, top + size::reminderRow,
                   theme::separator);
        top += size::reminderRow;
        ++visible;
    }
    if (!total) {
        c.card({x, size::reminderListTop, right, size::reminderListTop + 144});
        c.text(L"尚未添加应用",
               {x + 24, size::reminderListTop + 52, right - 24, size::reminderListTop + 88},
               size::body, theme::secondary, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (total > capacity)
        c.text(std::to_wstring(scroll_ + 1) + L"–" + std::to_wstring(scroll_ + visible) + L" / " +
                   std::to_wstring(total),
               {x, height - 32, right, height - 4}, size::body, theme::tertiary, 400,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
}
} // namespace tt
