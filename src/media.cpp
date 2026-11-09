#include "media.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/md5.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <stdexcept>
#include <sstream>
namespace nr {
static std::string avError(int n) {
    char b[256];
    av_strerror(n, b, sizeof b);
    return b;
}
static void check(int n, const char *what) {
    if (n < 0)
        throw std::runtime_error(std::string(what) + ": " + avError(n));
}
struct FrameDelete {
    void operator()(AVFrame *p) const { av_frame_free(&p); }
};
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
static Frame frame() {
    return Frame(av_frame_alloc());
}
static Frame videoFrame(int w, int h, AVPixelFormat pix) {
    auto f = frame();
    f->width = w;
    f->height = h;
    f->format = pix;
    check(av_frame_get_buffer(f.get(), 32), "video buffer");
    return f;
}
static Frame blackFrame(int w, int h, AVPixelFormat pix) {
    auto f = videoFrame(w, h, pix);
    ptrdiff_t lines[4];
    for (int i = 0; i < 4; i++)
        lines[i] = f->linesize[i];
    check(av_image_fill_black(f->data, lines, pix, AVCOL_RANGE_MPEG, w, h), "black frame");
    return f;
}
struct Captured {
    Frame f;
    i64 tc = 0, arrival = 0;
    AVRational rate{0, 1};
    bool audio = false;
    size_t bytes = 0;
};
class Capture {
    Ndi &ndi_;
    NDIlib_recv_instance_t recv_ = nullptr;
    std::jthread thread_;
    std::mutex mutex_;
    std::deque<Captured> q_;
    size_t bytes_ = 0;
    std::atomic<unsigned long long> queueVideo_ = 0, queueAudio_ = 0, fieldPairs_ = 0;
    std::string error_;
    SwsContext *sw_ = nullptr;
    Captured top_;
    bool hasTop_ = false;
    void push(Captured c) {
        std::lock_guard l(mutex_);
        while (!q_.empty() && (bytes_ + c.bytes > 128 * 1024 * 1024 || q_.size() >= 128 ||
                               c.arrival - q_.front().arrival > 500000)) {
            bytes_ -= q_.front().bytes;
            (q_.front().audio ? queueAudio_ : queueVideo_).fetch_add(1, std::memory_order_relaxed);
            q_.pop_front();
        }
        bytes_ += c.bytes;
        q_.push_back(std::move(c));
    }
    void run(std::stop_token st) {
        try {
            while (!st.stop_requested()) {
                NDIlib_video_frame_v2_t v;
                NDIlib_audio_frame_v3_t a;
                auto type = ndi_.api->recv_capture_v3(recv_, &v, &a, nullptr, 50);
                if (type == NDIlib_frame_type_video) {
                    try {
                        if (v.xres <= 0 || v.yres <= 0 || v.xres > 7680 || v.yres > 4320)
                            throw std::runtime_error("unsupported NDI dimensions");

                        AVPixelFormat src = AV_PIX_FMT_NONE, dst = AV_PIX_FMT_NONE;
                        const uint8_t *data[4]{v.p_data};
                        int lines[4]{v.line_stride_in_bytes};
                        switch (v.FourCC) {
                        case NDIlib_FourCC_video_type_UYVY:
                            src = AV_PIX_FMT_UYVY422;
                            dst = AV_PIX_FMT_YUV422P;
                            break;
                        case NDIlib_FourCC_video_type_BGRA:
                            src = AV_PIX_FMT_BGRA;
                            dst = AV_PIX_FMT_BGRA;
                            break;
                        case NDIlib_FourCC_video_type_BGRX:
                            src = AV_PIX_FMT_BGRA;
                            dst = AV_PIX_FMT_BGR0;
                            break;
                        case NDIlib_FourCC_video_type_RGBA:
                            src = AV_PIX_FMT_RGBA;
                            dst = AV_PIX_FMT_BGRA;
                            break;
                        case NDIlib_FourCC_video_type_RGBX:
                            src = AV_PIX_FMT_RGBA;
                            dst = AV_PIX_FMT_BGR0;
                            break;
                        case NDIlib_FourCC_video_type_PA16:
                            src = AV_PIX_FMT_NV16;
                            dst = AV_PIX_FMT_YUVA422P16LE;
                            break;
                        case NDIlib_FourCC_video_type_P216:
                            src = AV_PIX_FMT_NV16;
                            dst = AV_PIX_FMT_YUV422P16LE;
                            break;
                        default:
                            throw std::runtime_error("unsupported NDI FourCC: " +
                                                     std::to_string((int)v.FourCC));
                        }
                        Frame f;
                        if (v.FourCC == NDIlib_FourCC_video_type_P216 ||
                            v.FourCC == NDIlib_FourCC_video_type_PA16) {
                            f = videoFrame(v.xres, v.yres, dst);
                            if (dst == AV_PIX_FMT_YUVA422P16LE)
                                for (int y = 0; y < v.yres; y++)
                                    memcpy(f->data[3] + y * f->linesize[3],
                                           v.p_data + (i64)v.yres * v.line_stride_in_bytes * 2 +
                                               y * v.line_stride_in_bytes,
                                           v.xres * 2);
                            for (int y = 0; y < v.yres; y++) {
                                memcpy(f->data[0] + y * f->linesize[0], v.p_data + y * v.line_stride_in_bytes,
                                       v.xres * 2);
                                auto uv = (uint16_t *)(v.p_data + (i64)v.yres * v.line_stride_in_bytes +
                                                       y * v.line_stride_in_bytes);
                                auto u = (uint16_t *)(f->data[1] + y * f->linesize[1]),
                                     vv = (uint16_t *)(f->data[2] + y * f->linesize[2]);
                                for (int x = 0; x < v.xres / 2; x++) {
                                    u[x] = uv[x * 2];
                                    vv[x] = uv[x * 2 + 1];
                                }
                            }
                        } else {
                            f = videoFrame(v.xres, v.yres, dst);
                            sw_ = sws_getCachedContext(sw_, v.xres, v.yres, src, v.xres, v.yres, dst,
                                                       SWS_POINT, nullptr, nullptr, nullptr);
                            if (!sw_)
                                throw std::runtime_error("pixel conversion unavailable");
                            sws_scale(sw_, data, lines, 0, v.yres, f->data, f->linesize);
                        }
                        if (v.frame_format_type == NDIlib_frame_format_type_interleaved)
                            f->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
                        f->color_range = AVCOL_RANGE_MPEG;
                        f->colorspace = AVCOL_SPC_BT709;
                        Captured c;
                        c.tc = v.timecode == NDIlib_send_timecode_synthesize ? v.timestamp : v.timecode;
                        c.arrival = (i64)GetTickCount64() * 1000;
                        c.rate = {v.frame_rate_N, v.frame_rate_D};
                        c.bytes = (size_t)v.xres * v.yres * 8;
                        c.f = std::move(f);
                        if (v.frame_format_type == NDIlib_frame_format_type_field_0) {
                            if (hasTop_) fieldPairs_.fetch_add(1, std::memory_order_relaxed);
                            top_ = std::move(c);
                            hasTop_ = true;
                        } else if (v.frame_format_type == NDIlib_frame_format_type_field_1) {
                            bool match = c.rate.num > 0 && c.rate.den > 0 && hasTop_ &&
                                         top_.f->width == c.f->width && top_.f->height == c.f->height &&
                                         top_.f->format == c.f->format;
                            if (match && top_.tc != NDIlib_recv_timestamp_undefined &&
                                c.tc != NDIlib_recv_timestamp_undefined) {
                                i64 distance = c.tc - top_.tc;
                                i64 expected = 10000000LL * c.rate.den / (2 * c.rate.num);
                                match = std::abs(distance - expected) < expected / 2;
                            }
                            if (match) {
                                auto full =
                                    videoFrame(c.f->width, c.f->height * 2, (AVPixelFormat)c.f->format);
                                auto desc = av_pix_fmt_desc_get((AVPixelFormat)c.f->format);
                                for (int plane = 0; plane < 4; plane++) {
                                    if (!full->data[plane] || !top_.f->data[plane] || !c.f->data[plane])
                                        continue;
                                    int rows = c.f->height;
                                    if (plane == 1 || plane == 2)
                                        rows = AV_CEIL_RSHIFT(rows, desc->log2_chroma_h);
                                    int rowBytes =
                                        av_image_get_linesize((AVPixelFormat)c.f->format, c.f->width, plane);
                                    for (int y = 0; y < rows; y++) {
                                        memcpy(full->data[plane] + (y * 2) * full->linesize[plane],
                                               top_.f->data[plane] + y * top_.f->linesize[plane], rowBytes);
                                        memcpy(full->data[plane] + (y * 2 + 1) * full->linesize[plane],
                                               c.f->data[plane] + y * c.f->linesize[plane], rowBytes);
                                    }
                                }
                                full->flags = AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
                                full->color_range = top_.f->color_range;
                                full->colorspace = top_.f->colorspace;
                                c.f = std::move(full);
                                c.tc = top_.tc;
                                c.arrival = top_.arrival;
                                c.bytes += top_.bytes;
                                push(std::move(c));
                            } else
                                fieldPairs_.fetch_add(1, std::memory_order_relaxed);
                            hasTop_ = false;
                            top_.f.reset();
                        } else
                            push(std::move(c));
                    } catch (...) {
                        ndi_.api->recv_free_video_v2(recv_, &v);
                        throw;
                    }
                    ndi_.api->recv_free_video_v2(recv_, &v);
                } else if (type == NDIlib_frame_type_audio) {
                    try {
                        if (a.FourCC != NDIlib_FourCC_audio_type_FLTP || a.no_samples <= 0 ||
                            a.no_channels < 1 || a.no_channels > 64 || a.no_samples > a.sample_rate * 2)
                            throw std::runtime_error("unsupported NDI audio");
                        auto f = frame();
                        f->format = AV_SAMPLE_FMT_FLTP;
                        f->sample_rate = a.sample_rate;
                        f->nb_samples = a.no_samples;
                        av_channel_layout_default(&f->ch_layout, a.no_channels);
                        check(av_frame_get_buffer(f.get(), 0), "audio buffer");
                        for (int k = 0; k < a.no_channels; k++)
                            memcpy(f->extended_data[k], a.p_data + (i64)k * a.channel_stride_in_bytes,
                                   a.no_samples * 4);
                        Captured c;
                        c.audio = true;
                        c.tc = a.timecode == NDIlib_send_timecode_synthesize ? a.timestamp : a.timecode;
                        c.arrival = (i64)GetTickCount64() * 1000;
                        c.bytes = (size_t)a.no_samples * a.no_channels * 4;
                        c.f = std::move(f);
                        push(std::move(c));
                    } catch (...) {
                        ndi_.api->recv_free_audio_v3(recv_, &a);
                        throw;
                    }
                    ndi_.api->recv_free_audio_v3(recv_, &a);
                }
            }
        } catch (const std::exception &e) {
            std::lock_guard l(mutex_);
            error_ = e.what();
        }
    }

