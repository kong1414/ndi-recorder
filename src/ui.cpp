#include "version.h"
#include "manager.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <cstdio>
#include <stdexcept>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace nr {
static Manager *activeManager_ = nullptr;
static bool modalDialog_ = false;
static HWND window_;
static float dpiScale_ = 1, uiScale_ = 1;
static double previewScale_ = 0; // Nonzero only while the settings dialog previews a choice.
static ImGuiStyle baselineStyle_;
static float dp(float value) { return value * uiScale_; }
static ImVec2 dpSize(float x, float y) { return {dp(x), dp(y)}; }
static void fitWindow(HWND h, const RECT *suggested = nullptr) {
    RECT r; if (suggested) r = *suggested; else GetWindowRect(h, &r);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return;
    int w = std::min(r.right-r.left, mi.rcWork.right-mi.rcWork.left);
    int height = std::min(r.bottom-r.top, mi.rcWork.bottom-mi.rcWork.top);
    int x = std::clamp((int)r.left, (int)mi.rcWork.left, (int)mi.rcWork.right-w);
    int y = std::clamp((int)r.top, (int)mi.rcWork.top, (int)mi.rcWork.bottom-height);
    SetWindowPos(h, nullptr, x, y, w, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
static void popupSize(float w, float h) {
    auto avail = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize({std::min(dp(w), std::max(100.f, avail.x - dp(24))),
                              std::min(dp(h), std::max(100.f, avail.y - dp(24)))});
    ImGui::SetNextWindowPos({avail.x / 2, avail.y / 2}, ImGuiCond_Always, {.5f, .5f});
}
static void nextToolbar(float width = 100) {
    if (ImGui::GetItemRectMax().x + dp(width + 12) < ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x) ImGui::SameLine();
}
static ID3D11Device *device_;
static ID3D11DeviceContext *context_;
static IDXGISwapChain *swap_;
static ID3D11RenderTargetView *target_;
static bool closeRequested_ = false, hidden_ = false, resetScaleRequested_ = false;
static NOTIFYICONDATAW tray_{};
static void makeTarget() {
    ID3D11Texture2D *b = nullptr;
    if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&b)))) {
        device_->CreateRenderTargetView(b, nullptr, &target_);
        b->Release();
    }
}
static void hideWindow() {
    hidden_ = true;
    ShowWindow(window_, SW_HIDE);
}
static void restore() {
    hidden_ = false;
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
}

static LRESULT WINAPI proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Handle recovery before ImGui so a modal or focused input cannot swallow it.
    if (m == WM_KEYDOWN && (w == '0' || w == VK_NUMPAD0) && (GetKeyState(VK_CONTROL) & 0x8000) && !(l & (1LL << 30))) {
        resetScaleRequested_ = true;
        return 0;
    }
    if (m == WM_APP + 3) { resetScaleRequested_ = true; return 0; }
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return 1;
    switch (m) {
    case WM_DPICHANGED:
        dpiScale_ = HIWORD(w) / 96.f;
        fitWindow(h, reinterpret_cast<const RECT *>(l));
        return 0;
    case WM_DISPLAYCHANGE:
        fitWindow(h);
        dpiScale_ = ImGui_ImplWin32_GetDpiScaleForHwnd(h);
        return 0;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO *>(l);
        info->ptMinTrackSize = {640, 480};
        return 0;
    }
    case WM_TIMER:
        if (modalDialog_ && activeManager_)
            try {
                activeManager_->tick(!hidden_);
            } catch (...) {
            }
        return 0;
    case WM_SIZE:
        if (device_ && w != SIZE_MINIMIZED) {
            if (target_) {
                target_->Release();
                target_ = nullptr;
            }
            swap_->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
            makeTarget();
        }
        return 0;
    case WM_CLOSE:
        closeRequested_ = true;
        restore();
        return 0;
    case WM_APP + 2:
        restore();
        return 0;
    case WM_APP + 1:
        if (l == WM_LBUTTONDBLCLK)
            restore();
        if (l == WM_RBUTTONUP) {
            POINT p;
            GetCursorPos(&p);
            auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"打开 NDI Recorder");
            AppendMenuW(menu, MF_STRING, 2, L"退出…");
            SetForegroundWindow(h);
            int r = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (r == 1)
                restore();
            if (r == 2) {
                closeRequested_ = true;
                restore();
            }
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w)
            PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
