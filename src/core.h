#pragma once
#include <winsock2.h>
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <json.hpp>
#include <sqlite3.h>
namespace nr {
using json = nlohmann::json;
namespace fs = std::filesystem;
using i64 = long long;
std::wstring wide(const std::string &);
std::string utf8(const std::wstring &);
fs::path exeDir();
i64 nowMs();
i64 dayStart(i64 ms);
std::string dateText(i64 ms, bool clock = false);
std::string id();
std::string safeName(std::string s);
i64 parseDate(const std::string &);
int parseTime(const std::string &);
std::string timeText(int);
struct Profile {
    std::string codec = "h264", encoder = "auto";
    int width = 0, height = 0, fps = 0, mbps = 20, audioPair = 0, segmentMinutes = 60, fieldOrder = -1;
    bool lowBandwidth = false;
    double audioGainDB = -20, recordGainDB = 0;
    int lapseMode = 0;
    double lapseValue = 10;
    int lapseFps = 25;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Profile, codec, encoder, width, height, fps, mbps, audioPair,
                                                segmentMinutes, fieldOrder, lowBandwidth, audioGainDB,
                                                lapseMode, lapseValue, lapseFps, recordGainDB)
struct Channel {
    std::string key = id(), name = "NDI", source;
    bool enabled = true;
    Profile profile;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Channel, key, name, source, enabled, profile)
struct Plan {
    std::string key = id(), channel, name = "录制计划";
    int repeat = 0, start = 0, end = 3600, weekMask = 127;
    i64 date = 0;
    bool enabled = true;
    std::vector<i64> skips;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Plan, key, channel, name, repeat, start, end, weekMask, date,
                                                enabled, skips)
struct Settings {
    std::string storage = utf8((exeDir() / "recordings").wstring()), extraIPs;
    int previewFps = 15, logDays = 30, logMB = 1024;
    double reserveGB = 10, uiScale = 1.0, monitorVolume = 1.0;
    bool autoStart = false;
    Profile defaults;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, storage, extraIPs, previewFps, logDays, logMB,
                                                reserveGB, autoStart, defaults, uiScale, monitorVolume)
struct Occurrence {
    std::string plan;
    i64 start = 0, end = 0;
};
std::vector<Occurrence> occurrences(const Plan &, i64 from, i64 to);
bool conflicts(const Plan &, const Plan &);
std::string validate(const Profile &);
// Counters belong to one worker token; units/stages deliberately remain separate.
struct RecordingStats {
    unsigned long long sdkVideo = 0, sdkAudio = 0, queueVideo = 0, queueAudio = 0;
    unsigned long long filterVideo = 0, filterAudio = 0, fieldPairs = 0;
    unsigned long long repeated = 0, black = 0, silentSamples = 0, converted = 0;
    unsigned long long bytes = 0, frames = 0;
    double recentMBps = 0, averageMBps = 0, peakMBps = 0;
    bool sampled = false, complete = false;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RecordingStats, sdkVideo, sdkAudio, queueVideo, queueAudio,
    filterVideo, filterAudio, fieldPairs, repeated, black, silentSamples, converted, bytes, frames,
    recentMBps, averageMBps, peakMBps, sampled, complete)
// Fixed-size rolling diagnostics: no samples or counters grow with process uptime.
class RecentStats {
    std::deque<std::pair<i64, RecordingStats>> samples_;
  public:
    RecordingStats sample(i64 ms, const RecordingStats &s, double &seconds) {
        if (!samples_.empty() && (ms < samples_.back().first || s.frames < samples_.back().second.frames)) samples_.clear();
        samples_.push_back({ms,s});
        while (samples_.size()>2 && samples_[1].first <= ms-60000) samples_.pop_front();
        while (samples_.size()>62) samples_.pop_front();
        auto b=samples_.front().second; auto r=s;
        seconds=(ms-samples_.front().first)/1000.;
        auto sub=[](auto n,auto b){return n>=b?n-b:0;};
#define DELTA(x) r.x=sub(s.x,b.x)
        DELTA(sdkVideo); DELTA(sdkAudio); DELTA(queueVideo); DELTA(queueAudio);
        DELTA(filterVideo); DELTA(filterAudio); DELTA(fieldPairs); DELTA(repeated);
        DELTA(black); DELTA(silentSamples); DELTA(converted); DELTA(bytes); DELTA(frames);
#undef DELTA
        r.averageMBps=seconds>0?r.bytes/(1e6*seconds):0;
        r.peakMBps=0; r.complete=false;
        return r;
    }
};
// Source clock drift must be corrected before the 300 ms stale-picture threshold.
class SourceClock {
    double offset_=0;
    i64 driftSince_=-1;
    bool ready_=false;
  public:
    unsigned corrections=0;
    double errorUs=0;
    i64 map(i64 tc, i64 arrival, bool video) {
        if (!ready_) {offset_=double(arrival)-tc;ready_=true;}
        double mapped=tc+offset_;
        if (video) {
            errorUs=mapped-arrival;
            if (std::abs(errorUs)>100000) {
                if (driftSince_<0) driftSince_=arrival;
                if (std::abs(errorUs)>2000000 || arrival-driftSince_>=2000000) {
                    offset_=double(arrival)-tc; mapped=arrival; driftSince_=-1; ++corrections;
                }
            } else driftSince_=-1;
        }
        return (i64)std::llround(mapped);
    }
};
class WriteRate {
    struct Sample { i64 ms; unsigned long long bytes; };
    std::deque<Sample> samples_;
    i64 start_ = 0;
    unsigned long long base_ = 0;
  public:
    void reset(i64 ms, unsigned long long bytes = 0) { samples_ = {{ms, bytes}}; start_ = ms; base_ = bytes; }
    void sample(i64 ms, unsigned long long bytes, RecordingStats &s) {
        if (samples_.empty() || bytes < samples_.back().bytes) {
            reset(ms, bytes);
            s.recentMBps = s.averageMBps = s.peakMBps = 0; s.sampled = false;
        }
        s.bytes = bytes;
        auto last = samples_.back();
        if (ms <= last.ms) return;
        // Pad a partial interval to one second; include final writes without reporting a spurious burst.
        double instant = (bytes - last.bytes) / (1000. * std::max(1000LL, ms - last.ms));
        s.peakMBps = std::max(s.peakMBps, instant);
        samples_.push_back({ms, bytes});
        while (samples_.size() > 2 && samples_[1].ms <= ms - 5000) samples_.pop_front();
        // At most six regular one-second samples (plus one final partial interval).
        while (samples_.size() > 7) samples_.pop_front();
        s.recentMBps = (bytes - samples_.front().bytes) / (1000. * (ms - samples_.front().ms));
        s.averageMBps = ms > start_ ? (bytes - base_) / (1000. * (ms - start_)) : 0;
        s.sampled = true;
    }
};
struct Shared {
    volatile LONG stop = 0;
    volatile LONG seq = 0;
    unsigned version = 4, structBytes = sizeof(Shared);
    volatile LONG recordGainMilliDB = 0;
    RecordingStats stats, recentStats;
    double recentSeconds = 0;
    alignas(8) volatile LONG64 endWall = 0;
    DWORD pid = 0;
    ULONGLONG heartbeat = 0;
    int state = 0, width = 0, height = 0;
    double fps = 0, videoAge = 0, audioAge = 0, mbps = 0;
    unsigned long long frames = 0, drops = 0;
    char message[768]{}, encoder[64]{}, file[1024]{};
};
struct Job {
    Channel channel;
    Settings settings;
    std::string token = id(), dbPath, dataPath;
    i64 start = 0, end = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Job, channel, settings, token, dbPath, dataPath, start, end)
struct History {
    i64 row = 0, start = 0, end = 0;
    std::string channel, task, path, state, detail;
};
class Store {
    sqlite3 *db_ = nullptr;
    void exec(const char *);