  public:
    Capture(Ndi &n, const Channel &c) : ndi_(n) {
        NDIlib_recv_create_v3_t d;
        d.source_to_connect_to.p_ndi_name = c.source.c_str();
        d.bandwidth = c.profile.lowBandwidth ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
        // UYVY_BGRA delivers full interleaved frames (best/fastest may force separate fields).
        d.color_format =
            c.profile.codec == "ffv1" ? NDIlib_recv_color_format_best : NDIlib_recv_color_format_UYVY_BGRA;
        d.allow_video_fields = true;
        recv_ = ndi_.api->recv_create_v3(&d);
        if (!recv_)
            throw std::runtime_error("NDI receiver failed");
        thread_ = std::jthread([this](std::stop_token s) { run(s); });
    }
    ~Capture() {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
        if (recv_)
            ndi_.api->recv_destroy(recv_);
        sws_freeContext(sw_);
    }
    bool pop(Captured &c) {
        std::lock_guard l(mutex_);
        if (!error_.empty())
            throw std::runtime_error(error_);
        if (q_.empty())
            return false;
        c = std::move(q_.front());
        bytes_ -= c.bytes;
        q_.pop_front();
        return true;
    }
    void sample(RecordingStats &s, const RecordingStats *base = nullptr) const {
        NDIlib_recv_performance_t dropped{};
        ndi_.api->recv_get_performance(recv_, nullptr, &dropped);
        s.sdkVideo = std::max<int64_t>(0, dropped.video_frames);
        s.sdkAudio = std::max<int64_t>(0, dropped.audio_frames);
        s.queueVideo = queueVideo_.load(std::memory_order_relaxed);
        s.queueAudio = queueAudio_.load(std::memory_order_relaxed);
        s.fieldPairs = fieldPairs_.load(std::memory_order_relaxed);
        if (base) {
            auto delta = [](auto n, auto b) { return n >= b ? n-b : 0; };
            s.sdkVideo = delta(s.sdkVideo, base->sdkVideo); s.sdkAudio = delta(s.sdkAudio, base->sdkAudio);
            s.queueVideo = delta(s.queueVideo, base->queueVideo); s.queueAudio = delta(s.queueAudio, base->queueAudio);
            s.fieldPairs = delta(s.fieldPairs, base->fieldPairs);
        }
    }
};
class Filter {
    AVFilterGraph *g_ = nullptr;
    AVFilterContext *in_ = nullptr, *out_ = nullptr;

