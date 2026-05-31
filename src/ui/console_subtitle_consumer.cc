#include "audiosub/ui/console_subtitle_consumer.h"

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace audiosub::ui {

ConsoleSubtitleConsumer::ConsoleSubtitleConsumer(std::mutex* print_mutex)
    : print_mutex_(print_mutex) {}

void ConsoleSubtitleConsumer::OnSubtitleSegment(
    const core::SubtitleSegment& segment) {
  auto format_time = [](int64_t ms) -> std::string {
    int64_t total_sec = ms / 1000;
    int64_t rem_ms = ms % 1000;
    int min = static_cast<int>(total_sec / 60);
    int sec = static_cast<int>(total_sec % 60);
    int ms_part = static_cast<int>(rem_ms);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << min << ":"
        << std::setfill('0') << std::setw(2) << sec << "."
        << std::setfill('0') << std::setw(3) << ms_part;
    return oss.str();
  };

  std::string start_str = format_time(segment.start_ms);
  std::string end_str = format_time(segment.end_ms);

  std::string line = "[" + start_str + " -> " + end_str + "] " +
                     segment.text;

  if (print_mutex_) {
    std::lock_guard<std::mutex> lock(*print_mutex_);
    std::cout << "\r" << line << "\n> " << std::flush;
  } else {
    std::cout << line << std::endl;
  }
}

}  // namespace audiosub::ui
