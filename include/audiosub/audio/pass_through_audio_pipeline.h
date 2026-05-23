#pragma once

#include "audiosub/core/interfaces.h"

namespace audiosub::audio {

// Minimal pipeline skeleton for the next stage:
// WebRTC sink/replay -> ring buffer -> converter -> ASR.
// Current implementation only forwards PCM to the next consumer unchanged.
class PassThroughAudioPipeline : public core::IAudioFrameConsumer {
 public:
  PassThroughAudioPipeline() = default;

  void SetOutput(core::IAudioFrameConsumer* output) { output_ = output; }
  void OnPcmFrame(const core::PcmFrame& frame) override;

 private:
  core::IAudioFrameConsumer* output_ = nullptr;
};

}  // namespace audiosub::audio
