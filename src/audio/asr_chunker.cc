#include "audio/asr_chunker.h"

// “20ms 固定分帧器”

namespace audiosub::audio {

AsrChunker::AsrChunker(int target_sample_rate, int chunk_ms)
    : chunk_samples_(static_cast<size_t>(target_sample_rate * chunk_ms / 1000)) {}

void AsrChunker::Push(const std::vector<int16_t>& samples) {
  buffer_.insert(buffer_.end(), samples.begin(), samples.end());
}

std::optional<std::vector<int16_t>> AsrChunker::PopChunk() {
  if (buffer_.size() < chunk_samples_) return std::nullopt;
  std::vector<int16_t> out(buffer_.begin(), buffer_.begin() + chunk_samples_);
  buffer_.erase(buffer_.begin(), buffer_.begin() + chunk_samples_);
  return out;
}

}  // namespace audiosub::audio