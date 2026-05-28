#include "asr/null_asr_engine.h"

namespace audiosub::asr {

void NullASREngine::PushAudio(const core::PcmFrame& frame) {
  last_seen_timestamp_ms_ = frame.timestamp_ms;
  (void)consumer_;
  // Placeholder only: real ASR integration will consume buffered PCM and emit
  // subtitle segments asynchronously in a later stage.
}

}  // namespace audiosub::asr
