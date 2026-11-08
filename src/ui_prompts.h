#pragma once
#include "ui_canvas.h"
#include <windows.h>
#include <commctrl.h>
namespace tt::ui {
struct Prompt {
    enum Kind { Reminder, Rename, Merge, AddReminder, GroupMembers, MoveMember } kind{Reminder};
    HWND edit{}, combo{}, list{}, related{}, count{}, detail{};
    HFONT font{};
    HIMAGELIST images{};
    std::wstring appName, appPath, text;
    Millis baseUsed{};
    int minutes{60}, result{-1};
    bool device{}, accepted{};
    std::vector<AppInfo> choices;
    std::string target;
    std::vector<MergeCandidate> mergeCandidates;
    std::vector<bool> checked;
    std::vector<std::string> targets;
    std::vector<AppRow> members;
    bool updatingList{};
    UINT dpi{96};
    int hotButton{};
    ComPtr<ID2D1Factory> drawFactory;
    ComPtr<IDWriteFactory> textFactory;
    ComPtr<ID2D1DCRenderTarget> drawTarget;
    PaintCache paintCache;
};
bool showPrompt(HWND owner, Prompt &prompt);
} // namespace tt::ui