  public:
    explicit Store(const fs::path &);
    ~Store();
    Store(const Store &) = delete;
    void save(const Settings &, const std::vector<Channel> &, const std::vector<Plan> &);
    void load(Settings &, std::vector<Channel> &, std::vector<Plan> &);
    i64 history(const History &);
    void finish(i64 row, i64 end, const std::string &state, const std::string &detail = "");
    std::vector<History> historyPage(int offset = 0, int limit = 100);
    std::vector<History> historyRange(i64 from, i64 to);
    void renameHistory(i64 row, const std::string &path);
    void backup(const fs::path &path);
    void recoverInterrupted();
    void finishTask(const std::string &task, i64 end);
    bool saveStats(const std::string &task, const RecordingStats &) noexcept;
    json taskStats(const std::string &task);
    i64 lastSeen();
    void checkpoint(i64 time);
};
class Logger {
    fs::path dir_;
    std::string tag_;
    std::atomic<int> days_, mb_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::jthread thread_;
    std::atomic<unsigned> dropped_ = 0;
    void run(std::stop_token);

  public:
    Logger(fs::path dir, std::string tag, int days = 30, int mb = 1024);
    ~Logger();
    void write(const std::string &);
    void configure(int days, int mb) {
        days_ = days;
        mb_ = mb;
    }
};
struct Network {
    std::string name;
    double rx = 0, tx = 0, speed = 0;
    bool up = false;
};
struct Metrics {
    double cpu = 0, memoryGB = 0, totalMemoryGB = 0, processMB = 0, freeGB = 0, totalGB = 0;
    std::vector<Network> networks;
    void update(const fs::path &);

  private:
    i64 last_ = 0;
    unsigned long long idle_ = 0, kernel_ = 0, user_ = 0;
    std::map<unsigned long long, std::pair<unsigned long long, unsigned long long>> net_;
};
void setAutoStart(bool enabled);
void atomicJson(const fs::path &, const json &);
json readJson(const fs::path &);
} // namespace nr
