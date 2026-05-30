#include "audiosub/audio/audio_pipeline.h"

namespace audiosub::audio {

AudioPipeline::AudioPipeline(int target_sample_rate, int target_channels)
    : resampler_(target_sample_rate, target_channels) {}

void AudioPipeline::OnPcmFrame(const core::PcmFrame& frame) {
  if (!output_) return;

  auto converted = resampler_.Process(frame);
  if (!converted.samples.empty()) {
    output_->OnPcmFrame(converted);
  }
}

}  // namespace audiosub::audio
