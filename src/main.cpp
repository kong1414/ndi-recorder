#include "manager.h"
#include "media.h"
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace nr;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int k = 0; k < argc; k++)
        args.push_back(argv[k]);
    LocalFree(argv);
    try {
        if (argc >= 4 && args[1] == L"--diagnostic-export") {
            fs::path data=args[2], out=args[3];
            if(!fs::exists(data/L"recorder.db")) throw std::runtime_error("database not found");
            fs::create_directories(out);
            Store store(data/L"recorder.db");
            store.backup(out/L"recorder.db");
            return 0;
        }
        if (argc >= 3 && args[1] == L"--worker")
            return runWorker(args[2]);
        if (argc >= 3 && args[1] == L"--probe-encoders")
            return probeEncoders(args[2]);
        if (argc >= 4 && args[1] == L"--inspect-source") {
            Ndi ndi;
            return inspectSource(ndi, utf8(args[2]), args[3]);
        }
        if (argc >= 3 && args[1] == L"--list-sources") {
            Ndi ndi;
            Finder finder(ndi, "");
            Sleep(3000);
            atomicJson(args[2], finder.names());
            return 0;
        }
        if (argc >= 5 && args[1] == L"--test-source") {
            Ndi ndi;
            return testSource(ndi, utf8(args[2]), std::stoi(args[3]), std::stoi(args[4]),
                              argc > 5 ? std::stoi(args[5]) : 0, argc > 6 ? std::stoi(args[6]) : 0, argc > 7 ? std::stoi(args[7]) : 0);
        }
        if (argc >= 4 && args[1] == L"--record-test") {
            fs::path data = exeDir() / "test-data";
            fs::create_directories(data / "jobs");
            Job j;
            j.channel.source = utf8(args[2]);
            j.channel.name = "Test";
            j.channel.key = "test";
            j.settings.storage = utf8((data / "recordings").wstring());
            j.settings.reserveGB = 1;
            j.start = nowMs() + 5000;
            j.end = j.start + std::stoi(args[3]) * 1000LL;
            j.dataPath = utf8(data.wstring());
            j.dbPath = utf8((data / "recorder.db").wstring());
            if (argc > 4)
                j.channel.profile.codec = utf8(args[4]);
            if (argc > 5)
                j.channel.profile.encoder = utf8(args[5]);
            if (argc > 6) {
                j.channel.profile.lapseValue = std::stod(args[6]);
                j.channel.profile.lapseMode = j.channel.profile.lapseValue > 1 ? 1 : 0;
            }
            if (argc > 7)
                j.channel.profile.segmentMinutes = std::stoi(args[7]);
            fs::create_directories(wide(j.settings.storage));
            auto name = wide("Local\\NDIRecorder-" + j.token);
            HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared),
                                            name.c_str());
            auto sh = (Shared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
            if (!sh)
                throw std::runtime_error("test shared mapping failed");
            memset(sh, 0, sizeof(Shared));
    sh->version = 4; sh->structBytes = sizeof(Shared);
            sh->endWall = j.end;
            sh->recordGainMilliDB = (LONG)std::lround(j.channel.profile.recordGainDB * 1000);
            auto file = data / "jobs" / wide(j.token + ".json");
            atomicJson(file, j);
            std::jthread changeGain;
            if (argc > 8) {
                double gain = std::clamp(std::stod(args[8]), -24., 24.);
                i64 at = j.start + (j.end-j.start)/2;
                changeGain = std::jthread([sh, at, gain](std::stop_token stop) {
                    while (!stop.stop_requested() && nowMs() < at) Sleep(20);
                    if (!stop.stop_requested()) InterlockedExchange(&sh->recordGainMilliDB, (LONG)std::lround(gain*1000));
                });
            }
            int r = runWorker(file);
            changeGain.request_stop();
            if (changeGain.joinable()) changeGain.join();
            atomicJson(data / "last-test.json", {{"exit", r},
                                                 {"message", sh->message},
                                                 {"frames", sh->frames},
                                                 {"stats", sh->stats},
                                                 {"fps", sh->fps},
                                                 {"file", sh->file},
                                                 {"encoder", sh->encoder}});
            UnmapViewOfFile(sh);
            CloseHandle(map);
            return r;
        }
        HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\NDIRecorder.Desktop.v1");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            auto h = FindWindowW(L"NDIRecorderWindow", nullptr);
            if (h) {
                ShowWindow(h, SW_RESTORE);
                PostMessageW(h, WM_APP + 2, 0, 0);
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || std::find(args.begin(), args.end(), L"--reset-ui-scale") != args.end()) PostMessageW(h, WM_APP + 3, 0, 0);
                SetForegroundWindow(h);
            }
            if (mutex)
                CloseHandle(mutex);
            return 0;
        }
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        fs::path data = exeDir() / "data", capture;
        int exitSeconds = 0;
        std::string capturePanel;
        bool tray = false, resetScale = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        for (int i = 1; i < argc; i++) {
            if (args[i] == L"--data" && i + 1 < argc)
                data = args[++i];
            else if (args[i] == L"--capture" && i + 1 < argc)
                capture = args[++i];
            else if (args[i] == L"--capture-panel" && i + 1 < argc)
                capturePanel = utf8(args[++i]);
            else if (args[i] == L"--exit-after" && i + 1 < argc)
                exitSeconds = std::stoi(args[++i]);
            else if (args[i] == L"--reset-ui-scale")
                resetScale = true;
            else if (args[i] == L"--tray")
                tray = true;
        }
        Manager manager(data);
        if (resetScale) { manager.settings.uiScale = 1.; manager.save(); }
        int result = runUi(manager, tray, capture, exitSeconds, capturePanel);
        SetThreadExecutionState(ES_CONTINUOUS);
        CloseHandle(mutex);
        return result;
    } catch (const std::exception &e) {
        bool command = argc > 1 && args[1] != L"--tray" && args[1] != L"--data";
        if (command) {
            std::ofstream f(exeDir() / "command-error.log", std::ios::app);
            f << dateText(nowMs(), true) << " " << e.what() << "\n";
        } else
            MessageBoxW(nullptr, wide(e.what()).c_str(), L"NDI Recorder", MB_OK | MB_ICONERROR);
        return 1;
    }
}
