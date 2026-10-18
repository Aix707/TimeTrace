#pragma once
#include "core.h"
#include <windows.h>
namespace tt::ui {
struct Prompt {
    enum Kind { Reminder, Rename, Merge, AddReminder } kind{Reminder};
    HWND edit{}, combo{};
    HFONT font{};
    std::wstring appName, text;
    int minutes{60}, result{-1};
    bool device{}, accepted{};
    std::vector<AppInfo> choices;
    std::string target;
};
bool showPrompt(HWND owner, Prompt &prompt);
} // namespace tt::ui
