# NDI Recorder

**中文** | [English](README_EN.md)

Windows x64 桌面多路 NDI 定时录制程序：发现网络中的 NDI 源，按每路通道独立参数录制，支持定时计划、自动分段、断流补帧、实时监测与 7×24 小时长期运行诊断。

基于 C++20 / CMake / Win32 / D3D11 / Dear ImGui / FFmpeg / SQLite。

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011%20x64-blue)](docs/ACCEPTANCE.md)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)](CMakeLists.txt)
[![Release](https://img.shields.io/badge/release-v20261002-green)](CMakeLists.txt)

![主界面](docs/screenshots/main-window.png)

## 功能特性

### 多路通道与来源

- 自动发现局域网 NDI 源，也可粘贴完整源名（如 `HOST (Camera 1)`）或配置额外发现 IP
- 每路通道独立设置：输出编码、目标码率、分辨率、帧率、声道对、分段时长
- 空闲预览请求 NDI 最低带宽流（上限 15 fps），录制前约 5 秒才预热完整接收器，减轻网络负担
- 每路 16:9 黑边预览与双声道电平表

### 录制与编码

- **H.264 / MP4**：默认 20 Mbps；自动试编码选择 NVENC → QSV → AMF → Media Foundation；1080i50 自动转 1080p50；碎片化封装，分段在关键帧处完成
- **FFV1 / MKV 无损**：保存 NDI SDK 解码后的像素与浮点音频，并生成逐帧 `.checksums.json` 校验摘要
- **延时摄影**：N 倍快放或每隔 S 秒取一帧（仅 H.264，无音轨）
- **自动分段**：默认 60 分钟一段（1–1440 可调），按 `通道名/日期/起止时间_任务ID_段号` 组织，绝不覆盖已有文件
- **断流容错**：视频缺失补黑帧、音频缺失补静音，信号恢复后继续写入并登记缺失区间；源规格变化时有限重启任务
- **时钟校正**：源时间码与接收时钟持续漂移自动校正；连续补黑超过 10 秒交由恢复策略重启工作进程

### 定时计划与时间轴

- 单次 / 每天 / 每周指定星期计划，支持跳过单轮、停用、跨午夜；同路计划重叠校验
- 「立即录制」手动需求与计划需求可叠加互不影响
- 合并时间轴：上半计划、下半实际，拖动平移、滚轮缩放（1 分钟至 14 天）、小时/日/周视图、日期跳转、自动跟随当前时刻
- 状态配色：蓝=计划、红=录制、绿=完成、琥珀=信号缺失、紫=失败/中断、青=已恢复、灰=停用/错过
- 程序重启后仍在有效期内的计划自动继续，错过时段记为 missed

### 音频

- 每路录制增益 -24～+24 dB 实时调整（+6 dB 约两倍振幅），录制中可调
- 监听音量 0～400% 独立控制，只影响本机播放，不改变录制文件
- 监听同时仅一路、默认静音，跟随所选声道对与任务参数快照
- H.264 按 NDI SMPTE 参考电平 -20 dB 换算（EBU 可设 -14 dB）；FFV1 与延时摄影不施加增益

### 监测与现场诊断

- 每路「监测」：SDK 丢弃、接收队列溢出、处理缓存溢出、场配对失败、补黑/补静音计数与文件写入速度，不新增接收器、不播放声音
- 实时窗口（最近 60 秒）与任务累计两种视图；录制历史中可查看同一任务全部分段汇总
- 内置 24 小时诊断采集：`Start-24h-Debug.cmd` 定时采样进程 CPU/内存/句柄/I/O 与磁盘空间，在线备份数据库
- 离线录像检查：`Inspect-Recording.cmd` 完整解码检测黑场、静音区间与解码错误

### 可靠性与工程

- 主进程不负责编码：每路录制由独立工作进程执行，崩溃自动重试（2/4/8 秒退避，最多 3 次）
- 工作进程加入 kill-on-close Job Object，主进程退出不留孤儿进程
- 全链路有界缓冲：接收队列、视频/音频缓存、日志队列均有上限，编码持续落后自动止损
- 磁盘安全余量保护（默认 10 GB），不足时停止受影响录制，绝不自动删除录像
- SQLite（WAL + FULL 同步）保存配置与历史；配置导出/导入支持选择性合并与冲突检查
- Per-Monitor V2 高 DPI，软件缩放 80%–150% 叠加系统缩放；Ctrl+0 / Shift 启动 / `--reset-ui-scale` 一键恢复
- 单实例运行、系统托盘、登录自启可选

## 下载与运行

从 Releases 下载 `ndi-recorder-vYYYYMMDD-windows-x64.zip`，**完整解压**后运行 `ndi-recorder.exe`，同目录 DLL 必须保留。需要 Windows 10/11 x64 与 NDI 运行环境。

配置与历史保存在 exe 同目录 `data/recorder.db`，默认录像目录为 `recordings/`，请使用可写目录。升级时退出旧程序、备份 `data/`，再用新版程序和 DLL 覆盖即可，不要混用新旧 DLL。

基本流程：**添加来源 → 选择 NDI 源 → 设置通道参数 → 保存 → 立即录制或建立计划**。详见 [使用说明](docs/USER_GUIDE.md)。

## 从源码构建

需要 Visual Studio 2022（C++ 桌面开发）、Windows SDK、CMake 3.24+，以及按 [构建依赖](docs/BUILDING.md) 准备的 vendor 目录（NDI SDK、FFmpeg、Dear ImGui、SQLite、nlohmann JSON）。第三方依赖不随源码仓库分发。

```powershell
# 一步完成 Release 构建、回归测试、安装目录及 ZIP 打包
powershell -NoProfile -ExecutionPolicy Bypass -File tools/pack.ps1

# 或分步执行
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel 6
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/ndi-recorder-windows-x64
cpack --config build/CPackConfig.cmake -C Release
```

版本号为 `vYYYYMMDD` 日期格式，发布时修改 `CMakeLists.txt` 中的 `APP_RELEASE_DATE`，统一生成窗口标题、EXE 版本资源、manifest 与 ZIP 名称。

## 项目结构

```text
src/         # 主程序：core（模型/计划/SQLite）、ndi（SDK 对接）、media（编码封装）、manager（工作进程管理）、ui（界面）
tests/       # core / manager 单元测试与音画同步检查器
tools/       # 构建打包脚本与冒烟/诊断工具
docs/        # 使用说明、构建依赖、架构、验收记录
assets/      # 应用图标
vendor/      # 第三方依赖（不入库，见 docs/BUILDING.md）
```

## 文档

- [使用说明](docs/USER_GUIDE.md) — 安装、界面、通道、计划、音频、监测、诊断与排障
- [构建依赖](docs/BUILDING.md) — 依赖版本、校验与完整构建打包流程
- [架构](docs/ARCHITECTURE.md) — 进程模型、媒体链路、有界资源与失败策略
- [验收记录](docs/ACCEPTANCE.md) — 已通过测试、证据与尚未覆盖范围
- [第三方依赖许可](THIRD_PARTY_NOTICES.md)

## 当前状态

0.1 开发预览版（v20261002）。本机 Windows 10 上的构建、单元/集成/界面回归已通过，详见 [验收记录](docs/ACCEPTANCE.md)；仍需目标硬件上的四路 24/72 小时压力测试、真实设备多声道/场序验收与 Windows 11 实机验证。本版不提供 H.265、Windows 服务或多机 genlock。

## 许可证

本项目代码以 [MIT License](LICENSE) 发布。第三方组件保留各自许可，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。NDI 是 Vizrt NDI AB 的注册商标，本项目不是 NDI 官方或认证产品。
