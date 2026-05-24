# AudioSub

基于 `WebRTC-Native` 的语音转写与标注系统，当前仓库提供一个可运行的最小骨架，并围绕“多人协作、环境不统一”的现实前提，设计了基于预编译 `WebRTC SDK` 的团队开发方案。

## 目录结构

```text
AudioSub/
├── client/                         C++ 客户端主程序、WebRTC 接线
├── docs/                           协作、接入与规划文档
├── include/
│   └── audiosub/
│       ├── asr/                    ASR 模块头文件目录
│       ├── audio/                  音频模块头文件目录
│       ├── core/                   公共类型与接口头文件目录
│       ├── fusion/                 融合模块头文件目录
│       ├── mark/                   标注模块头文件目录
│       └── ui/                     展示模块头文件目录
├── scripts/
│   ├── bootstrap.ps1               一键准备环境 + 构建
│   ├── build.ps1                   构建入口
│   ├── fetch-webrtc-sdk.ps1        获取预编译 SDK
│   ├── pack-webrtc-sdk.ps1         维护者打包 SDK
│   ├── test_stage1a.py             基础信令验证
│   └── test_stage1b.py             WebRTC P2P 验证
├── signaling/                      Python 信令服务器
├── src/
│   ├── asr/                        ASR 模块实现
│   ├── audio/                      音频模块实现
│   ├── fusion/                     融合模块目录
│   ├── mark/                       标注模块目录
│   └── ui/                         展示模块目录
├── third_party/
│   ├── nlohmann/json.hpp
│   └── webrtc-sdk/                 本地生成，默认不入库
├── CMakeLists.txt
└── README.md
```

## 项目目标

课题目标分两层：

- `P0` 主链路：A 端发送音频，B 端获取 `PCM`，完成音频处理、本地 `ASR`、实时字幕输出
- `P1` 增强链路：A 端通过 `DataChannel` 发送标注，B 端完成字幕与标注的时间对齐和融合

当前仓库重点先完成了网络和协作基础设施，让团队可以在不统一 `WebRTC` 源码环境的情况下继续推进后续开发。

## 当前状态

- [x] 阶段 1a：Python 信令服务器 + C++ TCP 信令客户端
- [x] 阶段 1b：WebRTC `PeerConnection` + `DataChannel` 文字 P2P
- [ ] 阶段 2：A 端麦克风采集，B 端获取远端 `PCM`
- [ ] 阶段 3：接入本地 `ASR`，输出实时字幕
- [ ] 阶段 4：标注消息、字幕融合、指标观测

## 核心思路

- 维护者统一发布预编译 `WebRTC SDK`
- 协作者不需要拉取 `webrtc/src`，也不需要自己编译 `WebRTC`
- 项目后续按“薄适配层 + 可独立开发的核心模块”推进
- 大多数同学优先在 `mock/replay` 模式下开发音频处理、`ASR`、标注和融合逻辑

## 仓库包含什么

| 已在 Git 中 | 不在 Git 中 |
|-------------|-------------|
| C++ 客户端源码 | `third_party/webrtc-sdk/` |
| Python 信令服务器 | `webrtc.lib` |
| CMake 与 PowerShell 脚本 | WebRTC 头文件树 |
| `nlohmann/json.hpp` | `webrtc/src` 源码树 |
| 协作文档 | `build/` 编译产物 |

## 协作者快速开始

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022 或 Build Tools 2022
- 安装 `Desktop development with C++`
- Python 3.8+
- Git

补充说明：

- 不需要 `depot_tools`
- 不需要本地 `webrtc/src`
- 不需要自己编译 `WebRTC`
- `scripts/build.ps1` 会优先尝试自动发现 Visual Studio 自带的 `cmake.exe`

### 一键准备与编译

```powershell
git clone https://github.com/NJUPTzza/AudioSub.git
cd AudioSub
.\scripts\bootstrap.ps1 -Url "https://github.com/NJUPTzza/AudioSub/releases/download/webrtc-sdk-v1.0.0/webrtc-sdk-win-x64-release-771e6489c9.zip"
```

如果维护者已经把默认 SDK 地址写进了 [scripts/fetch-webrtc-sdk.ps1](scripts/fetch-webrtc-sdk.ps1)，则直接运行：

```powershell
.\scripts\bootstrap.ps1
```

### 运行 Demo

开 3 个终端：

```powershell
python signaling\server.py
.\build\client\Release\audiosub_client.exe --id A
.\build\client\Release\audiosub_client.exe --id B
```

当两端看到 `dc:open` 后，任一端输入文字并回车，另一端应显示 `<peer> ...`。

## 文档导航

- 协作与模块规划: [docs/collaboration-plan.md](docs/collaboration-plan.md)
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

## 自动化测试

基础信令验证：

```powershell
python .\scripts\test_stage1a.py
```

WebRTC P2P 验证：

```powershell
python .\scripts\test_stage1b.py
```

## 架构概览

```text
Client A  ──TCP/JSON──► Signaling Server ◄──TCP/JSON── Client B
    │                                              │
    └──────────── WebRTC P2P DataChannel ──────────┘
                  （文字/音频后续都走这里）
```

## 常见问题

**Q: clone 后编译报 `webrtc.lib not found`？**  
A: 先运行 `.\scripts\bootstrap.ps1`，或显式执行 `fetch-webrtc-sdk.ps1` 获取 SDK。

**Q: 能不能把 `webrtc.lib` 直接提交到 Git？**  
A: 不推荐。库体积较大，仓库历史会迅速膨胀。当前默认采用 `GitHub Releases` 分发。

**Q: SDK 和源码版本不一致会怎样？**  
A: 可能导致链接失败或运行时异常。请以 `third_party/webrtc-sdk/VERSION.txt` 为准，换 SDK 后建议重新构建。

**Q: 为什么别人电脑不需要 WebRTC 源码环境？**  
A: 因为团队统一消费预编译 SDK，只有维护者负责 `WebRTC` 源码拉取、编译与打包。