static bool input(const char *label, std::string &s, size_t max = 1024) {
    std::vector<char> b(std::max(max, s.size() + 64), 0);
    memcpy(b.data(), s.data(), s.size());
    if (ImGui::InputText(label, b.data(), b.size())) {
        s = b.data();
        return true;
    }
    return false;
}
static fs::path dialog(bool save, bool folder = false) {
    IFileDialog *d = nullptr;
    HRESULT hr =
        folder || !save
            ? CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))
            : CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d));
    if (FAILED(hr))
        return {};
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) {
        COMDLG_FILTERSPEC f[] = {{L"JSON 配置", L"*.json"}};
        d->SetFileTypes(1, f);
        d->SetDefaultExtension(L"json");
    }
    fs::path result;
    modalDialog_ = true;
    if (SUCCEEDED(d->Show(window_))) {
        IShellItem *i = nullptr;
        if (SUCCEEDED(d->GetResult(&i))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(i->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            i->Release();
        }
    }
    modalDialog_ = false;
    d->Release();
    return result;
}
static void openPath(const std::string &p, bool directory) {
    fs::path path(wide(p));
    if (directory)
        path = path.parent_path();
    ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
static bool button(const char *label, ImVec2 size = {0, 0}) {
    size = dpSize(size.x, size.y);
    if (size.x <= 0) size.x = std::max(dp(64), ImGui::CalcTextSize(label, nullptr, true).x + dp(20));
    if (size.y <= 0) size.y = dp(32);
    return ImGui::Button(label, size);
}
static void showError(const std::string &message) {
    bool previous = modalDialog_;
    modalDialog_ = true;
    MessageBoxW(window_, wide(message).c_str(), L"无法完成操作", MB_OK | MB_ICONWARNING);
    modalDialog_ = previous;
}
static ImU32 stateColor(const std::string &state) {
    if (state == "recording") return IM_COL32(203, 45, 57, 255);
    if (state == "completed") return IM_COL32(28, 128, 86, 255);
    if (state == "no_video" || state == "no_audio") return IM_COL32(180, 111, 12, 255);
    if (state == "failed" || state == "unreadable" || state == "interrupted") return IM_COL32(133, 64, 159, 255);
    if (state == "recovered") return IM_COL32(24, 126, 142, 255);
    return IM_COL32(111, 120, 134, 255);
}
static const char *stateName(const std::string &state) {
    if (state == "recording") return "正在录制";
    if (state == "completed") return "已完成";
    if (state == "no_video") return "视频缺失";
    if (state == "no_audio") return "音频缺失";
    if (state == "failed") return "录制失败";
    if (state == "unreadable") return "文件不可读";
    if (state == "interrupted") return "异常中断";
    if (state == "recovered") return "已恢复可读";
    if (state == "missed") return "已错过";
    return state.c_str();
}
static void statsPopup(Store &store, const std::string &task, const Runtime *live = nullptr) {
    popupSize(720, 650);
    if (!ImGui::BeginPopupModal("录制监测", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    static json saved;
    static bool total=false;
    if(ImGui::IsWindowAppearing()) total=false;
    if (!live && ImGui::IsWindowAppearing()) {
        try { saved = store.taskStats(task); } catch (...) { saved = nullptr; }
    }
    RecordingStats s;
    if(live) {
        ImGui::Checkbox("查看任务累计", &total);
        s=total?live->snapshot.stats:live->snapshot.recentStats;
        if(!total) ImGui::Text("最近 60 秒 · 当前覆盖 %.0f 秒",live->snapshot.recentSeconds);
        ImGui::Text("最近收到视频 %.2f 秒前 / 音频 %.2f 秒前",live->snapshot.videoAge,live->snapshot.audioAge);
    }
    else if (!saved.is_null()) s = saved.get<RecordingStats>();
    ImGui::BeginChild("stats-scroll", dpSize(0, -46));
    bool fresh = live && live->process && live->snapshot.state == 2 &&
                 GetTickCount64() - live->snapshot.heartbeat < 3000;
    if (!s.sampled) ImGui::TextWrapped("无统计数据（旧任务或当前尚未开始采样）。");
    else {
        if (live) ImGui::TextUnformatted(fresh ? "实时监测 · 每秒刷新" : "任务已结束或状态未更新 · 保留最后快照");
        else ImGui::TextUnformatted(s.complete ? "完整任务统计（包含全部分段）" : "部分统计 · 任务异常中断或最后快照未保存");
        if (fresh) ImGui::Text("文件写入：%.2f MB/s（近 5 秒）", s.recentMBps);
        else ImGui::TextUnformatted("实时文件写入：--");
        if(live && !total) ImGui::Text("窗口平均 %.2f MB/s",s.averageMBps);
        else ImGui::Text("任务平均 %.2f MB/s   采样峰值 %.2f MB/s", s.averageMBps, s.peakMBps);
        ImGui::Text("所选范围写入 %.2f MB   输出视频 %llu 帧", s.bytes / 1e6, s.frames);
        ImGui::SeparatorText("接收与缓存丢弃（按阶段分别计数）");
        ImGui::Text("NDI SDK：视频 %llu 帧 / 音频 %llu 块", s.sdkVideo, s.sdkAudio);
        ImGui::Text("接收队列溢出：视频 %llu 帧 / 音频 %llu 块", s.queueVideo, s.queueAudio);
        ImGui::Text("处理缓存溢出：视频 %llu 帧 / 音频 %llu 块", s.filterVideo, s.filterAudio);
        ImGui::Text("场配对失败 %llu 次", s.fieldPairs);
        ImGui::SeparatorText("输出补偿与帧率转换");
        ImGui::Text("重复画面 %llu 帧   黑画面填充 %llu 帧", s.repeated, s.black);
        ImGui::Text("静音填充 %llu 个采样点 / 每声道", s.silentSamples);
        ImGui::Text("时间轴选帧跳过 %llu 帧", s.converted);
        ImGui::TextWrapped("帧率转换、延时摄影会正常跳过或重复画面，不计入上方丢帧。不同阶段、视频帧、音频块和采样点不相加。");
        ImGui::TextWrapped("SDK 指标反映接收队列未及时取出的帧，不能据此测量发送端或网络的全部丢失。写入量是 FFmpeg 文件 I/O 计数，包含文件封装；不代表物理磁盘落盘速度。单位 MB = 1,000,000 字节。");
    }
    ImGui::EndChild();
    if (button("关闭")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
static void profileEditor(Profile &p, const Profile &defaults = Profile{}) {
    if (button("恢复默认参数")) p = defaults;
    ImGui::SameLine();
    ImGui::TextDisabled("仅恢复录制参数，保存后生效");
    ImGui::SeparatorText("文件分段");
    ImGui::InputInt("每段时长（分钟）", &p.segmentMinutes);
    ImGui::TextDisabled("默认 60 分钟；1–1440 分钟，按实际录制时间分段");
    ImGui::TextDisabled("分段在关键帧处完成，长度可能略超设定值");
    ImGui::SeparatorText("视频与接收质量");
    const char *codecs[] = {"H.264 / MP4", "FFV1 无损 / MKV"};
    int codec = p.codec == "ffv1";
    if (ImGui::Combo("视频编码", &codec, codecs, 2)) {
        p.codec = codec ? "ffv1" : "h264";
        if (codec) {
            p.width = p.height = p.fps = p.lapseMode = 0;
        }
    }
    ImGui::Checkbox("录制时使用最低带宽", &p.lowBandwidth);
    ImGui::SameLine();
    ImGui::TextDisabled("未勾选时录制使用最高质量");
    if (codec) {
        ImGui::TextWrapped("保存 NDI 解码像素和隔行结构，PCM 浮点音频；无损模式不设置目标码率。");
    } else {
        const char *enc[] = {"auto", "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"};
        int e = 0;
        for (int i = 0; i < 5; i++)
            if (p.encoder == enc[i])
                e = i;
        if (ImGui::Combo("编码器", &e, enc, 5))
            p.encoder = enc[e];
        ImGui::InputInt("视频码率 Mbps", &p.mbps);
        ImGui::InputDouble("音频电平换算 dB", &p.audioGainDB, 1, 6, "%.1f");
        ImGui::TextDisabled("默认 -20 dB 按 NDI SMPTE 参考电平换算，EBU 可设 -14 dB");
        float boost = (float)p.recordGainDB;
        if (ImGui::SliderFloat("录制增益 dB", &boost, -24.f, 24.f, "%+.1f dB")) p.recordGainDB = std::isfinite(boost) ? std::clamp(boost, -24.f, 24.f) : 0.;
        ImGui::TextWrapped("0 dB 保持原电平；+6 dB 约为两倍振幅。录制中可在通道「音频」实时调整。");
        ImGui::InputInt("宽度（0 跟随）", &p.width);
        ImGui::InputInt("高度（0 跟随）", &p.height);
        ImGui::InputInt("帧率（0 自动）", &p.fps);
        const char *fields[] = {"自动 / 默认顶场优先", "顶场优先", "底场优先"};
        int fi = p.fieldOrder + 1;
        if (ImGui::Combo("输入场序", &fi, fields, 3))
            p.fieldOrder = fi - 1;
        ImGui::TextDisabled("1080i50 自动转 1080p50；逐行源保留帧率");
        const char *lapse[] = {"普通录制（含声音）", "延时摄影：加速倍数", "延时摄影：每隔 N 秒取一帧"};
        ImGui::Combo("录制方式", &p.lapseMode, lapse, 3);
        if (p.lapseMode) {
            ImGui::InputDouble(p.lapseMode == 1 ? "加速倍数" : "取帧间隔（秒）", &p.lapseValue, 1, 10,
                               "%.2f");
            ImGui::InputInt("成片帧率", &p.lapseFps);
            double factor = p.lapseMode == 1 ? p.lapseValue : p.lapseValue * p.lapseFps;
            ImGui::Text("每小时实时时长约产生 %.1f 秒视频，无音轨", 3600 / std::max(1., factor));
        }
    }
    ImGui::SeparatorText("音频声道");
    std::string selected = "声道 " + std::to_string(p.audioPair * 2 + 1) + " / " + std::to_string(p.audioPair * 2 + 2);
    if (ImGui::BeginCombo("录入声道", selected.c_str())) {
        for (int pair = 0; pair < 32; ++pair) {
            auto label = "声道 " + std::to_string(pair * 2 + 1) + " / " + std::to_string(pair * 2 + 2);
            if (ImGui::Selectable(label.c_str(), p.audioPair == pair)) p.audioPair = pair;
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("每次录入两路声音，分别作为文件的左、右声道");
    ImGui::TextDisabled("通常选择 1 / 2；单声道源自动复制到左右声道");
    ImGui::TextDisabled("空闲预览始终请求最低带宽；不等于转换成 NDI HX");
}
static void screenshot(const fs::path &path) {
    ID3D11Texture2D *b = nullptr, *copy = nullptr;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&b))))
        return;
    D3D11_TEXTURE2D_DESC d;
    b->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(&d, nullptr, &copy))) {
        b->Release();
        return;
    }
    context_->CopyResource(copy, b);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(context_->Map(copy, 0, D3D11_MAP_READ, 0, &m))) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih;
        ih.biWidth = d.Width;
        ih.biHeight = -(LONG)d.Height;
        ih.biPlanes = 1;
        ih.biBitCount = 32;
        ih.biCompression = BI_RGB;
        ih.biSizeImage = d.Width * d.Height * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + ih.biSizeImage;
        std::ofstream f(path, std::ios::binary);
        f.write((char *)&fh, sizeof fh);
        f.write((char *)&ih, sizeof ih);
        std::vector<unsigned char> row(d.Width * 4);
        for (UINT y = 0; y < d.Height; y++) {
            auto src = (unsigned char *)m.pData + y * m.RowPitch;
            for (UINT x = 0; x < d.Width; x++) {
                row[x * 4] = src[x * 4 + 2];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4];
                row[x * 4 + 3] = 255;
            }
            f.write((char *)row.data(), row.size());
        }
        context_->Unmap(copy, 0);
    }
    copy->Release();
    b->Release();
}
class UI {
    Manager &m_;
    double view_ = 0, span_ = 21600000;
    bool followNow_ = true;
    std::string deleteKey_;
    bool captureStats_ = false;
    bool exportOpen_=false, importOpen_=false, importReady_=false, importSettings_=false;
    json importData_;
    std::set<std::string> selectedPlans_, selectedChannels_;
    std::string importStatus_;
    void cancelMonitoring() {
        for(auto &[key,rt]:m_.runtimes) {rt->monitor=false;if(rt->preview) rt->preview->monitor(false);}
    }
    void loadImport(const json &j) {
        Manager::validateConfig(j);
        importData_={{"version",1},{"settings",j.at("settings").get<Settings>()},
            {"channels",j.at("channels").get<std::vector<Channel>>()},{"plans",j.at("plans").get<std::vector<Plan>>()}};
        selectedPlans_.clear();selectedChannels_.clear();
        importSettings_=false;importReady_=false;importStatus_.clear();importOpen_=true;
    }
    void configDialogs() {
        if(exportOpen_) {ImGui::OpenPopup("导出配置说明");exportOpen_=false;}
        popupSize(640,410);
        if(ImGui::BeginPopupModal("导出配置说明",nullptr,ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BeginChild("export-content",dpSize(0,-46));
            ImGui::TextWrapped("将保存一个 JSON 配置文件，便于备份或迁移。");
            ImGui::SeparatorText("包含内容");
            ImGui::BulletText("基础设置：存储路径、预览、日志、缩放、监听音量等");
            ImGui::BulletText("%d 路通道：来源名称、编码、音频、分段参数",(int)m_.channels.size());
            ImGui::BulletText("%d 个计划：日期、时段、重复规则、启用状态及跳过日期",(int)m_.plans.size());
            ImGui::SeparatorText("不包含内容");
            ImGui::TextWrapped("录像文件、录制历史、监测统计、诊断日志和当前临时监听状态。");
            ImGui::TextWrapped("配置包含来源名称、额外发现 IP 和本机路径；迁移到其他电脑后请检查存储位置。");
            ImGui::EndChild();
            if(button("选择位置并导出",{170,32})) {
                auto path=dialog(true);
                if(!path.empty()) try {m_.exportConfig(path);ImGui::CloseCurrentPopup();} catch(const std::exception &e){showError(e.what());}
            }
            ImGui::SameLine();if(button("取消")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if(importOpen_){ImGui::OpenPopup("选择导入内容");importOpen_=false;}
        popupSize(790,670);
        if(ImGui::BeginPopupModal("选择导入内容",nullptr,ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("只合并勾选内容，保留现有任务。相同标识且内容相同会跳过；不同内容或时间重叠将阻止导入。");
            ImGui::BeginChild("import-selection",dpSize(0,-140));
            bool changed=ImGui::Checkbox("同时导入基础设置（会替换本机基础设置）",&importSettings_);
            if(importSettings_) ImGui::TextWrapped("导入存储路径：%s",importData_["settings"].get<Settings>().storage.c_str());
            ImGui::SeparatorText("选择计划任务 · 必要通道自动带入");
            if(button("全选计划")){for(auto &v:importData_["plans"]) selectedPlans_.insert(v.at("key").get<std::string>());changed=true;}
            ImGui::SameLine();if(button("清空选择")){selectedPlans_.clear();selectedChannels_.clear();importSettings_=false;changed=true;}
            for(auto &v:importData_["plans"]) {
                auto plan=v.get<Plan>();bool on=selectedPlans_.count(plan.key)>0;
                std::string channel;
                for(auto &c:importData_["channels"]) if(c.at("key")==plan.channel) channel=c.at("name").get<std::string>();
                auto label=plan.name+" / "+channel+"##import-plan-"+plan.key;
                if(ImGui::Checkbox(label.c_str(),&on)){if(on)selectedPlans_.insert(plan.key);else selectedPlans_.erase(plan.key);changed=true;}
                ImGui::TextDisabled("    %s  %s - %s  %s%s",dateText(plan.date).c_str(),timeText(plan.start).c_str(),timeText(plan.end).c_str(),
                    plan.repeat==0?"单次":plan.repeat==1?"每天":"每周",plan.enabled?"":"（停用）");
            }
            ImGui::SeparatorText("额外导入通道（无需导入计划时也可单独选择）");
            for(auto &v:importData_["channels"]) {
                auto c=v.get<Channel>();bool needed=false;
                for(auto &entry:importData_["plans"]) {auto plan=entry.get<Plan>();if(selectedPlans_.count(plan.key) && plan.channel==c.key)needed=true;}
                bool on=needed || selectedChannels_.count(c.key)>0;
                ImGui::BeginDisabled(needed);
                if(ImGui::Checkbox((c.name+"##import-channel-"+c.key).c_str(),&on)){if(on)selectedChannels_.insert(c.key);else selectedChannels_.erase(c.key);changed=true;}
                ImGui::EndDisabled();ImGui::SameLine();ImGui::TextDisabled("%s%s",needed?"随计划带入 · ":"",c.source.c_str());
            }
            ImGui::EndChild();
            if(changed){importReady_=false;importStatus_.clear();}
            if(m_.busy()) ImGui::TextColored({.65f,.2f,.12f,1},"请先停止全部录制，预览不会修改配置。");
            else ImGui::TextDisabled("已选计划 %d 个 / 额外通道 %d 路",(int)selectedPlans_.size(),(int)selectedChannels_.size());
            ImGui::BeginChild("import-status",dpSize(0,48));ImGui::TextWrapped("%s",importStatus_.c_str());ImGui::EndChild();
            if(button("检查所选",{110,32})) {
                try {auto merged=m_.previewImport(importData_,selectedPlans_,selectedChannels_,importSettings_);
                    importReady_=true;importStatus_="检查通过；导入后共 "+std::to_string(merged["channels"].size())+" 路通道、"+std::to_string(merged["plans"].size())+" 个计划。启用的计划会按规则执行。";
                } catch(const std::exception &e){importReady_=false;importStatus_=e.what();}
            }
            ImGui::SameLine();ImGui::BeginDisabled(!importReady_ || m_.busy());
            if(button("确认导入",{110,32})) {
                try {m_.applyConfig(m_.previewImport(importData_,selectedPlans_,selectedChannels_,importSettings_));historyDirty_=true;ImGui::CloseCurrentPopup();}
                catch(const std::exception &e){importReady_=false;importStatus_=e.what();}
            }
            ImGui::EndDisabled();ImGui::SameLine();if(button("取消")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    bool settingsOpen_ = false, channelOpen_ = false, planOpen_ = false, historyOpen_ = false;
    Settings editSettings_;
    Channel editChannel_;
    Plan editPlan_;
    std::string editDate_, editStart_, editEnd_;
    bool newChannel_ = false;
    int historyPage_ = 0, networkIndex_ = 0;
    bool networkChosen_ = false;
    std::vector<History> history_, range_;
    i64 historyRefresh_ = 0;
    bool historyDirty_ = false;
    std::string jumpDate_;
    double navigationBase_ = (double)dayStart(nowMs());
    json captureInfo_;
    ImVec4 captureListen_{};
    bool captureEnabled_ = false;
    void planDialog(Plan p) {
        editPlan_ = p;
        editDate_ = dateText(p.date);
        editStart_ = timeText(p.start);
        editEnd_ = timeText(p.end);
        planOpen_ = true;
    }
    void createPlan(const Channel &c, i64 a, i64 b) {
        Plan p;
        p.channel = c.key;
        p.date = dayStart(a);
        p.start = (int)((a - p.date) / 3600000) * 3600;
        p.end = (int)((b - dayStart(b)) / 3600000) * 3600;
        planDialog(p);
    }
    void savePlan() {
        int a = parseTime(editStart_), b = parseTime(editEnd_);
        i64 d = parseDate(editDate_);
        if (a < 0 || b < 0 || d < 0)
            throw std::runtime_error("请输入有效日期和时分秒");
        if (editPlan_.repeat == 2 && !editPlan_.weekMask)
            throw std::runtime_error("至少选择一个星期");
        editPlan_.start = a;
        editPlan_.end = b;
        editPlan_.date = d;
        for (auto &p : m_.plans)
            if (conflicts(editPlan_, p))
                throw std::runtime_error("与计划「" + p.name + "」重叠");
        auto it =
            std::find_if(m_.plans.begin(), m_.plans.end(), [&](auto &p) { return p.key == editPlan_.key; });
        if (it == m_.plans.end())
            m_.plans.push_back(editPlan_);
        else
            *it = editPlan_;
        m_.save();
    }
    void preview(Channel &c, Runtime &r, ImVec2 pos) {
        bool visible = ImGui::IsRectVisible(pos, {pos.x + dp(256), pos.y + dp(144)});
        r.preview->visible(visible, m_.settings.previewFps);
        if (visible && r.preview->snapshot(r.image, r.generation) && !r.image.pixels.empty()) {
            if (r.texture) {
                r.texture->Release();
                r.texture = nullptr;
            }
            D3D11_TEXTURE2D_DESC d{};
            d.Width = r.image.w;
            d.Height = r.image.h;
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data{r.image.pixels.data(), (UINT)r.image.w * 4, 0};
            ID3D11Texture2D *t = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&d, &data, &t))) {
                device_->CreateShaderResourceView(t, nullptr, &r.texture);
                t->Release();
            }
        }
        auto draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, {pos.x + dp(256), pos.y + dp(144)}, IM_COL32(10, 12, 15, 255), 0);
        if (r.texture && r.image.online) {
            float scale = std::min(dp(256) / r.image.w, dp(144) / r.image.h);
            float w = r.image.w * scale, h = r.image.h * scale;
            ImVec2 a{pos.x + (dp(256) - w) / 2, pos.y + (dp(144) - h) / 2};
            draw->AddImage((ImTextureID)(intptr_t)r.texture, a, {a.x + w, a.y + h});
        } else
            draw->AddText({pos.x + dp(84), pos.y + dp(62)}, IM_COL32(200, 205, 213, 255), "等待 NDI");
        draw->AddCircleFilled({pos.x + dp(11), pos.y + dp(12)}, dp(4),
                              r.image.online ? IM_COL32(64, 218, 155, 255) : IM_COL32(132, 142, 160, 255));
        if (r.process)
            draw->AddText({pos.x + dp(174), pos.y + dp(6)},
                          r.snapshot.state == 2 ? IM_COL32(255, 89, 103, 255) : IM_COL32(239, 183, 81, 255),
                          r.snapshot.state == 2 ? "● REC" : "准备中");
        for (int k = 0; k < 2; k++) {
            float level = (float)(20 * std::log10(std::max(1e-6, k ? r.image.peakR : r.image.peakL)));
            float fraction = std::clamp((level + 60) / 60, 0.f, 1.f);
            float x = pos.x + dp(265) + k * dp(9);
            draw->AddRectFilled({x, pos.y}, {x + dp(5), pos.y + dp(144)}, IM_COL32(224, 230, 236, 255));
            draw->AddRectFilled({x, pos.y + dp(144) * (1 - fraction)}, {x + dp(5), pos.y + dp(144)},
                                level > -3 ? IM_COL32(246, 79, 92, 255) : IM_COL32(44, 205, 151, 255));
        }
    }
    void timeline(Channel &c, Runtime &r, ImVec2 p, float width) {
        auto dl = ImGui::GetWindowDrawList();
        if (captureEnabled_ && captureInfo_.is_null()) captureInfo_ = {{"x", p.x}, {"y", p.y}, {"width", width}, {"height", dp(160)}};
        const float h = dp(160);
        const float half = h / 2;
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("timeline", {width, h},
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered = ImGui::IsItemHovered();
        bool timelineActive = ImGui::IsItemActive();
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
        auto &io = ImGui::GetIO();
        auto x = [&](double t) { return p.x + (float)((t - view_) / span_ * width); };
        auto t = [&](float px) { return view_ + (px - p.x) / width * span_; };
        dl->PushClipRect(p, {p.x + width, p.y + h}, true);
        dl->AddRectFilled(p, {p.x + width, p.y + h}, IM_COL32(247, 249, 252, 255), 0);
        double step = span_ <= 600000     ? 60000
                      : span_ <= 7200000  ? 600000
                      : span_ <= 86400000 ? 3600000
                                          : 86400000;
        for (double v = std::floor(view_ / step) * step; v < view_ + span_; v += step)
            dl->AddLine({x(v), p.y}, {x(v), p.y + h}, IM_COL32(208, 216, 227, 110));
        // A quiet time-anchored dot grid remains visible through translucent blocks.
        double dotStep = step / 4;
        for (double v = std::floor(view_ / dotStep) * dotStep; v < view_ + span_; v += dotStep)
            for (float yy = p.y + dp(12); yy < p.y + h; yy += dp(16))
                dl->AddCircleFilled({x(v), yy}, dp(.8f), IM_COL32(151, 166, 187, 95), 4);
        for (i64 day = dayStart((i64)view_); day < view_ + span_; day += 86400000LL) {
            float a = std::max(p.x, x((double)day));
            float b = std::min(p.x + width, x((double)(day + 86400000LL)));
            if (b - a < dp(80)) continue;
            dl->PushClipRect({a, p.y}, {b, p.y + h}, true);
            dl->AddText(ImGui::GetFont(), dp(28), {a + dp(12), p.y + h - dp(36)}, IM_COL32(93, 112, 139, 48), dateText(day).c_str());
            dl->PopClipRect();
        }
        for (int enabledPass = 0; enabledPass < 2; ++enabledPass)
        for (auto &plan : m_.plans)
            if (plan.channel == c.key && plan.enabled == (enabledPass != 0)) {
                auto displayPlan = plan;
                displayPlan.enabled = true;
                for (auto &o : occurrences(displayPlan, (i64)view_, (i64)(view_ + span_))) {
                    float a = std::max(p.x, x(o.start)), b = std::min(p.x + width, x(o.end));
                    bool active = r.process && r.snapshot.state == 2 && !r.stopping && nowMs() >= o.start && nowMs() < o.end;
                    ImU32 col = !c.enabled || !plan.enabled ? IM_COL32(111, 120, 134, 255)
                                : active ? stateColor("recording")
                                         : IM_COL32(41, 102, 177, 255);
                    dl->AddRectFilled({a, p.y}, {std::max(a + dp(2), b), p.y + half}, (col & 0x00ffffff) | IM_COL32(0,0,0,58));
                    dl->AddRectFilled({a, p.y}, {std::max(a + dp(2), b), p.y + dp(3)}, col);
                    if (b - a > dp(55)) {
                        dl->PushClipRect({a, p.y}, {b, p.y + half}, true);
                        dl->AddText({a + dp(8), p.y + dp(16)}, IM_COL32(22, 29, 39, 255), plan.name.c_str());
                        std::string label = timeText(plan.start) + " - " + timeText(plan.end);
                        dl->AddText({a + dp(8), p.y + dp(46)}, IM_COL32(42, 50, 62, 255), label.c_str());
                        dl->PopClipRect();
                    }
                    bool over = hovered && io.MousePos.x >= a && io.MousePos.x <= b &&
                                io.MousePos.y >= p.y && io.MousePos.y < p.y + half;
                    if (over) {
                        ImGui::SetTooltip("%s\n%s → %s\n双击编辑计划；左键拖动平移时间轴",
                                          plan.name.c_str(), dateText(o.start, true).c_str(),
                                          dateText(o.end, true).c_str());
                        if (ImGui::IsMouseDoubleClicked(0)) planDialog(plan);
                    }
                }
            }
        for (int gapPass = 0; gapPass < 2; ++gapPass)
        for (auto &rec : range_)
            if (rec.channel == c.key && (rec.state == "no_video" || rec.state == "no_audio") == (gapPass != 0)) {
                i64 end = rec.end ? rec.end : nowMs();
                if (rec.start >= view_ + span_ || end < view_)
                    continue;
                float a = std::max(p.x, x(rec.start)),
                      b = std::min(p.x + width, x(std::max(end, rec.start + 1000)));
                bool gap = rec.state == "no_video" || rec.state == "no_audio";
                ImU32 col = stateColor(rec.state);
                float y = p.y + (gap ? h - dp(10) : half);
                dl->AddRectFilled({a, y}, {std::max(a + dp(2), b), p.y + h},
                                  gap ? col : (col & 0x00ffffff) | IM_COL32(0,0,0,85));
                if (!gap) dl->AddRectFilled({a, p.y + h - dp(3)}, {std::max(a + dp(2), b), p.y + h}, col);
                // File boundaries stay subtle and do not split the plan/actual band.
                if(!gap && !rec.path.empty() && rec.start>=view_) {
                    for(float yy=p.y+h-dp(25);yy<p.y+h-dp(5);yy+=dp(7))
                        dl->AddLine({a,yy},{a,std::min(yy+dp(3),p.y+h-dp(4))},IM_COL32(60,75,95,75),dp(1));
                }
                if (!gap && b - a > dp(60)) {
                    dl->PushClipRect({a, y}, {b, p.y + h - dp(10)}, true);
                    dl->AddText({a + dp(8), y + dp(12)}, IM_COL32(22, 29, 39, 255), stateName(rec.state));
                    auto duration = dateText(rec.start, true).substr(11) + " - " + dateText(end, true).substr(11);
                    dl->AddText({a + dp(8), y + dp(40)}, IM_COL32(42, 50, 62, 255), duration.c_str());
                    dl->PopClipRect();
                }
                if (hovered && io.MousePos.x >= a && io.MousePos.x <= b && io.MousePos.y >= y &&
                    io.MousePos.y < p.y + h) {
                    ImGui::SetTooltip("%s\n%s\n%s", stateName(rec.state), rec.path.c_str(), rec.detail.c_str());
                    if (ImGui::IsMouseDoubleClicked(0) && !rec.path.empty())
                        openPath(rec.path, false);
                }
            }
        float current = x((double)nowMs());
        if (current >= p.x && current <= p.x + width) {
            dl->AddLine({current, p.y}, {current, p.y + h}, IM_COL32(255, 96, 113, 255), dp(1.5f));
        }
        if (hovered && io.MouseWheel != 0) {
            double cursor = t(io.MousePos.x), fraction = (io.MousePos.x - p.x) / width;
            span_ = std::clamp(span_ * std::pow(0.8, io.MouseWheel), 60000., 14 * 86400000.);
            view_ = cursor - fraction * span_; followNow_=false;
            historyDirty_ = true;
        }
        if ((timelineActive && ImGui::IsMouseDragging(0, dp(3))) ||
            (hovered && ImGui::IsMouseDragging(2))) {
            view_ -= io.MouseDelta.x / width * span_; followNow_=false;
            historyDirty_ = true;
        }
        if (hovered && io.MouseWheelH != 0) {
            view_ -= io.MouseWheelH * span_ / 12; followNow_=false;
            historyDirty_ = true;
        }
        dl->PopClipRect();
    }
    void audioPopup(Channel &c, Runtime &r) {
        popupSize(610, 370);
        if (!ImGui::BeginPopupModal("录制声音增益", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
        ImGui::BeginChild("audio-scroll", dpSize(0, -46));
        const auto &active = r.process ? r.activeProfile : c.profile;
        bool available = active.codec == "h264" && !active.lapseMode;
        ImGui::TextUnformatted(c.name.c_str());
        ImGui::TextWrapped("录制增益会改变写入文件的声音。监听音量只影响扬声器/耳机，请在主界面底部调整。");
        ImGui::BeginDisabled(!available);
        float gain = (float)active.recordGainDB;
        bool changed = ImGui::SliderFloat("录制增益", &gain, -24.f, 24.f, "%+.1f dB");
        bool save = ImGui::IsItemDeactivatedAfterEdit();
        if (button("-3 dB")) { gain = std::max(-24.f, gain-3); changed = save = true; }
        ImGui::SameLine();
        if (button("+3 dB")) { gain = std::min(24.f, gain+3); changed = save = true; }
        ImGui::SameLine();
        if (button("恢复 0 dB")) { gain = 0; changed = save = true; }
        if (changed) {
            gain = std::isfinite(gain) ? std::clamp(gain, -24.f, 24.f) : 0.f;
            c.profile.recordGainDB = gain;
            if (r.process && r.shared) {
                r.activeProfile.recordGainDB = gain;
                InterlockedExchange(&r.shared->recordGainMilliDB, (LONG)std::lround(gain*1000));
            }
        }
        if (save) try { m_.save(); } catch (const std::exception &e) { showError(e.what()); }
        ImGui::EndDisabled();
        if (!available) ImGui::TextWrapped("FFV1 无损录制保持原始音频；延时摄影没有音轨，因此当前模式不应用录制增益。");
        else {
            ImGui::Text("参考电平换算 %+.1f dB · 录制增益 %+.1f dB", active.audioGainDB, active.recordGainDB);
            ImGui::TextWrapped(r.process ? "调整实时生效，经过现有音频缓冲后写入，不中断录制；也会保留为下次任务的设置。"
                                        : "此设置将用于下一次录制。");
            ImGui::TextWrapped("增益过大可能削波失真；接近电平表红区时请降低增益。");
        }
        ImGui::EndChild();
        if (button("关闭")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void popups() {
        if (settingsOpen_) {
            ImGui::OpenPopup("基础设置");
            settingsOpen_ = false;
        }
        popupSize(620, 690);
        if (ImGui::BeginPopupModal("基础设置", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BeginChild("setting-scroll", dpSize(0, -46), false, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::SeparatorText("显示缩放（立即预览）");
            const char *scales[] = {"80%", "90%", "100% · 跟随系统", "110%", "125%", "150%"};
            const double factors[] = {.8, .9, 1., 1.1, 1.25, 1.5};
            int selected = 2;
            for (int i = 0; i < 6; ++i) if (std::abs(editSettings_.uiScale-factors[i]) < .001) selected = i;
            if (ImGui::Combo("软件缩放", &selected, scales, 6)) previewScale_ = editSettings_.uiScale = factors[selected];
            if (button("恢复系统缩放")) previewScale_ = editSettings_.uiScale = 1.;
            ImGui::Text("当前屏幕 DPI：%.0f%%  ·  有效界面缩放：%.0f%%", dpiScale_*100, uiScale_*100);
            ImGui::TextWrapped("软件缩放叠加在 Windows 缩放之上；取消将恢复之前的设置。\n应急恢复：Ctrl+0，或按住 Shift 启动程序，回到 100% 软件缩放。");
            ImGui::Separator();
            input("录像存储路径", editSettings_.storage);
            ImGui::SameLine();
            if (button("选择…")) {
                auto p = dialog(false, true);
                if (!p.empty())
                    editSettings_.storage = utf8(p.wstring());
            }
            ImGui::InputDouble("安全余量 GB", &editSettings_.reserveGB, 1, 10, "%.1f");
            ImGui::InputInt("预览刷新 fps", &editSettings_.previewFps);
            input("额外发现 IP（逗号分隔）", editSettings_.extraIPs);
            ImGui::Checkbox("登录 Windows 后启动", &editSettings_.autoStart);
            ImGui::InputInt("日志保留天数", &editSettings_.logDays);
            ImGui::InputInt("日志总上限 MB", &editSettings_.logMB);
            ImGui::TextDisabled("日志保留设置下次启动生效；存储和录制设置用于新任务");
            ImGui::SeparatorText("新通道默认参数");
            profileEditor(editSettings_.defaults);
            ImGui::EndChild();
            if (button("保存设置", {140, 0})) {
                try {
                    auto e = validate(editSettings_.defaults);
                    if (!e.empty())
                        throw std::runtime_error(e);
                    if (editSettings_.storage.empty() || editSettings_.reserveGB < 0 ||
                        editSettings_.previewFps < 1 || editSettings_.previewFps > 30 ||
                        editSettings_.logDays < 1 || editSettings_.logMB < 16)
                        throw std::runtime_error("基础参数超出范围");
                    fs::create_directories(wide(editSettings_.storage));
                    setAutoStart(editSettings_.autoStart);
                    m_.settings = editSettings_;
                    previewScale_ = 0;
                    m_.save();
                    m_.refreshDiscovery();
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception &e) {
                    showError(e.what());
                }
            }
            ImGui::SameLine();
            if (button("取消")) {
                previewScale_ = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (channelOpen_) {
            ImGui::OpenPopup("通道设置");
            channelOpen_ = false;
        }
        popupSize(640, 700);
        if (ImGui::BeginPopupModal("通道设置", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BeginChild("channel-scroll", dpSize(0, -60), false, ImGuiWindowFlags_HorizontalScrollbar);
            input("通道名称", editChannel_.name);
            input("NDI 完整源名", editChannel_.source);
            if (ImGui::BeginCombo("选择发现的来源", editChannel_.source.c_str())) {
                for (auto &name : m_.finder->names())
                    if (ImGui::Selectable(name.c_str(), editChannel_.source == name)) {
                        editChannel_.source = name;
                        if (newChannel_)
                            editChannel_.name = name;
                    }
                ImGui::EndCombo();
            }
            if (button("刷新来源")) m_.finder->refresh();
            ImGui::SameLine();
            if (m_.finder->refreshing()) ImGui::TextDisabled("正在搜索 NDI 来源…");
            else ImGui::TextDisabled("已发现 %d 个来源 · 列表持续自动更新", (int)m_.finder->names().size());
            ImGui::Separator();
            profileEditor(editChannel_.profile, m_.settings.defaults);
            ImGui::TextWrapped("正在录制的参数保持不变，修改将在下一次任务生效。");
            ImGui::EndChild();
            if (button(newChannel_ ? "添加通道" : "保存通道", {130, 0})) {
                try {
                    auto e = validate(editChannel_.profile);
                    if (!e.empty())
                        throw std::runtime_error(e);
                    if (editChannel_.source.empty() || editChannel_.name.empty())
                        throw std::runtime_error("源名和通道名称不能为空");
                    if (newChannel_)
                        m_.channels.push_back(editChannel_);
                    else
                        for (auto &c : m_.channels)
                            if (c.key == editChannel_.key) {
                                c = editChannel_;
                            }
                    m_.save();
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception &e) {
                    showError(e.what());
                }
            }
            ImGui::SameLine();
            if (button("取消"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (planOpen_) {
            ImGui::OpenPopup("计划设置");
            planOpen_ = false;
        }
        popupSize(660, 650);
        if (ImGui::BeginPopupModal("计划设置", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::BeginChild("plan-scroll", dpSize(0, -46), false, ImGuiWindowFlags_HorizontalScrollbar);
            input("计划名称", editPlan_.name);
            const char *types[] = {"指定日期一次", "每天重复", "每周重复"};
            ImGui::Combo("重复规则", &editPlan_.repeat, types, 3);
            input("日期 YYYY-MM-DD", editDate_, 64);
            if(button("今天",{65,32})) editDate_=dateText(nowMs());
            ImGui::SameLine();if(button("明天",{65,32})) editDate_=dateText(nowMs()+86400000);
            ImGui::SameLine();if(button("下个整点开始",{145,32})) {
                auto at=(nowMs()/3600000+1)*3600000;editDate_=dateText(at);
                editStart_=timeText((int)((at-dayStart(at))/1000));editEnd_=timeText((parseTime(editStart_)+3600)%86400);
            }
            input("开始 HH:MM:SS", editStart_, 64);
            input("结束 HH:MM:SS", editEnd_, 64);
            int startSeconds=parseTime(editStart_);
            ImGui::BeginDisabled(startSeconds<0);
            if(button("录制 1 小时",{125,32})) editEnd_=timeText((startSeconds+3600)%86400);
            ImGui::SameLine();if(button("录制 2 小时",{125,32})) editEnd_=timeText((startSeconds+7200)%86400);
            ImGui::SameLine();if(button("录制 4 小时",{125,32})) editEnd_=timeText((startSeconds+14400)%86400);
            ImGui::EndDisabled();
            ImGui::TextDisabled("快捷时长从上方开始时间计算；结束不晚于开始表示跨次日。");
            ImGui::TextDisabled("新计划默认分、秒为 00；已保存计划保持原时间。时区 UTC+8。");
            if (editPlan_.repeat == 2) {
                const char *days[] = {"一", "二", "三", "四", "五", "六", "日"};
                for (int i = 0; i < 7; i++) {
                    bool on = editPlan_.weekMask & (1 << i);
                    if (i)
                        ImGui::SameLine();
                    if (ImGui::Checkbox(days[i], &on)) {
                        if (on)
                            editPlan_.weekMask |= 1 << i;
                        else
                            editPlan_.weekMask &= ~(1 << i);
                    }
                }
            }
            ImGui::Checkbox("启用计划", &editPlan_.enabled);
            if(editPlan_.repeat && ImGui::CollapsingHeader("例外日期（可选）")) {
                ImGui::TextWrapped("保存会修改整个重复计划；跳过只影响选定日期。");
            if (button("跳过填写日期的这一轮")) {
                auto d = parseDate(editDate_);
                if (d >= 0 &&
                    std::find(editPlan_.skips.begin(), editPlan_.skips.end(), d) == editPlan_.skips.end())
                    editPlan_.skips.push_back(d);
            }
            ImGui::SameLine();
            if (button("恢复该轮")) {
                auto d = parseDate(editDate_);
                std::erase(editPlan_.skips, d);
            }
            ImGui::Text("已跳过 %d 次", (int)editPlan_.skips.size());
            }
            ImGui::Separator();
            ImGui::EndChild();
            if (button("保存计划", {130, 0})) {
                try {
                    savePlan();
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception &e) {
                    showError(e.what());
                }
            }
            ImGui::SameLine();
            bool existingPlan=std::any_of(m_.plans.begin(),m_.plans.end(),[&](auto &p){return p.key==editPlan_.key;});
            ImGui::BeginDisabled(!existingPlan);
            if (button("删除此计划")) {
                std::erase_if(m_.plans, [&](auto &p) { return p.key == editPlan_.key; });
                m_.save();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            ImGui::EndDisabled();
            if (button("取消"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!deleteKey_.empty())
            ImGui::OpenPopup("删除通道确认");
        if (ImGui::BeginPopupModal("删除通道确认", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("删除通道及其计划，保留已录制文件。");
            if (button("删除")) {
                auto it = m_.runtimes.find(deleteKey_);
                if (it != m_.runtimes.end() && it->second->process)
                    showError("请先停止录制再删除");
                else {
                    std::erase_if(m_.channels, [&](auto &c) { return c.key == deleteKey_; });
                    std::erase_if(m_.plans, [&](auto &p) { return p.channel == deleteKey_; });
                    m_.runtimes.erase(deleteKey_);
                    m_.save();
                    deleteKey_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (button("取消")) {
                deleteKey_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (historyOpen_) {
            ImGui::OpenPopup("录制历史");
            historyOpen_ = false;
            history_ = m_.store.historyPage(historyPage_ * 50, 50);
        }
        popupSize(1050, 590);
        if (ImGui::BeginPopupModal("录制历史", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            if (button("上一页") && historyPage_ > 0) {
                historyPage_--;
                history_ = m_.store.historyPage(historyPage_ * 50, 50);
            }
            ImGui::SameLine();
            if (button("下一页") && history_.size() == 50) {
                historyPage_++;
                history_ = m_.store.historyPage(historyPage_ * 50, 50);
            }
            ImGui::SameLine();
            if (button("刷新"))
                history_ = m_.store.historyPage(historyPage_ * 50, 50);
            ImGui::SameLine();
            ImGui::Text("第 %d 页", historyPage_ + 1);
            if (ImGui::BeginTable("historytable", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  dpSize(0, -45))) {
                ImGui::TableSetupColumn("开始时间", ImGuiTableColumnFlags_WidthFixed, dp(180));
                ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, dp(95));
                ImGui::TableSetupColumn("文件 / 详情");
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, dp(135));
                ImGui::TableHeadersRow();
                for (auto &h : history_) {
                    ImGui::PushID((int)h.row);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(dateText(h.start, true).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(stateColor(h.state)), "%s", stateName(h.state));
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", h.path.empty() ? h.detail.c_str() : h.path.c_str());
                    ImGui::TableNextColumn();
                    if (!h.path.empty()) {
                        if (button("播放"))
                            openPath(h.path, false);
                        ImGui::SameLine();
                        if (button("目录"))
                            openPath(h.path, true);
                    }
                    if (button("监测")) ImGui::OpenPopup("录制监测");
                    statsPopup(m_.store, h.task);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (button("关闭"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

  public:
    json captureState() const {
        std::vector<std::string> listening;
        for(auto &[key,rt]:m_.runtimes) if(rt->monitor) listening.push_back(key);
        return {{"listening",listening},{"planStart",editStart_},{"planEnd",editEnd_},{"followNow",followNow_},{"view", view_}, {"span", span_}, {"uiScale", uiScale_}, {"dpiScale", dpiScale_},
                {"timeline", captureInfo_}, {"plans", m_.plans.size()}, {"monitorVolume", m_.settings.monitorVolume}};
    }
    void captureInput(const std::string &panel, i64 elapsed) {
        if (panel.rfind("test-", 0) != 0 || captureInfo_.is_null()) return;
        // Deterministic application-input fixture, independent of desktop cursor/focus.
        auto &io = ImGui::GetIO();
        int stage = (int)(elapsed / 150);
        static int previous = -1;
        auto &t = captureInfo_;
        float x = t["x"].get<float>() + t["width"].get<float>()*.45f;
        float y = t["y"].get<float>() + t["height"].get<float>()*.3f;
        if (panel == "test-navigation" || panel == "test-volume") {
            auto key = panel == "test-navigation" ? "navigation" : "volume";
            if (!t.contains(key)) return;
            x = t[key][0].get<float>() + (t[key][2].get<float>()-t[key][0].get<float>())*.75f;
            y = (t[key][1].get<float>()+t[key][3].get<float>())/2;
        }
        if(panel=="test-listen") {x=(captureListen_.x+captureListen_.z)/2;y=(captureListen_.y+captureListen_.w)/2;}
        if(panel=="test-cancel-listen") {
            if(!t.contains("cancelListen")) return;
            x=(t["cancelListen"][0].get<float>()+t["cancelListen"][2].get<float>())/2;
            y=(t["cancelListen"][1].get<float>()+t["cancelListen"][3].get<float>())/2;
        }
        io.AddFocusEvent(true);
        if (panel == "test-drag") x += std::clamp(stage-7, 0, 10)*10.f;
        io.AddMousePosEvent(x, y);
        if (stage != previous) {
            if (panel == "test-wheel") { if (stage == 8) io.AddMouseWheelEvent(0, -1); }
            else {
                if (stage == 7) io.AddMouseButtonEvent(0, true);
                if (stage == (panel == "test-drag" ? 17 : 10)) io.AddMouseButtonEvent(0, false);
            }
            previous = stage;
        }
    }
    void resetScale() {
        previewScale_ = 0; editSettings_.uiScale = m_.settings.uiScale = 1.;
        try { m_.save(); } catch (const std::exception &e) { m_.log.write(e.what()); }
    }
    explicit UI(Manager &m, const std::string &capturePanel = {}, bool captureEnabled = false) : m_(m), captureEnabled_(captureEnabled) {
        view_ = nowMs() - 3600000;
        jumpDate_ = dateText(nowMs());
        captureStats_ = capturePanel == "monitor";
        if(capturePanel=="export") exportOpen_=true;
        if(capturePanel=="plan" && !m_.channels.empty()) createPlan(m_.channels.front(),nowMs(),nowMs()+3600000);
        if(capturePanel=="import") loadImport({{"version",1},{"settings",m_.settings},{"channels",m_.channels},{"plans",m_.plans}});
        if((capturePanel=="listening" || capturePanel=="test-cancel-listen") && !m_.channels.empty()) {auto &rt=m_.runtime(m_.channels.front());rt.monitor=true;rt.preview->monitor(true);}
        if (capturePanel == "channel") {
            editChannel_ = Channel{};
            editChannel_.profile = m_.settings.defaults;
            newChannel_ = true;
            channelOpen_ = true;
        } else if (capturePanel == "settings") {
            editSettings_ = m_.settings;
            settingsOpen_ = true;
        } else if (capturePanel == "history") historyOpen_ = true;
    }
    bool draw() {
        captureInfo_ = nullptr;
        bool quit = false;
        auto &io = ImGui::GetIO();
        if(followNow_) view_=nowMs()-span_/6;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("NDI Recorder", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored({.10f, .22f, .36f, 1}, "NDI RECORDER " APP_VERSION);
        ImGui::SameLine();
        ImGui::TextDisabled("多路定时收录   /   本地时间 UTC+8");
        if (ImGui::GetWindowWidth() > dp(720)) ImGui::SameLine(ImGui::GetWindowWidth() - dp(225));
        ImGui::TextUnformatted(dateText(nowMs(), true).c_str());
        if (button("+ 添加来源")) {
            editChannel_ = Channel{};
            editChannel_.profile = m_.settings.defaults;
            newChannel_ = true;
            channelOpen_ = true;
        }
        nextToolbar(120);
        if (button("基础设置")) {
            editSettings_ = m_.settings;
            settingsOpen_ = true;
        }
        nextToolbar(120);
        if (button("录制历史"))
            historyOpen_ = true;
        nextToolbar(120);
        if (button("导出配置")) exportOpen_=true;
        nextToolbar(120);
        if (button("导入配置")) {
            auto path=dialog(false);
            if(!path.empty()) try {loadImport(readJson(path));} catch(const std::exception &e){showError(e.what());}
        }
        nextToolbar(120);
        if (button("回到现在")) {view_ = nowMs() - span_ / 6;followNow_=true;}
        nextToolbar(120);
        if(ImGui::Checkbox("自动跟随", &followNow_) && followNow_) view_=nowMs()-span_/6;
        nextToolbar(120);
        if (button("小时"))
            span_ = 3600000;
        nextToolbar(120);
        if (button("日")) {
            span_ = 86400000;
            view_ = dayStart(nowMs()); followNow_=false;
        }
        nextToolbar(120);
        if (button("周")) {
            span_ = 7 * 86400000.;
            view_ = dayStart(nowMs()); followNow_=false;
        }
        nextToolbar(120);
        ImGui::SetNextItemWidth(dp(115));
        input("##jump", jumpDate_, 64);
        nextToolbar(120);
        if (button("跳转")) {
            auto t = parseDate(jumpDate_);
            if (t >= 0) {view_ = (double)t;followNow_=false;}
        }
        ImGui::Separator();
        float left = dp(530);
        float width = std::max(dp(360), ImGui::GetContentRegionAvail().x - left - ImGui::GetStyle().ScrollbarSize);
        ImGui::BeginChild("channels", {0, std::max(dp(160), ImGui::GetContentRegionAvail().y - dp(205))}, false, ImGuiWindowFlags_HorizontalScrollbar);
        auto origin = ImGui::GetCursorScreenPos();
        auto dl = ImGui::GetWindowDrawList();
        ImGui::TextDisabled("16:9 预览 / 最低带宽");
        ImGui::SameLine(dp(300));
        ImGui::TextDisabled("通道控制");
        double step = span_ <= 600000     ? 60000
                      : span_ <= 7200000  ? 600000
                      : span_ <= 86400000 ? 3600000
                                          : 86400000;
        for (double v = std::floor(view_ / step) * step; v < view_ + span_; v += step) {
            float x = origin.x + left + (float)((v - view_) / span_ * width);
            if (x < origin.x + left)
                continue;
            auto text = dateText((i64)v, true);
            text = span_ > 86400000 ? text.substr(5, 5) : text.substr(11, 5);
            dl->AddText({x + dp(3), origin.y}, IM_COL32(45, 49, 56, 255), text.c_str());
        }
        if (nowMs() - historyRefresh_ > 1500 || (historyDirty_ && nowMs()-historyRefresh_ > 150)) {
            historyRefresh_ = nowMs();
            historyDirty_ = false;
            range_ = m_.store.historyRange((i64)view_, (i64)(view_ + span_));
        }
        ImGui::Dummy({left + width, dp(1)});
        if (m_.channels.empty()) {
            ImGui::Dummy(dpSize(0, 60));
            ImGui::TextColored({.18f, .26f, .36f, 1}, "  添加第一路 NDI 来源，开始配置录制计划");
            ImGui::TextDisabled("  左侧实时预览；通过「计划」新建。时间轴左键拖动平移，滚轮缩放。");
        }
        for (auto &c : m_.channels) {
            ImGui::PushID(c.key.c_str());
            auto &r = m_.runtime(c);
            auto pos = ImGui::GetCursorScreenPos();
            preview(c, r, {pos.x, pos.y + dp(8)});
            auto draw = ImGui::GetWindowDrawList();
            const float controls = pos.x + dp(300);
            draw->PushClipRect({controls, pos.y}, {pos.x + left - dp(12), pos.y + dp(24)}, true);
            draw->AddText({controls, pos.y + dp(2)}, IM_COL32(18, 23, 30, 255), c.name.c_str());
            draw->PopClipRect();
            ImGui::SetCursorScreenPos({controls, pos.y + dp(28)});
            if (!r.manual) {
                if (button("立即录制", {100, 32})) m_.beginManual(c);
            } else {
                if (button("结束手动", {100, 32})) m_.stopManual(c);
            }
            ImGui::SameLine();
            if(r.monitor) {
                ImGui::PushStyleColor(ImGuiCol_Button,{.12f,.48f,.32f,1});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,{.10f,.56f,.36f,1});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,{.08f,.40f,.26f,1});
                ImGui::PushStyleColor(ImGuiCol_Text,{1,1,1,1});
            }
            bool wasMonitoring=r.monitor;
            if(button(r.monitor?"监听中###listen":"监听###listen",{100,32})) {
                bool enable=!r.monitor;cancelMonitoring();r.monitor=enable;r.preview->monitor(enable);
            }
            if(captureEnabled_ && c.key==m_.channels.front().key) {
                auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();captureListen_={a.x,a.y,b.x,b.y};
            }
            if(wasMonitoring) ImGui::PopStyleColor(4);
            ImGui::SetCursorScreenPos({controls,pos.y+dp(66)});
            if(button("计划…",{64,32})) ImGui::OpenPopup("plans-menu");
            if(ImGui::BeginPopup("plans-menu")) {
                if(ImGui::MenuItem("+ 新建计划")) createPlan(c,nowMs(),nowMs()+3600000);
                ImGui::Separator();
                for(auto &p:m_.plans) if(p.channel==c.key) {
                    auto label=p.name+(p.enabled?"":"（已停用）")+"##"+p.key;
                    if(ImGui::MenuItem(label.c_str())) planDialog(p);
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();if(button("音频",{64,32})) ImGui::OpenPopup("录制声音增益");
            audioPopup(c,r);
            ImGui::SameLine();
            if(r.blocked) {if(button("恢复",{64,32})) m_.resume(c);}
            else if(button("监测",{64,32})) ImGui::OpenPopup("录制监测");
            ImGui::SetCursorScreenPos({controls,pos.y+dp(104)});
            if(ImGui::Checkbox("计划启用",&c.enabled)) m_.save();
            ImGui::SameLine();
            if(button("设置",{64,32})){editChannel_=c;newChannel_=false;channelOpen_=true;}
            ImGui::SameLine();if(button("×",{32,32})) deleteKey_=c.key;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("删除通道（需要确认）");
            if (captureStats_) { ImGui::OpenPopup("录制监测"); captureStats_ = false; }
            statsPopup(m_.store, r.token, &r);
            std::string status =
                r.error.empty()
                    ? (r.process        ? (r.snapshot.state != 2 ? std::string(r.snapshot.message)
                                                                 : std::string(r.snapshot.encoder) + "  " +
                                                                std::to_string(r.snapshot.width) + "x" +
                                                                std::to_string(r.snapshot.height) + " / " +
                                                                std::to_string((int)r.snapshot.fps) + " fps")
                       : r.image.online ? "在线 · 等待计划"
                                        : "等待 NDI 信号")
                    : r.error;
            bool liveStats = r.process && r.snapshot.state == 2 && r.snapshot.stats.sampled &&
                             GetTickCount64() - r.snapshot.heartbeat < 3000;
            if (liveStats) {
                char summary[180];
                snprintf(summary, sizeof summary, "%.2f MB/s · 近60秒丢帧 V%llu/A%llu", r.snapshot.stats.recentMBps,
                         r.snapshot.recentStats.sdkVideo, r.snapshot.recentStats.sdkAudio);
                status = summary;
            }
            draw->PushClipRect({controls, pos.y + dp(140)}, {pos.x + left - dp(12), pos.y + dp(160)}, true);
            draw->AddText({controls, pos.y + dp(143)}, r.error.empty() ? IM_COL32(45,49,56,255) : IM_COL32(155,43,27,255), status.c_str());
            draw->PopClipRect();
            ImGui::SetCursorScreenPos({controls, pos.y + dp(140)});
            ImGui::InvisibleButton("channel-status", {left - dp(312), dp(20)});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", c.name.c_str(), status.c_str());
            timeline(c, r, {pos.x + left, pos.y}, width);
            ImGui::SetCursorScreenPos({pos.x, pos.y + dp(174)});
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::BeginChild("footer", {0, 0}, false, ImGuiWindowFlags_HorizontalScrollbar);
        if (view_ < navigationBase_ - 14*86400000. || view_ > navigationBase_ + 14*86400000.)
            navigationBase_ = (double)dayStart((i64)view_);
        float dayOffset = (float)((view_-navigationBase_) / 86400000.);
        ImGui::TextUnformatted("时间导航"); ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(dp(160), ImGui::GetContentRegionAvail().x-dp(255)));
        bool movedTime = ImGui::SliderFloat("##time-navigation", &dayOffset, -14, 14, "");
        if (!captureInfo_.is_null()) captureInfo_["navigation"] = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
        if (movedTime) {
            view_ = navigationBase_ + dayOffset * 86400000.; followNow_=false; historyDirty_ = true;
        }
        ImGui::SameLine();
        auto viewLabel=dateText((i64)view_,true).substr(5,11);
        if(span_<3600000) ImGui::Text("%s · %.0f 分钟",viewLabel.c_str(),span_/60000.);
        else ImGui::Text("%s · %.1f 小时",viewLabel.c_str(),span_/3600000.);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("可视范围起点：%s\n显示跨度，非累计运行时长；最大 14 天。",dateText((i64)view_,true).c_str());
        std::string monitoring = "未监听";
        bool anyMonitoring=false;
        for (auto &ch : m_.channels) if (m_.runtime(ch).monitor) { monitoring = ch.name; anyMonitoring=true; break; }
        ImGui::TextUnformatted("监听音量"); ImGui::SameLine();
        ImGui::SetNextItemWidth(dp(220));
        float volume = (float)(m_.settings.monitorVolume * 100);
        bool volumeChanged = ImGui::SliderFloat("##monitor-volume", &volume, 0, 400, "%.0f%%");
        bool saveVolume = ImGui::IsItemDeactivatedAfterEdit();
        if (!captureInfo_.is_null()) captureInfo_["volume"] = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
        ImGui::SameLine();
        if (button("－", {32, 32})) { volume = std::max(0.f, volume-25); volumeChanged = saveVolume = true; }
        ImGui::SameLine();
        if (button("＋", {32, 32})) { volume = std::min(400.f, volume+25); volumeChanged = saveVolume = true; }
        ImGui::SameLine();
        if (button("100%", {64, 32})) { volume = 100; volumeChanged = saveVolume = true; }
        if (volumeChanged) m_.settings.monitorVolume = std::isfinite(volume) ? std::clamp(volume/100., 0., 4.) : 1.;
        if (saveVolume) try { m_.save(); } catch (const std::exception &e) { showError(e.what()); }
        nextToolbar(130);ImGui::BeginDisabled(!anyMonitoring);
        if(button("取消全部监听",{125,32})) {cancelMonitoring();monitoring="未监听";}
        if(!captureInfo_.is_null()) captureInfo_["cancelListen"]={ImGui::GetItemRectMin().x,ImGui::GetItemRectMin().y,ImGui::GetItemRectMax().x,ImGui::GetItemRectMax().y};
        ImGui::EndDisabled();nextToolbar(200);
        auto shortName=wide(monitoring);if(shortName.size()>18) shortName=shortName.substr(0,18)+L"…";
        ImGui::Text("%s · 仅影响监听",utf8(shortName).c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",monitoring.c_str());
        auto &mt = m_.metrics;
        ImGui::Separator();
        ImGui::TextColored(mt.freeGB < m_.settings.reserveGB ? ImVec4(.75f, .18f, .22f, 1)
                                                             : ImVec4(.12f, .43f, .30f, 1),
                           "存储  %.1f / %.1f GB 可用", mt.freeGB, mt.totalGB);
        ImGui::SameLine();
        ImGui::Text("CPU %.0f%%    内存 %.1f / %.1f GB    主进程 %.0f MB", mt.cpu, mt.memoryGB,
                    mt.totalMemoryGB, mt.processMB);
        double writeMBps = 0;
        for (auto &[key, rt] : m_.runtimes)
            if (rt->process && rt->snapshot.state == 2 && GetTickCount64() - rt->snapshot.heartbeat < 3000)
                writeMBps += rt->snapshot.stats.recentMBps;
        ImGui::SameLine(); ImGui::Text("文件写入 %.2f MB/s", writeMBps);
        double mbps = 0;
        bool lossless = false;
        for (auto &c : m_.channels)
            if (m_.runtime(c).process) {
                mbps += c.profile.mbps + 0.192;
                lossless |= c.profile.codec == "ffv1";
            }
        ImGui::SameLine();
        if (mbps > 0 && !lossless)
            ImGui::TextDisabled("预计可录 %.1f 小时", std::max(0., mt.freeGB - m_.settings.reserveGB) *
                                                          1073741824 * 8 / (mbps * 1e6) / 3600);
        else
            ImGui::TextDisabled("预计可录 --");
        if (!mt.networks.empty()) {
            if (!networkChosen_) {
                for (int i = 0; i < (int)mt.networks.size(); i++)
                    if (mt.networks[i].up && mt.networks[i].speed > 0) {
                        networkIndex_ = i;
                        networkChosen_ = true;
                        break;
                    }
            }
            networkIndex_ = std::clamp(networkIndex_, 0, (int)mt.networks.size() - 1);
            auto &n = mt.networks[networkIndex_];
            ImGui::SetNextItemWidth(dp(300));
            if (ImGui::BeginCombo("网卡", n.name.c_str())) {
                for (int i = 0; i < (int)mt.networks.size(); i++)
                    if (ImGui::Selectable(mt.networks[i].name.c_str(), i == networkIndex_)) {
                        networkIndex_ = i;
                        networkChosen_ = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s   RX %.1f Mbps   TX %.1f Mbps   链路 %.0f Mbps",
                                n.up ? "已连接" : "未连接", n.rx, n.tx, n.speed);
        }
        ImGui::TextDisabled("左键拖动平移 · 滚轮缩放 · 双击计划编辑 · Ctrl+0 恢复缩放");
        const char *labels[] = {"计划", "正在录制", "已完成", "信号缺失", "失败 / 中断", "已恢复", "停用 / 错过"};
        ImU32 colors[] = {IM_COL32(41,102,177,255), stateColor("recording"), stateColor("completed"),
                          stateColor("no_video"), stateColor("failed"), stateColor("recovered"), stateColor("missed")};
        for (int i = 0; i < 7; ++i) {
            ImGui::SameLine();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(colors[i]), "■ %s", labels[i]);
        }
        ImGui::EndChild();
        configDialogs();
        popups();
        if (closeRequested_) {
            ImGui::OpenPopup("关闭程序");
            closeRequested_ = false;
        }
        if (ImGui::BeginPopupModal("关闭程序", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(m_.busy() ? "正在录制。退出将停止任务并完成文件封装。"
                                             : "退出后不再执行定时计划。");
            if (button("退出程序", {125, 0})) {
                quit = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (button("缩到系统托盘", {150, 0})) {
                hideWindow();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (button("取消", {90, 0}))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::End();
        return quit;
    }
};
int runUi(Manager &m, bool tray, const fs::path &capture, int autoExit, const std::string &capturePanel) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof wc,
                   CS_CLASSDC,
                   proc,
                   0,
                   0,
                   GetModuleHandleW(nullptr),
                   nullptr,
                   LoadCursor(nullptr, IDC_ARROW),
                   nullptr,
                   nullptr,
                   L"NDIRecorderWindow",
                   nullptr};
    wc.hIcon = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_RECORDER),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_RECORDER),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    RegisterClassExW(&wc);
    window_ = CreateWindowW(wc.lpszClassName, L"NDI Recorder " APP_VERSION_W L" · 定时收录", WS_OVERLAPPEDWINDOW, 100, 70, 1480,
                            920, nullptr, nullptr, wc.hInstance, nullptr);
    dpiScale_ = ImGui_ImplWin32_GetDpiScaleForHwnd(window_);
    RECT initial{100, 70, 100 + (LONG)(1480*dpiScale_), 70 + (LONG)(920*dpiScale_)};
    fitWindow(window_, &initial);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &swap_, &device_, &fl, &context_);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &swap_, &device_, &fl, &context_);
    if (FAILED(hr))
        throw std::runtime_error("D3D11 device initialization failed");
    makeTarget();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 17, nullptr,
                                 io.Fonts->GetGlyphRangesChineseFull());
    ImGui::StyleColorsLight();
    auto &s = ImGui::GetStyle();
    s.WindowPadding = {16, 14};
    s.FramePadding = {10, 7.5f};
    s.ItemSpacing = {9, 7};
    s.WindowRounding = s.ChildRounding = s.PopupRounding = s.FrameRounding = 0;
    s.GrabRounding = s.ScrollbarRounding = s.TabRounding = 0;
    s.FrameBorderSize = 1;
    s.Colors[ImGuiCol_WindowBg] = {1, 1, 1, 1};
    s.Colors[ImGuiCol_ChildBg] = {1, 1, 1, 1};
    s.Colors[ImGuiCol_PopupBg] = {1, 1, 1, 1};
    s.Colors[ImGuiCol_Text] = {.06f, .07f, .09f, 1};
    s.Colors[ImGuiCol_TextDisabled] = {.20f, .22f, .25f, 1};
    s.Colors[ImGuiCol_Border] = {.79f, .82f, .87f, 1};
    s.Colors[ImGuiCol_FrameBg] = {.97f, .98f, .99f, 1};
    s.Colors[ImGuiCol_FrameBgHovered] = {.91f, .95f, .99f, 1};
    s.Colors[ImGuiCol_FrameBgActive] = {.86f, .92f, .99f, 1};
    s.Colors[ImGuiCol_Button] = {.94f, .96f, .98f, 1};
    s.Colors[ImGuiCol_ButtonHovered] = {.85f, .91f, .98f, 1};
    s.Colors[ImGuiCol_ButtonActive] = {.74f, .84f, .96f, 1};
    s.Colors[ImGuiCol_Header] = {.85f, .92f, .99f, 1};
    s.Colors[ImGuiCol_HeaderHovered] = {.79f, .88f, .98f, 1};
    s.Colors[ImGuiCol_HeaderActive] = {.71f, .82f, .96f, 1};
    s.Colors[ImGuiCol_CheckMark] = {.12f, .37f, .68f, 1};
    s.Colors[ImGuiCol_Separator] = {.85f, .88f, .92f, 1};
    baselineStyle_ = s;
    uiScale_ = 0; previewScale_ = 0;
    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX11_Init(device_, context_);
    tray_.cbSize = sizeof tray_;
    tray_.hWnd = window_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = WM_APP + 1;
    tray_.hIcon = wc.hIconSm;
    wcscpy_s(tray_.szTip, L"NDI Recorder " APP_VERSION_W L" · 定时收录");
    Shell_NotifyIconW(NIM_ADD, &tray_);
    activeManager_ = &m;
    SetTimer(window_, 1, 20, nullptr);
    hidden_ = tray;
    if (!tray)
        ShowWindow(window_, SW_SHOWDEFAULT);
    UI ui(m, capture.empty() ? std::string{} : capturePanel, !capture.empty());
    bool done = false, saved = false, savedInitial = false;
    int captureDpiStage = -1;
    i64 start = nowMs(), lastTick = 0;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (resetScaleRequested_) { ui.resetScale(); resetScaleRequested_ = false; }
        bool visible = !hidden_ && !IsIconic(window_);
        if (nowMs() - lastTick >= 20) {
            m.tick(visible);
            lastTick = nowMs();
        }
        if (autoExit > 0 && nowMs() - start >= autoExit * 1000LL)
            done = true;
        if (!visible) {
            Sleep(20);
            continue;
        }
        if (!capture.empty() && capturePanel == "dpi-cycle") {
            int stage = std::min(4, (int)((nowMs()-start)/1500));
            if (stage != captureDpiStage) {
                const int dpi[] = {96, 120, 144, 192, 96};
                RECT rect; GetWindowRect(window_, &rect);
                float ratio = dpi[stage] / (96.f * dpiScale_);
                rect.right = rect.left + (LONG)((rect.right-rect.left)*ratio);
                rect.bottom = rect.top + (LONG)((rect.bottom-rect.top)*ratio);
                SendMessageW(window_, WM_DPICHANGED, MAKEWPARAM(dpi[stage], dpi[stage]), (LPARAM)&rect);
                captureDpiStage = stage;
            }
        }
        float scale = dpiScale_ * (float)(previewScale_ > 0 ? previewScale_ : m.settings.uiScale);
        if (std::abs(scale-uiScale_) > .001f) {
            uiScale_ = scale;
            s = baselineStyle_; // Always scale the original baseline, never the previous result.
            s.ScaleAllSizes(scale);
            s.FontScaleDpi = scale; // ImGui 1.92 rasterizes glyphs at the actual pixel size.
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (!capture.empty()) ui.captureInput(capturePanel, nowMs()-start);
        ImGui::NewFrame();
        if (ui.draw())
            done = true;
        if (!capture.empty() && !savedInitial && nowMs()-start > 500) {
            auto initial = capture; initial += L".initial.json";
            atomicJson(initial, ui.captureState()); savedInitial = true;
        }
        ImGui::Render();
        float clear[4] = {1, 1, 1, 1};
        context_->OMSetRenderTargets(1, &target_, nullptr);
        context_->ClearRenderTargetView(target_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!capture.empty() && capturePanel == "dpi-cycle" && captureDpiStage >= 0) {
            static int capturedStage = -1;
            if (capturedStage != captureDpiStage && nowMs()-start > captureDpiStage*1500+500) {
                auto file = capture; file += L".stage" + std::to_wstring(captureDpiStage) + L".bmp";
                screenshot(file); capturedStage = captureDpiStage;
            }
        }
        if (!capture.empty() && !saved && nowMs() - start > (capturePanel == "monitor" ? 12000 : 4000)) {
            screenshot(capture);
            auto stateFile = capture; stateFile += L".json";
            atomicJson(stateFile, ui.captureState());
            saved = true;
        }
        HRESULT present = swap_->Present(1, 0);
        if (present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) {
            m.log.write("D3D device lost; recording continues in tray");
            hideWindow();
        }
    }
    activeManager_ = nullptr;
    KillTimer(window_, 1);
    m.stopAll();
    Shell_NotifyIconW(NIM_DELETE, &tray_);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (target_)
        target_->Release();
    swap_->Release();
    context_->Release();
    device_->Release();
    DestroyWindow(window_);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return 0;
}
} // namespace nr
