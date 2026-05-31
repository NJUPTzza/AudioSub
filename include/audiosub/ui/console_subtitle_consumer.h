#pragma once

#include <mutex>

#include "audiosub/core/interfaces.h"

namespace audiosub::ui {

class ConsoleSubtitleConsumer : public core::ISubtitleConsumer {
 public:
  explicit ConsoleSubtitleConsumer(std::mutex* print_mutex = nullptr);

  void OnSubtitleSegment(const core::SubtitleSegment& segment) override;

 private:
  std::mutex* print_mutex_;
};

}  // namespace audiosub::ui
