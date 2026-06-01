// 字幕与标注融合器
//
// 职责：
//   1) 收集 A 端发来的标注（MarkMessage），按 seq 去重；
//   2) 当一条字幕产生时，把"发生时刻落在该字幕时间范围内"的标注挑出来，
//      拼成 EnhancedSubtitleSegment 返回，并把这些标注标记为"已认领"；
//   3) 把一直没被任何字幕认领、又已经不可能再匹配的标注，作为"无归属标注"
//      收集出来单独展示，保证标注不会凭空消失。
//
// 线程安全：标注由网络线程写入（AddMark），字幕由 ASR 线程读取（Fuse），
// 两者并发访问内部容器，因此用一把 mutex 保护。

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "core/types.h"

namespace audiosub::fusion {

class SubtitleMarkFuser {
 public:
  // 收到一条标注。返回 false 表示这个 seq 之前已经处理过（去重命中）。
  bool AddMark(const core::MarkMessage& mark) {
    std::lock_guard<std::mutex> lock(mutex_);
    // unordered_set::insert 返回的 second 为 false 说明 seq 已存在 -> 重复。
    if (!seen_seq_.insert(mark.seq).second) {
      return false;
    }
    entries_.push_back(Entry{mark, /*claimed=*/false, /*reported=*/false});
    return true;
  }

  // 给定一条字幕，找出时间范围内的所有标注，组装成增强字幕返回，
  // 并把命中的标注标记为"已认领"。
  core::EnhancedSubtitleSegment Fuse(const core::SubtitleSegment& sub) {
    core::EnhancedSubtitleSegment out;
    out.subtitle = sub;

    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& e : entries_) {
      // 核心对齐判断：标注发生时刻是否落在字幕时间范围内。
      // 加一点容差 kToleranceMs：标注往往比"开始说话"晚几百毫秒敲下，
      // 字幕的 end_ms 又是"识别完成"时刻，给点缓冲匹配更稳。
      if (e.mark.event_time_ms >= sub.start_ms - kToleranceMs &&
          e.mark.event_time_ms <= sub.end_ms + kToleranceMs) {
        out.marks.push_back(e.mark);
        e.claimed = true;
      }
    }
    return out;
  }

  // 当一条新字幕开始时调用：发生时刻早于(字幕开始 - 容差)、却一直没被认领的
  // 标注，已经不可能再匹配当前或更晚的字幕（未来字幕时间只会更晚），
  // 判为"无归属"返回出来（只返回一次，靠 reported 去重）。
  std::vector<core::MarkMessage> CollectOrphansBefore(int64_t subtitle_start_ms) {
    return CollectOrphans(subtitle_start_ms - kToleranceMs);
  }

  // 退出时调用：把所有剩余未认领、未上报的标注全部收出来兜底展示。
  std::vector<core::MarkMessage> CollectRemainingOrphans() {
    return CollectOrphans(INT64_MAX);
  }

 private:
  struct Entry {
    core::MarkMessage mark;
    bool claimed;   // 是否已被某条字幕认领
    bool reported;  // 是否已作为"无归属"上报过（避免重复打印）
  };

  std::vector<core::MarkMessage> CollectOrphans(int64_t cutoff_ms) {
    std::vector<core::MarkMessage> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& e : entries_) {
      if (!e.claimed && !e.reported && e.mark.event_time_ms < cutoff_ms) {
        result.push_back(e.mark);
        e.reported = true;
      }
    }
    return result;
  }

  // 时间匹配容差（毫秒）。先给 1500ms，后面可按实测调。
  static constexpr int64_t kToleranceMs = 1500;

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::unordered_set<std::uint64_t> seen_seq_;  // 已见过的 seq，用于去重
};

}  // namespace audiosub::fusion
