#include "platform.h"
#include "ui_canvas.h"
#include <functional>
#include <wrl/client.h>
#include "ui.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace tt {
void Service::send(Command) {}
bool autoStartEnabled() {
    return false;
}
bool setAutoStart(bool) {
    return true;
}
} // namespace tt

namespace {
using Microsoft::WRL::ComPtr;
int checks{};
void require(bool condition, const char *description) {
    if (!condition)
        throw std::runtime_error(description);
    ++checks;
}
struct Graphics {
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> fonts;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    Graphics() {
        require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                           factory.GetAddressOf())) &&
                    SUCCEEDED(DWriteCreateFactory(
                        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(fonts.GetAddressOf()))),
                "Create native drawing factories");
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                    SUCCEEDED(wic->CreateBitmap(1140, 810, GUID_WICPixelFormat32bppPBGRA,
                                                WICBitmapCacheOnLoad, &bitmap)),
                "Create isolated drawing surface");
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        require(SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties,
                                                               &target)),
                "Create software drawing target");
    }
};
tt::Snapshot fixture() {
    tt::Snapshot result;
    result.day = tt::localDay(tt::utcNow());
    result.total = 15000;
    result.selectedTotal = 10000;
    result.buckets.resize(24);
    result.apps = {{{"a", L"应用 A", L"a.exe"}, 10000},
                   {{"b", L"应用 B", L"b.exe"}, 5000}};
    result.selected = result.apps.front();
    result.members = {result.selected};
    for (const auto &row : result.apps)
        result.catalog.push_back(row.app);
    tt::ReminderView reminder;
    reminder.rule.id = 1;
    reminder.rule.app = "a";
    reminder.name = L"应用 A";
    result.reminders.push_back(reminder);
    return result;
}
} // namespace
namespace tt {
struct UiTestAccess {
    static_assert(std::is_same_v<decltype(&Ui::act), void (Ui::*)(Ui::Hit)>,
                  "Actions must own their input across nested dialog message loops");
    static void draw(Graphics &graphics, Ui &ui, float height = 810);
    static int hit(const Ui &ui, int action, const std::string &app = {},
                   Ui::Hit::Role role = Ui::Hit::Control);
    static void focusAndIcons(Graphics &graphics);
    static HWND createWindow(Ui &ui);
    static void failuresAndLifetime(Graphics &graphics);
};
void UiTestAccess::draw(Graphics &graphics, Ui &ui, float height) {
    ui.textFactory_ = graphics.fonts;
    graphics.target->BeginDraw();
    auto drawn = ui.draw(graphics.target.Get(), 1140, height);
    auto ended = graphics.target->EndDraw();
    require(SUCCEEDED(drawn) && SUCCEEDED(ended), "Draw native view");
}
int UiTestAccess::hit(const Ui &ui, int action, const std::string &app, Ui::Hit::Role role) {
    auto found = std::find_if(ui.hits_.begin(), ui.hits_.end(), [&](const auto &entry) {
        return entry.action == action && entry.app == app && entry.role == role;
    });
    require(found != ui.hits_.end(), "Expected control exists");
    return static_cast<int>(found - ui.hits_.begin());
}
void UiTestAccess::focusAndIcons(Graphics &graphics) {
    require(tt::applicationNameKey(L"ÄÖÜ Приложение").find(
                tt::applicationNameKey(L"äöü приложение")) != std::string::npos,
            "Search normalization handles non-ASCII case consistently");
    tt::Ui ui(nullptr, nullptr, {});
    ui.snapshot_ = fixture();
    ui.day_ = ui.snapshot_.day;
    ui.page_ = 3;
    ui.selected_ = "a";
    draw(graphics, ui);
    ui.focus_ = hit(ui, 217, "a");
    ui.snapshot_.reminders.front().state.token = "notification";
    draw(graphics, ui);
    require(ui.focus_ == hit(ui, 217, "a"),
            "Reminder actions preserve focus on source management");
    draw(graphics, ui, 680);
    require(ui.focus_ == hit(ui, 217, "a"),
            "Clipped source controls retain identity without invalid focus geometry");
    ui.focus_ = hit(ui, 100);
    ui.snapshot_.reminders.front().state.token.clear();
    draw(graphics, ui);
    require(ui.focus_ == hit(ui, 100) &&
                !ui.hits_[ui.focus_].sameControl(
                    ui.hits_[hit(ui, 100, {}, tt::Ui::Hit::Navigation)]),
            "Back and top overview navigation have separate identities");
    ui.promptMembers("a");
    require(ui.pendingMembers_ == "a", "Source manager requests member icons only on demand");

    ui.page_ = 0;
    ui.focus_ = -1;
    draw(graphics, ui);
    ui.focus_ = hit(ui, 211, "b");
    std::swap(ui.snapshot_.apps[0], ui.snapshot_.apps[1]);
    ui.hover_ = ui.focus_;
    draw(graphics, ui);
    require(ui.focus_ == hit(ui, 211, "b"), "Rank changes preserve focused application");
    require(ui.hover_ == -1, "Rebuilt controls discard stale pointer indices");
    std::erase_if(ui.snapshot_.apps, [](const auto &row) { return row.app.id == "b"; });
    draw(graphics, ui);
    require(ui.focus_ == -1, "Removing a focused application clears focus");

    ui.page_ = 1;
    ui.snapshot_.apps.clear();
    auto icon = LoadIconW(nullptr, IDI_APPLICATION);
    require(icon != nullptr, "Load system icon for reminder regression");
    ui.snapshot_.reminders.front().icon = std::shared_ptr<void>(icon, [](void *) {});
    draw(graphics, ui);
    require(ui.paintCache_.icons.contains(ui.snapshot_.reminders.front().icon),
            "Reminder icons work without selected-period usage rows");
}
HWND UiTestAccess::createWindow(Ui &ui) {
    WNDCLASSW definition{};
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpfnWndProc = tt::Ui::proc;
    definition.lpszClassName = L"TimeTrace.UiRegression";
    RegisterClassW(&definition);
    auto window = CreateWindowExW(0, definition.lpszClassName, L"UI regression",
                                  WS_OVERLAPPEDWINDOW, 0, 0, 1140, 810, nullptr, nullptr,
                                  definition.hInstance, &ui);
    require(window != nullptr, "Create hidden UI regression window");
    return window;
}
void UiTestAccess::failuresAndLifetime(Graphics &graphics) {
    tt::ui::PaintCache cache;
    tt::ui::Canvas first(graphics.target.Get(), graphics.fonts.Get(), cache);
    auto brush = first.brush;
    tt::ui::Canvas second(graphics.target.Get(), graphics.fonts.Get(), cache);
    require(SUCCEEDED(first.status) && second.brush.Get() == brush.Get(),
            "Canvases reuse their target brush");
    require(first.font(tt::ui::metrics::body) != nullptr, "Create cached text format");
    auto formats = cache.formats.size();
    cache.resetTarget();
    require(!cache.brush && cache.icons.empty() && cache.formats.size() == formats,
            "Target reset releases target resources and retains fonts");

    tt::ui::Canvas missing(nullptr, nullptr, cache);
    missing.fill({0, 0, 32, 32}, tt::ui::theme::text);
    missing.text(L"unavailable", {0, 0, 200, 40});
    missing.duration(1000, {0, 0, 200, 80});
    require(FAILED(missing.status), "Missing graphics resources safely stop drawing");
    tt::ui::Canvas invalidFont(graphics.target.Get(), graphics.fonts.Get(), cache);
    require(invalidFont.font(-1) == nullptr && FAILED(invalidFont.status),
            "Font creation failure is reported without dereferencing a null resource");
    invalidFont.text(L"still failed", {0, 0, 200, 40});
    require(FAILED(invalidFont.status), "Drawing failure remains sticky within one paint");

    tt::Ui ui(nullptr, nullptr, {});
    tt::Snapshot failure;
    failure.error = L"无法打开数据文件：测试错误";
    ui.update(std::move(failure));
    require(!ui.serviceError_.empty(), "Background startup error survives without a window");
    auto window = createWindow(ui);
    ui.paint();
    require(ui.target_ && ui.paintCache_.brush && !ui.serviceError_.empty(),
            "Opening a window retains the startup error");
    ui.paintCache_.brush = brush;
    InvalidateRect(window, nullptr, FALSE);
    ui.paint();
    require(!ui.target_ && !ui.paintCache_.brush && ui.paintCache_.icons.empty(),
            "EndDraw failure releases target and dependent resources");
    require(!GetUpdateRect(window, nullptr, FALSE),
            "Drawing failure completes painting without scheduling a busy retry");
    InvalidateRect(window, nullptr, FALSE);
    ui.paint();
    require(ui.target_ && ui.paintCache_.brush, "Next ordinary paint recreates resources");
    ui.close();
    require(!ui.window_ && !ui.target_ && !ui.paintCache_.brush && !ui.serviceError_.empty(),
            "Closing releases drawing resources and retains background startup error");
    createWindow(ui);
    ui.paint();
    require(!ui.serviceError_.empty(), "Reopening preserves an unavailable service error");
    tt::Snapshot recovered;
    recovered.request = ui.request_;
    recovered.day = ui.day_;
    // A healthy response supersedes a transient unsolicited error.
    if (!recovered.request)
        recovered.request = ++ui.request_;
    ui.update(std::move(recovered));
    require(ui.serviceError_.empty(), "Successful service response clears a transient error");
    ui.close();
}
} // namespace tt
int main() {
    auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        require(SUCCEEDED(apartment), "Initialize graphics apartment");
        {
            Graphics graphics;
            tt::UiTestAccess::focusAndIcons(graphics);
            tt::UiTestAccess::failuresAndLifetime(graphics);
        }
        CoUninitialize();
        std::cout << "PASS: " << checks << " UI checks\n";
        return 0;
    } catch (const std::exception &error) {
        if (SUCCEEDED(apartment))
            CoUninitialize();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
