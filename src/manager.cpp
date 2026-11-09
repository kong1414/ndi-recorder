#include "manager.h"
#include "media.h"
#include <stdexcept>
namespace nr {
Runtime::~Runtime() {
    if (shared)
        InterlockedExchange(&shared->stop, 1);
    if (process) {
        if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT)
            TerminateProcess(process, 3);
        CloseHandle(process);
    }
    if (texture)
        texture->Release();
    if (shared)
        UnmapViewOfFile(shared);
    if (map)
        CloseHandle(map);
    if (killJob)
        CloseHandle(killJob);
}
Manager::Manager(fs::path d) : data_(d), store(d / "recorder.db"), log(d / "logs", "app") {
    store.load(settings, channels, plans);
    log.configure(settings.logDays, settings.logMB);
    store.recoverInterrupted();
    auto previous = store.lastSeen(), now = nowMs();
    if (previous > 0 && previous < now) {
        auto from = std::max(previous, now - 90 * 86400000LL);
        for (auto &p : plans)
            for (auto &o : occurrences(p, from, now))
                if (o.end <= now) {
                    bool enabled = false;
                    for (auto &c : channels)
                        if (c.key == p.channel)
                            enabled = c.enabled;
                    if (enabled)
                        store.history({0, std::max(previous, o.start), o.end, p.channel, p.key, "", "missed",
                                       "程序未运行，无法补录"});
                }
    }
    store.checkpoint(now);
    fs::create_directories(data_ / "jobs");
    try {
        fs::create_directories(wide(settings.storage));
    } catch (const std::exception &e) {
        log.write(e.what());
    }
    refreshDiscovery();
    recovery_ =
        std::jthread([this](std::stop_token st) { inspectInterrupted(data_ / "recorder.db", log, st); });
    log.write("application start");
}
Manager::~Manager() {
    recovery_.request_stop();
    if (recovery_.joinable())
        recovery_.join();
    stopAll();
    runtimes.clear();
    try {
        store.checkpoint(nowMs());
    } catch (...) {
    }
    log.write("application stop");
}
Runtime &Manager::runtime(const Channel &c) {
    auto &r = runtimes[c.key];
    if (!r) {
        r = std::make_unique<Runtime>();
        r->preview = std::make_unique<Preview>(ndi, c.source);
        r->previewSource = c.source;
    }
    return *r;
}
void Manager::refreshDiscovery() {
    finder = std::make_unique<Finder>(ndi, settings.extraIPs);
}
void Manager::save() {
    store.save(settings, channels, plans);
}
void Manager::start(Channel &c, Runtime &r, i64 begin, i64 end) {
    Job j;
    j.channel = c;
    j.settings = settings;
    j.dbPath = utf8((data_ / "recorder.db").wstring());
    j.dataPath = utf8(data_.wstring());
    j.start = begin;
    j.end = end;
    r.token = j.token;
    r.statsSavedAt = 0; r.savedStats.clear();
    auto name = wide("Local\\NDIRecorder-" + j.token);
    r.map =
        CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
    if (!r.map)
        throw std::runtime_error("worker shared memory failed");
    r.shared = (Shared *)MapViewOfFile(r.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!r.shared)
        throw std::runtime_error("worker map failed");
    memset(r.shared, 0, sizeof(Shared));
    r.shared->version = 4; r.shared->structBytes = sizeof(Shared);
    r.shared->heartbeat = GetTickCount64();
    r.shared->endWall = end;
    r.shared->recordGainMilliDB = (LONG)std::lround(c.profile.recordGainDB * 1000);
    auto file = data_ / "jobs" / wide(j.token + ".json");
    atomicJson(file, j);
    auto app = exeDir() / L"ndi-recorder.exe";
    std::wstring cmd = L"\"" + app.wstring() + L"\" --worker \"" + file.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, exeDir().c_str(), &si, &pi))
        throw std::runtime_error("worker process creation failed");
    r.killJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!r.killJob ||
        !SetInformationJobObject(r.killJob, JobObjectExtendedLimitInformation, &limits, sizeof limits) ||
        !AssignProcessToJobObject(r.killJob, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 3);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        throw std::runtime_error("worker lifetime isolation failed");
    }
    r.process = pi.hProcess;
    r.stopping = false;
    r.error.clear();
    r.healthySince = 0;
    r.activeProfile = c.profile;
    r.activeSource = c.source;
    r.startedAt = nowMs();
    memset(&r.snapshot, 0, sizeof r.snapshot);
    r.snapshot.heartbeat = GetTickCount64();
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    log.write("start channel=" + c.key + " task=" + j.token);
}
void Manager::collect(Channel &c, Runtime &r) {
    if (!r.process)
        return;
    if (r.shared) {
        for (int tries = 0; tries < 3; tries++) {
            LONG seq = InterlockedCompareExchange(&r.shared->seq, 0, 0);
            if (seq & 1)
                continue;
            Shared candidate{};
            MemoryBarrier();
            memcpy(&candidate, r.shared, sizeof(Shared));
            MemoryBarrier();
            if (seq == InterlockedCompareExchange(&r.shared->seq, 0, 0) && candidate.version == 4 && candidate.structBytes == sizeof(Shared)) {
                memcpy(&r.snapshot, &candidate, sizeof candidate);
                break;
            }
        }
        if (r.snapshot.state == 2 && !r.stopping && GetTickCount64() - r.snapshot.heartbeat < 1000) {
            r.error.clear();
            if (!r.healthySince)
                r.healthySince = GetTickCount64();
            if (GetTickCount64() - r.healthySince >= 30000)
                r.retries = 0;
        } else
            r.healthySince = 0;
        if (GetTickCount64() - r.snapshot.heartbeat > 30000) {
            r.error = "工作进程无响应，已停止";
            TerminateProcess(r.process, 5);
        }
    }
    if (nowMs() - r.statsSavedAt >= 5000 && r.snapshot.stats.sampled) {
        r.statsSavedAt = nowMs();
        auto serialized = json(r.snapshot.stats).dump();
        if (serialized != r.savedStats && store.saveStats(r.token, r.snapshot.stats)) r.savedStats = serialized;
    }
    if (WaitForSingleObject(r.process, 0) != WAIT_OBJECT_0)
        return;
    if (r.snapshot.stats.sampled) store.saveStats(r.token, r.snapshot.stats);
    DWORD code = 0;
    GetExitCodeProcess(r.process, &code);
    if (code) {
        if (code != 5)
            r.error = r.snapshot.state == 4 && r.snapshot.message[0] ? r.snapshot.message : "录制工作进程失败";
        r.retries++;
        r.retryAt = nowMs() + (1LL << std::min(3, r.retries)) * 1000;
        r.blocked = r.retries > 3 || r.error.find("storage") != std::string::npos ||
                    r.error.find("disk") != std::string::npos;
        store.history({0, nowMs(), nowMs(), c.key, r.token, "", "failed", r.error});
        store.finishTask(r.token, nowMs());
        log.write("worker failure " + c.key + " " + r.error);
    }
    if (!code) {
        r.retries = 0;
        r.retryAt = 0;
        r.error.clear();
    }
    r.healthySince = 0;
    CloseHandle(r.process);
    r.process = nullptr;
    if (r.shared)
        UnmapViewOfFile(r.shared);
    r.shared = nullptr;
    if (r.map)
        CloseHandle(r.map);
    r.map = nullptr;
    if (r.killJob)
        CloseHandle(r.killJob);
    r.killJob = nullptr;
    r.stopping = false;
    std::error_code ec;
    fs::remove(data_ / "jobs" / wide(r.token + ".json"), ec);
}
void Manager::tick(bool visible) {
    i64 now = nowMs();
    if (now - checkpointAt_ >= 5000) {
        store.checkpoint(now);
        checkpointAt_ = now;
    }
    if (now - metricsAt_ >= 1000) {
        metricsAt_ = now;
        metrics.update(wide(settings.storage));
    }
    for (auto &c : channels) {
        auto &r = runtime(c);
        if (!visible)
            r.preview->visible(false, settings.previewFps);
        collect(c, r);
        const auto &previewSource = r.process ? r.activeSource : c.source;
        if (r.previewSource != previewSource) {
            r.preview = std::make_unique<Preview>(ndi, previewSource);
            r.previewSource = previewSource;
            r.preview->monitor(r.monitor);
            r.generation = 0;
            r.image = {};
        }
        r.preview->audioProfile(r.process ? r.activeProfile : c.profile);
        r.preview->volume(settings.monitorVolume);
        bool demand = r.manual;
        i64 begin = r.manual ? now : INT64_MAX, end = r.manual ? 0 : now;
        if (c.enabled)
            for (auto &p : plans)
                if (p.channel == c.key)
                    for (auto &o : occurrences(p, now, now + 5001)) {
                        if (r.process && r.snapshot.state == 2 && !r.manual && o.start > now)
                            continue;
                        demand = true;
                        begin = std::min(begin, o.start);
                        if (!r.manual)
                            end = std::max(end, o.end);
                    }
        if (!r.process && metrics.freeGB >= 0 && metrics.freeGB < settings.reserveGB && demand) {
            r.blocked = true;
            r.error = "磁盘安全余量不足，请清理后手动恢复";
            if (r.shared)
                InterlockedExchange(&r.shared->stop, 1);
        }
        if (demand && !r.blocked && !r.process && now >= r.retryAt) {
            try {
                start(c, r, std::max(begin, now), end);
            } catch (const std::exception &e) {
                r.error = e.what();
                r.blocked = true;
                log.write(r.error);
            }
        }
        if (r.shared && !r.stopping && demand)
            InterlockedExchange64(&r.shared->endWall, end);
        if (!demand && r.process && !r.stopping) {
            // Natural completion must drain the worker's 250 ms jitter buffer.
            // An explicit cancellation before the deadline is irreversible; renewed demand
            // starts a fresh worker after this one has finished.
            i64 previousEnd = InterlockedCompareExchange64(&r.shared->endWall, 0, 0);
            if (previousEnd == 0 || previousEnd > now) {
                InterlockedExchange64(&r.shared->endWall, now);
                InterlockedExchange(&r.shared->stop, 1);
            }
            r.stopping = true;
        }
    }
}
void Manager::resume(Channel &c) {
    auto &r = runtime(c);
    r.blocked = false;
    r.error.clear();
    r.retries = 0;
    r.retryAt = 0;
}
void Manager::beginManual(Channel &c) {
    resume(c);
    runtime(c).manual = true;
}
void Manager::stopManual(Channel &c) {
    runtime(c).manual = false;
}
void Manager::stopAll() {
    for (auto &[k, r] : runtimes) {
        r->manual = false;
        if (r->shared)
            InterlockedExchange(&r->shared->stop, 1);
    }
    i64 deadline = nowMs() + 8000;
    for (auto &[k, r] : runtimes)
        if (r->process)
            WaitForSingleObject(r->process, (DWORD)std::max(0LL, deadline - nowMs()));
}
bool Manager::busy() const {
    for (auto &[k, r] : runtimes)
        if (r->process)
            return true;
    return false;
}
void Manager::exportConfig(const fs::path &p) {
    atomicJson(p, {{"version", 1}, {"settings", settings}, {"channels", channels}, {"plans", plans}});
}
void Manager::importConfig(const fs::path &p) {applyConfig(readJson(p));}
void Manager::validateConfig(const json &j) {
    if (j.value("version", 0) != 1)
        throw std::runtime_error("配置版本不支持");
    for(auto field:{"channels","plans"}) {
        if(!j.at(field).is_array()) throw std::runtime_error("配置列表格式无效");
        for(auto &entry:j.at(field)) if(!entry.contains("key") || !entry.at("key").is_string() || entry.at("key").get<std::string>().empty())
            throw std::runtime_error("配置缺少任务或通道标识");
    }
    auto s = j.at("settings").get<Settings>();
    auto c = j.at("channels").get<std::vector<Channel>>();
    auto plansNew = j.at("plans").get<std::vector<Plan>>();
    if (s.storage.empty() || s.previewFps < 1 || s.previewFps > 30 || s.reserveGB < 0 || s.logDays < 1 ||
        s.logMB < 16 || !std::isfinite(s.monitorVolume) || s.monitorVolume < 0 || s.monitorVolume > 4 || !std::isfinite(s.uiScale) || s.uiScale < .8 || s.uiScale > 1.5 || !validate(s.defaults).empty())
        throw std::runtime_error("基础配置无效");
    std::set<std::string> keys;
    for (auto &v : c) {
        auto e = validate(v.profile);
        if (!e.empty() || v.key.empty() || v.name.empty() || v.source.empty() || !keys.insert(v.key).second)
            throw std::runtime_error("通道配置无效");
    }
    std::set<std::string> planKeys;
    for (auto &v : plansNew) {
        if (v.key.empty() || v.name.empty() || !planKeys.insert(v.key).second || !keys.count(v.channel) || v.start < 0 || v.start >= 86400 ||
            v.end < 0 || v.end >= 86400 || v.repeat < 0 || v.repeat > 2 || !v.weekMask ||
            v.date < parseDate("2000-01-01") || v.date > parseDate("2200-01-01"))
            throw std::runtime_error("计划配置无效");
        for (auto &w : plansNew)
            if (conflicts(v, w))
                throw std::runtime_error("配置存在重叠计划");
    }
}
json Manager::previewImport(const json &j, const std::set<std::string> &selectedPlans,
                            const std::set<std::string> &selectedChannels, bool includeSettings) const {
    validateConfig(j);
    if(selectedPlans.empty() && selectedChannels.empty() && !includeSettings)
        throw std::runtime_error("请至少选择一项内容");
    auto mergedChannels=channels; auto mergedPlans=plans;
    auto needed=selectedChannels; std::set<std::string> foundPlans, foundChannels;
    for(auto &entry:j.at("plans")) {
        auto plan=entry.get<Plan>();
        if(!selectedPlans.count(plan.key)) continue;
        foundPlans.insert(plan.key);needed.insert(plan.channel);
        auto it=std::find_if(mergedPlans.begin(),mergedPlans.end(),[&](auto &v){return v.key==plan.key;});
        if(it==mergedPlans.end()) mergedPlans.push_back(plan);
        else if(json(*it)!=json(plan)) throw std::runtime_error("同标识计划已有不同内容："+plan.name+"，请取消选择后重试");
    }
    for(auto &entry:j.at("channels")) {
        auto channel=entry.get<Channel>();
        if(!needed.count(channel.key)) continue;
        foundChannels.insert(channel.key);
        auto it=std::find_if(mergedChannels.begin(),mergedChannels.end(),[&](auto &v){return v.key==channel.key;});
        if(it==mergedChannels.end()) mergedChannels.push_back(channel);
        else if(json(*it)!=json(channel)) throw std::runtime_error("同标识通道已有不同配置："+channel.name+"，不会覆盖，请取消相关选择");
    }
    if(foundPlans!=selectedPlans || foundChannels!=needed) throw std::runtime_error("选择的任务或通道不存在");
    json merged={{"version",1},{"settings",includeSettings?j.at("settings"):json(settings)},
                 {"channels",mergedChannels},{"plans",mergedPlans}};
    validateConfig(merged); // Also reject overlaps with the user's existing schedules.
    return merged;
}
void Manager::applyConfig(const json &j) {
    if(busy()) throw std::runtime_error("请先停止全部录制再导入配置");
    validateConfig(j);
    auto s=j.at("settings").get<Settings>();
    auto c=j.at("channels").get<std::vector<Channel>>();
    auto plansNew=j.at("plans").get<std::vector<Plan>>();
    store.save(s, c, plansNew);
    settings = s;
    channels = c;
    plans = plansNew;
    runtimes.clear();
    log.configure(settings.logDays,settings.logMB);
    refreshDiscovery();
}
} // namespace nr
