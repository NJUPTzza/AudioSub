#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "audiosub/asr/whisper_asr_engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "whisper.h"

namespace {
    static std::string ConvertTraditionalToSimplifiedUtf8(const std::string& input) {
        if (input.empty()) {
            return input;
        }

#ifdef _WIN32
        // UTF-8 -> UTF-16
        int wide_len = MultiByteToWideChar(
            CP_UTF8,
            0,
            input.data(),
            static_cast<int>(input.size()),
            nullptr,
            0);

        if (wide_len <= 0) {
            return input;
        }

        std::wstring wide(wide_len, L'\0');

        MultiByteToWideChar(
            CP_UTF8,
            0,
            input.data(),
            static_cast<int>(input.size()),
            wide.data(),
            wide_len);

        // 繁体/异体中文 -> 简体中文
        int simp_len = LCMapStringEx(
            L"zh-CN",
            LCMAP_SIMPLIFIED_CHINESE,
            wide.data(),
            static_cast<int>(wide.size()),
            nullptr,
            0,
            nullptr,
            nullptr,
            0);

        if (simp_len <= 0) {
            return input;
        }

        std::wstring simplified(simp_len, L'\0');

        LCMapStringEx(
            L"zh-CN",
            LCMAP_SIMPLIFIED_CHINESE,
            wide.data(),
            static_cast<int>(wide.size()),
            simplified.data(),
            simp_len,
            nullptr,
            nullptr,
            0);

        // UTF-16 -> UTF-8
        int utf8_len = WideCharToMultiByte(
            CP_UTF8,
            0,
            simplified.data(),
            static_cast<int>(simplified.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (utf8_len <= 0) {
            return input;
        }

        std::string output(utf8_len, '\0');

        WideCharToMultiByte(
            CP_UTF8,
            0,
            simplified.data(),
            static_cast<int>(simplified.size()),
            output.data(),
            utf8_len,
            nullptr,
            nullptr);

        return output;
#else
        return input;
#endif
    }
    bool HasSpeechLikeEnergy(const std::vector<int16_t>& samples) {
        if (samples.empty()) {
            return false;
        }

        int64_t sum_abs = 0;
        int peak = 0;

        for (int16_t s : samples) {
            const int v = std::abs(static_cast<int>(s));
            sum_abs += v;
            if (v > peak) {
                peak = v;
            }
        }

        const int avg_abs = static_cast<int>(sum_abs / samples.size());
        const bool has_speech = avg_abs > 180 && peak > 230;

       /* static int debug_count = 0;
        if (debug_count < 200) {
            std::cout << "[vad] avg_abs=" << avg_abs
                << ", peak=" << peak
                << ", speech=" << (has_speech ? "true" : "false")
                << std::endl;
            ++debug_count;
        }*/

        return has_speech;
    }

}  // namespace

namespace audiosub::asr {

    WhisperASREngine::WhisperASREngine(std::string model_path)
        : model_path_(std::move(model_path)) {
    }

    WhisperASREngine::~WhisperASREngine() {
        if (ctx_) {
            whisper_free(ctx_);
            ctx_ = nullptr;
        }
    }

    bool WhisperASREngine::Initialize() {
        whisper_context_params cparams = whisper_context_default_params();

        // 先用 CPU，避免因为机器没有 GPU 导致接入阶段不稳定。
        cparams.use_gpu = false;

        ctx_ = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
        if (!ctx_) {
            std::cerr << "[whisper] failed to load model: " << model_path_ << "\n";
            return false;
        }

        std::cout << "[whisper] model loaded: " << model_path_ << "\n";
        return true;
    }

    void WhisperASREngine::PushAudio(const core::PcmFrame& frame) {
        if (!ctx_ || !consumer_) {
            return;
        }

        if (frame.sample_rate != kTargetSampleRate ||
            frame.channels != 1 ||
            frame.bits_per_sample != 16 ||
            frame.samples.empty()) {
            return;
        }

        // 静音/底噪不送入 whisper，减少幻觉字幕。
        const bool has_speech = HasSpeechLikeEnergy(frame.samples);

        // 不要直接丢静音帧，否则一句话说完后的停顿不会进入缓存，
        // 会导致上一句话要等下一句话补足缓存后才显示。
        pending_pcm_.reserve(pending_pcm_.size() + frame.samples.size());

        for (int16_t sample : frame.samples) {
            pending_pcm_.push_back(static_cast<float>(sample) / 32768.0f);
        }

        if (has_speech) {
            segment_has_speech_ = true;
            silence_frame_count_ = 0;
        }
        else {
            if (segment_has_speech_) {
                ++silence_frame_count_;
            }
        }

        // 每帧大约 10ms，60 帧约 0.6 秒。
        // 说完后停顿 0.6 秒，就提前识别当前段。
        constexpr int kSilenceFramesToFlush = 60;

        // 至少攒够 1 秒音频再识别，避免太短导致 Whisper 幻觉。
        constexpr int kMinFlushSamples = kTargetSampleRate * 1;

        const bool long_enough =
            static_cast<int>(pending_pcm_.size()) >= kSegmentSamples;

        const bool speech_ended =
            segment_has_speech_ &&
            silence_frame_count_ >= kSilenceFramesToFlush &&
            static_cast<int>(pending_pcm_.size()) >= kMinFlushSamples;

        if (long_enough || speech_ended) {
            RunInference();

            pending_pcm_.clear();
            segment_has_speech_ = false;
            silence_frame_count_ = 0;
        }
    }

    void WhisperASREngine::RunInference() {
        if (!ctx_ || !consumer_ || pending_pcm_.empty()) {
            return;
        }

        whisper_full_params params =
            whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

        params.print_realtime = false;
        params.print_progress = false;
        params.print_timestamps = false;
        params.print_special = false;
        params.translate = false;
        params.no_context = true;
        params.single_segment = true;
        params.n_threads = 4;

        params.language = "zh";
        params.initial_prompt = nullptr;

        const int sample_count =
            std::min(static_cast<int>(pending_pcm_.size()), kSegmentSamples);

        const int ret = whisper_full(ctx_, params, pending_pcm_.data(), sample_count);
        if (ret != 0) {
            std::cerr << "[whisper] whisper_full failed, ret=" << ret << "\n";
            return;
        }

        const int n_segments = whisper_full_n_segments(ctx_);
        std::string text;

        for (int i = 0; i < n_segments; ++i) {
            const char* seg_text = whisper_full_get_segment_text(ctx_, i);
            if (seg_text) {
                text += seg_text;
            }
        }

        if (text.empty()) {
            return;
        }

        // 强制繁体转简体。一定要在输出字幕前做。
        text = ConvertTraditionalToSimplifiedUtf8(text);

        // 过滤 Whisper 在低能量噪声下常见的幻觉字幕。
        // 注意：这里写简体词，因为上面已经统一转成简体。
        if (text.find("字幕") != std::string::npos ||
            text.find("制作") != std::string::npos ||
            text.find("贝尔") != std::string::npos ||
            text.find("叹气") != std::string::npos ||
            text.find("音乐") != std::string::npos ||
            text.find("掌声") != std::string::npos) {
            return;
        }

        // 过滤连续重复的幻觉字幕。
        // 注意：重复过滤要放在简体转换之后。
        if (text == last_text_) {
            return;
        }
        last_text_ = text;

        const int64_t start_ms = segment_index_ * kSegmentSeconds * 1000;
        const int64_t end_ms = start_ms + kSegmentSeconds * 1000;
        ++segment_index_;

        core::SubtitleSegment segment;
        segment.start_ms = start_ms;
        segment.end_ms = end_ms;
        segment.text = text;
        segment.is_final = true;

        consumer_->OnSubtitleSegment(segment);
    }

}  // namespace audiosub::asr