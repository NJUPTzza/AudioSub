# AudioSub

基于 `WebRTC-Native` 的实时语音转写与标注系统。

A 端讲话，音频经 `WebRTC` P2P 传到 B 端；B 端用本地 `whisper.cpp` 做 `ASR` 生成字幕，并把 A 端通过 `DataChannel` 发来的标注与字幕按统一时间轴对齐融合，再将增强字幕回传给 A 端。系统提供命令行（CLI）与 `Qt` 图形界面（飞书风格聊天）两套前端，二者共用同一核心引擎。

## 功能特性

- `P0` 主链路：`WebRTC` 音频传输 + B 端本地 `ASR`（`whisper.cpp`）+ 实时字幕
- `P1` 标注通道：A 端经 `DataChannel` 发送结构化标注（`seq` / `event_time_ms` / `payload.text`），支持按 `seq` 去重
- `P1` 字幕与标注融合：统一时间轴对齐，输出「字幕正文 / 时间范围 / 对应标注」；未对齐标注单独兜底展示
- 增强项：B → A 字幕回传
- 指标观测：端到端字幕延迟、标注匹配误差、`DataChannel` 标注可见延迟；退出时汇总均值 / 峰值
- 双前端：CLI 与 `Qt` GUI 共用核心引擎，经 C ABI DLL 跨越 `/MT`（WebRTC）与 `/MD`（Qt）两套运行时

## 目录结构

```text
AudioSub/
├── client/
│   ├── audiosub_engine.{h,cc}      核心引擎：WebRTC/信令/ASR/融合接线，对外抛事件回调
│   ├── peer_connection_client.*    WebRTC PeerConnection 封装
│   ├── signaling_client.*          TCP + 行 JSON 信令客户端
│   ├── wasapi_mic_capture.*        WASAPI 麦克风采集（备用音频链路）
│   ├── main.cc                     CLI 外壳，链接核心引擎
│   └── capi/                       C ABI DLL（audiosub_capi），供 GUI 动态调用
├── gui/                            Qt Widgets 前端（audiosub_gui，飞书风格聊天）
├── include/
│   ├── asr/ audio/ core/           公共头文件
│   ├── fusion/                     字幕-标注融合器（header-only）
│   └── proto/                      DataChannel 消息协议
├── src/
│   ├── asr/                        whisper.cpp 引擎实现
│   └── audio/                      PCM 环形缓冲、ASR 音频格式转换
├── signaling/                      Python 信令服务器
├── scripts/                        构建 / SDK 获取 / 自动化测试脚本
├── docs/                           协作、接入与规划文档
├── third_party/
│   ├── nlohmann/json.hpp
│   ├── whisper.cpp/                git 子模块
│   └── webrtc-sdk/                 预编译 WebRTC SDK（本地生成，默认不入库）
├── CMakeLists.txt
└── README.md
```

## 当前状态

- [x] 阶段 1：Python 信令服务器 + WebRTC `PeerConnection` / `DataChannel` 文字 P2P
- [x] 阶段 2：A 端麦克风采集，B 端获取远端 `PCM`
- [x] 阶段 3：接入本地 `ASR`（`whisper.cpp`），输出实时字幕
- [x] 阶段 4：标注通道、字幕与标注融合、指标观测与退出汇总
- [x] 阶段 5：`Qt` 图形界面（飞书风格聊天）

## 架构概览

```text
A 讲话 ──WebRTC 音频──► B 收到 PCM ──► whisper.cpp ASR ──► 字幕
A 标注 ──DataChannel──► B 时间轴对齐融合 ──► 增强字幕 ──B→A 回传──► A

                 ┌── audiosub_client.exe   (CLI, /MT)
audiosub_engine ─┤
   核心引擎       └── audiosub_capi.dll (C ABI) ── audiosub_gui.exe (Qt, /MD)
```

## 协作者快速开始

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022 或 Build Tools 2022（安装 `Desktop development with C++`）
- Python 3.8+（运行信令服务器）
- Git
- 可选：`Qt 6`（`MSVC 2022 64-bit` 组件）—— 仅在需要构建 GUI 时安装

补充说明：

- 不需要 `depot_tools`、不需要本地 `webrtc/src`、不需要自己编译 `WebRTC`
- `scripts/build.ps1` 会优先尝试自动发现 Visual Studio 自带的 `cmake.exe`

### 拉取代码与依赖

```powershell
git clone --recurse-submodules https://github.com/NJUPTzza/AudioSub.git
cd AudioSub
# 若已 clone 但忘了子模块：
git submodule update --init --recursive
```

获取预编译 WebRTC SDK（与 `bootstrap` 一并完成环境准备 + 构建）：

```powershell
.\scripts\bootstrap.ps1 -Url "https://github.com/NJUPTzza/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release-771e6489c9.zip"
```

如果默认 SDK 地址已写进 [scripts/fetch-webrtc-sdk.ps1](scripts/fetch-webrtc-sdk.ps1)，直接运行：

```powershell
.\scripts\bootstrap.ps1
```

### ASR 模型

B 端 `ASR` 默认加载 `third_party/whisper.cpp/models/ggml-small.bin`。请自行下载对应模型放到该路径（whisper.cpp 提供下载脚本，或从其模型发布页获取）。

### 构建

```powershell
.\scripts\build.ps1 -Config Release
```

