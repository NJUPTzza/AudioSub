#pragma once

#include "audiosub/core/interfaces.h"

namespace audiosub::asr {

// Placeholder ASR engine used to keep the collaboration skeleton buildable
// before whisper.cpp or another engine is integrated.
class NullASREngine : public core::IASREngine {
 public:
  void PushAudio(const core::PcmFrame& frame) override;
  void SetSubtitleConsumer(core::ISubtitleConsumer* consumer) override {
    consumer_ = consumer;
  }

 private:
  core::ISubtitleConsumer* consumer_ = nullptr;
  int64_t last_seen_timestamp_ms_ = 0;
};

}  // namespace audiosub::asr
