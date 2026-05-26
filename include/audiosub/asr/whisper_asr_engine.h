#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audiosub/core/interfaces.h"

struct whisper_context;

namespace audiosub::asr {

    class WhisperASREngine : public core::IASREngine {
    public:
        explicit WhisperASREngine(std::string model_path);
        ~WhisperASREngine() override;

        WhisperASREngine(const WhisperASREngine&) = delete;
        WhisperASREngine& operator=(const WhisperASREngine&) = delete;

        bool Initialize();
        bool initialized() const { return ctx_ != nullptr; }

        void PushAudio(const core::PcmFrame& frame) override;
        void SetSubtitleConsumer(core::ISubtitleConsumer* consumer) override {
            consumer_ = consumer;
        }

    private:
        void RunInference();

        std::string model_path_;
        whisper_context* ctx_ = nullptr;
        core::ISubtitleConsumer* consumer_ = nullptr;

        std::vector<float> pending_pcm_;
        int64_t segment_index_ = 0;
        std::string last_text_;
        bool segment_has_speech_ = false;
        int silence_frame_count_ = 0;
        // 当前 AudioProcessingWorker 已经输出 16kHz mono PCM。
        static constexpr int kTargetSampleRate = 16000;
        static constexpr int kSegmentSeconds = 4;
        static constexpr int kSegmentSamples = kTargetSampleRate * kSegmentSeconds;
    };

}  // namespace audiosub::asr