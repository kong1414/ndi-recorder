# 构建与打包

Windows x64，Visual Studio 2022 C++ 桌面工具，Windows SDK，CMake 3.24+。本机使用 MSVC 19.44 / SDK 10.0.26100.0。

## 依赖布局
vendor/imgui：Dear ImGui v1.92.9b（含 Win32/D3D11 backend）
vendor/ndi/include 与 vendor/ndi/bin/Processing.NDI.Lib.x64.dll：NDI 6 SDK，已测试本机 6.3.2
vendor/ffmpeg/include、lib、bin：FFmpeg LGPL shared Windows x64 开发包
vendor/sqlite/sqlite3.c、sqlite3.h：SQLite amalgamation 3.53.4
vendor/json.hpp：nlohmann JSON 3.12.0

依赖未写入源代码仓库。tools/dependencies.json 记录本次依赖版本和压缩包校验值；NDI 请从已安装官方 SDK 复制。不要混用不同版本的 FFmpeg 头文件、导入库和 DLL。

FFmpeg 本机版本 n8.1.2-54-gc573a95381-20260919，来自 BtbN LGPL shared 构建。
原始下载 URL 使用 latest，现有 SHA256 锁定本次归档；今后 URL 内容改变时必须显式更新依赖并重新验收，不能静默替换。
NDI runtime 是动态加载；FFmpeg shared DLL 是动态链接，不能仅携带 ffmpeg.exe。

## 构建
    powershell -ExecutionPolicy Bypass -File tools/build.ps1

等价命令：
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release --parallel 6
    ctest --test-dir build -C Release --output-on-failure

输出 build/Release/ndi-recorder.exe。程序运行时 Windows 会锁定该 exe，请退出程序再覆盖构建；测试脚本可使用单独复制的 ndi-test-runner.exe。

## 集成测试
    Copy-Item build/Release/ndi-recorder.exe build/Release/ndi-test-runner.exe
    python tools/integration-smoke.py --long

脚本只创建和关闭自己持有的测试源及录制进程。报告写入 build/acceptance。
单独探测编码器：
    build/Release/ndi-recorder.exe --probe-encoders build/encoders.json

内置测试源：
    build/Release/ndi-recorder.exe --test-source MyTest 50 60
50 表示 1080i50，60 表示 1080p60；最后参数为发送秒数。
源包含每秒一次、秒内位置按确定性散列变化的闪光、移动条和同步 1 kHz 脉冲音，可附加断流开始秒数和断流持续秒数。

## 打包
    powershell -ExecutionPolicy Bypass -File tools/pack.ps1

将 exe、必要共享 DLL、README 和依赖许可复制到 dist/ndi-recorder-windows-x64，不复制测试录像和用户配置。
打包不修改已存在的 data/ 或 recordings/。发布前请完成真实源、目标硬件和长时间验收。


新增回归：CTest 包含 manager 和 sync-checker；`python tools/recovery-smoke.py` 检查首帧晚到及无源到期。同步测试要求至少三次一对一配对，不能忽略未匹配事件；新测试源使用非固定秒内相位以识别整秒错位。旧的严格周期源仍不能排除整秒歧义。

2026-09-30：打包统一使用 CMake install 清单与 CPack ZIP。`tools/pack.ps1` 默认先构建并测试，`-SkipBuild` 跳过已完成的构建。发布目录为 `dist/ndi-recorder-windows-x64/`，ZIP 为 `dist/ndi-recorder-<版本>-windows-x64.zip`，完整命令和升级说明见根目录 README.md。
