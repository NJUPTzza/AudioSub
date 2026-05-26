#include "audiosub/asr/null_asr_engine.h"

#include <string>

namespace audiosub::asr {

    void NullASREngine::PushAudio(const core::PcmFrame& frame) {
        last_seen_timestamp_ms_ = frame.timestamp_ms;

        if (!consumer_ || frame.sample_rate <= 0 || frame.samples.empty()) {
            return;
        }

        accumulated_samples_ += static_cast<int64_t>(frame.samples.size());

        // 输入已经是 16kHz mono，累计约 1 秒音频输出一条模拟字幕。
        const int64_t samples_per_segment = frame.sample_rate;

        while (accumulated_samples_ >= samples_per_segment) {
            accumulated_samples_ -= samples_per_segment;

            const int64_t start_ms = segment_index_ * 1000;
            const int64_t end_ms = start_ms + 1000;
            ++segment_index_;

            core::SubtitleSegment segment;
            segment.start_ms = start_ms;
            segment.end_ms = end_ms;
            segment.text = "<mock transcript: audio received>";
            segment.is_final = true;

            consumer_->OnSubtitleSegment(segment);
        }
    }

}  // namespace audiosub::asr