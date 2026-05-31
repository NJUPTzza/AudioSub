#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audiosub::core {

// A unified PCM frame shape shared by WebRTC adapters, replay tools, and ASR.
struct PcmFrame {
  int sample_rate = 0; // 采样率
  int channels = 0; // 声道数
  int bits_per_sample = 16; // 位深
  int64_t timestamp_ms = 0; // 时间戳
  std::vector<int16_t> samples; // 	交错存储的 int16 PCM 样本
};

// A subtitle segment produced by the ASR pipeline.
struct SubtitleSegment {
  int64_t start_ms = 0; // 开始时间
  int64_t end_ms = 0; // 结束时间
  std::string text; // 文本
  bool is_final = false; // 是否是最终结果
  int64_t latency_ms = 0; // 识别推理耗时（端到端字幕延迟指标）
};

// A structured annotation message sent over DataChannel.
struct MarkMessage {
  uint64_t seq = 0; // 序列号
  int64_t event_time_ms = 0; // 事件时间
  std::string text;
};

// Final fused output shown to users after subtitle/mark alignment.
struct EnhancedSubtitleSegment {
  SubtitleSegment subtitle;
  std::vector<MarkMessage> marks;
};

}  // namespace audiosub::core
