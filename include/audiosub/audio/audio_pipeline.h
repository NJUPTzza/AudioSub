#pragma once

#include <memory>

#include "audiosub/audio/audio_resampler.h"
#include "audiosub/core/interfaces.h"

namespace audiosub::audio {

class AudioPipeline : public core::IAudioFrameConsumer {
 public:
  AudioPipeline(int target_sample_rate, int target_channels);

  void SetOutput(core::IAudioFrameConsumer* output) { output_ = output; }

  void OnPcmFrame(const core::PcmFrame& frame) override;

  AudioResampler& resampler() { return resampler_; }

 private:
  AudioResampler resampler_;
  core::IAudioFrameConsumer* output_ = nullptr;
};

}  // namespace audiosub::audio
