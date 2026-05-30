#pragma once

#include <cstdint>
#include <vector>

#include "audiosub/core/types.h"

namespace audiosub::audio {

class AudioResampler {
 public:
  AudioResampler(int target_sample_rate, int target_channels);

  core::PcmFrame Process(const core::PcmFrame& input);

  int target_sample_rate() const { return target_sample_rate_; }
  int target_channels() const { return target_channels_; }

 private:
  std::vector<int16_t> ConvertChannels(const int16_t* data,
                                       int frames,
                                       int src_channels,
                                       int dst_channels);

  int target_sample_rate_;
  int target_channels_;
};

}  // namespace audiosub::audio
