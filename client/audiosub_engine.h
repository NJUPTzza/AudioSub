// audiosub_engine.h
// =================
// 把原来散在 main.cc 里的「WebRTC + 信令 + ASR + 字幕/标注融合」全部接线收进
// 一个引擎类。引擎不做任何控制台打印，而是通过 std::function 事件回调把结构化
// 结果抛给上层（CLI 打印，或 Qt 刷 UI）。
//
// 这样同一套核心逻辑可以被两个外壳复用：
//   - audiosub_client.exe（CLI）：回调里 std::cout 打印，保持原行为；
//   - audiosub_capi.dll（C ABI）：回调里转成 C 函数指针，喂给 Qt 前端。
//
// 线程模型不变：WebRTC/ASR 的回调发生在各自后台线程，引擎只是把数据透传到
// 事件回调里；上层（尤其 Qt）需要自己负责把回调切回 UI 线程。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/interfaces.h"
#include "core/types.h"
#include "audio/asr_audio_converter.h"
#include "audio/pcm_ring_buffer.h"
#include "asr/whisper_cpp_engine.h"
#include "fusion/subtitle_mark_fuser.h"
#include "peer_connection_client.h"
#include "signaling_client.h"

namespace audiosub::engine {

// 一条字幕里对齐到的单条标注（含匹配误差指标）。
struct MarkInfo {
  std::uint64_t seq = 0;
  std::string text;
  std::int64_t err_ms = 0;  // 标注匹配误差（标注时刻到字幕时间窗的距离）
};

// 一条「带标注增强」的字幕事件。本端识别和对端回传统一用这个结构。
struct SubtitleEvent {
  int index = 0;
  std::int64_t start_ms = 0;   // 起始现实时间（Unix ms）
  std::int64_t end_ms = 0;     // 结束现实时间（Unix ms）
  std::string text;
  std::int64_t latency_ms = 0;  // 端到端字幕延迟（识别推理耗时）
  bool remote = false;          // false=本端识别, true=对端回传
  std::vector<MarkInfo> marks;  // 对齐到本条字幕的标注
};

// 单项指标的累计统计（次数/总和/峰值），用于退出汇总。
struct MetricStat {
  std::int64_t count = 0;
  std::int64_t sum = 0;
  std::int64_t max = 0;
};

struct MetricsSummary {
  MetricStat lat;  // 端到端字幕延迟
  MetricStat err;  // 标注匹配误差
  MetricStat vis;  // DataChannel 标注可见延迟
};

// 引擎核心类。所有重逻辑（WebRTC、ASR、融合）都在内部后台线程跑。
class AudiosubEngine : public core::ISubtitleConsumer {
 public:
  // 启动参数。id 为 "A"/"B"，audio_path 为 "wasapi"/"webrtc"。
  struct Config {
    std::string id;
    std::string host = "127.0.0.1";
    int port = 8888;
    std::string audio_path = "wasapi";
  };

  // 事件回调签名（均可能在后台线程被调用）。
  using StateCb = std::function<void(const std::string& state)>;
  using SubtitleCb = std::function<void(const SubtitleEvent& sub)>;
  using MarkCb = std::function<void(std::uint64_t seq, const std::string& text,
                                    std::int64_t visible_ms)>;
  using OrphanCb =
      std::function<void(std::uint64_t seq, const std::string& text)>;

  AudiosubEngine();
  ~AudiosubEngine() override;

  AudiosubEngine(const AudiosubEngine&) = delete;
  AudiosubEngine& operator=(const AudiosubEngine&) = delete;

  // 注册回调。约定在 Start() 之前设置。
  void SetStateCallback(StateCb cb) { state_cb_ = std::move(cb); }
  void SetSubtitleCallback(SubtitleCb cb) { subtitle_cb_ = std::move(cb); }
  void SetMarkCallback(MarkCb cb) { mark_cb_ = std::move(cb); }
  void SetOrphanCallback(OrphanCb cb) { orphan_cb_ = std::move(cb); }

  // 完整启动：初始化 WebRTC、ASR、连接信令、挂回调。成功返回 true。
  // 失败时通过返回值区分（详见 .cc 内日志），调用方应放弃使用本实例。
  bool Start(const Config& cfg);

  // 角色：A 为 offerer。
  bool IsOfferer() const { return is_offerer_; }

  // A 端讲话开关（B 端调用返回 false）。
  bool SetTalking(bool on);

  // 发送一条标注（自增 seq，event_time_ms 取当前现实时间）。
  // 返回 false 表示 DataChannel 尚未打开。
  bool SendNote(const std::string& utf8_text);

  // 优雅停止：关信令、关 WebRTC、停后台线程。可重复调用。
  // 停止前会把剩余「无归属标注」通过 orphan 回调兜底抛出。
  void Stop();

  // 当前累计指标快照。
  MetricsSummary GetMetrics() const;

  // ISubtitleConsumer：ASR 线程产出一条字幕时回调。
  void OnSubtitleSegment(const core::SubtitleSegment& segment) override;

 private:
  // 收到对端 DataChannel 文本消息的处理（区分 annotation / subtitle / 普通文本）。
  void HandlePeerMessage(const std::string& text);

  // 指标累计（线程安全）。
  void AddLat(std::int64_t v);
  void AddErr(std::int64_t v);
  void AddVis(std::int64_t v);

  // === 配置 / 角色 ===
  Config cfg_;
  bool is_offerer_ = false;
  std::atomic<bool> started_{false};
  std::atomic<bool> stopped_{false};

  // === 事件回调 ===
  StateCb state_cb_;
  SubtitleCb subtitle_cb_;
  MarkCb mark_cb_;
  OrphanCb orphan_cb_;

  // === 核心组件 ===
  PeerConnectionClient pc_;
  SignalingClient signaling_;
  audio::PcmRingBuffer local_audio_buffer_{128};
  audio::PcmRingBuffer remote_audio_buffer_{128};
  audio::PcmRingBuffer remote_audio_asr_buffer_{256};
  audio::AsrAudioConverter asr_converter_;
  std::unique_ptr<asr::WhisperCppEngine> asr_engine_;
  fusion::SubtitleMarkFuser mark_fuser_;

  // === 后台线程 ===
  std::thread local_audio_worker_;
  std::thread remote_audio_worker_;
  std::thread asr_worker_;
  std::atomic<std::uint64_t> asr_sent_frames_{0};

  // === 标注序号 / 字幕计数 ===
  std::atomic<std::uint64_t> annotation_seq_{0};
  std::atomic<int> subtitle_count_{0};

  // === 指标累计 ===
  mutable std::mutex metrics_mutex_;
  MetricsSummary metrics_;
};

}  // namespace audiosub::engine
