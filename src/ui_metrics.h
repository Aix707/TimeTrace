#pragma once

namespace tt::ui::metrics {
// Three text sizes, including the duration display. Weight and color supply
// the remaining hierarchy; glyphs use these sizes as well.
constexpr int body = 16, title = 22, display = 44;
constexpr int control = 36, icon = 32, row = 64, preferenceRow = 80;
constexpr int inset = 24, gap = 12, radius = 8, groupRadius = 12;
constexpr int actionWidth = 120, compactWidth = 80, valueWidth = 160, wideWidth = 216;
constexpr int contentWidth = 820, toolbar = 64;
constexpr int reminderListTop = 466, reminderRow = preferenceRow, listFooter = 40;
constexpr int overviewListGap = 44, overviewBottom = 16;
constexpr int detailBarBottom = 20;
} // namespace tt::ui::metrics
