# AudioSub

基于 `WebRTC-Native` 的实时语音转写与标注系统。

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
│       ├── fusion/                 融合模块头文件目录（P1 占位）
│       ├── mark/                   标注模块头文件目录（P1 占位）
│       └── ui/                     展示模块头文件目录
├── models/                         whisper 模型文件
├── scripts/
│   ├── bootstrap.ps1               一键准备环境 + 构建
│   ├── build.ps1                   构建入口
│   ├── fetch-webrtc-sdk.ps1        获取预编译 SDK
│   └── pack-webrtc-sdk.ps1         维护者打包 SDK
├── signaling/                      Python 信令服务器
├── src/
│   ├── asr/                        ASR 模块实现
│   ├── audio/                      音频模块实现
│   ├── fusion/                     融合模块目录（P1 占位）
│   ├── mark/                       标注模块目录（P1 占位）
│   └── ui/                         展示模块目录
├── third_party/
│   ├── nlohmann/json.hpp
│   ├── whisper.cpp/                whisper.cpp 子模块
│   └── webrtc-sdk/                 本地生成，默认不入库
├── CMakeLists.txt
└── README.md
```

## 项目目标

- `P0` 主链路：A 端发送音频，B 端获取 `PCM`，完成音频处理、本地 `ASR`、实时字幕输出
- `P1` 增强链路：A 端通过 `DataChannel` 发送标注，B 端完成字幕与标注的时间对齐和融合

## 当前状态

- [x] 阶段 1a：Python 信令服务器 + C++ TCP 信令客户端
- [x] 阶段 1b：WebRTC `PeerConnection` + `DataChannel` 文字 P2P
- [x] 阶段 2：A 端麦克风采集，B 端获取远端 `PCM`
- [x] 阶段 3：音频处理（重采样 + 声道转换）
- [x] 阶段 4：接入 `whisper.cpp`，实时语音转写
- [x] 阶段 5：端到端接线与验证
- [ ] 阶段 6（P1）：标注通道
- [ ] 阶段 7（P1）：字幕与标注融合

## 数据流

```text
A端: 麦克风 → ADM → AudioSource → AudioTrack → PeerConnection
                                                         ↓ P2P
B端: PeerConnection → AudioTrack → RemoteAudioSink::OnData()
         → PcmFrame(48kHz/stereo) → PcmRingBuffer
         → AudioPipeline(48kHz→16kHz, stereo→mono)
         → WhisperASREngine(工作线程, 5秒块式识别)
         → SubtitleSegment → ConsoleSubtitleConsumer(控制台)
```

## 快速开始

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022 或 Build Tools 2022
- 安装 `Desktop development with C++`
- Python 3.8+
- Git

### 一键准备与编译

```powershell
git clone https://github.com/NJUPTzza/AudioSub.git
cd AudioSub
.\scripts\bootstrap.ps1
```

### 下载 whisper 模型

```powershell
Invoke-WebRequest -Uri "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin" -OutFile "models\ggml-small.bin"
```

### 运行

开 3 个终端：

```powershell
# 终端 1：信令服务器
python signaling\server.py

# 终端 2：A 端（麦克风采集）
.\build\client\Release\audiosub_client.exe --id A

# 终端 3：B 端（音频接收 + ASR 字幕）
.\build\client\Release\audiosub_client.exe --id B --lang zh
```

B 端连接后对麦克风说话，控制台将实时输出字幕。

输入 `/quit` 退出，A 端退出后 B 端也会自动退出。

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

## 架构概览

```text
Client A  ──TCP/JSON──► Signaling Server ◄──TCP/JSON── Client B
    │                                              │
    └──────────── WebRTC P2P DataChannel ──────────┘
                  （音频走 AudioTrack，标注走 DataChannel）
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
