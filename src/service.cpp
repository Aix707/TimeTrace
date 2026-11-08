#include "platform.h"
#include "reminders.h"
#include <algorithm>
#include <set>
#include <objbase.h>

namespace tt {
Service::Service(std::filesystem::path dir, HWND target)
    : directory_(std::move(dir)), target_(target), thread_([this] { run(); }) {}
Service::~Service() {
    finish();
}
bool Service::finish() {
    send({Command::Stop});
    if (thread_.joinable())
        thread_.join();
    return shutdownSaved_;
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
        std::map<std::string, ResolvedApp> registered;
        std::map<std::string, ResolvedApp> registrations;
        for (const auto &app : db.identitySources()) {
            if (!app.id.starts_with("app:"))
                registrations.emplace(app.id, resolver.identify(app));
        }
        std::vector<Usage> pending;
        struct Activity {
            Usage usage;
            std::string day;
            bool interrupted{};
        };
        std::deque<Activity> activities;
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
                    if (!activities.empty() && !activities.back().interrupted &&
                        activities.back().day == part.day &&
                        activities.back().usage.app == usage.app &&
                        activities.back().usage.startUtc + activities.back().usage.duration ==
                            part.startUtc)
                        activities.back().usage.duration += part.duration;
                    else
                        activities.push_back({{usage.app, part.startUtc, part.duration}, part.day});
                }
            },
            [&] {
                if (activities.empty() || !activities.back().interrupted)
                    activities.push_back({{}, {}, true});
            });
        auto reload = [&](bool resetDevice = false) {
            auto day = engine.day();
            engine.load(db.reminderRules(), db.reminderStates(day), db.dailyUsage(day), day,
                        resetDevice);
            mapping = db.appMappings();
        };
        bool mappingsChanged = false;
        auto consume = [&] {
            if (mappingsChanged) {
                reload();
                mappingsChanged = false;
            }
            if (!registrations.empty()) {
                // Commit the old mapping before regrouping reminder rules and states.
                persist();
                for (auto it = registrations.begin(); it != registrations.end();) {
                    const auto &app = it->second;
                    db.registerApp(app.source, app.representative);
                    mappingsChanged = true;
                    registered.insert_or_assign(it->first, app);
                    it = registrations.erase(it);
                }
                reload();
                mappingsChanged = false;
            }
            // Timeline callbacks never write to SQLite. A failed day transition leaves
            // this unconsumed event queued while Timeline still advances exactly once.
            while (!activities.empty()) {
                const auto &activity = activities.front();
                if (activity.interrupted) {
                    engine.resetDevice();
                } else {
                    changeDay(activity.day);
                    const auto &usage = activity.usage;
                    auto root = mapping.find(usage.app);
                    engine.add(root == mapping.end() ? usage.app : root->second, usage.duration);
                    if (!usage.app.empty()) {
                        if (!pending.empty() && pending.back().app == usage.app &&
                            pending.back().startUtc + pending.back().duration == usage.startUtc)
                            pending.back().duration += usage.duration;
                        else
                            pending.push_back(usage);
                    }
                }
                activities.pop_front();
            }
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
            auto needed = command.iconApps;
            if (needed.empty())
                for (const auto &row : data->apps) {
                    if (needed.size() >= std::min<size_t>(64, command.iconCount))
                        break;
                    needed.push_back(row.app.id);
                }
            if (!data->selected.app.id.empty())
                needed.insert(needed.begin(), data->selected.app.id);
            std::set<std::string> unique;
            std::erase_if(needed, [&](const auto &id) { return !unique.insert(id).second; });
            if (needed.size() > 64)
                needed.resize(64);
            std::erase_if(icons, [&](const auto &item) {
                return std::find(needed.begin(), needed.end(), item.first) == needed.end();
            });
            for (const auto &id : needed) {
                auto app = std::find_if(data->catalog.begin(), data->catalog.end(),
                                        [&](const auto &entry) { return entry.id == id; });
                if (app == data->catalog.end())
                    continue;
                auto [found, inserted] = icons.try_emplace(id);
                // Cache failures too; keep only this view's icons, up to 64 handles.
                if (inserted)
                    found->second = loadAppIcon(app->path);
            }
            for (auto &row : data->apps)
                if (auto found = icons.find(row.app.id); found != icons.end())
                    row.icon = found->second;
            if (auto found = icons.find(data->selected.app.id); found != icons.end())
                data->selected.icon = found->second;
            data->paused = latest.paused;
            data->error = error;
            if (PostMessageW(target_, WM_SNAPSHOT, 0, reinterpret_cast<LPARAM>(data.get())))
                data.release();
        };
        bool previousPaused = false, stop = false;
        unsigned stopAttempts{};
        while (!stop) {
            Command command;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return !queue_.empty(); });
                command = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                error.clear();
                switch (command.kind) {
                case Command::Observe: {
                    if (latest.tick && command.sample.tick < latest.tick)
                        break;
                    latest = command.sample;
                    bool collecting = !latest.paused && !latest.locked && !latest.sleeping;
                    auto app =
                        collecting ? resolver.resolve(latest.window, latest.pid) : ResolvedApp{};
                    if (!app.source.id.empty()) {
                        auto previous = registered.find(app.source.id);
                        if (previous == registered.end() ||
                            previous->second.representative.id != app.representative.id ||
                            previous->second.representative.name != app.representative.name ||
                            previous->second.representative.path != app.representative.path ||
                            previous->second.source.name != app.source.name ||
                            previous->second.source.path != app.source.path)
                            registrations.insert_or_assign(app.source.id, app);
                    }
                    timeline.observe(
                        {latest.tick, latest.utc, latest.lastInput, app.source.id, collecting});
                    consume();
                    changeDay(localDay(latest.utc));
                    if (previousPaused != latest.paused) {
                        db.setSetting("paused", latest.paused ? "1" : "0");
                        previousPaused = latest.paused;
                    }
                    evaluate();
                    if (latest.tick - savedTick >= 5000)
                        persist();
                    break;
                }
                case Command::Query:
                    consume();
                    persist();
                    evaluate();
                    snapshot(command);
                    break;
                case Command::ReleaseView:
                    icons.clear();
                    break;
                case Command::SetReminder: {
                    consume();
                    persist();
                    auto changed = db.setReminder(command.app, command.value, command.enabled);
                    if (changed)
                        reload(command.app.empty());
                    evaluate();
                    break;
                }
                case Command::RemoveReminder:
                    consume();
                    persist();
                    db.removeReminder(command.value);
                    reload();
                    break;
                case Command::ActOnReminder: {
                    consume();
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
                    consume();
                    db.renameApp(command.app, command.name);
                    break;
                case Command::Merge:
                    consume();
                    persist();
                    db.mergeApps(command.app, command.target);
                    reload();
                    evaluate();
                    break;
                case Command::Clear:
                    db.clear();
                    pending.clear();
                    activities.clear();
                    timeline.reset();
                    reload(true);
                    break;
                case Command::Stop:
                    consume();
                    persist();
                    shutdownSaved_ = true;
                    stop = true;
                    break;
                }
            } catch (const std::exception &e) {
                error = L"记录保存失败：" + wide(e.what());
                auto data = new Snapshot;
                data->day = localDay(utcNow());
                data->error = error;
                data->request = command.request;
                if (!PostMessageW(target_, WM_SNAPSHOT, 0, reinterpret_cast<LPARAM>(data)))
                    delete data;
                if (command.kind == Command::Stop) {
                    if (++stopAttempts < 2) {
                        Sleep(100);
                        send({Command::Stop});
                    } else {
                        stop = true;
                    }
                }
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
