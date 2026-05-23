#include "audiosub/audio/pass_through_audio_pipeline.h"

namespace audiosub::audio {

void PassThroughAudioPipeline::OnPcmFrame(const core::PcmFrame& frame) {
  if (!output_) {
    return;
  }
  output_->OnPcmFrame(frame);
}

}  // namespace audiosub::audio
