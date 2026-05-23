#include "audiosub/audio/pcm_ring_buffer.h"

#include <utility>

namespace audiosub::audio {

PcmRingBuffer::PcmRingBuffer(std::size_t capacity_frames)
    : capacity_frames_(capacity_frames == 0 ? 1 : capacity_frames) {}

bool PcmRingBuffer::Push(core::PcmFrame frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return false;
  }
  if (queue_.size() >= capacity_frames_) {
    queue_.pop_front();
  }
  queue_.push_back(std::move(frame));
  cv_.notify_one();
  return true;
}

std::optional<core::PcmFrame> PcmRingBuffer::Pop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) {
    return std::nullopt;
  }
  core::PcmFrame frame = std::move(queue_.front());
  queue_.pop_front();
  return frame;
}

std::optional<core::PcmFrame> PcmRingBuffer::WaitPop() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
  if (queue_.empty()) {
    return std::nullopt;
  }
  core::PcmFrame frame = std::move(queue_.front());
  queue_.pop_front();
  return frame;
}

void PcmRingBuffer::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  cv_.notify_all();
}

std::size_t PcmRingBuffer::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

bool PcmRingBuffer::closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

}  // namespace audiosub::audio
