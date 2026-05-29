#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "api/media_stream_interface.h"
#include "audiosub/audio/pcm_ring_buffer.h"

namespace audiosub {

class RemoteAudioSink : public webrtc::AudioTrackSinkInterface {
 public:
  explicit RemoteAudioSink(audio::PcmRingBuffer& buffer);

  void OnData(const void* audio_data,
              int bits_per_sample,
              int sample_rate,
              size_t number_of_channels,
              size_t number_of_frames,
              std::optional<int64_t> absolute_capture_timestamp_ms) override;

 private:
  audio::PcmRingBuffer& buffer_;
  int64_t next_timestamp_ms_ = 0;
};

}  // namespace audiosub
