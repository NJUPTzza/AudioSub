# AudioSub 协作方案

## 1. 目标与约束

本项目需要完成一个基于 `WebRTC-Native` 的实时语音转写与标注系统，任务要求可以拆成两层：

- `P0` 主链路：`A -> B` 音频传输、`B` 端获取 `PCM`、音频处理、本地 `ASR`、实时字幕输出
- `P1` 增强链路：`A -> B` 标注消息、字幕与标注对齐融合、双端展示与指标观测

当前团队存在两个现实约束：

- 只有少数成员具备拉取和编译 `WebRTC` 源码的条件
- 其他成员本地开发环境难以完全统一

因此协作目标不是“让所有人都能编译 WebRTC 源码”，而是：

- 用统一的预编译 `WebRTC SDK` 作为基础依赖
- 把强依赖 `WebRTC` 的代码收敛到很薄的一层
- 让大多数业务逻辑可以在 `mock/replay` 模式下独立开发和测试

## 2. 当前仓库现状

仓库已经完成了第一阶段的基础能力：

- `signaling/` 已有 Python 信令服务器
- `client/signaling_client.*` 已有 TCP + JSON 信令客户端
- `client/peer_connection_client.*` 已有 `PeerConnection + DataChannel` 封装
- `scripts/fetch-webrtc-sdk.ps1` 已支持协作者下载统一 SDK
- `scripts/pack-webrtc-sdk.ps1` 已支持维护者从本地 `webrtc/src` 打包 SDK

也就是说，项目已经具备了“由维护者提供二进制 SDK，协作者直接消费”的基础条件。

## 3. 总体架构

建议把项目拆成四层：

### 3.1 Adapter 层

职责：对接外部依赖，不承载核心业务。

- `webrtc_adapter`
  - 建连
  - 远端音频接收
  - `DataChannel` 收发
- `signaling_adapter`
  - TCP/JSON 信令
- `asr_adapter`
  - `whisper.cpp` 或同类离线模型接入

### 3.2 Core 层

职责：项目核心逻辑，尽量不直接依赖 `WebRTC`。

- `audio_buffer`
  - `RingBuffer`
  - 帧队列
  - 背压和丢帧策略
- `audio_pipeline`
  - 重采样
  - 声道转换
  - 统一成 `ASR` 输入格式
- `subtitle_engine`
  - 字幕片段结构
  - 分段组织
  - 输出策略
- `mark_protocol`
  - 标注消息结构
  - 序列号去重
  - 基础校验
- `fusion_engine`
  - 字幕与标注按时间轴对齐
  - 输出增强字幕
- `metrics`
  - 端到端延迟
  - 标注延迟
  - 匹配误差

### 3.3 App 层

职责：把各模块组装成可运行程序。

- `sender_app`
  - A 端
  - 采集音频或回放音频
  - 发送标注
- `receiver_app`
  - B 端
  - 接收音频
  - 执行转写
  - 展示字幕与融合结果
- `replay_tool`
  - 不走 WebRTC，直接用本地音频驱动后半链路

### 3.4 Scripts 层

职责：降低环境门槛，统一开发入口。

- `bootstrap.ps1`
  - 检查环境
  - 下载 `WebRTC SDK`
  - 下载 `ASR` 模型
- `build.ps1`
  - 统一构建入口
- `test_*.ps1` / `test_*.py`
  - 自动化验证脚本

## 4. 数据流设计

### 4.1 P0 主链路

```text
Mic / WAV
  -> A 端 AudioSource
  -> WebRTC AudioTrack
  -> 网络传输
  -> B 端 RemoteAudioSink
  -> RingBuffer
  -> AudioConverter(48k/stereo -> 16k/mono)
  -> ASREngine
  -> SubtitleSegment
  -> Console/UI
```

### 4.2 P1 标注增强链路

```text
A 端输入 mark
  -> DataChannel
  -> B 端 MarkMessage
  -> FusionEngine
  -> EnhancedSubtitleSegment
  -> Console/UI
```

### 4.3 线程原则

