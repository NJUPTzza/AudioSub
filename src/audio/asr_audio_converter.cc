#include "audio/asr_audio_converter.h"

#include <algorithm>
#include <cmath>

// “格式转换器” 48k/stereo -> 16k/mono

namespace audiosub::audio {

std::vector<int16_t> AsrAudioConverter::StereoToMono(
    const std::vector<int16_t>& in, int channels) {
  if (channels <= 1) return in;
  std::vector<int16_t> out;
  out.reserve(in.size() / channels);
  for (size_t i = 0; i + channels <= in.size(); i += channels) {
    int32_t sum = 0;
    for (int c = 0; c < channels; ++c) sum += in[i + c];
    out.push_back(static_cast<int16_t>(sum / channels));
  }
  return out;
}

std::vector<int16_t> AsrAudioConverter::ResampleLinear(
    const std::vector<int16_t>& in, int in_rate, int out_rate) {
  if (in.empty() || in_rate <= 0 || out_rate <= 0 || in_rate == out_rate) return in;

  const double ratio = static_cast<double>(out_rate) / static_cast<double>(in_rate);
  const size_t out_n = static_cast<size_t>(std::floor(in.size() * ratio));
  std::vector<int16_t> out;
  out.resize(out_n);

  for (size_t i = 0; i < out_n; ++i) {
    double src_pos = static_cast<double>(i) / ratio;
    size_t idx = static_cast<size_t>(src_pos);
    double frac = src_pos - static_cast<double>(idx);

    size_t idx2 = std::min(idx + 1, in.size() - 1);
    double v = (1.0 - frac) * in[idx] + frac * in[idx2];
    out[i] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
  }
  return out;
}

core::PcmFrame AsrAudioConverter::ToAsrFormat(const core::PcmFrame& in) const {
  core::PcmFrame out;
  out.timestamp_ms = in.timestamp_ms;
  out.bits_per_sample = 16;
  out.channels = 1;
  out.sample_rate = 16000;

  auto mono = StereoToMono(in.samples, in.channels);
  out.samples = ResampleLinear(mono, in.sample_rate, 16000);
  return out;
}

}  // namespace audiosub::audio