#pragma once
#include "core.h"
#include <Processing.NDI.Lib.h>
#include <Processing.NDI.DynamicLoad.h>
#include <d3d11.h>
namespace nr {
struct Ndi {
    HMODULE dll = nullptr;
    const NDIlib_v6 *api = nullptr;
    Ndi();
    ~Ndi();
};
class Finder {
    Ndi &ndi_;
    std::jthread thread_;
    std::mutex mutex_;
    std::vector<std::string> names_;
    std::string ips_;
    std::atomic<bool> refreshRequested_{false}, refreshing_{true};

  public:
    Finder(Ndi &, const std::string &);
    ~Finder();
    std::vector<std::string> names();
    void refresh() { refreshing_ = true; refreshRequested_ = true; }
    bool refreshing() const { return refreshing_; }
};
struct PreviewFrame {
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    double fps = 0, peakL = 0, peakR = 0;
    bool online = false;
    unsigned generation = 0;
    std::string spec;
};
class Preview {
    Ndi &ndi_;
    std::string source_;
    std::jthread thread_;
    std::mutex mutex_;
    PreviewFrame frame_;
    std::atomic<bool> visible_{true};
    std::atomic<bool> monitor_{false};
    std::atomic<int> fps_{15};
    std::atomic<double> volume_{1.};
    int audioPair_ = 0;
    double audioGainDB_ = -20;
    void run(std::stop_token);

  public:
    Preview(Ndi &, std::string);
    ~Preview();
    void monitor(bool v) { monitor_ = v; }
    void volume(double v) { volume_ = std::clamp(v, 0., 4.); }
    void audioProfile(const Profile &p) {
        std::lock_guard l(mutex_);
        audioPair_ = p.audioPair;
        audioGainDB_ = p.codec == "ffv1" ? 0 : p.audioGainDB + (p.lapseMode ? 0 : p.recordGainDB);
    }
    void visible(bool v, int fps) {
        visible_ = v;
        fps_ = fps;
    }
    bool snapshot(PreviewFrame &, unsigned &generation);
};
int inspectSource(Ndi &, const std::string &, const fs::path &);
int testSource(Ndi &, const std::string &name, int mode, int seconds, int outageAfter = 0,
               int outageSeconds = 0, int clockJumpMs = 0);
} // namespace nr
