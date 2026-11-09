#include "manager.h"
#include <iostream>
#include <stdexcept>
using namespace nr;
static void require(bool v, const char *msg) { if (!v) throw std::runtime_error(msg); }
static void release(Runtime &r) {
    if (r.process) CloseHandle(r.process);
    r.process = nullptr;
    if (r.shared) UnmapViewOfFile(r.shared);
    r.shared = nullptr;
    if (r.map) CloseHandle(r.map);
    r.map = nullptr;
    r.stopping = false;
}
static void attach(Runtime &r, const Channel &c, bool finished = false, int code = 0) {
    release(r);
    r.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
    r.shared = (Shared *)MapViewOfFile(r.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    require(r.shared != nullptr, "test mapping");
    memset(r.shared, 0, sizeof(Shared));
    r.shared->version = 4; r.shared->structBytes = sizeof(Shared);
    r.shared->heartbeat = GetTickCount64();
    r.shared->state = finished ? (code ? 4 : 3) : 2;
    r.shared->endWall = nowMs() + 3600000;
    strcpy_s(r.shared->message, "current failure");
    r.activeProfile = c.profile;
    r.activeSource = c.source;
    if (!finished) {
        r.process = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    } else {
        auto app = exeDir() / "manager-tests.exe";
        auto cmd = L"\"" + app.wstring() + L"\" --child " + std::to_wstring(code);
        STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
        require(CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &si, &pi), "test child");
        CloseHandle(pi.hThread);
        r.process = pi.hProcess;
        require(WaitForSingleObject(r.process, 5000) == WAIT_OBJECT_0, "test child exit");
    }
}
int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--child") return std::stoi(argv[2]);
    try {
        auto dir = fs::temp_directory_path() / wide("ndi-manager-tests-" + id());
        Manager m(dir);
        m.settings.storage = utf8(dir.wstring());
        m.settings.reserveGB = 0;
        Channel c; c.source = "MissingRegressionSource";
        m.channels.push_back(c);
        auto &r = m.runtime(m.channels[0]);
        Plan p; p.channel = c.key; p.repeat = 1; p.start = p.end = 0;
        m.plans.push_back(p);
        attach(r, c);
        m.tick(false);
        require(!r.stopping && !r.shared->stop, "active plan continues");
        r.snapshot.stats.frames = 7;
        r.shared->stats.frames = 42;
        r.shared->version = 999;
        m.tick(false);
        require(r.snapshot.stats.frames == 7, "reject incompatible shared protocol");
        r.shared->version = 4;
        r.shared->stats.sampled = true;
        r.token = "stats-protocol-test";
        r.statsSavedAt = 0;
        m.tick(false);
        require(r.snapshot.stats.frames == 42, "shared stats snapshot accepted");
        require(m.store.taskStats(r.token)["frames"] == 42, "manager checkpoints task stats");
        m.plans[0].enabled = false;
        m.tick(false);
        require(r.stopping && r.shared->stop && r.shared->endWall <= nowMs(), "disabled plan stops immediately");
        auto stopEnd = r.shared->endWall;
        m.plans[0].enabled = true;
        m.tick(false);
        require(r.stopping && r.shared->endWall == stopEnd, "renewed demand cannot revive stopping worker");
        release(r);
        attach(r, c);
        m.plans[0].enabled = false;
        r.shared->endWall = nowMs() - 1;
        m.tick(false);
        require(r.stopping && !r.shared->stop, "natural deadline drains jitter buffer");
        release(r);
        attach(r, c);
        r.manual = true;
        m.plans[0].enabled = false;
        m.tick(false);
        require(!r.stopping && r.shared->endWall == 0, "manual demand survives disabled plan");
        m.settings.reserveGB = 1e12;
        m.tick(false);
        require(!r.blocked && !r.shared->stop, "new storage settings do not stop active worker");
        release(r);
        m.tick(false);
        require(r.blocked && !r.process, "new job checks current storage reserve");
        m.settings.reserveGB = 0; r.manual = false; m.resume(m.channels[0]);
        for (int n = 1; n <= 4; ++n) {
            attach(r, c, true, 1);
            r.error = "stale error";
            m.tick(false);
            require(r.retries == n, "consecutive failure count");
            require(r.blocked == (n == 4), "three retries allowed before blocking");
            require(r.error == "current failure", "fresh failure replaces stale error");
        }
        m.resume(m.channels[0]);
        r.retries = 2; r.error = "old failure";
        attach(r, c); r.healthySince = GetTickCount64() - 31000;
        r.manual = true;
        m.tick(false);
        require(r.retries == 0 && r.error.empty(), "healthy worker resets failure budget and status");
        release(r);
        attach(r, c); r.retries = 2;
        r.healthySince = GetTickCount64() - 31000;
        r.shared->heartbeat = GetTickCount64() - 2000;
        m.tick(false);
        require(r.retries == 2, "stale recording snapshot does not reset failure budget");
        release(r); r.manual = false;
        r.retries = 2; r.error = "old failure";
        attach(r, c, true, 0);
        m.tick(false);
        require(r.retries == 0 && r.error.empty(), "normal completion clears failure state");
        m.plans[0].date=dayStart(nowMs());
        Channel extra;extra.name="Imported";extra.source="OtherSource";
        Channel unused;unused.name="Unused";unused.source="UnusedSource";
        Plan first;first.channel=extra.key;first.date=dayStart(nowMs());first.start=3600;first.end=7200;
        Plan second=first;second.key=id();second.start=10800;second.end=14400;
        json incoming={{"version",1},{"settings",m.settings},{"channels",std::vector<Channel>{extra,unused}},
                       {"plans",std::vector<Plan>{first,second}}};
        incoming["settings"]["monitorVolume"]=2.0;
        auto merged=m.previewImport(incoming,{first.key},{},false);
        require(merged["channels"].size()==2 && merged["plans"].size()==2,"only selected plan and required channel merged");
        require(merged["settings"]["monitorVolume"]==m.settings.monitorVolume,"settings excluded by default");
        require(m.channels.size()==1 && m.plans.size()==1,"preview does not mutate state");
        auto before=m.plans.size();
        auto bad=incoming;bad["plans"][0]["channel"]="missing";
        bool rejected=false;try{m.previewImport(bad,{first.key},{},false);}catch(...){rejected=true;}
        require(rejected && m.plans.size()==before,"invalid references rejected without mutation");
        attach(r,c);rejected=false;try{m.applyConfig(merged);}catch(...){rejected=true;}
        require(rejected,"import blocked while recording");release(r);
        m.applyConfig(merged);
        require(m.channels.size()==2 && m.plans.size()==2,"selected merge applied");
        auto again=m.previewImport(incoming,{first.key},{},false);
        require(again["channels"].size()==2 && again["plans"].size()==2,"same import is idempotent");
        bad=incoming;bad["channels"][0]["source"]="Changed";
        rejected=false;try{m.previewImport(bad,{first.key},{},false);}catch(...){rejected=true;}
        require(rejected,"same channel key with different content cannot overwrite");
        auto overlap=second;overlap.start=4000;overlap.end=6000;
        bad=incoming;bad["plans"]=std::vector<Plan>{overlap};
        rejected=false;try{m.previewImport(bad,{overlap.key},{},false);}catch(...){rejected=true;}
        require(rejected,"overlap with existing plan rejected");
        auto settingsOnly=m.previewImport(incoming,{}, {},true);
        require(settingsOnly["settings"]["monitorVolume"]==2.0 && settingsOnly["plans"].size()==2,"explicit settings import");
        std::cout << "PASS: cancellation, renewed/manual demand, storage isolation, retry budget and recovery\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
