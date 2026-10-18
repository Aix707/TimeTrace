#include "ui.h"
#include <algorithm>

namespace tt {
namespace {
namespace theme = ui::theme;
namespace size = ui::metrics;
std::wstring displayDay(const std::string &day, bool weekday = true) {
    auto date = std::to_wstring(std::stoi(day.substr(5, 2))) + L"月" +
                std::to_wstring(std::stoi(day.substr(8, 2))) + L"日";
    if (!weekday)
        return date;
    const wchar_t *names[] = {L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日"};
    int n = 0;
    while (n < 6 && shiftDay(weekStart(day), n) != day)
        ++n;
    return date + L"  " + names[n];
}
std::wstring usageLabel(const std::string &day, bool weekly, bool paused) {
    bool current =
        weekly ? weekStart(day) == weekStart(localDay(utcNow())) : day == localDay(utcNow());
    std::wstring label =
        weekly ? (current ? L"本周用时" : L"当周用时") : (current ? L"今日用时" : L"当日用时");
    return paused ? label + L" · 已暂停" : label;
}
} // namespace
void Ui::drawPeriod(ui::Canvas &c, float x, float right, float top) {
    segments(c, {right - 176, top, right, top + size::control}, {L"按天", L"按周"}, weekly_ ? 1 : 0,
             301);
    std::wstring period = weekly_ ? displayDay(weekStart(day_), false) + L" — " +
                                        displayDay(shiftDay(weekStart(day_), 6), false)
                                  : displayDay(day_);
    bool current =
        weekly_ ? weekStart(day_) == weekStart(localDay(utcNow())) : day_ == localDay(utcNow());
    if (current)
        period = (weekly_ ? L"本周 · " : L"今日 · ") + period;
    float dateWidth = weekly_ ? 320.0f : 276.0f, dateLeft = (x + right - dateWidth) / 2;
    auto arrow = [&](D2D1_RECT_F r, bool next) {
        bool enabled = !next || (weekly_ ? weekStart(day_) < weekStart(localDay(utcNow()))
                                         : day_ < localDay(utcNow()));
        if (hover_ == static_cast<int>(hits_.size()) && enabled)
            c.fill(r, theme::hover, size::radius);
        c.symbol(next ? L'\xE76C' : L'\xE76B', r, size::body,
                 enabled ? theme::secondary : 0xC7C7CC);
        hits_.push_back({r, enabled ? (next ? 304 : 303) : 0, {}});
    };
    arrow({dateLeft - size::control, top, dateLeft, top + size::control}, false);
    c.text(period, {dateLeft, top, dateLeft + dateWidth, top + size::control}, size::body,
           theme::secondary, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
    hits_.push_back({{dateLeft, top, dateLeft + dateWidth, top + size::control}, 305, {}});
    arrow({dateLeft + dateWidth, top, dateLeft + dateWidth + size::control, top + size::control},
          true);
}
void Ui::drawChart(ui::Canvas &c, float x, float right, float top, float bottom, Millis total) {
    float chartRight = right - 80;
    Millis peak = 3600000;
    for (auto value : snapshot_.buckets)
        peak = std::max(peak, value);
    Millis unit = peak <= 3600000 ? 1800000 : 3600000;
    peak = ((peak + unit - 1) / unit) * unit;
    for (int i = 0; i < 3; ++i) {
        float y = top + (bottom - top) * i / 2;
        c.line(x, y, chartRight, y, theme::separator);
        auto value = peak * (2 - i) / 2;
        auto label = !value                 ? L"0"
                     : value % 3600000 == 0 ? std::to_wstring(value / 3600000) + L"小时"
                                            : std::to_wstring(value / 60000) + L"分钟";
        c.text(label, {chartRight + size::gap, y - 14, right, y + 14}, size::body, theme::tertiary);
    }
    int count = weekly_ ? 7 : 24;
    float step = (chartRight - x) / count;
    float bar = std::min(weekly_ ? 32.0f : 18.0f, step * .48f);
    for (int i = 0; i < count; ++i) {
        auto value = i < static_cast<int>(snapshot_.buckets.size()) ? snapshot_.buckets[i] : 0;
        float h = (bottom - top) * static_cast<float>(value) / peak;
        float bx = x + i * step + (step - bar) / 2;
        if (value)
            c.fill({bx, bottom - std::max(2.0f, h), bx + bar, bottom}, theme::blue, 2);
        if (weekly_) {
            const wchar_t *days[] = {L"周一", L"周二", L"周三", L"周四", L"周五", L"周六", L"周日"};
            c.text(days[i], {x + i * step, bottom + 8, x + (i + 1) * step, bottom + 36}, size::body,
                   theme::tertiary, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (i % 6 == 0)
            c.text(std::to_wstring(i) + L"时",
                   {x + i * step, bottom + 8, x + i * step + 48, bottom + 36}, size::body,
                   theme::tertiary);
    }
    if (!weekly_)
        c.text(L"24时", {chartRight - 48, bottom + 8, chartRight, bottom + 36}, size::body,
               theme::tertiary, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!total && page_ == 3)
        c.text(L"暂无使用记录", {x, top, chartRight, bottom}, size::body, theme::tertiary, 400,
               DWRITE_TEXT_ALIGNMENT_CENTER);
}
void Ui::drawUsage(ui::Canvas &c, float x, float right, float height, Millis total) {
    drawPeriod(c, x, right, 94);
    float summary = height * .20f;
    c.text(usageLabel(day_, weekly_, snapshot_.paused), {x, summary, right, summary + 28},
           size::body, theme::secondary, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
    c.duration(total, {x, summary + 29, right, summary + 104});
    drawChart(c, x, right, height * .36f, height * .60f - 60, total);
}
void Ui::drawOverview(ui::Canvas &c, float x, float right, float height) {
    drawUsage(c, x, right, height, snapshot_.total);
    float listTop = height * .60f;
    c.line(x, listTop - size::gap, right, listTop - size::gap, theme::separator);
    c.text(L"应用使用", {x, listTop, right, listTop + 32}, size::title, theme::text, 600);
    int count = 0;
    for (const auto &row : snapshot_.apps)
        if (row.used > 0)
            ++count;
    int capacity = std::max(
        1, static_cast<int>((height - size::overviewBottom - listTop - size::overviewListGap) /
                            size::row));
    scroll_ = std::clamp(scroll_, 0, std::max(0, count - capacity));
    if (!count)
        c.text(L"暂无使用记录", {x, listTop + 64, right, listTop + 100}, size::title,
               theme::secondary, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
    const UINT32 palette[] = {theme::blue,   theme::green, theme::indigo,
                              theme::orange, theme::pink,  theme::purple};
    int index = 0, visible = 0;
    float y = listTop + size::overviewListGap;
    for (const auto &row : snapshot_.apps) {
        if (row.used <= 0)
            continue;
        int n = index++;
        if (n < scroll_)
            continue;
        if (visible >= capacity)
            break;
        D2D1_RECT_F rect{x - 8, y, right + 8, y + size::row};
        if (hover_ == static_cast<int>(hits_.size()))
            c.fill(rect, theme::hover, size::radius);
        applicationIcon(c, &row, {x, y + 16, x + size::icon, y + 16 + size::icon});
        c.text(row.app.name, {x + 56, y + 8, x + std::min(276.0f, (right - x) * .30f), y + 56},
               size::body);
        float trackLeft = x + std::min(300.0f, (right - x) * .34f), trackRight = right - 212;
        float fraction = static_cast<float>(row.used) / std::max<Millis>(snapshot_.total, 1);
        if (trackRight > trackLeft)
            c.fill({trackLeft, y + 30,
                    trackLeft + std::max(3.0f, (trackRight - trackLeft) * fraction), y + 35},
                   palette[n % 6], 2.5f);
        c.text(ui::timeText(row.used), {right - 192, y + 8, right - 28, y + 56}, size::body,
               theme::text, 400, DWRITE_TEXT_ALIGNMENT_TRAILING);
        c.symbol(L'\xE76C', {right - 16, y + 16, right + 4, y + 48}, size::body, theme::tertiary);
        hits_.push_back({rect, 211, row.app.id});
        if (visible + 1 < std::min(count - scroll_, capacity))
            c.line(x + 56, y + size::row, right, y + size::row, theme::separator);
        y += size::row;
        ++visible;
    }
    if (count > capacity)
        c.text(std::to_wstring(scroll_ + 1) + L"–" + std::to_wstring(scroll_ + visible) + L" / " +
                   std::to_wstring(count),
               {right - 180, listTop, right, listTop + 32}, size::body, theme::tertiary, 400,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
}
void Ui::drawDetail(ui::Canvas &c, float x, float right, float height) {
    drawUsage(c, x, right, height, snapshot_.selectedTotal);
    float reminderTop = height * .60f;
    c.line(x, reminderTop - size::gap, right, reminderTop - size::gap, theme::separator);
    c.text(L"应用提醒", {x, reminderTop, right, reminderTop + 32}, size::title, theme::text, 600);
    float rowTop = reminderTop + 40;
    c.card({x, rowTop, right, rowTop + size::preferenceRow});
    auto view = reminder(selected_);
    if (view) {
        auto column = ui::reminderValueRect(right, reminderTop);
        column.right -= 20;
        column.bottom = reminderTop + 32;
        c.text(L"每日时长", column, size::body, theme::secondary, 400,
               DWRITE_TEXT_ALIGNMENT_CENTER);
        appReminderRow(c, *view, x, right, rowTop, false);
        if (!view->state.token.empty())
            reminderActions(c, right - size::inset, rowTop + size::preferenceRow + size::gap,
                            selected_);
    } else {
        c.rowText(L"尚未设置", {}, x + size::inset, rowTop, right - 180);
        button(c, ui::controlRect(right - size::inset, rowTop + 22), L"添加提醒", 207, selected_,
               true);
    }
    float barTop = height - size::detailBarBottom - size::control;
    c.line(x, barTop - 16, right, barTop - 16, theme::separator);
    const AppRow *chosen = nullptr;
    for (const auto &row : snapshot_.apps)
        if (row.app.id == selected_)
            chosen = &row;
    linkButton(c, ui::controlRect(x + size::compactWidth, barTop, size::compactWidth), L"返回",
               100);
    applicationIcon(c, chosen, {x + 96, barTop + 2, x + 128, barTop + 34});
    c.text(chosen ? chosen->app.name : L"应用详情",
           {x + 144, barTop, right - 276, barTop + size::control}, size::title, theme::text, 600);
    button(c, ui::controlRect(right - size::actionWidth - size::gap, barTop), L"重命名", 212,
           selected_);
    if (snapshot_.catalog.size() > 1)
        button(c, ui::controlRect(right, barTop), L"合并应用", 213, selected_);
}
} // namespace tt
