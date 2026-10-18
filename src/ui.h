#pragma once
#include "platform.h"
#include "ui_canvas.h"
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <wrl/client.h>

namespace tt {
namespace ui {
struct Canvas;
}
class Ui {
  public:
    Ui(Service *service, HWND host, std::filesystem::path directory);
    ~Ui();
    void show(bool reminders = false);
    void update(Snapshot snapshot);
    void request();
    bool visible() const {
        return window_ != nullptr && !IsIconic(window_);
    }
    void close();
    bool renderPng(const std::filesystem::path &path, Snapshot snapshot, int page = 0,
                   std::string selected = {}, int width = 1140, int height = 810);

  private:
    Service *service_;
    HWND host_{}, window_{};
    std::filesystem::path directory_;
    Snapshot snapshot_;
    uint64_t request_{};
    int page_{}, scroll_{}, focus_{-1}, hover_{-1};
    bool weekly_{}, followToday_{true};
    std::string day_, selected_;
    struct Hit {
        D2D1_RECT_F rect;
        int action;
        std::string app;
    };
    std::vector<Hit> hits_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> textFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    ui::PaintCache paintCache_;
    std::shared_ptr<void> appIcon_;
    void factories();
    void button(ui::Canvas &canvas, D2D1_RECT_F rect, const std::wstring &label, int action,
                std::string app = {}, bool primary = false, bool danger = false);
    void linkButton(ui::Canvas &canvas, D2D1_RECT_F rect, const std::wstring &label, int action,
                    std::string app = {}, bool danger = false, bool disclosure = false);
    void segments(ui::Canvas &canvas, D2D1_RECT_F rect, const std::vector<std::wstring> &labels,
                  int selected, int firstAction);
    void toggle(ui::Canvas &canvas, D2D1_RECT_F rect, bool enabled, int action);
    void applicationIcon(ui::Canvas &canvas, const AppRow *app, D2D1_RECT_F rect);
    void drawSettings(ui::Canvas &canvas, float x, float right);
    void drawUsage(ui::Canvas &canvas, float x, float right, float height, Millis total);
    void drawOverview(ui::Canvas &canvas, float x, float right, float height);
    void drawDetail(ui::Canvas &canvas, float x, float right, float height);
    void drawPeriod(ui::Canvas &canvas, float x, float right, float top);
    void drawChart(ui::Canvas &canvas, float x, float right, float top, float bottom, Millis total);
    void appReminderRow(ui::Canvas &canvas, const ReminderView &view, float x, float right,
                        float top, bool showApp);
    void reminderActions(ui::Canvas &canvas, float right, float top, const std::string &app = {});
    int hitAt(float x, float y) const;

    void paint();
    void draw(ID2D1RenderTarget *rt, float width, float height);
    void act(const Hit &hit);
    void promptReminder(const std::string &app, bool device = false, bool chooseApp = false);
    void promptApp(const std::string &app, bool merge);
    void drawReminders(ui::Canvas &canvas, float x, float right, float height);
    const ReminderView *reminder(const std::string &app) const;
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
};
} // namespace tt
