#include "platform.h"
#include "reminders.h"
#include <algorithm>
#include <set>
#include <objbase.h>

namespace tt {
Service::Service(std::filesystem::path dir, HWND target)
    : directory_(std::move(dir)), target_(target), thread_([this] { run(); }) {}
Service::~Service() {
    send({Command::Stop});
    if (thread_.joinable())
        thread_.join();
}
void Service::send(Command command) {
    if (!alive_)
        return;
    std::lock_guard lock(mutex_);
    if (command.kind == Command::Query || command.kind == Command::ReleaseView)
        std::erase_if(queue_,
                      [](const Command &pending) { return pending.kind == Command::Query; });
    queue_.push_back(std::move(command));
    ready_.notify_one();
}
void Service::run() {
    auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        if (FAILED(apartment))
            throw std::runtime_error("Cannot initialize background apartment");
        Storage db(directory_ / L"data.db");
        AppResolver resolver;
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid)))
            throw std::runtime_error("Cannot create reminder session identity");
        wchar_t token[40]{};
        StringFromGUID2(guid, token, 40);
        std::wstring seed = token;
        seed.erase(std::remove(seed.begin(), seed.end(), L'{'), seed.end());
        seed.erase(std::remove(seed.begin(), seed.end(), L'}'), seed.end());
        ReminderEngine engine(utf8(seed));
        auto today = localDay(utcNow());
        engine.load(db.reminderRules(), db.reminderStates(today), db.dailyUsage(today), today,
                    true);
        auto mapping = db.appMappings();
        std::set<std::string> registered;
        std::vector<Usage> pending;
        Sample latest{};
        std::wstring error;
        std::unordered_map<std::string, std::shared_ptr<void>> icons;
        Millis savedTick = static_cast<Millis>(GetTickCount64());
        auto persist = [&] {
            db.commit(pending, engine.dirty() ? engine.states() : std::vector<ReminderState>{});
            pending.clear();
            engine.saved();
            savedTick = static_cast<Millis>(GetTickCount64());
        };
        auto changeDay = [&](const std::string &day) {
            if (day == engine.day())
                return;
            persist();
            engine.changeDay(db.reminderStates(day), db.dailyUsage(day), day);
        };
        Timeline timeline(
            [&](const Usage &usage) {
                for (const auto &part : splitLocal(usage)) {
                    changeDay(part.day);
                    auto root = mapping.find(usage.app);
                    engine.add(root == mapping.end() ? usage.app : root->second, part.duration);
                    if (usage.app.empty())
                        continue;
                    if (!pending.empty() && pending.back().app == usage.app &&
                        pending.back().startUtc + pending.back().duration == part.startUtc)
                        pending.back().duration += part.duration;
                    else
                        pending.push_back({usage.app, part.startUtc, part.duration});
                }
            },
            [&] { engine.resetDevice(); });
        auto reload = [&](bool resetDevice = false) {
            auto day = localDay(utcNow());
            engine.load(db.reminderRules(), db.reminderStates(day), db.dailyUsage(day), day,
                        resetDevice);
            mapping = db.appMappings();
        };
        auto available = [&] {
            return latest.tick > 0 && !latest.paused && !latest.locked && !latest.sleeping &&
                   latest.tick - latest.lastInput < kIdleMs;
        };
        auto evaluate = [&] {
            if (!engine.due(available()))
                return;
            auto before = engine;
            auto events = engine.evaluate(true);
            try {
                persist();
            } catch (...) {
                engine = std::move(before);
                throw;
            }
            for (auto &event : events) {
                for (const auto &app : db.applications())
                    if (app.id == event.rule.app)
                        event.name = app.name;
                auto p = new ReminderEvent(std::move(event));
                if (!PostMessageW(target_, WM_NOTICE, 0, reinterpret_cast<LPARAM>(p)))
                    delete p;
            }
        };
        auto snapshot = [&](const Command &command) {
            auto data =
                std::make_unique<Snapshot>(db.snapshot(command.day, command.weekly, command.app));
            data->reminders = engine.views();
            data->request = command.request;
            for (auto &view : data->reminders) {
                view.name = view.rule.kind == ReminderKind::Device ? L"休息提醒" : L"";
                for (const auto &app : data->catalog)
                    if (app.id == view.rule.app)
                        view.name = app.name;
            }
            std::vector<AppRow *> needed;
            for (auto &row : data->apps)
                if (row.app.id == command.app)
                    needed.push_back(&row);
            for (bool reminders : {true, false})
                for (auto &row : data->apps) {
                    if (needed.size() >= 64)
                        break;
                    if (reminders &&
                        !std::any_of(data->reminders.begin(), data->reminders.end(),
                                     [&](const auto &view) { return view.rule.app == row.app.id; }))
                        continue;
                    if (std::find(needed.begin(), needed.end(), &row) == needed.end())
                        needed.push_back(&row);
                }
            std::erase_if(icons, [&](const auto &item) {
                return std::none_of(needed.begin(), needed.end(),
                                    [&](const auto *row) { return row->app.id == item.first; });
            });
            for (auto row : needed) {
                auto [found, inserted] = icons.try_emplace(row->app.id);
                // Cache failures too; keep only this view's icons, up to 64 handles.
                if (inserted)
                    found->second = loadAppIcon(row->app.path);
                row->icon = found->second;
            }
            data->paused = latest.paused;
            data->error = error;
            if (PostMessageW(target_, WM_SNAPSHOT, 0, reinterpret_cast<LPARAM>(data.get())))
                data.release();
        };
        bool previousPaused = false, stop = false;
        while (!stop) {
            Command command;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return !queue_.empty(); });
                command = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                switch (command.kind) {
                case Command::Observe: {
                    if (latest.tick && command.sample.tick < latest.tick)
                        break;
                    latest = command.sample;
                    if (previousPaused != latest.paused) {
                        db.setSetting("paused", latest.paused ? "1" : "0");
                        previousPaused = latest.paused;
                    }
                    bool collecting = !latest.paused && !latest.locked && !latest.sleeping;
                    auto app = collecting ? resolver.resolve(latest.window, latest.pid) : AppInfo{};
                    if (!app.id.empty() && registered.insert(app.id).second) {
                        db.app(app);
                        if (!mapping.contains(app.id))
                            mapping[app.id] = app.id;
                    }
                    timeline.observe(
                        {latest.tick, latest.utc, latest.lastInput, app.id, collecting});
                    changeDay(localDay(latest.utc));
                    evaluate();
                    if (latest.tick - savedTick >= 5000)
                        persist();
                    break;
                }
                case Command::Query:
                    persist();
                    evaluate();
                    snapshot(command);
                    break;
                case Command::ReleaseView:
                    icons.clear();
                    break;
                case Command::SetReminder: {
                    persist();
                    auto changed = db.setReminder(command.app, command.value, command.enabled);
                    if (changed)
                        reload(command.app.empty());
                    evaluate();
                    break;
                }
                case Command::RemoveReminder:
                    persist();
                    db.removeReminder(command.value);
                    reload();
                    break;
                case Command::ActOnReminder: {
                    changeDay(localDay(utcNow()));
                    auto before = engine;
                    if (engine.apply(command.action)) {
                        try {
                            persist();
                        } catch (...) {
                            engine = std::move(before);
                            throw;
                        }
                    }
                    break;
                }
                case Command::Rename:
                    db.renameApp(command.app, command.name);
                    break;
                case Command::Merge:
                    persist();
                    db.mergeApps(command.app, command.target);
                    reload();
                    evaluate();
                    break;
                case Command::Clear:
                    db.clear();
                    pending.clear();
                    timeline.reset();
                    reload(true);
                    break;
                case Command::Stop:
                    persist();
                    stop = true;
                    break;
                }
                error.clear();
            } catch (const std::exception &e) {
                error = L"记录保存失败：" + wide(e.what());
                auto data = new Snapshot;
                data->day = localDay(utcNow());
                data->error = error;
                data->request = command.request;
                if (!PostMessageW(target_, WM_SNAPSHOT, 0, reinterpret_cast<LPARAM>(data)))
                    delete data;
                if (pending.size() > 8192) {
                    pending.clear();
                    timeline.reset();
                    reload(true);
                }
                if (command.kind == Command::Stop)
                    stop = true;
            }
        }
    } catch (const std::exception &e) {
        auto data = new Snapshot;
        data->day = localDay(utcNow());
        data->error = L"无法打开数据文件：" + wide(e.what());
        if (!PostMessageW(target_, WM_SNAPSHOT, 0, reinterpret_cast<LPARAM>(data)))
            delete data;
    }
    if (SUCCEEDED(apartment))
        CoUninitialize();
    alive_ = false;
}
} // namespace tt
