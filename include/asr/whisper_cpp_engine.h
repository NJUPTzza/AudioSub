#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/interfaces.h"

struct whisper_context;

namespace audiosub::asr {

// 段落式 + VAD 的 whisper.cpp 引擎。
// - 上游必须保证传入 16kHz / mono / 16bit PCM。
// - 内部按"攒满 4 秒 或 检测到说话结束"触发一次推理，推理完立即清空。
// - 不做滑动窗口反复识别同一段音频。
class WhisperCppEngine : public core::IASREngine {
 public:
  explicit WhisperCppEngine(std::string model_path);
  ~WhisperCppEngine() override;

  WhisperCppEngine(const WhisperCppEngine&) = delete;
  WhisperCppEngine& operator=(const WhisperCppEngine&) = delete;

  bool Initialize();
  void PushAudio(const core::PcmFrame& frame) override;
  // A 端停止说话时调用：强制识别缓冲里尚未 flush 的语音段。
  void FlushPending();
  void SetSubtitleConsumer(core::ISubtitleConsumer* consumer) override {
    consumer_ = consumer;
  }

 private:
  void RunInference();
  void ResetSegmentState();

  std::string model_path_;
  whisper_context* ctx_ = nullptr;
  core::ISubtitleConsumer* consumer_ = nullptr;

  // 累积的 16k / mono / float PCM，攒满 1 段就推理 + 清空。
  std::vector<float> pending_pcm_;

  // 当前待识别音频段开始进入 ASR 缓冲的现实时间（Unix ms）。
  // 输出字幕时用它作为起始时间，便于控制台/UI 展示真实时间。
  int64_t pending_start_wall_ms_ = 0;

  // 上一次输出的文本，用于去重。
  std::string last_text_;

  // 当前缓冲里是否检测到了语音（VAD 状态机）。
  bool segment_has_speech_ = false;
  // 连续静音帧计数，用来检测"说话结束"。
  int silence_frame_count_ = 0;
  // 本段音频里最后一次检测到语音的现实时间；用作字幕 end_ms（说话结束），
  // 而不是 whisper 推理完成时刻，避免把下一句的时间也算进上一句窗口。
  int64_t last_speech_wall_ms_ = 0;

  mutable std::mutex inference_mutex_;

  static constexpr int kTargetSampleRate = 16000;
  static constexpr int kSegmentSeconds = 4;
  static constexpr int kSegmentSamples = kTargetSampleRate * kSegmentSeconds;
};

}  // namespace audiosub::asr
