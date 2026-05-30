#include "audiosub/audio/audio_resampler.h"

#include <algorithm>
#include <cmath>

#include "common_audio/resampler/include/resampler.h"

namespace audiosub::audio {

AudioResampler::AudioResampler(int target_sample_rate, int target_channels)
    : target_sample_rate_(target_sample_rate),
      target_channels_(target_channels) {}

core::PcmFrame AudioResampler::Process(const core::PcmFrame& input) {
  if (input.samples.empty()) {
    core::PcmFrame out;
    out.sample_rate = target_sample_rate_;
    out.channels = target_channels_;
    out.bits_per_sample = 16;
    out.timestamp_ms = input.timestamp_ms;
    return out;
  }

  int src_channels = input.channels;
  int src_frames = static_cast<int>(input.samples.size()) / src_channels;

  auto mono_data = ConvertChannels(input.samples.data(), src_frames,
                                   src_channels, target_channels_);
  int mono_frames = static_cast<int>(mono_data.size()) / target_channels_;

  core::PcmFrame out;
  out.channels = target_channels_;
  out.bits_per_sample = 16;
  out.timestamp_ms = input.timestamp_ms;

  if (input.sample_rate == target_sample_rate_) {
    out.sample_rate = target_sample_rate_;
    out.samples = std::move(mono_data);
  } else {
    webrtc::Resampler resampler;
    resampler.Reset(input.sample_rate, target_sample_rate_,
                    static_cast<size_t>(target_channels_));

    size_t max_out_len = static_cast<size_t>(
        mono_frames * target_sample_rate_ / input.sample_rate * 2 + 256);
    std::vector<int16_t> resampled(max_out_len);
    size_t out_len = 0;

    int rc = resampler.Push(mono_data.data(),
                            static_cast<size_t>(mono_data.size()),
                            resampled.data(), max_out_len, out_len);
    if (rc != 0 || out_len == 0) {
      out.sample_rate = target_sample_rate_;
      return out;
    }

    out.sample_rate = target_sample_rate_;
    out.samples.assign(resampled.data(), resampled.data() + out_len);
  }

  return out;
}

std::vector<int16_t> AudioResampler::ConvertChannels(const int16_t* data,
                                                     int frames,
                                                     int src_channels,
                                                     int dst_channels) {
  if (src_channels == dst_channels) {
    return std::vector<int16_t>(data, data + frames * src_channels);
  }

  std::vector<int16_t> out(frames * dst_channels);

  if (src_channels == 2 && dst_channels == 1) {
    for (int i = 0; i < frames; ++i) {
      int32_t sum = static_cast<int32_t>(data[i * 2]) +
                    static_cast<int32_t>(data[i * 2 + 1]);
      out[i] = static_cast<int16_t>(sum / 2);
    }
  } else if (src_channels == 1 && dst_channels == 2) {
    for (int i = 0; i < frames; ++i) {
      out[i * 2] = data[i];
      out[i * 2 + 1] = data[i];
    }
  } else {
    for (int i = 0; i < frames; ++i) {
      int32_t sum = 0;
      for (int c = 0; c < src_channels; ++c) {
        sum += data[i * src_channels + c];
      }
      int16_t avg = static_cast<int16_t>(sum / src_channels);
      for (int c = 0; c < dst_channels; ++c) {
        out[i * dst_channels + c] = avg;
      }
    }
  }

  return out;
}

}  // namespace audiosub::audio
