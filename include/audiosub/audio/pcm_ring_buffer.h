#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "audiosub/core/types.h"

namespace audiosub::audio {

class PcmRingBuffer {
 public:
  explicit PcmRingBuffer(std::size_t capacity_frames);

  PcmRingBuffer(const PcmRingBuffer&) = delete;
  PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;

  bool Push(core::PcmFrame frame);
  std::optional<core::PcmFrame> WaitPop();
  void Close();

 private:
  const std::size_t capacity_frames_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<core::PcmFrame> queue_;
  bool closed_ = false;
};

}  // namespace audiosub::audio
