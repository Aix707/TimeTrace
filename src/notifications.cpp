#include "platform.h"
#include "notification_protocol.h"
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/base.h>

namespace tt {
bool showToast(const ReminderEvent &event) {
    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument xml;
        xml.LoadXml(reminderToastXml(event));
        auto toast = winrt::Windows::UI::Notifications::ToastNotification(xml);
        toast.Tag(std::to_wstring(event.rule.id));
        toast.Group(L"reminders");
        auto notifier =
            winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(
                L"TimeTrace.Desktop");
        if (notifier.Setting() != winrt::Windows::UI::Notifications::NotificationSetting::Enabled)
            return false;
        notifier.Show(toast);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace tt