- 音频回调线程只做轻量复制和投递
- `RingBuffer` 之后交给工作线程处理
- `ASR` 在独立线程或任务队列执行
- 展示层不阻塞音频和 `ASR` 链路

## 5. 推荐目录结构

建议在当前仓库上逐步过渡到下面的结构：

```text
AudioSub/
├── apps/
│   ├── sender/
│   ├── receiver/
│   └── replay/
├── core/
│   ├── audio/
│   ├── subtitle/
│   ├── mark/
│   ├── fusion/
│   └── metrics/
├── adapters/
│   ├── webrtc/
│   ├── signaling/
│   └── asr/
├── signaling/
├── scripts/
├── docs/
└── third_party/
```

过渡期间不需要一次性重写，当前 `client/` 可以先保留，然后逐步把能力迁出：

- `client/signaling_client.*` -> `adapters/signaling/`
- `client/peer_connection_client.*` -> `adapters/webrtc/`
- `client/main.cc` -> `apps/sender/` 或 `apps/receiver/`

## 6. 核心接口约定

为了让团队并行开发，先统一接口，而不是先统一所有机器环境。

### 6.1 音频帧

```cpp
struct PcmFrame {
  int sample_rate = 0;
  int channels = 0;
  int bits_per_sample = 16;
  int64_t timestamp_ms = 0;
  std::vector<int16_t> samples;
};
```

### 6.2 字幕片段

```cpp
struct SubtitleSegment {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  std::string text;
  bool is_final = false;
};
```

### 6.3 标注消息

```cpp
struct MarkMessage {
  uint64_t seq = 0;
  int64_t event_time_ms = 0;
  std::string text;
};
```

### 6.4 融合结果

```cpp
struct EnhancedSubtitleSegment {
  SubtitleSegment subtitle;
  std::vector<MarkMessage> marks;
};
```

### 6.5 关键抽象接口

- `IAudioFrameConsumer`
  - 接收 `PcmFrame`
- `IAudioSource`
  - 可以来自 `WebRTC` 或本地 `WAV`
- `IASREngine`
  - 输入统一格式 PCM
  - 输出 `SubtitleSegment`
- `IMarkChannel`
  - 发送和接收 `MarkMessage`
- `IFusionEngine`
  - 输入字幕和标注
  - 输出融合后的字幕结构

## 7. 角色分工建议

推荐采用“1 名集成人 + 多名模块开发者”的协作模式。

### 7.1 集成人

职责建议由当前能稳定处理 `WebRTC` 环境的成员承担。

- 维护 `WebRTC SDK`
- 负责 `webrtc_adapter`
- 负责最终端到端联调
- 控制版本与发布节奏

### 7.2 音频链路负责人

- 设计 `PcmFrame`
- 实现 `RingBuffer`
- 实现重采样和声道转换
- 确保音频线程与工作线程解耦

### 7.3 ASR 负责人

- 接入 `whisper.cpp`
- 设计 `IASREngine`
- 实现字幕片段输出
- 调整分段与文本刷新策略

### 7.4 标注与融合负责人

- 定义 `MarkMessage` 协议
- 实现消息去重与排序
- 实现字幕与标注对齐
- 输出增强字幕结构

### 7.5 工具与测试负责人

- 维护 `bootstrap.ps1`
- 实现 `replay_tool`
- 编写自动化测试
- 统计关键指标

## 8. 开发模式

### 8.1 Full Mode

用于最终联调和验收：

- 真实 A/B 端
- 真实 `WebRTC`
- 真实 `DataChannel`
- 真实音频链路

### 8.2 Replay Mode

用于大多数成员本地开发：

- 不依赖 `WebRTC`
- 用本地 `wav/pcm` 文件模拟 B 端收到的远端音频
- 直接验证：
  - 音频缓冲
  - 音频转换
  - `ASR`
  - 字幕展示
  - 融合逻辑

这两种模式必须复用同一套核心接口，避免出现两套逻辑。

## 9. 里程碑计划

### 里程碑 M0：统一基础设施

目标：

