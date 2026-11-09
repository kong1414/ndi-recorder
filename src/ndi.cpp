#include "ndi.h"
#include <stdexcept>
#include <mmsystem.h>
namespace nr {

namespace {
struct AudioMonitor {
    HWAVEOUT out = nullptr;
    struct Block {
        WAVEHDR hdr{};
        std::vector<short> data;
    };
    std::deque<std::unique_ptr<Block>> blocks;
    int rate = 0;
    double previousGain = -1;
    ~AudioMonitor() { reset(); }
    void reset() {
        previousGain = -1;
        if (out) {
            waveOutReset(out);
            for (auto &b : blocks)
                waveOutUnprepareHeader(out, &b->hdr, sizeof(WAVEHDR));
            blocks.clear();
            waveOutClose(out);
            out = nullptr;
        }
    }
    void feed(const NDIlib_audio_frame_v3_t &a, bool enabled, int pair, double gainDB, double volume) {
        if (!enabled) {
            reset();
            return;
        }
        if (a.FourCC != NDIlib_FourCC_audio_type_FLTP || !a.p_data || a.no_samples <= 0 ||
            pair < 0 || (a.no_channels == 1 ? pair != 0 : pair * 2 + 1 >= a.no_channels)) {
            reset();
            return;
        }
        if (out && rate != a.sample_rate)
            reset();
        if (!out) {
            WAVEFORMATEX f{};
            f.wFormatTag = WAVE_FORMAT_PCM;
            f.nChannels = 2;
            f.nSamplesPerSec = a.sample_rate;
            f.wBitsPerSample = 16;
            f.nBlockAlign = 4;
            f.nAvgBytesPerSec = f.nSamplesPerSec * 4;
            if (waveOutOpen(&out, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                out = nullptr;
                return;
            }
            rate = a.sample_rate;
        }
        while (!blocks.empty() && (blocks.front()->hdr.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(out, &blocks.front()->hdr, sizeof(WAVEHDR));
            blocks.pop_front();
        }
        if (blocks.size() >= 4)
            return;
        auto b = std::make_unique<Block>();
        b->data.resize(a.no_samples * 2);
        auto l = (float *)(a.p_data + (i64)pair * 2 * a.channel_stride_in_bytes);
        auto r = (float *)(a.p_data + (i64)(a.no_channels == 1 ? 0 : pair * 2 + 1) * a.channel_stride_in_bytes);
        double targetGain = std::pow(10., gainDB / 20.) * volume;
        if (previousGain < 0) previousGain = targetGain;
        for (int k = 0; k < a.no_samples; k++) {
            float gain = (float)(previousGain + (targetGain-previousGain) * (k+1) / a.no_samples);
            b->data[k * 2] = (short)(std::clamp(l[k] * gain, -1.f, 1.f) * 32767);
            b->data[k * 2 + 1] = (short)(std::clamp(r[k] * gain, -1.f, 1.f) * 32767);
        }
        previousGain = targetGain;
        b->hdr.lpData = (LPSTR)b->data.data();
        b->hdr.dwBufferLength = (DWORD)b->data.size() * 2;
        if (waveOutPrepareHeader(out, &b->hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            if (waveOutWrite(out, &b->hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                blocks.push_back(std::move(b));
            else
                waveOutUnprepareHeader(out, &b->hdr, sizeof(WAVEHDR));
        }
    }
};
} // namespace

Ndi::Ndi() {
    dll = LoadLibraryExW((exeDir() / L"Processing.NDI.Lib.x64.dll").c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll)
        throw std::runtime_error("NDI runtime missing");
    auto load = (const NDIlib_v6 *(*)(void))GetProcAddress(dll, "NDIlib_v6_load");
    if (!load || !(api = load()) || !api->initialize())
        throw std::runtime_error("NDI initialize failed");
}
Ndi::~Ndi() {
    if (api)
        api->destroy();
    if (dll)
        FreeLibrary(dll);
}
Finder::Finder(Ndi &n, const std::string &i) : ndi_(n), ips_(i) {
    thread_ = std::jthread([this](std::stop_token st) {
        NDIlib_find_create_t d;
        d.show_local_sources = true;
        d.p_extra_ips = ips_.empty() ? nullptr : ips_.c_str();
        auto f = ndi_.api->find_create_v2(&d);
        if (!f) { refreshing_ = false; return; }
        while (!st.stop_requested()) {
            if (refreshRequested_.exchange(false)) {
                ndi_.api->find_destroy(f);
                f = ndi_.api->find_create_v2(&d);
                if (!f) { refreshing_ = false; return; }
            }
            ndi_.api->find_wait_for_sources(f, 500);
            uint32_t count = 0;
            auto src = ndi_.api->find_get_current_sources(f, &count);
            std::vector<std::string> v;
            for (uint32_t x = 0; x < count; x++)
                if (src[x].p_ndi_name)
                    v.push_back(src[x].p_ndi_name);
            std::lock_guard l(mutex_);
            names_ = std::move(v);
            if (!refreshRequested_) refreshing_ = false;
        }
        ndi_.api->find_destroy(f);
    });
}
Finder::~Finder() {
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}
std::vector<std::string> Finder::names() {
    std::lock_guard l(mutex_);
    return names_;
}
Preview::Preview(Ndi &n, std::string s) : ndi_(n), source_(std::move(s)) {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}
Preview::~Preview() {
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}
bool Preview::snapshot(PreviewFrame &f, unsigned &g) {
    std::lock_guard l(mutex_);
    if (g == frame_.generation)
        return false;
    f = frame_;
    g = frame_.generation;
    return true;
}
void Preview::run(std::stop_token st) {
    AudioMonitor monitor;
    NDIlib_recv_create_v3_t d;
    d.source_to_connect_to.p_ndi_name = source_.c_str();
    d.bandwidth = NDIlib_recv_bandwidth_lowest;
    d.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    d.allow_video_fields = false;
    auto r = ndi_.api->recv_create_v3(&d);
    if (!r)
        return;
    ULONGLONG last = 0, lastVideo = 0;
    while (!st.stop_requested()) {
        if (!monitor_)
            monitor.reset();
        NDIlib_video_frame_v2_t v;
        NDIlib_audio_frame_v3_t a;
        auto type = ndi_.api->recv_capture_v3(r, &v, &a, nullptr, 100);
        if (type == NDIlib_frame_type_video) {
            lastVideo = GetTickCount64();
            if (visible_ && v.xres > 0 && v.yres > 0 &&
                GetTickCount64() - last >= 1000 / std::max(1, fps_.load())) {
                last = GetTickCount64();
                std::lock_guard l(mutex_);
                double scale = std::min(320. / v.xres, 640. / v.yres);
                frame_.w = std::max(1, (int)std::round(v.xres * scale));
                frame_.h = std::max(1, (int)std::round(v.yres * scale));
                frame_.pixels.resize(frame_.w * frame_.h * 4);
                for (int y = 0; y < frame_.h; y++)
                    for (int x = 0; x < frame_.w; x++) {
                        auto p = v.p_data + (i64)(y * v.yres / frame_.h) * v.line_stride_in_bytes +
                                 (x * v.xres / frame_.w) * 4;
                        auto o = &frame_.pixels[(y * frame_.w + x) * 4];
                        o[0] = p[2];
                        o[1] = p[1];
                        o[2] = p[0];
                        o[3] = 255;
                    }
                frame_.fps = v.frame_rate_D ? (double)v.frame_rate_N / v.frame_rate_D : 0;
                frame_.online = true;
                frame_.spec = std::to_string(v.xres) + "x" + std::to_string(v.yres) + " / " +
                              std::to_string((int)std::round(frame_.fps)) + " fps (低带宽预览)";
                frame_.generation++;
            }
            ndi_.api->recv_free_video_v2(r, &v);
        } else if (type == NDIlib_frame_type_audio) {
            int pair;
            double gainDB;
            {
                std::lock_guard lock(mutex_);
                pair = audioPair_;
                gainDB = audioGainDB_;
            }
            monitor.feed(a, monitor_, pair, gainDB, volume_.load(std::memory_order_relaxed));
            double l = 0, rr = 0;
            if (a.FourCC == NDIlib_FourCC_audio_type_FLTP && a.p_data && pair >= 0 &&
                (a.no_channels == 1 ? pair == 0 : pair * 2 + 1 < a.no_channels)) {
                auto p = (float *)(a.p_data + (i64)pair * 2 * a.channel_stride_in_bytes);
                auto q = (float *)(a.p_data + (i64)(a.no_channels == 1 ? 0 : pair * 2 + 1) * a.channel_stride_in_bytes);
                for (int k = 0; k < a.no_samples; k++) {
                    l = std::max(l, (double)std::abs(p[k]));
                    rr = std::max(rr, (double)std::abs(q[k]));
                }
            }
            {
                std::lock_guard lock(mutex_);
                double meterGain = std::pow(10., gainDB / 20.);
                frame_.peakL = l * meterGain;
                frame_.peakR = rr * meterGain;
            }
            ndi_.api->recv_free_audio_v3(r, &a);
        }
        if (GetTickCount64() - lastVideo > 1000) {
            std::lock_guard l(mutex_);
            if (frame_.online) {
                frame_.online = false;
                frame_.generation++;
            }
        }
    }
    ndi_.api->recv_destroy(r);
}

int inspectSource(Ndi &ndi, const std::string &name, const fs::path &file) {
    NDIlib_recv_create_v3_t d;
    d.source_to_connect_to.p_ndi_name = name.c_str();
    d.color_format = NDIlib_recv_color_format_best;
    d.allow_video_fields = true;
    auto r = ndi.api->recv_create_v3(&d);
    json out = json::array();
    auto deadline = GetTickCount64() + 10000;
    while (out.size() < 4 && GetTickCount64() < deadline) {
        NDIlib_video_frame_v2_t v;
        if (ndi.api->recv_capture_v3(r, &v, nullptr, nullptr, 500) == NDIlib_frame_type_video) {
            out.push_back({{"x", v.xres},
                           {"y", v.yres},
                           {"stride", v.line_stride_in_bytes},
                           {"fourcc", (int)v.FourCC},
                           {"type", (int)v.frame_format_type},
                           {"rateN", v.frame_rate_N},
                           {"rateD", v.frame_rate_D},
                           {"timecode", v.timecode}});
            ndi.api->recv_free_video_v2(r, &v);
        }
    }
    ndi.api->recv_destroy(r);
    atomicJson(file, out);
    return out.empty() ? 1 : 0;
}

int testSource(Ndi &ndi, const std::string &name, int mode, int seconds, int outageAfter, int outageSeconds, int clockJumpMs) {
    NDIlib_send_create_t d;
    d.p_ndi_name = name.c_str();
    d.clock_video = false;
    d.clock_audio = false;
    auto s = ndi.api->send_create(&d);
    if (!s)
        return 1;
    int fps = mode == 50 ? 25 : 60;
    std::vector<unsigned char> pix(1920 * 1080 * 2);
    std::vector<float> audio(48000 / fps * 2);
    auto start = std::chrono::steady_clock::now();
    i64 base = nowMs() * 10000;
    auto pulse = [](double t) {
        unsigned second = (unsigned)std::floor(t);
        unsigned hash = second + 0x9e3779b9u;
        hash = (hash ^ (hash >> 16)) * 0x85ebca6bu;
        hash = (hash ^ (hash >> 13)) * 0xc2b2ae35u;
        hash ^= hash >> 16;
        double onset = 0.15 + (hash % 7) * 0.05;
        double phase = t - second - onset;
        return phase >= 0 && phase < 0.04;
    };
    int n = 0;
    while (n < seconds * fps) {
        double time = (double)n / fps;
        bool offline = outageAfter > 0 && time >= outageAfter && time < outageAfter + outageSeconds;
        if (!offline) {
            for (int y = 0; y < 1080; y++)
                for (int x = 0; x < 1920; x += 2) {
                    int field = (mode == 50 ? (y % 2) : 0);
                    double ft = time + field / 50.;
                    bool flash = pulse(ft);
                    int bar = (int)(ft * 350) % 1800;
                    unsigned char yy = flash ? 235 : ((x >= bar && x < bar + 100) ? 180 : 32);
                    auto p = &pix[(y * 1920 + x) * 2];
                    p[0] = 128;
                    p[1] = yy;
                    p[2] = 128;
                    p[3] = yy;
                }
            NDIlib_video_frame_v2_t v;
            v.xres = 1920;
            v.yres = 1080;
            v.FourCC = NDIlib_FourCC_video_type_UYVY;
            v.frame_rate_N = fps;
            v.frame_rate_D = 1;
            v.picture_aspect_ratio = 16.f / 9;
            v.frame_format_type =
                mode == 50 ? NDIlib_frame_format_type_interleaved : NDIlib_frame_format_type_progressive;
            v.p_data = pix.data();
            v.line_stride_in_bytes = 3840;
            v.timecode = base + (i64)std::llround(time * 1e7) + (time>=10?clockJumpMs*10000LL:0);
            ndi.api->send_send_video_v2(s, &v);
            int samples = 48000 / fps;
            for (int k = 0; k < samples; k++) {
                double at = time + k / 48000.;
                float a =
                    pulse(at) ? 0.4f * (float)std::sin(at * 2 * 3.141592653589793 * 1000) : 0;
                audio[k] = a;
                audio[k + samples] = a;
            }
            NDIlib_audio_frame_v2_t a;
            a.sample_rate = 48000;
            a.no_channels = 2;
            a.no_samples = samples;
            a.timecode = v.timecode;
            a.p_data = audio.data();
            a.channel_stride_in_bytes = samples * 4;
            ndi.api->send_send_audio_v2(s, &a);
        }
        n++;
        std::this_thread::sleep_until(start + std::chrono::nanoseconds((i64)(n * 1e9 / fps)));
    }
    ndi.api->send_destroy(s);
    return 0;
}
} // namespace nr