- GUI 默认开启（`AUDIOSUB_BUILD_GUI=ON`），需在顶层 `CMakeLists.txt` 或命令行通过 `QT_PREFIX_PATH` 指定 Qt 安装前缀（默认 `E:/Qt/6.11.1/msvc2022_64`）。
- 不需要 GUI 时可关闭，只编 CLI：

```powershell
cmake -S . -B build -DAUDIOSUB_BUILD_GUI=OFF
cmake --build build --config Release
```

### 运行

先起信令服务器：

```powershell
python signaling\server.py
```

命令行前端（两个终端，A 讲话方 / B 接收方）：

```powershell
.\build\client\Release\audiosub_client.exe --id A
.\build\client\Release\audiosub_client.exe --id B
```

CLI 命令：`/talk on|off` 开关麦克风，`/note <文本>` 发送标注，`/quit` 退出。

> 音频链路：默认走 `--audio-path webrtc`（WebRTC ADM 采集 + 内建 3A + Opus，经 RTP 媒体流传输，CLI 与 GUI 均为此默认）。另有对照链路 `--audio-path wasapi`（WASAPI 直采 raw PCM、绕开 3A，经 DataChannel 二进制帧传输），用于规避同机调试时降噪削弱人声导致的 ASR 幻觉。

图形界面前端：

```powershell
.\build\gui\Release\audiosub_gui.exe --id A
.\build\gui\Release\audiosub_gui.exe --id B
```

A 端点「开始说话」，在输入框回车发标注；两端以聊天气泡形式查看字幕与标注，顶部指标条实时刷新。

## 指标观测

| 指标 | 目标 | 含义 |
|------|------|------|
| 端到端字幕延迟 | ≤ 1500ms | 音频到字幕的推理耗时 |
| 标注匹配误差 | ≤ 500ms | 标注时刻到字幕时间窗的距离 |
| DataChannel 可见延迟 | ≤ 300ms | 标注从发出到对端收到的时延（接收方侧才有样本）|

目标值用于观测评估，CLI 退出时打印均值 / 峰值汇总，GUI 在指标条实时显示。

## 仓库包含什么

| 已在 Git 中 | 不在 Git 中 |
|-------------|-------------|
| C++ 客户端 / 引擎 / C ABI DLL 源码 | `third_party/webrtc-sdk/`（含 `webrtc.lib`、头文件树）|
| Qt GUI 源码 | `build/` 编译产物 |
| Python 信令服务器 | ASR 模型 `*.bin` |
| CMake 与 PowerShell 脚本 | |
| `nlohmann/json.hpp`、协作文档 | |

## 自动化测试

WebRTC P2P 链路验证（信令握手 + DataChannel 双向文字）：

```powershell
python .\scripts\test_stage1b.py
```

## 文档导航

- 链路详解（流程图 + 逐环节说明）: [docs/pipeline-deep-dive.md](docs/pipeline-deep-dive.md)
- 当前实现链路总览（速查）: [docs/current-implementation-flow.md](docs/current-implementation-flow.md)
- 新同学入组说明: [docs/new-collaborator-setup.md](docs/new-collaborator-setup.md)
- 发群消息模板: [docs/team-message-template.md](docs/team-message-template.md)

## 维护者工作流

### 发布 WebRTC SDK

在维护者机器已编译好 `E:\webrtc\src\out\Release\obj\webrtc.lib` 后执行：

```powershell
.\scripts\pack-webrtc-sdk.ps1 -Zip
```

之后：

- 将 zip 上传到 `GitHub Releases`
- 把下载地址发给协作者，或写入 [scripts/fetch-webrtc-sdk.ps1](scripts/fetch-webrtc-sdk.ps1) 的 `$DefaultReleaseUrl`
- 用 `VERSION.txt` 锁定 `WebRTC commit` 和 `gn args`

推荐命名：

```text
Tag:   webrtc-sdk-v1.0.0
Asset: webrtc-sdk-win-x64-release-<commit>.zip
```

### WebRTC 编译参数

```text
is_debug=false
is_clang=true
use_custom_libcxx=false
is_component_build=false
rtc_use_h264=true
ffmpeg_branding="Chrome"
rtc_include_tests=false
rtc_build_examples=false
treat_warnings_as_errors=false
proprietary_codecs=true
rtc_enable_protobuf=false
target_cpu="x64"
```

## 常见问题

**Q: clone 后编译报 `webrtc.lib not found`？**  
A: 先运行 `.\scripts\bootstrap.ps1`，或显式执行 `fetch-webrtc-sdk.ps1` 获取 SDK。

**Q: GUI 构建失败 / 找不到 Qt？**  
A: 确认已安装 `Qt 6`（`MSVC 2022 64-bit`），并把 `QT_PREFIX_PATH` 指向你的 Qt 安装目录；或用 `-DAUDIOSUB_BUILD_GUI=OFF` 只编 CLI。

**Q: 启动后没有字幕？**  
A: 检查 `third_party/whisper.cpp/models/ggml-small.bin` 模型是否就位，且 A 端已「开始说话」。

**Q: SDK 和源码版本不一致会怎样？**  
A: 可能导致链接失败或运行时异常。请以 `third_party/webrtc-sdk/VERSION.txt` 为准，换 SDK 后建议重新构建。
