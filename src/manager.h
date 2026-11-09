#pragma once
#include "ndi.h"
namespace nr {
struct Runtime {
    std::unique_ptr<Preview> preview;
    PreviewFrame image;
    unsigned generation = 0;
    ID3D11ShaderResourceView *texture = nullptr;
    HANDLE process = nullptr, map = nullptr, killJob = nullptr;
    Shared *shared = nullptr;
    Shared snapshot{};
    bool manual = false, blocked = false, stopping = false, monitor = false;
    int retries = 0;
    i64 retryAt = 0, startedAt = 0, statsSavedAt = 0;
    std::string savedStats;
    ULONGLONG healthySince = 0;
    Profile activeProfile;
    std::string error, token, previewSource, activeSource;
    ~Runtime();
};
class Manager {
    std::jthread recovery_;
    fs::path data_;
    i64 metricsAt_ = 0, checkpointAt_ = 0;
    void start(Channel &, Runtime &, i64 begin, i64 end);
    void collect(Channel &, Runtime &);

  public:
    Settings settings;
    std::vector<Channel> channels;
    std::vector<Plan> plans;
    Store store;
    Logger log;
    Ndi ndi;
    std::unique_ptr<Finder> finder;
    Metrics metrics;
    std::map<std::string, std::unique_ptr<Runtime>> runtimes;
    explicit Manager(fs::path);
    ~Manager();
    Runtime &runtime(const Channel &);
    void tick(bool visible);
    void save();
    void beginManual(Channel &);
    void stopManual(Channel &);
    void resume(Channel &);
    void stopAll();
    bool busy() const;
    void refreshDiscovery();
    static void validateConfig(const json &);
    json previewImport(const json &, const std::set<std::string> &planKeys,
                       const std::set<std::string> &channelKeys, bool includeSettings) const;
    void applyConfig(const json &);
    void importConfig(const fs::path &);
    void exportConfig(const fs::path &);
    fs::path dataPath() const { return data_; }
};
int runUi(Manager &, bool tray, const fs::path &screenshot = {}, int autoExit = 0, const std::string &capturePanel = {});
} // namespace nr
