#include "remote_audio_sink.h"

#include <utility>

namespace audiosub {

RemoteAudioSink::RemoteAudioSink(audio::PcmRingBuffer& buffer)
    : buffer_(buffer) {}

void RemoteAudioSink::OnData(
    const void* audio_data,
    int bits_per_sample,
    int sample_rate,
    size_t number_of_channels,
    size_t number_of_frames,
    std::optional<int64_t> absolute_capture_timestamp_ms) {
  if (!audio_data || bits_per_sample != 16) return;

  core::PcmFrame frame;
  frame.sample_rate = sample_rate;
  frame.channels = static_cast<int>(number_of_channels);
  frame.bits_per_sample = bits_per_sample;
  frame.timestamp_ms = absolute_capture_timestamp_ms.value_or(
      next_timestamp_ms_);

  size_t total_samples = number_of_frames * number_of_channels;
  const int16_t* samples = static_cast<const int16_t*>(audio_data);
  frame.samples.assign(samples, samples + total_samples);

  next_timestamp_ms_ +=
      static_cast<int64_t>(number_of_frames) * 1000 / sample_rate;

  buffer_.Push(std::move(frame));
}

}  // namespace audiosub
