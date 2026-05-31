#pragma once

#include <vector>
#include <cstdint>

#include "core/types.h"

// "格式转换器" 48k/stereo -> 16k/mono
namespace audiosub::audio {

class AsrAudioConverter {
 public:
  // 把任意输入帧转换为 16k/mono/16bit
  core::PcmFrame ToAsrFormat(const core::PcmFrame& in) const;

 private:
  static std::vector<int16_t> StereoToMono(const std::vector<int16_t>& in, int channels);
  static std::vector<int16_t> ResampleLinear(
      const std::vector<int16_t>& in, int in_rate, int out_rate);
};

}  // namespace audiosub::audio