- 固化 `WebRTC SDK` 分发方式
- 固化 `ASR` 模型分发方式
- 定义核心接口和目录边界

验收：

- 新成员仅执行脚本即可完成基本环境准备
- 所有人使用同一版本 `SDK + 模型`

### 里程碑 M1：P0 音频链路

目标：

- A/B 建连
- A 端发送音频
- B 端稳定获取 `PCM`

验收：

- 控制台可看到音频帧持续到达
- 连续运行 3 分钟不崩溃

### 里程碑 M2：P0 音频处理

目标：

- `RingBuffer`
- 重采样
- 声道转换
- 工作线程消费

验收：

- 输出符合 `ASR` 输入要求的 PCM
- 音频回调线程无阻塞重逻辑

### 里程碑 M3：P0 实时语音转写

目标：

- 接入本地 `ASR`
- 输出可读字幕
- 包含文本和时间范围

验收：

- 至少稳定输出一条可读字幕
- 控制台展示结构清晰

### 里程碑 M4：P1 标注通道

目标：

- A 端发送标注
- B 端接收并解析

验收：

- 标注消息结构稳定可扩展
- 支持基本去重

### 里程碑 M5：P1 融合与指标

目标：

- 字幕与标注时间轴对齐
- 输出增强字幕
- 观测关键指标

验收：

- 同时看到字幕和标注
- 能给出延迟和匹配误差统计

## 10. 每周执行计划

### 第 1 周

- 集成人
  - 固化 `SDK` 和模型下载方式
  - 拆出接口头文件
  - 提供最小运行样例
- 音频负责人
  - 完成 `PcmFrame` 和 `RingBuffer`
- `ASR` 负责人
  - 验证 `whisper.cpp` 单独可跑
- 标注负责人
  - 定义 `MarkMessage` 协议
- 工具负责人
  - 搭建 `replay_tool` 骨架

### 第 2 周

- 集成人
  - 实现远端音频回调接入点
- 音频负责人
  - 完成重采样和声道转换
- `ASR` 负责人
  - 打通 PCM 到字幕输出
- 标注负责人
  - 完成消息收发与解析
- 工具负责人
  - 补充回放测试和日志脚本

### 第 3 周

- 集成人
  - 合并各模块
  - 进行 `Full Mode` 联调
- 音频负责人
  - 调整稳定性和缓存参数
- `ASR` 负责人
  - 调整字幕输出粒度
- 标注负责人
  - 完成融合逻辑
- 工具负责人
  - 增加延迟统计和 3 分钟稳定性验证

## 11. 协作规则

- 强依赖 `WebRTC` 的改动优先由集成人合并
- `core/` 代码禁止直接 include `WebRTC` 头文件
- 新功能必须同时说明：
  - 输入是什么
  - 输出是什么
  - 运行在哪个线程
- 每个模块至少提供一种可脱离 `WebRTC` 的验证方式
- 合并请求优先验证 `replay mode`
- 每周固定一次 `full mode` 联调，不把所有开发都绑定到真实网络环境上

## 12. 当前仓库的第一步改造建议

建议先做下面几项，不要一开始就大重构：

1. 保留当前 `client/` 代码，作为可运行基线
2. 新增 `docs/` 文档和接口头文件目录
3. 先把 `audio/subtitle/mark/fusion` 的公共结构定义出来
4. 新增 `replay_tool`，让 `ASR` 和融合逻辑可以脱离 `WebRTC` 开发
5. 等主链路接口稳定后，再逐步把 `client/` 中的适配代码迁移到 `adapters/`

## 13. 结论

适合当前团队的方案不是“所有人都去搭 `WebRTC` 源码环境”，而是：

- 用预编译 `WebRTC SDK` 统一依赖
- 用接口和分层设计降低环境耦合
- 用 `replay mode` 让大多数成员先完成核心业务
- 最后由少数具备 `WebRTC` 环境的人做统一集成和验收

这样既满足题目对 `C++ + WebRTC-Native + 模块清晰 + 数据流清晰` 的要求，也能在团队环境不统一的情况下稳定推进。
