#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "core/types.h"

namespace audiosub::audio {

// Thread-safe frame queue used to decouple WebRTC/audio callbacks from heavier
// ASR or fusion work on background threads.
class PcmRingBuffer {
 public:
  explicit PcmRingBuffer(std::size_t capacity_frames);

  PcmRingBuffer(const PcmRingBuffer&) = delete;
  PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;

  bool Push(core::PcmFrame frame);
  std::optional<core::PcmFrame> Pop();
  std::optional<core::PcmFrame> WaitPop();

  void Close();

  std::size_t size() const;
  std::size_t capacity() const { return capacity_frames_; }
  bool closed() const;

 private:
  const std::size_t capacity_frames_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<core::PcmFrame> queue_;
  bool closed_ = false;
};

}  // namespace audiosub::audio