  public:
    Filter(const AVFrame *f, AVRational rate, const Profile &p, AVPixelFormat pix) {
        g_ = avfilter_graph_alloc();
        g_->nb_threads = 2;
        char args[512];
        snprintf(args, sizeof args,
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1:frame_rate=%d/%d",
                 f->width, f->height, f->format, rate.num, rate.den);
        check(avfilter_graph_create_filter(&in_, avfilter_get_by_name("buffer"), "in", args, nullptr, g_),
              "filter input");
        check(avfilter_graph_create_filter(&out_, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
                                           g_),
              "filter output");
        std::string chain;
        if (p.codec == "h264" && (f->flags & AV_FRAME_FLAG_INTERLACED)) {
            chain = "bwdif=mode=send_field:parity=" +
                    std::string(p.fieldOrder == 0   ? "tff"
                                : p.fieldOrder == 1 ? "bff"
                                                    : "auto") +
                    ":deint=all,";
        }
        int w = p.width ? p.width : f->width, h = p.height ? p.height : f->height;
        if (p.codec == "ffv1")
            chain = "null";
        else
            chain += "scale=" + std::to_string(w) + ":" + std::to_string(h) +
                     ":flags=bicubic,format=pix_fmts=" + av_get_pix_fmt_name(pix);
        auto ins = avfilter_inout_alloc(), outs = avfilter_inout_alloc();
        outs->name = av_strdup("in");
        outs->filter_ctx = in_;
        outs->pad_idx = 0;
        ins->name = av_strdup("out");
        ins->filter_ctx = out_;
        ins->pad_idx = 0;
        int r = avfilter_graph_parse_ptr(g_, chain.c_str(), &ins, &outs, nullptr);
        avfilter_inout_free(&ins);
        avfilter_inout_free(&outs);
        check(r, "filter parse");
        check(avfilter_graph_config(g_, nullptr), "filter config");
    }
    ~Filter() { avfilter_graph_free(&g_); }
    void feed(AVFrame *f) {
        check(av_buffersrc_add_frame_flags(in_, f, AV_BUFFERSRC_FLAG_KEEP_REF), "filter feed");
    }
    Frame take() {
        auto f = frame();
        int r = av_buffersink_get_frame(out_, f.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return {};
        check(r, "filter read");
        f->pts = av_rescale_q(f->pts, av_buffersink_get_time_base(out_), AVRational{1, 1000000});
        return f;
    }
};
static AVCodecContext *openVideo(const std::string &name, int w, int h, AVRational rate, AVPixelFormat pix,
                                 int mbps, bool interlaced) {
    auto codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec)
        throw std::runtime_error(name + " not in FFmpeg build");
    auto c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->pix_fmt = pix;
    c->time_base = av_inv_q(rate);
    c->framerate = rate;
    c->bit_rate = (i64)mbps * 1000000;
    c->gop_size = std::max(1, (int)std::round(av_q2d(rate) * 2));
    c->max_b_frames = 0;
    c->thread_count = 2;
    c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    if (interlaced) {
        c->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
        c->field_order = AV_FIELD_TT;
    }
    if (name == "ffv1") {
        c->level = 3;
        av_opt_set(c->priv_data, "coder", "1", 0);
        av_opt_set(c->priv_data, "slicecrc", "1", 0);
    }
    if (name == "h264_nvenc") {
        av_opt_set(c->priv_data, "preset", "p4", 0);
        av_opt_set(c->priv_data, "rc", "vbr", 0);
    }
    if (name == "h264_mf")
        av_opt_set(c->priv_data, "hw_encoding", "0", 0);
    int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        avcodec_free_context(&c);
        throw std::runtime_error(name + ": " + avError(r));
    }
    return c;
}
static bool trial(const std::string &name, int w, int h, AVRational rate, AVPixelFormat pix, int mbps,
                  std::string &err) {
    AVCodecContext *c = nullptr;
    AVPacket *p = av_packet_alloc();
    bool got = false;
    try {
        c = openVideo(name, w, h, rate, pix, mbps, false);
        for (int k = 0; k < 5; k++) {
            auto f = blackFrame(w, h, pix);
            f->pts = k;
            check(avcodec_send_frame(c, f.get()), "probe send");
            while (avcodec_receive_packet(c, p) >= 0) {
                got = true;
                av_packet_unref(p);
            }
        }
        check(avcodec_send_frame(c, nullptr), "probe flush");
        while (avcodec_receive_packet(c, p) >= 0) {
            got = true;
            av_packet_unref(p);
        }
        if (!got)
            err = "encoder produced no packets";
    } catch (const std::exception &e) {
        err = e.what();
    }
    av_packet_free(&p);
    avcodec_free_context(&c);
    return got;
}
int probeEncoders(const fs::path &out) {
    json result = json::array();
    for (auto name : {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"}) {
        std::string e;
        bool ok = trial(name, 1920, 1080, {60, 1}, AV_PIX_FMT_NV12, 20, e);
        result.push_back({{"encoder", name}, {"usable", ok}, {"error", e}});
    }
    atomicJson(out, result);
    return 0;
}
class Mux {
    Job job_;
    Store &store_;
    Logger &log_;
    AVCodecContext *v_ = nullptr, *a_ = nullptr;
    AVFormatContext *m_ = nullptr;
    AVStream *vs_ = nullptr, *as_ = nullptr;
    AVPacket *p_ = av_packet_alloc();
    i64 row_ = 0, baseUs_ = 0;
    unsigned long long closedBytes_ = 0;
    int segment_ = 0;
    std::string path_, firstPath_;
    AVMD5 *videoHash_ = av_md5_alloc(), *audioHash_ = av_md5_alloc();
    unsigned long long hashFrames_ = 0, hashSamples_ = 0;
    double wallFactor() const {
        const auto &p = job_.channel.profile;
        return p.lapseMode == 1 ? p.lapseValue : p.lapseMode == 2 ? p.lapseValue * p.lapseFps : 1.;
    }
    i64 wallTime(i64 encodedUs) const {
        return job_.start + (i64)std::llround(encodedUs * wallFactor() / 1000.);
    }
    static std::string stamp(i64 ms) {
        auto s=dateText(ms,true);
        s.erase(std::remove(s.begin(),s.end(),'-'),s.end());
        s.erase(std::remove(s.begin(),s.end(),':'),s.end());
        std::replace(s.begin(),s.end(),' ','_');
        return s;
    }
    void open(i64 at) {
        baseUs_ = at;
        auto dir = fs::path(wide(job_.settings.storage)) / wide(safeName(job_.channel.name)) /
                   wide(dateText(wallTime(at)));
        fs::create_directories(dir);
        auto stem =
            stamp(wallTime(at)) + "_recording_" + job_.token + "_" + std::to_string(++segment_);
        auto file = dir / wide(stem + (job_.channel.profile.codec == "ffv1" ? ".mkv" : ".mp4"));
        if (fs::exists(file))
            throw std::runtime_error("output collision");
        path_ = utf8(file.wstring());
        if (firstPath_.empty())
            firstPath_ = path_;
        check(avformat_alloc_output_context2(
                  &m_, nullptr, job_.channel.profile.codec == "ffv1" ? "matroska" : "mp4", path_.c_str()),
              "mux create");
        m_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        vs_ = avformat_new_stream(m_, nullptr);
        vs_->time_base = v_->time_base;
        vs_->avg_frame_rate = v_->framerate;
        check(avcodec_parameters_from_context(vs_->codecpar, v_), "video parameters");
        if (a_) {
            as_ = avformat_new_stream(m_, nullptr);
            as_->time_base = a_->time_base;
            check(avcodec_parameters_from_context(as_->codecpar, a_), "audio parameters");
        }
        check(avio_open(&m_->pb, path_.c_str(), AVIO_FLAG_WRITE), "open recording");
        AVDictionary *opts = nullptr;
        if (job_.channel.profile.codec == "h264") {
            av_dict_set(&opts, "movflags", "frag_keyframe+delay_moov+default_base_moof+negative_cts_offsets",
                        0);
            av_dict_set(&opts, "frag_duration", "2000000", 0);
        }
        int r = avformat_write_header(m_, &opts);
        av_dict_free(&opts);
        check(r, "write header");
        row_ = store_.history({0, wallTime(at), 0, job_.channel.key, job_.token, path_, "recording", ""});
        log_.write("segment start " + path_);
    }
    void close(i64 at) {
        if (!m_)
            return;
        int r = av_write_trailer(m_);
        if (m_->pb) closedBytes_ += std::max<int64_t>(0, m_->pb->bytes_written);
        int io = avio_closep(&m_->pb);
        avformat_free_context(m_);
        m_ = nullptr;
        if(r>=0 && io>=0) {
            auto old=fs::path(wide(path_));
            auto final=old.parent_path()/wide(stamp(wallTime(baseUs_))+"-"+stamp(wallTime(at))+"_"+
                job_.token+"_"+std::to_string(segment_)+utf8(old.extension().wstring()));
            std::error_code ec;
            fs::rename(old,final,ec);
            if(!ec) {
                if(firstPath_==path_) firstPath_=utf8(final.wstring());
                path_=utf8(final.wstring()); store_.renameHistory(row_,path_);
            } else log_.write("segment rename deferred: "+ec.message()+" path="+path_);
        }
        log_.write("segment end "+path_+" wall_end="+dateText(wallTime(at),true));
        store_.finish(row_, wallTime(at), r < 0 || io < 0 ? "failed" : "completed",
                      r < 0    ? avError(r)
                      : io < 0 ? avError(io)
                               : "");
        if (r < 0 || io < 0)
            throw std::runtime_error("finish recording failed");
    }
    void packets(AVCodecContext *c, bool video) {
        while (true) {
            int r = avcodec_receive_packet(c, p_);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            check(r, "encode receive");
            i64 us = av_rescale_q(p_->pts, c->time_base, AVRational{1, 1000000});
            if (video && (p_->flags & AV_PKT_FLAG_KEY) &&
                (us - baseUs_) * wallFactor() >= job_.channel.profile.segmentMinutes * 60LL * 1000000) {
                close(us);
                open(us);
            }
            if (video && p_->duration <= 0)
                p_->duration = 1;
            auto stream = video ? vs_ : as_;
            p_->pts -= av_rescale_q(baseUs_, {1, 1000000}, c->time_base);
            p_->dts -= av_rescale_q(baseUs_, {1, 1000000}, c->time_base);
            av_packet_rescale_ts(p_, c->time_base, stream->time_base);
            p_->stream_index = stream->index;
            int wr = av_interleaved_write_frame(m_, p_);
            av_packet_unref(p_);
            check(wr, "write packet");
        }
    }

  public:
    unsigned long long writtenBytes() const {
        return closedBytes_ + (m_ && m_->pb ? std::max<int64_t>(0, m_->pb->bytes_written) : 0);
    }
    Mux(Job j, Store &s, Logger &l, AVFrame *first, AVRational rate, int nativeAudioRate)
        : job_(j), store_(s), log_(l) {
        av_md5_init(videoHash_);
        av_md5_init(audioHash_);
        auto &p = job_.channel.profile;
        int w = p.width ? p.width : first->width, h = p.height ? p.height : first->height;
        AVPixelFormat pix = p.codec == "ffv1" ? (AVPixelFormat)first->format : AV_PIX_FMT_NV12;
        std::string chosen = p.codec == "ffv1" ? "ffv1" : p.encoder;
        if (chosen == "auto") {
            std::string errors;
            for (auto name : {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"}) {
                std::string e;
                if (trial(name, w, h, rate, pix, p.mbps, e)) {
                    chosen = name;
                    break;
                }
                errors += std::string(name) + ": " + e + "; ";
            }
            if (chosen == "auto")
                throw std::runtime_error("no usable H.264 encoder: " + errors);
        } else if (p.codec == "h264") {
            std::string e;
            if (!trial(chosen, w, h, rate, pix, p.mbps, e))
                throw std::runtime_error(e);
        }
        v_ = openVideo(chosen, w, h, rate, pix, p.mbps,
                       p.codec == "ffv1" && (first->flags & AV_FRAME_FLAG_INTERLACED));
        encoder = chosen;
        if (!p.lapseMode) {
            auto codec = avcodec_find_encoder(p.codec == "ffv1" ? AV_CODEC_ID_PCM_F32LE : AV_CODEC_ID_AAC);
            a_ = avcodec_alloc_context3(codec);
            a_->sample_rate = p.codec == "ffv1" ? nativeAudioRate : 48000;
            a_->sample_fmt = p.codec == "ffv1" ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&a_->ch_layout, 2);
            a_->time_base = {1, a_->sample_rate};
            a_->bit_rate = 192000;
            a_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            check(avcodec_open2(a_, codec, nullptr), "audio encoder");
        }
        open(0);
    }
    std::string encoder;
    ~Mux() {
        if (m_) {
            av_write_trailer(m_);
            avio_closep(&m_->pb);
            avformat_free_context(m_);
        }
        avcodec_free_context(&v_);
        avcodec_free_context(&a_);
        av_packet_free(&p_);
        av_free(videoHash_);
        av_free(audioHash_);
    }
    AVPixelFormat pix() const { return v_->pix_fmt; }
    int width() const { return v_->width; }
    int height() const { return v_->height; }
    int audioRate() const { return a_ ? a_->sample_rate : 48000; }
    int audioSamples() const { return a_ && a_->frame_size ? a_->frame_size : 1024; }
    const std::string &path() const { return path_; }
    void video(AVFrame *f, i64 pts) {
        if (job_.channel.profile.codec == "ffv1") {
            // Hash the same tightly packed plane bytes without copying the entire frame.
            auto desc = av_pix_fmt_desc_get((AVPixelFormat)f->format);
            int planes = av_pix_fmt_count_planes((AVPixelFormat)f->format);
            for (int plane = 0; plane < planes; ++plane) {
                int rows = (plane == 1 || plane == 2) ? AV_CEIL_RSHIFT(f->height, desc->log2_chroma_h) : f->height;
                int bytes = av_image_get_linesize((AVPixelFormat)f->format, f->width, plane);
                check(bytes, "hash linesize");
                for (int y = 0; y < rows; ++y)
                    av_md5_update(videoHash_, f->data[plane] + (i64)y * f->linesize[plane], bytes);
            }
            hashFrames_++;
        }
        f->pts = pts;
        f->pict_type = pts % std::max(1, v_->gop_size) == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        check(avcodec_send_frame(v_, f), "encode video");
        packets(v_, true);
    }
    void audio(const std::vector<float> &l, const std::vector<float> &r, i64 pts) {
        if (!a_)
            return;
        auto f = frame();
        f->format = a_->sample_fmt;
        f->sample_rate = a_->sample_rate;
        f->nb_samples = (int)l.size();
        av_channel_layout_copy(&f->ch_layout, &a_->ch_layout);
        check(av_frame_get_buffer(f.get(), 0), "audio encode buffer");
        f->pts = pts;
        if (a_->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            memcpy(f->data[0], l.data(), l.size() * 4);
            memcpy(f->data[1], r.data(), r.size() * 4);
        } else {
            auto p = (float *)f->data[0];
            for (size_t k = 0; k < l.size(); k++) {
                p[k * 2] = l[k];
                p[k * 2 + 1] = r[k];
            }
        }
        if (job_.channel.profile.codec == "ffv1") {
            av_md5_update(audioHash_, f->data[0], l.size() * 8);
            hashSamples_ += l.size();
        }
        check(avcodec_send_frame(a_, f.get()), "encode audio");
        packets(a_, false);
    }
    void finish(i64 us) {
        check(avcodec_send_frame(v_, nullptr), "flush video");
        packets(v_, true);
        if (a_) {
            check(avcodec_send_frame(a_, nullptr), "flush audio");
            packets(a_, false);
        }
        close(us);
        if (job_.channel.profile.codec == "ffv1") {
            auto hex = [](AVMD5 *ctx) {
                unsigned char digest[16];
                av_md5_final(ctx, digest);
                char out[33];
                for (int n = 0; n < 16; n++)
                    snprintf(out + n * 2, 3, "%02x", digest[n]);
                return std::string(out);
            };
            atomicJson(fs::path(wide(firstPath_ + ".checksums.json")),
                       {{"video_md5", hex(videoHash_)},
                        {"audio_md5", hex(audioHash_)},
                        {"pixel_format", av_get_pix_fmt_name(v_->pix_fmt)},
                        {"sample_rate", audioRate()},
                        {"frames", hashFrames_},
                        {"audio_samples", hashSamples_},
                        {"segments", segment_}});
        }
    }
};
struct AudioBlock {
    i64 start = 0;
    std::vector<float> l, r;
};
struct AudioTiming {
    i64 next = INT64_MIN, lastComp = 0;
};
static AudioBlock convertAudio(AVFrame *f, i64 us, int pair, SwrContext *&swr, int &sampleRate,
                               int outputRate, bool lossless, AudioTiming &timing) {
    if ((f->ch_layout.nb_channels == 1 && pair != 0) ||
        (pair * 2 + 1 >= f->ch_layout.nb_channels && f->ch_layout.nb_channels != 1))
        throw std::runtime_error("selected audio pair absent");
    if (!swr || sampleRate != f->sample_rate) {
        swr_free(&swr);
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLTP, outputRate, &stereo, AV_SAMPLE_FMT_FLTP,
                                  f->sample_rate, 0, nullptr),
              "resampler");
        check(swr_init(swr), "resampler init");
        sampleRate = f->sample_rate;
    }
    AudioBlock b;
    b.start = av_rescale(us, outputRate, 1000000) -
              av_rescale(swr_get_delay(swr, f->sample_rate), outputRate, f->sample_rate);
    if (timing.next != INT64_MIN) {
        i64 error = b.start - timing.next;
        if (std::abs(error) <= 1)
            b.start = timing.next;
        else if (!lossless && std::abs(error) < outputRate / 50) {
            if (us - timing.lastComp >= 1000000) {
                int delta = (int)std::clamp(error, -(i64)outputRate / 1000, (i64)outputRate / 1000);
                check(swr_set_compensation(swr, delta, outputRate), "audio clock compensation");
                timing.lastComp = us;
            }
            b.start = timing.next;
        }
    }
    int cap = swr_get_out_samples(swr, f->nb_samples);
    b.l.resize(cap);
    b.r.resize(cap);
    const uint8_t *in[2] = {f->extended_data[pair * 2],
                            f->extended_data[f->ch_layout.nb_channels == 1 ? 0 : pair * 2 + 1]};
    uint8_t *out[2] = {(uint8_t *)b.l.data(), (uint8_t *)b.r.data()};
    int n = swr_convert(swr, out, cap, in, f->nb_samples);
    check(n, "audio resample");
    b.l.resize(n);
    b.r.resize(n);
    timing.next = b.start + n;
    return b;
}
void inspectInterrupted(const fs::path &db, Logger &log, std::stop_token stop) {
    try {
        Store store(db);
        for (int offset = 0; offset < 10000 && !stop.stop_requested(); offset += 100) {
            auto rows = store.historyPage(offset, 100);
            if (rows.empty())
                break;
            for (auto &row : rows) {
                if (stop.stop_requested())
                    return;
                if (row.state != "interrupted" || row.path.empty())
                    continue;
                AVFormatContext *f = avformat_alloc_context();
                ULONGLONG deadline = GetTickCount64() + 3000;
                f->interrupt_callback = {[](void *p) -> int { return GetTickCount64() > *(ULONGLONG *)p; },
                                         &deadline};
                f->probesize = 1024 * 1024;
                f->max_analyze_duration = 2000000;
                int rc = avformat_open_input(&f, row.path.c_str(), nullptr, nullptr);
                bool playable = false;
                if (rc >= 0) {
                    rc = avformat_find_stream_info(f, nullptr);
                    auto packet = av_packet_alloc();
                    playable = rc >= 0 && av_read_frame(f, packet) >= 0;
                    av_packet_free(&packet);
                }
                i64 end = row.end;
                if (playable && f->duration > 0)
                    end = row.start + f->duration / 1000;
                store.finish(row.row, end, playable ? "recovered" : "unreadable",
                             playable ? "异常文件可读取，尾部完整性未保证，原件已保留"
                                      : "异常文件无法读取，原件已保留");
                log.write("recovery inspection " + row.path + (playable ? " readable" : " unreadable"));
                avformat_close_input(&f);
            }
        }
    } catch (const std::exception &e) {
        log.write(std::string("recovery inspection failed: ") + e.what());
    }
}
int runWorker(const fs::path &jobFile) {
    Job job = readJson(jobFile).get<Job>();
    auto name = wide("Local\\NDIRecorder-" + job.token);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!mapping)
        return 2;
    auto shared = (Shared *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!shared) {
        CloseHandle(mapping);
        return 2;
    }
    if (shared->version != 4 || shared->structBytes != sizeof(Shared)) {
        UnmapViewOfFile(shared); CloseHandle(mapping); return 2;
    }
    RecordingStats stats;
    WriteRate writeRate;
    RecentStats recentStats;
    bool monitor = GetEnvironmentVariableW(L"NDI_RECORDER_DISABLE_STATS", nullptr, 0) == 0;
    auto publish = [&](int state, const std::string &msg) {
        InterlockedIncrement(&shared->seq);
        shared->state = state;
        shared->pid = GetCurrentProcessId();
        shared->heartbeat = GetTickCount64();
        strncpy_s(shared->message, msg.c_str(), _TRUNCATE);
        InterlockedIncrement(&shared->seq);
    };
    int result = 0;
    std::unique_ptr<Logger> log;
    std::unique_ptr<Store> store;
    std::unique_ptr<Mux> mux;
    i64 elapsed = 0;
    i64 lastMonitor = 0;
    SwrContext *swr = nullptr;
    try {
        log = std::make_unique<Logger>(fs::path(wide(job.dataPath)) / "logs", "worker-" + job.channel.key,
                                       job.settings.logDays, job.settings.logMB);
        store = std::make_unique<Store>(fs::path(wide(job.dbPath)));
        auto err = validate(job.channel.profile);
        if (!err.empty())
            throw std::runtime_error(err);
        Ndi ndi;
        Capture cap(ndi, job.channel);
        publish(1, "正在连接与准备编码器");
        Captured first, firstAudio;
        const i64 requestedStart = job.start;
        while (true) {
            i64 end = InterlockedCompareExchange64(&shared->endWall, 0, 0);
            if (InterlockedCompareExchange(&shared->stop, 0, 0) || (end > 0 && nowMs() >= end)) {
                i64 until = std::min(nowMs(), end > 0 ? end : nowMs());
                if (until > requestedStart)
                    store->history({0, requestedStart, until, job.channel.key, job.token, "", "no_video",
                                    "等待首帧期间未录制"});
                publish(3, "已停止：未收到 NDI 首帧");
                UnmapViewOfFile(shared);
                CloseHandle(mapping);
                return 0;
            }
            Captured candidate;
            if (cap.pop(candidate)) {
                if (candidate.audio)
                    firstAudio = std::move(candidate);
                else {
                    first = std::move(candidate);
                    break;
                }
            }
            publish(1, "等待 NDI 首帧");
            Sleep(5);
        }
        i64 firstWall = nowMs() - ((i64)GetTickCount64() * 1000 - first.arrival) / 1000;
        if (firstWall > job.start) {
            store->history({0, job.start, firstWall, job.channel.key, job.token, "", "no_video",
                            "首帧晚到，此前未录制；从真实输入规格开始"});
            job.start = firstWall;
            log->write("late first frame: recording starts at actual source availability");
        }
        auto &p = job.channel.profile;
        AVRational inputRate = first.rate;
        if (inputRate.num <= 0 || inputRate.den <= 0)
            inputRate = {25, 1};
        AVRational rate = inputRate;
        if (p.codec == "h264" && (first.f->flags & AV_FRAME_FLAG_INTERLACED))
            rate.num *= 2;
        if (p.fps)
            rate = {p.fps, 1};
        if (p.lapseMode)
            rate = {p.lapseFps, 1};
        if (!firstAudio.f) {
            auto limit = GetTickCount64() + 100;
            Captured c;
            while (GetTickCount64() < limit) {
                if (cap.pop(c) && c.audio) {
                    firstAudio = std::move(c);
                    break;
                }
                Sleep(1);
            }
        }
        int nativeAudioRate = firstAudio.f ? firstAudio.f->sample_rate : 48000;
        mux = std::make_unique<Mux>(job, *store, *log, first.f.get(), rate, nativeAudioRate);
        writeRate.reset(std::max<i64>(GetTickCount64(), (i64)GetTickCount64() + job.start - nowMs()));
        lastMonitor = GetTickCount64();
        Filter filter(first.f.get(), inputRate, p, mux->pix());
        auto black = blackFrame(mux->width(), mux->height(), mux->pix());
        std::deque<Frame> videos;
        std::deque<AudioBlock> audios;
        Frame current;
        AudioTiming audioTiming;
        int sr = 0;
        int outAudioRate = mux->audioRate();
        double period = 1e6 / av_q2d(rate);
        double lapse = p.lapseMode == 1 ? p.lapseValue : p.lapseMode == 2 ? p.lapseValue * p.lapseFps : 1;
        i64 startMono = (i64)GetTickCount64() * 1000 + (job.start - nowMs()) * 1000;
        SourceClock sourceClock;
        bool clockSet = false, fallbackLogged = false;
        i64 lastVideo = 0, lastAudio = 0, frameNo = 0, audioNo = 0;
        int formatW = first.f->width, formatH = first.f->height, formatPix = first.f->format;
        int formatInter = first.f->flags & AV_FRAME_FLAG_INTERLACED;
        i64 processingStart = (i64)GetTickCount64() * 1000;
        i64 outageRow = 0, audioOutageRow = 0, lastStats = 0;
        bool started = false, currentUsed = false;
        i64 lastDiagnostic=0, unexpectedBlackSince=0;
        RecordingStats captureBaseline;
        bool baselineSet = false;
        double previousGain = std::pow(10., (p.audioGainDB + p.recordGainDB) / 20.);
        auto ingest = [&](Captured &c) {
            double mapped = (double)(c.arrival - startMono);
            bool valid = c.tc != NDIlib_recv_timestamp_undefined && c.tc != NDIlib_send_timecode_synthesize &&
                         c.tc != 0;
            if (valid) {
                auto before=sourceClock.corrections;
                mapped=sourceClock.map(c.tc/10, c.arrival-startMono, !c.audio);
                clockSet=true;
                if(sourceClock.corrections!=before) {
                    videos.clear(); audios.clear(); current.reset();
                    audioTiming.next=INT64_MIN;
                    log->write("source clock rebase error_us="+std::to_string(sourceClock.errorUs)+
                        " corrections="+std::to_string(sourceClock.corrections));
                }
            } else if (!clockSet && !fallbackLogged) {
                fallbackLogged = true;
                log->write("source timestamp unavailable; arrival clock fallback");
            }
            c.f->pts = (i64)std::llround(mapped);
            if (c.audio) {
                lastAudio = c.arrival;
                if (!p.lapseMode) {
                    if (p.codec == "ffv1" && c.f->sample_rate != outAudioRate)
                        throw std::runtime_error("lossless audio sample rate changed; restart required");
                    auto b = convertAudio(c.f.get(), c.f->pts, p.audioPair, swr, sr, outAudioRate,
                                          p.codec == "ffv1", audioTiming);
                    if (p.codec == "h264") {
                        double boost = std::clamp(InterlockedCompareExchange(&shared->recordGainMilliDB, 0, 0) / 1000., -24., 24.);
                        double targetGain = std::pow(10., (p.audioGainDB + boost) / 20.);
                        // Smooth changes over this audio block; no extra buffer or processing pass.
                        for (size_t k = 0; k < b.l.size(); ++k) {
                            double gain = previousGain + (targetGain-previousGain) * (k+1) / b.l.size();
                            b.l[k] = (float)std::clamp(b.l[k] * gain, -1., 1.);
                            b.r[k] = (float)std::clamp(b.r[k] * gain, -1., 1.);
                        }
                        previousGain = targetGain;
                    }
                    if (b.start + (i64)b.l.size() > 0)
                        audios.push_back(std::move(b));
                    while (audios.size() > 128) {
                        if (monitor && audios.front().start >= 0) ++stats.filterAudio;
                        audios.pop_front();
                    }
                }
            } else {
                lastVideo = c.arrival;
                if (c.f->width != formatW || c.f->height != formatH || c.f->format != formatPix ||
                    av_cmp_q(c.rate, inputRate) != 0 ||
                    (c.f->flags & AV_FRAME_FLAG_INTERLACED) != formatInter)
                    throw std::runtime_error("NDI format changed; restart channel for new segment");
                filter.feed(c.f.get());
                while (auto f = filter.take()) {
                    if (f->pts >= -1000000)
                        videos.push_back(std::move(f));
                    while (videos.size() >
                           std::min<size_t>(
                               64, std::max<size_t>(2, 128 * 1024 * 1024 /
                                                           av_image_get_buffer_size(mux->pix(), mux->width(),
                                                                                    mux->height(), 1)))) {
                        if (monitor && videos.front()->pts >= 0) {
                            double sourceFps = av_q2d(inputRate) *
                                ((p.codec == "h264" && formatInter) ? 2 : 1);
                            bool reducing = p.lapseMode || av_q2d(rate) < sourceFps;
                            double nextTick = frameNo * period * lapse + period / 2;
                            // During sparse sampling, frames older than another usable candidate
                            // are intentionally superseded, even if the bounded cache evicts them.
                            if (reducing && videos.size() > 1 && videos[1]->pts <= nextTick)
                                ++stats.converted;
                            else ++stats.filterVideo;
                        }
                        videos.pop_front();
                    }
                }
            }
        };
        ingest(first);
        if (firstAudio.f)
            ingest(firstAudio);
        log->write("recording " + mux->encoder + " source=" + job.channel.source);
        while (!InterlockedCompareExchange(&shared->stop, 0, 0)) {
            Captured c;
            int captured = 0;
            while (captured++ < 128 && cap.pop(c))
                ingest(c);
            i64 mono = (i64)GetTickCount64() * 1000;
            if (monitor && !baselineSet && mono >= startMono) { cap.sample(captureBaseline); baselineSet = true; }
            elapsed = mono - startMono - 250000; // bounded jitter lookahead
            i64 end = InterlockedCompareExchange64(&shared->endWall, 0, 0);
            if (end > 0 && elapsed >= (end - job.start) * 1000)
                break;
            if (elapsed >= 0) {
                started = true;
                double tick = frameNo * period * lapse;
                if (tick <= elapsed) {
                    if (mono - processingStart > 15000000 && elapsed - tick > 2000000)
                        throw std::runtime_error("encoding overload >2 seconds, stopping to bound latency");
                    while (!videos.empty() && videos.front()->pts <= tick + period / 2) {
                        if (monitor && current && !currentUsed && current->pts >= 0) ++stats.converted;
                        currentUsed = false;
                        current = std::move(videos.front());
                        videos.pop_front();
                    }
                    bool videoMissing = !current || tick - current->pts > 300000;
                    // Fresh input with prolonged synthetic black indicates a stalled timing/filter path.
                    if(videoMissing && mono-lastVideo<200000 && !p.lapseMode) {
                        if(!unexpectedBlackSince) unexpectedBlackSince=mono;
                        if(mono-unexpectedBlackSince>10000000)
                            throw std::runtime_error("fresh video but synthetic black >10s; restart receiver/encoder");
                    } else unexpectedBlackSince=0;
                    if (videoMissing && !outageRow) {
                        outageRow = store->history({0, job.start + (i64)tick / 1000, 0, job.channel.key,
                                                    job.token, "", "no_video", "黑画面填充"});
                        log->write("video gap: black fill tick_us="+std::to_string(tick)+" picture_pts="+
                            std::to_string(current?current->pts:-1)+" arrival_age_us="+std::to_string(mono-lastVideo));
                    }
                    if (!videoMissing && outageRow) {
                        store->finish(outageRow, job.start + (i64)tick / 1000, "no_video", "已恢复");
                        outageRow = 0;
                        log->write("video restored");
                    }
                    if (monitor) {
                        if (videoMissing) ++stats.black;
                        else if (currentUsed) ++stats.repeated;
                    }
                    if (!videoMissing) currentUsed = true;
                    auto send = Frame(av_frame_clone(videoMissing ? black.get() : current.get()));
                    mux->video(send.get(), frameNo);
                    if (!p.lapseMode) {
                        i64 until = (i64)std::llround((frameNo + 1) * period * outAudioRate / 1e6);
                        while (audioNo + mux->audioSamples() <= until) {
                            int n = mux->audioSamples();
                            std::vector<float> left(n, 0), right(n, 0);
                            int covered = 0;
                            i64 coveredUntil = audioNo;
                            for (auto &b : audios) {
                                i64 a = std::max(audioNo, b.start),
                                    z = std::min(audioNo + n, b.start + (i64)b.l.size());
                                if (z > a) {
                                    std::copy_n(b.l.begin() + (a - b.start), (size_t)(z - a),
                                                left.begin() + (a - audioNo));
                                    std::copy_n(b.r.begin() + (a - b.start), (size_t)(z - a),
                                                right.begin() + (a - audioNo));
                                    covered += (int)std::max(0LL, z - std::max(a, coveredUntil));
                                    coveredUntil = std::max(coveredUntil, z);
                                }
                            }
                            if (monitor) stats.silentSamples += std::max(0, n - covered);
                            bool missing = covered < n / 2;
                            if (missing && !audioOutageRow)
                                audioOutageRow =
                                    store->history({0, job.start + audioNo * 1000 / outAudioRate, 0,
                                                    job.channel.key, job.token, "", "no_audio", "静音填充"});
                            if (!missing && audioOutageRow) {
                                store->finish(audioOutageRow, job.start + audioNo * 1000 / outAudioRate,
                                              "no_audio", "已恢复");
                                audioOutageRow = 0;
                            }
                            mux->audio(left, right, audioNo);
                            audioNo += n;
                            while (!audios.empty() &&
                                   audios.front().start + (i64)audios.front().l.size() <= audioNo)
                                audios.pop_front();
                        }
                    }
                    frameNo++;
                }
            }
            if (monitor && started && mono / 1000 - lastMonitor >= 1000) {
                lastMonitor = mono / 1000;
                cap.sample(stats, &captureBaseline);
                stats.frames = frameNo;
                writeRate.sample(lastMonitor, mux->writtenBytes(), stats);
                InterlockedIncrement(&shared->seq);
                shared->stats = stats;
                shared->recentStats=recentStats.sample(lastMonitor,stats,shared->recentSeconds);
                InterlockedIncrement(&shared->seq);
            }
            if(started && mono-lastDiagnostic>=60000000) {
                lastDiagnostic=mono;
                log->write("health "+json({{"task",job.token},{"file",mux->path()},
                    {"video_age_ms",(mono-lastVideo)/1000},{"audio_age_ms",(mono-lastAudio)/1000},
                    {"source_clock_error_us",sourceClock.errorUs},{"clock_corrections",sourceClock.corrections},
                    {"video_cache",videos.size()},{"audio_cache",audios.size()},{"stats",stats}}).dump());
            }
            if (mono - lastStats > 250000) {
                lastStats = mono;
                ULARGE_INTEGER free, total;
                if (!GetDiskFreeSpaceExW(wide(job.settings.storage).c_str(), &free, &total, nullptr))
                    throw std::runtime_error("storage unavailable");
                if (free.QuadPart < job.settings.reserveGB * 1073741824.)
                    throw std::runtime_error("disk safety reserve reached");
                InterlockedIncrement(&shared->seq);
                shared->heartbeat = GetTickCount64();
                shared->state = started ? 2 : 1;
                shared->frames = frameNo;
                shared->drops = stats.queueVideo; // Legacy field: video queue drops only, never sum audio/fields.
                shared->width = mux->width();
                shared->height = mux->height();
                shared->fps = av_q2d(rate);
                shared->videoAge = (mono - lastVideo) / 1e6;
                shared->audioAge = (mono - lastAudio) / 1e6;
                strncpy_s(shared->file, mux->path().c_str(), _TRUNCATE);
                strncpy_s(shared->encoder, mux->encoder.c_str(), _TRUNCATE);
                strncpy_s(shared->message, started ? "录制中" : "预热完成，等待开始", _TRUNCATE);
                InterlockedIncrement(&shared->seq);
            }
            Sleep(1);
        }
        if (outageRow)
            store->finish(outageRow, job.start + std::max(0LL, elapsed) / 1000, "no_video", "停止时仍无视频");
        if (audioOutageRow)
            store->finish(audioOutageRow, job.start + std::max(0LL, elapsed) / 1000, "no_audio",
                          "停止时仍无音频");
        if (monitor) cap.sample(stats, &captureBaseline);
        mux->finish(std::max(0LL, (i64)(frameNo * period)));
        if (monitor) {
            stats.frames = frameNo;
            writeRate.sample(GetTickCount64(), mux->writtenBytes(), stats);
            stats.complete = true;
            stats.recentMBps = 0;
            InterlockedIncrement(&shared->seq);
            shared->stats = stats;
            shared->frames = frameNo;
            InterlockedIncrement(&shared->seq);
            store->saveStats(job.token, stats);
        }
        InterlockedIncrement(&shared->seq);
        shared->frames = frameNo;
        InterlockedIncrement(&shared->seq);
        strncpy_s(shared->file, mux->path().c_str(), _TRUNCATE);
        publish(3, "已完成");
        log->write("recording completed");
    } catch (const std::exception &e) {
        result = 1;
        publish(4, e.what());
        if (log)
            log->write(std::string("ERROR ") + e.what());
        if (mux) {
            try {
                double factor = job.channel.profile.lapseMode == 1 ? job.channel.profile.lapseValue
                                : job.channel.profile.lapseMode == 2
                                    ? job.channel.profile.lapseValue * job.channel.profile.lapseFps
                                    : 1.;
                mux->finish((i64)(std::max(0LL, elapsed) / factor));
            } catch (...) {
            }
        }
        if (store) {
            try {
                store->history({0, nowMs(), nowMs(), job.channel.key, job.token, "", "failed", e.what()});
            } catch (...) {
            }
        }
    }
    if (result && monitor && mux) {
        writeRate.sample(GetTickCount64(), mux->writtenBytes(), stats);
        stats.complete = false; stats.recentMBps = 0;
        InterlockedIncrement(&shared->seq); shared->stats = stats; InterlockedIncrement(&shared->seq);
        if (store) store->saveStats(job.token, stats);
    }
    swr_free(&swr);
    mux.reset();
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return result;
}
} // namespace nr
