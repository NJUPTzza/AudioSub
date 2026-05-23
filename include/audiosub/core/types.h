#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audiosub::core {

// A unified PCM frame shape shared by WebRTC adapters, replay tools, and ASR.
struct PcmFrame {
  int sample_rate = 0;
  int channels = 0;
  int bits_per_sample = 16;
  int64_t timestamp_ms = 0;
  std::vector<int16_t> samples;
};

// A subtitle segment produced by the ASR pipeline.
struct SubtitleSegment {
  int64_t start_ms = 0;
  int64_t end_ms = 0;
  std::string text;
  bool is_final = false;
};

// A structured annotation message sent over DataChannel.
struct MarkMessage {
  uint64_t seq = 0;
  int64_t event_time_ms = 0;
  std::string text;
};

// Final fused output shown to users after subtitle/mark alignment.
struct EnhancedSubtitleSegment {
  SubtitleSegment subtitle;
  std::vector<MarkMessage> marks;
};

}  // namespace audiosub::core
