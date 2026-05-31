#pragma once

#include <vector>
#include <cstdint>
#include <optional>

// “20ms 固定分帧器”

namespace audiosub::audio {

class AsrChunker {
 public:
  // target_sample_rate=16000, chunk_ms=20 => 320 samples/chunk
  AsrChunker(int target_sample_rate, int chunk_ms);

  void Push(const std::vector<int16_t>& samples);
  std::optional<std::vector<int16_t>> PopChunk();

 private:
  size_t chunk_samples_;
  std::vector<int16_t> buffer_;
};

}  // namespace audiosub::audio