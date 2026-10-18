#pragma once
#include "core.h"
#include "ui_metrics.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <tuple>
#include <algorithm>
namespace tt::ui {
using Microsoft::WRL::ComPtr;
namespace theme {
constexpr UINT32 blue = 0x007AFF, green = 0x34C759, red = 0xFF3B30;
constexpr UINT32 indigo = 0x5856D6, orange = 0xFF9500, pink = 0xFF2D55;
constexpr UINT32 purple = 0xAF52DE, teal = 0x5AC8FA, yellow = 0xFFCC00, gray = 0x8E8E93;
constexpr UINT32 background = 0xFFFFFF, surface = 0xFFFFFF;
constexpr UINT32 text = 0x1D1D1F, secondary = 0x6E6E73, tertiary = 0x86868B;
constexpr UINT32 border = 0xE5E5EA, separator = 0xEEEEF1, track = 0xEDEDF1;
constexpr UINT32 blueTint = 0xEAF3FF, redTint = 0xFFF0EF, hover = 0xF5F5F7;
} // namespace theme
struct PaintCache {
    std::map<std::tuple<int, int, int>, ComPtr<IDWriteTextFormat>> formats;
    std::map<std::shared_ptr<void>, ComPtr<ID2D1Bitmap>, std::owner_less<std::shared_ptr<void>>>
        icons;
    ComPtr<IWICImagingFactory> wic;
};
inline std::wstring timeText(Millis value) {
    return formatDuration(value);
}
inline D2D1_COLOR_F color(UINT32 rgb, float alpha = 1) {
    return D2D1::ColorF(rgb, alpha);
}
inline D2D1_RECT_F controlRect(float right, float top, float width = metrics::actionWidth) {
    return {right - width, top, right, top + metrics::control};
}
inline D2D1_RECT_F reminderValueRect(float right, float top) {
    return controlRect(right - metrics::inset - metrics::compactWidth - metrics::gap, top,
                       metrics::valueWidth);
}
struct Canvas {
    ID2D1RenderTarget *rt;
    IDWriteFactory *fonts;
    ComPtr<ID2D1SolidColorBrush> brush;
    PaintCache &cache;
    Canvas(ID2D1RenderTarget *r, IDWriteFactory *f, PaintCache &p) : rt(r), fonts(f), cache(p) {
        rt->CreateSolidColorBrush(color(theme::text), &brush);
    }
    void outline(D2D1_RECT_F r, UINT32 c, float radius = 0) {
        brush->SetColor(color(c));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), 1);
    }
    void card(D2D1_RECT_F r) {
        // A single hairline keeps cards defined without blur surfaces or shadow bitmaps.
        fill(r, theme::surface, metrics::groupRadius);
        outline(r, theme::border, metrics::groupRadius);
    }
    void fill(D2D1_RECT_F r, UINT32 c, float radius = 0) {
        brush->SetColor(color(c));
        if (radius)
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        else
            rt->FillRectangle(r, brush.Get());
    }
    void line(float x1, float y1, float x2, float y2, UINT32 c, float thickness = 1) {
        brush->SetColor(color(c));
        rt->DrawLine({x1, y1}, {x2, y2}, brush.Get(), thickness);
    }
    void circle(float x, float y, float radius, UINT32 c) {
        brush->SetColor(color(c));
        rt->FillEllipse({{x, y}, radius, radius}, brush.Get());
    }
    bool icon(const std::shared_ptr<void> &h, D2D1_RECT_F rect) {
        if (!h)
            return false;
        auto found = cache.icons.find(h);
        if (found == cache.icons.end()) {
            if (!cache.wic &&
                FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&cache.wic))))
                return false;
            ComPtr<IWICBitmap> source;
            if (FAILED(cache.wic->CreateBitmapFromHICON(static_cast<HICON>(h.get()), &source)))
                return false;
            ComPtr<IWICFormatConverter> converted;
            if (FAILED(cache.wic->CreateFormatConverter(&converted)) ||
                FAILED(converted->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                             WICBitmapDitherTypeNone, nullptr, 0,
                                             WICBitmapPaletteTypeCustom)))
                return false;
            ComPtr<ID2D1Bitmap> bitmap;
            if (FAILED(rt->CreateBitmapFromWicBitmap(converted.Get(), &bitmap)))
                return false;
            if (cache.icons.size() >= 64)
                cache.icons.erase(cache.icons.begin());
            found = cache.icons.emplace(h, std::move(bitmap)).first;
        }
        rt->DrawBitmap(found->second.Get(), rect);
        return true;
    }
    IDWriteTextFormat *font(int size, int weight = 400, int face = 0) {
        auto key = std::make_tuple(size, weight, face);
        auto &format = cache.formats[key];
        if (!format) {
            const wchar_t *family = face == 1   ? L"Segoe Fluent Icons"
                                    : face == 2 ? L"Segoe UI"
                                                : L"Microsoft YaHei UI";
            fonts->CreateTextFormat(family, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    static_cast<float>(size), L"zh-CN", &format);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> sign;
            fonts->CreateEllipsisTrimmingSign(format.Get(), &sign);
            format->SetTrimming(&trim, sign.Get());
        }
        return format.Get();
    }
    void text(const std::wstring &value, D2D1_RECT_F r, int size = metrics::body,
              UINT32 c = theme::text, int weight = 400,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, int face = 0) {
        auto format = font(size, weight, face);
        format->SetTextAlignment(align);
        brush->SetColor(color(c));
        rt->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format, r, brush.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void duration(Millis value, D2D1_RECT_F rect) {
        value = std::max<Millis>(0, value);
        auto minutes = value / 60000, hours = minutes / 60;
        std::wstring text =
            hours ? std::to_wstring(hours) + L" 小时" +
                        (minutes % 60 ? L" " + std::to_wstring(minutes % 60) + L" 分钟" : L"")
            : minutes ? std::to_wstring(minutes) + L" 分钟"
                      : std::to_wstring(value / 1000) + L" 秒";
        ComPtr<IDWriteTextLayout> layout;
        fonts->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                font(metrics::title), rect.right - rect.left,
                                rect.bottom - rect.top, &layout);
        if (!layout)
            return;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        for (UINT32 i = 0; i < text.size(); ++i)
            if (text[i] >= L'0' && text[i] <= L'9') {
                layout->SetFontSize(static_cast<float>(metrics::display), {i, 1});
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {i, 1});
            }
        brush->SetColor(color(theme::text));
        rt->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void symbol(wchar_t glyph, D2D1_RECT_F r, int size, UINT32 c) {
        text(std::wstring(1, glyph), r, size, c, 400, DWRITE_TEXT_ALIGNMENT_CENTER, 1);
    }
    void rowText(const std::wstring &label, const std::wstring &description, float left, float top,
                 float right) {
        float labelTop = top + (description.empty() ? (metrics::preferenceRow - 28) / 2.0f : 12.0f);
        text(label, {left, labelTop, right, labelTop + 28}, metrics::body);
        if (!description.empty())
            text(description, {left, top + 42, right, top + 68}, metrics::body, theme::secondary);
    }
};
} // namespace tt::ui
