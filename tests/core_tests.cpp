#include "core.h"
#include <iostream>
#include <stdexcept>
using namespace nr;
static void require(bool ok, const char *msg) {
    if (!ok)
        throw std::runtime_error(msg);
}
int main() {
    try {
        auto d = parseDate("2026-09-21");
        require(d > 0 && dateText(d) == "2026-09-21", "date roundtrip");
        require(parseDate("2026-02-30") == -1, "invalid date");
        require(parseTime("23:59:59") == 86399, "seconds precision");
        require(parseTime("24:00:00") == -1, "invalid hour");
        Plan p;
        p.channel = "a";
        p.date = d;
        p.start = 23 * 3600 + 59 * 60 + 58;
        p.end = 2;
        auto v = occurrences(p, d, d + 2 * 86400000);
        require(v.size() == 1 && v[0].end - v[0].start == 4000, "cross midnight duration");
        auto q = p;
        q.key = id();
        q.date = d + 86400000;
        q.start = 1;
        q.end = 3;
        require(conflicts(p, q), "cross midnight overlap");
        q.start = 2;
        require(!conflicts(p, q), "adjacent intervals allowed");
        p.repeat = 2;
        p.weekMask = 1;
        p.start = 10;
        p.end = 20;
        v = occurrences(p, d, d + 7 * 86400000);
        require(v.size() == 1 && v[0].start == d + 10000, "Monday weekly");
        p.skips.push_back(d);
        require(occurrences(p, d, d + 86400000).empty(), "skip occurrence");
        p.skips.clear();
        p.repeat = 1;
        require(occurrences(p, d, d + 3 * 86400000).size() == 3, "daily expansion");
        p.start = p.end = 0;
        require(occurrences(p, d, d + 86400000).size() == 1, "24 hour plan");
        require(json::object().get<Settings>().uiScale == 1., "old config uses system scale");
        RecordingStats stats;
        WriteRate rate;
        rate.reset(1000);
        for (int i = 1; i <= 8; ++i) rate.sample(1000 + i*1000, i*2000000ULL, stats);
        require(std::abs(stats.recentMBps-2.) < .001 && std::abs(stats.averageMBps-2.) < .001, "five second and average write rate");
        rate.sample(10000, 19000000, stats); // Cumulative bytes include the closed segment.
        require(std::abs(stats.peakMBps-3.) < .001 && stats.bytes == 19000000, "segment continuity and peak");
        rate.sample(10000, 19000000, stats);
        require(std::isfinite(stats.recentMBps), "zero elapsed sample safe");
        stats = {}; rate.reset(10000); rate.sample(11000, 1000000, stats);
        require(stats.averageMBps == 1 && stats.peakMBps == 1, "new task resets rate and peak");
        auto peak = stats.peakMBps;
        rate.sample(11100, 9000000, stats);
        require(stats.peakMBps == 8, "partial final interval is padded to one second, not 80 MB/s");
        rate.sample(12000, 10, stats);
        require(!stats.sampled && stats.averageMBps == 0 && stats.recentMBps == 0, "counter reset clears stale rates");
        require(json::object().get<Profile>().recordGainDB == 0, "old channel has unity recording gain");
        require(json::object().get<Settings>().monitorVolume == 1, "old settings use 100 percent monitoring");
        Profile invalidGain; invalidGain.recordGainDB = 25;
        require(!validate(invalidGain).empty(), "recording gain limit enforced");
        Profile profile;
        require(validate(profile).empty(), "default profile");
        profile.width = 1919;
        profile.height = 1080;
        require(!validate(profile).empty(), "odd dimensions rejected");
        profile = Profile{};
        profile.codec = "ffv1";
        profile.lapseMode = 1;
        require(!validate(profile).empty(), "lossless lapse rejected");
        SourceClock clock;
        clock.map(100000000,0,true);
        for(i64 t=10000;t<10000000;t+=10000) clock.map(100000000+t,t,true);
        for(i64 t=10000000;t<15000000;t+=10000) clock.map(100000000+t-600000,t,true);
        require(clock.corrections==1,"sub-two-second clock regression is corrected");
        require(std::abs(clock.map(115000000-600000,15000000,true)-15000000)<1000,"clock recovered");
        RecentStats rolling; RecordingStats counters; double window=0; RecordingStats recent;
        for(int t=0;t<=86400;t++) {counters.frames=t*60ULL;counters.black=t;counters.bytes=t*1000000ULL;
            recent=rolling.sample(t*1000LL,counters,window);}
        require(window==60 && recent.black==60 && recent.frames==3600 && recent.bytes==60000000,"24h rolling counters bounded to 60 seconds");
        auto root = fs::temp_directory_path() / wide("ndi-recorder-test-" + id());
        fs::create_directories(root);
        {
            Store store(root / "test.db");
            Settings s;
            s.storage = utf8((root / "recordings").wstring());
            Channel ch;
            ch.name = "中文通道";
            ch.source = "Test (NDI)";
            std::vector<Channel> chs{ch};
            std::vector<Plan> ps{p};
            store.save(s, chs, ps);
            Settings back;
            std::vector<Channel> bc;
            std::vector<Plan> bp;
            store.load(back, bc, bp);
            require(bc.size() == 1 && bc[0].name == ch.name && bp.size() == 1, "sqlite config roundtrip");
            stats.complete = true;
            require(store.saveStats("job", stats), "stats write");
            auto restored = store.taskStats("job").get<RecordingStats>();
            require(restored.bytes == stats.bytes && restored.complete, "stats roundtrip");
            stats.complete = false; stats.bytes = 0;
            require(store.saveStats("job", stats) && store.taskStats("job")["complete"] == true, "late snapshot cannot overwrite final stats");
            require(store.taskStats("legacy").is_null(), "old history has no stats");
            auto row = store.history({0, d, 0, ch.key, "job", "a.mp4", "recording", ""});
            store.finish(row, d + 1000, "completed");
            store.renameHistory(row,"final.mp4");
            require(store.historyPage()[0].path=="final.mp4","renamed path persisted");
            store.backup(root/"snapshot.db");
            {Store snapshot(root/"snapshot.db");require(snapshot.historyPage()[0].path=="final.mp4","online snapshot consistent");}
            require(store.historyPage()[0].state == "completed", "history finish");
            auto r = store.history({0, d + 2000, 0, ch.key, "job2", "b.mp4", "recording", ""});
            store.finishTask("job2", d + 3000);
            require(store.historyPage()[0].state == "interrupted", "crash recovery");
        }
        atomicJson(root / "config.json", json{{"name", "测试"}});
        require(readJson(root / "config.json")["name"] == "测试", "atomic json roundtrip");
        std::cout << "PASS: calendar boundaries, conflicts, profiles, SQLite transactions/history, JSON\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
