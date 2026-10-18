#include <fstream>
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    std::ofstream out(argv[1]);
    try {
        winrt::init_apartment();
        auto items =
            winrt::Windows::UI::Notifications::ToastNotificationManager::History().GetHistory(
                L"TimeTrace.Desktop");
        out << "{\"notification_history_count\": " << items.Size()
            << ", \"registered_notification_seen\": " << (items.Size() > 0 ? "true" : "false")
            << "}\n";
        return items.Size() > 0 ? 0 : 3;
    } catch (const winrt::hresult_error &e) {
        out << "{\"hresult\": " << e.code().value << ", \"registered_notification_seen\": false}\n";
        return 4;
    }
}
