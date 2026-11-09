#include "core.h"
#include <psapi.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <stdexcept>
namespace nr {
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0, 0, 0);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, 0, 0);
    return r;
}
fs::path exeDir() {
    wchar_t b[32768];
    GetModuleFileNameW(nullptr, b, 32768);
    return fs::path(b).parent_path();
}
i64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
i64 dayStart(i64 t) {
    return ((t + 28800000) / 86400000) * 86400000 - 28800000;
}
std::string dateText(i64 ms, bool clock) {
    time_t sec = ms / 1000 + 28800;
    tm t{};
    gmtime_s(&t, &sec);
    char b[64];
    strftime(b, sizeof b, clock ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d", &t);
    return b;
}
std::string id() {
    static std::atomic<unsigned> n = 0;
    std::ostringstream s;
    s << std::hex << nowMs() << '-' << GetCurrentProcessId() << '-' << ++n;
    return s.str();
}
std::string safeName(std::string s) {
    for (char &c : s)
        if ((unsigned char)c < 32 || std::string("\\/:*?\"<>|").find(c) != std::string::npos)
            c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' '))
        s.pop_back();
    return s.empty() ? "channel" : s;
}
i64 parseDate(const std::string &s) {
    int y, m, d;
    char tail;
    if (sscanf_s(s.c_str(), "%d-%d-%d%c", &y, &m, &d, &tail, 1) != 3)
        return -1;
    std::chrono::year_month_day v{std::chrono::year(y), std::chrono::month((unsigned)m),
                                  std::chrono::day((unsigned)d)};
    if (!v.ok() || y < 2000 || y > 2200)
        return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days(v).time_since_epoch())
               .count() -
           28800000;
}
int parseTime(const std::string &s) {
    int h, m, v;
    char tail;
    if (sscanf_s(s.c_str(), "%d:%d:%d%c", &h, &m, &v, &tail, 1) != 3 || h < 0 || h > 23 || m < 0 || m > 59 ||
        v < 0 || v > 59)
        return -1;
    return h * 3600 + m * 60 + v;
}
std::string timeText(int v) {
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d", v / 3600, v / 60 % 60, v % 60);
    return b;
}
std::vector<Occurrence> occurrences(const Plan &p, i64 from, i64 to) {
    std::vector<Occurrence> out;
    if (!p.enabled || to <= from)
        return out;
    for (i64 d = dayStart(from) - 86400000; d <= dayStart(to); d += 86400000) {
        if (p.repeat == 0 && d != p.date)
            continue;
        int wd = (int)(((d + 28800000) / 86400000 + 3) % 7);
        if (p.repeat == 2 && !(p.weekMask & (1 << wd)))
            continue;
        if (std::find(p.skips.begin(), p.skips.end(), d) != p.skips.end())
            continue;
        i64 a = d + p.start * 1000LL, b = d + p.end * 1000LL;
        if (p.end <= p.start)
            b += 86400000;
        if (a < to && b > from)
            out.push_back({p.key, a, b});
    }
    return out;
}
bool conflicts(const Plan &a, const Plan &b) {
    if (a.key == b.key || a.channel != b.channel || !a.enabled || !b.enabled)
        return false;
    i64 lo = dayStart(nowMs()), hi = lo + 16 * 86400000LL;
    if (a.repeat == 0) {
        lo = a.date - 86400000;
        hi = a.date + 2 * 86400000;
    } else if (b.repeat == 0) {
        lo = b.date - 86400000;
        hi = b.date + 2 * 86400000;
    }
    if (a.repeat && b.repeat)
        for (auto *p : {&a, &b})
            for (auto d : p->skips)
                hi = std::max(hi, d + 15 * 86400000LL);
    auto aa = occurrences(a, lo, hi), bb = occurrences(b, lo, hi);
    for (auto &x : aa)
        for (auto &y : bb)
            if (x.start < y.end && y.start < x.end)
                return true;
    return false;
}
std::string validate(const Profile &p) {
    if (p.codec != "h264" && p.codec != "ffv1")
        return "不支持的视频编码";
    if (p.width < 0 || p.height < 0 || (p.width == 0) != (p.height == 0) || p.width % 2 || p.height % 2 ||
        p.width > 7680 || p.height > 4320)
        return "分辨率须为偶数，或宽高均为 0 跟随输入";
    if (p.fps < 0 || p.fps > 120 || p.mbps < 1 || p.mbps > 500 || p.segmentMinutes < 1 ||
        p.segmentMinutes > 1440)
        return "帧率、码率或分段长度超出范围";
    if (!std::isfinite(p.audioGainDB) || p.audioGainDB < -60 || p.audioGainDB > 20)
        return "音频电平换算超出范围";
    if (!std::isfinite(p.recordGainDB) || p.recordGainDB < -24 || p.recordGainDB > 24)
        return "录制增益须在 -24 到 +24 dB 之间";
    if (p.audioPair < 0 || p.audioPair > 31)
        return "音频声道对超出范围";
    if (p.lapseMode < 0 || p.lapseMode > 2 || !std::isfinite(p.lapseValue) || p.lapseValue < 1 ||
        p.lapseValue > 86400 || p.lapseFps < 1 || p.lapseFps > 60)
        return "延时摄影参数超出范围";
    if (p.codec == "ffv1" && (p.width || p.height || p.fps || p.lapseMode))
        return "无损模式不能缩放、改变帧率或延时摄影";
    return {};
}
void atomicJson(const fs::path &p, const json &j) {
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << j.dump(2);
        f.flush();
        if (!f)
            throw std::runtime_error("write config failed");
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("replace config failed");
}
json readJson(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("read config failed");
    return json::parse(f);
}
void Store::exec(const char *s) {
    char *e = nullptr;
    int rc = sqlite3_exec(db_, s, nullptr, nullptr, &e);
    if (rc != SQLITE_OK) {
        std::string msg = e ? e : "sqlite error";
        sqlite3_free(e);
        throw std::runtime_error(msg);
    }
}
Store::Store(const fs::path &p) {
    fs::create_directories(p.parent_path());
    if (sqlite3_open(utf8(p.wstring()).c_str(), &db_) != SQLITE_OK)
        throw std::runtime_error("cannot open database");
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY "
         "KEY,v TEXT NOT NULL); CREATE TABLE IF NOT EXISTS history(id INTEGER PRIMARY KEY,channel TEXT,task "
         "TEXT,path TEXT,start INTEGER,end INTEGER,state TEXT,detail TEXT); CREATE INDEX IF NOT EXISTS "
         "history_time ON history(start,end); CREATE TABLE IF NOT EXISTS task_stats(task TEXT PRIMARY KEY, data TEXT NOT NULL);");
}
Store::~Store() {
    sqlite3_close(db_);
}
struct Statement {
    sqlite3_stmt *p = nullptr;
    Statement(sqlite3 *d, const char *s) {
        if (sqlite3_prepare_v2(d, s, -1, &p, 0) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(d));
    }
    ~Statement() { sqlite3_finalize(p); }
    void text(int n, const std::string &v) { sqlite3_bind_text(p, n, v.c_str(), -1, SQLITE_TRANSIENT); }
    void num(int n, i64 v) { sqlite3_bind_int64(p, n, v); }
    void done() {
        if (sqlite3_step(p) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
};
void Store::save(const Settings &s, const std::vector<Channel> &c, const std::vector<Plan> &p) {
    json j = {{"version", 1}, {"settings", s}, {"channels", c}, {"plans", p}};
    exec("BEGIN IMMEDIATE");
    try {
        Statement q(db_, "INSERT OR REPLACE INTO config VALUES('main',?)");
        q.text(1, j.dump());
        q.done();
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}
void Store::load(Settings &s, std::vector<Channel> &c, std::vector<Plan> &p) {
    Statement q(db_, "SELECT v FROM config WHERE k='main'");
    if (sqlite3_step(q.p) == SQLITE_ROW) {
        auto j = json::parse((const char *)sqlite3_column_text(q.p, 0));
        s = j.at("settings").get<Settings>();
        if (!std::isfinite(s.uiScale) || s.uiScale < .8 || s.uiScale > 1.5) s.uiScale = 1.;
        if (!std::isfinite(s.monitorVolume) || s.monitorVolume < 0 || s.monitorVolume > 4) s.monitorVolume = 1.;
        c = j.at("channels").get<std::vector<Channel>>();
        p = j.at("plans").get<std::vector<Plan>>();
    }
}
i64 Store::history(const History &h) {
    Statement q(db_, "INSERT INTO history(channel,task,path,start,end,state,detail) VALUES(?,?,?,?,?,?,?)");
    q.text(1, h.channel);
    q.text(2, h.task);
    q.text(3, h.path);
    q.num(4, h.start);
    q.num(5, h.end);
    q.text(6, h.state);
    q.text(7, h.detail);
    q.done();
    return sqlite3_last_insert_rowid(db_);
}
void Store::finish(i64 r, i64 e, const std::string &s, const std::string &d) {
    Statement q(db_, "UPDATE history SET end=?,state=?,detail=? WHERE id=?");
    q.num(1, e);
    q.text(2, s);
    q.text(3, d);
    q.num(4, r);
    q.done();
}
void Store::renameHistory(i64 row, const std::string &path) {
    Statement q(db_, "UPDATE history SET path=? WHERE id=?");
    q.text(1,path); q.num(2,row); q.done();
}
void Store::backup(const fs::path &path) {
    sqlite3 *dest=nullptr;
    if (sqlite3_open(utf8(path.wstring()).c_str(), &dest)!=SQLITE_OK) {
        if(dest) sqlite3_close(dest);
        throw std::runtime_error("diagnostic backup open failed");
    }
    auto b=sqlite3_backup_init(dest,"main",db_,"main");
    int rc=SQLITE_ERROR;
    if(b) {
        auto deadline=GetTickCount64()+15000;
        do {
            rc=sqlite3_backup_step(b,128);
            if(rc==SQLITE_BUSY || rc==SQLITE_LOCKED) Sleep(20);
        } while((rc==SQLITE_OK || rc==SQLITE_BUSY || rc==SQLITE_LOCKED) && GetTickCount64()<deadline);
        sqlite3_backup_finish(b);
    }
    sqlite3_close(dest);
    if(rc!=SQLITE_DONE) throw std::runtime_error("diagnostic backup incomplete; retry later");
}
static History row(sqlite3_stmt *q) {
    auto t = [&](int n) {
        auto p = sqlite3_column_text(q, n);
        return p ? std::string((const char *)p) : std::string();
    };
    return {sqlite3_column_int64(q, 0),
            sqlite3_column_int64(q, 4),
            sqlite3_column_int64(q, 5),
            t(1),
            t(2),
            t(3),
            t(6),
            t(7)};
}
std::vector<History> Store::historyPage(int offset, int limit) {
    Statement q(db_, "SELECT id,channel,task,path,start,end,state,detail FROM history ORDER BY start DESC "
                     "LIMIT ? OFFSET ?");
    q.num(1, limit);
    q.num(2, offset);
    std::vector<History> r;
    while (sqlite3_step(q.p) == SQLITE_ROW)
        r.push_back(row(q.p));
    return r;
}
std::vector<History> Store::historyRange(i64 a, i64 b) {
    Statement q(db_, "SELECT id,channel,task,path,start,end,state,detail FROM history WHERE start<? AND "
                     "(end>? OR end=0) ORDER BY start DESC LIMIT 1000");
    q.num(1, b);
    q.num(2, a);
    std::vector<History> r;
    while (sqlite3_step(q.p) == SQLITE_ROW)
        r.push_back(row(q.p));
    return r;
}
void Store::finishTask(const std::string &t, i64 e) {
    Statement q(
        db_, "UPDATE history SET state='interrupted',end=?,detail='工作进程异常退出' WHERE task=? AND end=0");
    q.num(1, e);
    q.text(2, t);
    q.done();
}
bool Store::saveStats(const std::string &task, const RecordingStats &s) noexcept {
    // Optional telemetry must never wait behind the recorder's history transaction.
    sqlite3_busy_timeout(db_, 0);
    bool ok = false;
    try {
        Statement q(db_, "INSERT INTO task_stats(task,data) VALUES(?,?) ON CONFLICT(task) DO UPDATE SET data=excluded.data WHERE json_extract(task_stats.data,'$.complete') IS NOT 1");
        q.text(1, task); q.text(2, json(s).dump()); q.done(); ok = true;
    } catch (...) {}
    sqlite3_busy_timeout(db_, 5000);
    return ok;
}
json Store::taskStats(const std::string &task) {
    Statement q(db_, "SELECT data FROM task_stats WHERE task=?");
    q.text(1, task);
    return sqlite3_step(q.p) == SQLITE_ROW ? json::parse((const char *)sqlite3_column_text(q.p, 0)) : json();
}
i64 Store::lastSeen() {
    Statement q(db_, "SELECT v FROM config WHERE k='last_seen'");
    return sqlite3_step(q.p) == SQLITE_ROW ? sqlite3_column_int64(q.p, 0) : 0;
}
void Store::checkpoint(i64 t) {
    Statement q(db_, "INSERT OR REPLACE INTO config(k,v) VALUES('last_seen',?)");
    q.num(1, t);
    q.done();
}
void Store::recoverInterrupted() {
    Statement q(db_, "UPDATE history SET state='interrupted',detail='上次进程异常退出，文件待检查',end=CASE "
                     "WHEN end=0 THEN start ELSE end END WHERE state='recording'");
    q.done();
}
Logger::Logger(fs::path d, std::string t, int days, int mb)
    : dir_(d), tag_(safeName(t)), days_(days), mb_(mb) {
    fs::create_directories(dir_);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}
Logger::~Logger() {
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
void Logger::write(const std::string &s) {
    std::lock_guard l(mutex_);
    if (queue_.size() >= 2048) {
        ++dropped_;
        return;
    }
    queue_.push_back(dateText(nowMs(), true) + " " + s);
    cv_.notify_one();
}
void Logger::run(std::stop_token st) {
    i64 cleaned = 0;
    while (true) {
        std::deque<std::string> q;
        {
            std::unique_lock l(mutex_);
            cv_.wait_for(l, std::chrono::seconds(1), [&] { return !queue_.empty() || st.stop_requested(); });
            q.swap(queue_);
        }
        try {
            auto p = dir_ / wide(dateText(nowMs()) + "-" + tag_ + ".log");
            std::error_code ec;
            if (fs::exists(p) && fs::file_size(p, ec) > 16 * 1024 * 1024) {
                auto old = p;
                old += L".1";
                fs::remove(old, ec);
                fs::rename(p, old, ec);
            }
            std::ofstream f(p, std::ios::app | std::ios::binary);
            if (auto n = dropped_.exchange(0))
                f << "WARNING log queue overflow: " << n << "\n";
            for (auto &s : q)
                f << s << "\n";
            if (nowMs() - cleaned > 60000) {
                cleaned = nowMs();
                std::vector<fs::directory_entry> entries;
                uintmax_t bytes = 0;
                for (auto &e : fs::directory_iterator(dir_)) {
                    if (!e.is_regular_file() ||
                        e.path().filename().wstring().find(L".log") == std::wstring::npos)
                        continue;
                    auto age = fs::file_time_type::clock::now() - e.last_write_time();
                    if (age > std::chrono::hours(days_.load() * 24)) {
                        fs::remove(e.path(), ec);
                        continue;
                    }
                    entries.push_back(e);
                    bytes += e.file_size();
                }
                std::sort(entries.begin(), entries.end(),
                          [](auto &a, auto &b) { return a.last_write_time() < b.last_write_time(); });
                for (auto &e : entries) {
                    if (bytes <= (uintmax_t)mb_.load() * 1024 * 1024)
                        break;
                    if (e.path() == p)
                        continue;
                    auto n = e.file_size();
                    if (fs::remove(e.path(), ec))
                        bytes -= n;
                }
            }
        } catch (...) {
        }
        if (st.stop_requested()) {
            std::lock_guard l(mutex_);
            if (queue_.empty())
                break;
        }
    }
}
void Metrics::update(const fs::path &disk) {
    i64 t = nowMs();
    FILETIME i, k, u;
    auto cv = [](FILETIME f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    if (GetSystemTimes(&i, &k, &u)) {
        auto a = cv(i), b = cv(k), c = cv(u);
        auto all = (b - kernel_) + (c - user_);
        if (last_ && all)
            cpu = 100. * (all - (a - idle_)) / all;
        idle_ = a;
        kernel_ = b;
        user_ = c;
    }
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    totalMemoryGB = ms.ullTotalPhys / 1073741824.;
    memoryGB = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.;
    PROCESS_MEMORY_COUNTERS pm{sizeof pm};
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    processMB = pm.WorkingSetSize / 1048576.;
    ULARGE_INTEGER free, total, avail;
    if (GetDiskFreeSpaceExW(disk.c_str(), &avail, &total, &free)) {
        freeGB = avail.QuadPart / 1073741824.;
        totalGB = total.QuadPart / 1073741824.;
    } else {
        freeGB = totalGB = -1;
    }
    PMIB_IF_TABLE2 table = nullptr;
    networks.clear();
    if (GetIfTable2(&table) == NO_ERROR) {
        for (ULONG n = 0; n < table->NumEntries; n++) {
            auto &r = table->Table[n];
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.InterfaceAndOperStatusFlags.FilterInterface)
                continue;
            auto old = net_[r.InterfaceLuid.Value];
            double sec = (t - last_) / 1000.;
            Network v{utf8(r.Alias), 0, 0, r.ReceiveLinkSpeed / 1e6, r.OperStatus == IfOperStatusUp};
            if (last_ && sec > 0 && r.InOctets >= old.first && r.OutOctets >= old.second) {
                v.rx = (r.InOctets - old.first) * 8 / sec / 1e6;
                v.tx = (r.OutOctets - old.second) * 8 / sec / 1e6;
            }
            net_[r.InterfaceLuid.Value] = {r.InOctets, r.OutOctets};
            networks.push_back(v);
        }
        FreeMibTable(table);
    }
    last_ = t;
}
void setAutoStart(bool enabled) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        throw std::runtime_error("cannot open startup registry");
    if (enabled) {
        auto s = L"\"" + (exeDir() / L"ndi-recorder.exe").wstring() + L"\" --tray";
        RegSetValueExW(k, L"NDIRecorder", 0, REG_SZ, (const BYTE *)s.c_str(), (DWORD)((s.size() + 1) * 2));
    } else
        RegDeleteValueW(k, L"NDIRecorder");
    RegCloseKey(k);
}
} // namespace nr
