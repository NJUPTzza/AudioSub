// whisper.cpp 引擎实现（按参考仓库 jkl-io/AudioSub feature/yupingyang-p0-audio-link
// 的 whisper_asr_engine.cc 1:1 实现）。
//
// 核心策略：
//   1) PushAudio 累积 PCM。用极简能量 VAD 区分"是否有人在说话"。
//   2) 攒满 4 秒强制 flush，或检测到说话停顿 600ms 提前 flush。
//   3) 每次 flush = 一次 whisper_full + 一段字幕，之后立刻清空。
//   4) whisper 中文模型默认输出繁体，用 Win32 LCMapStringEx 强转简体。
//   5) 仅过滤训练集中常见的 6 个高频幻觉关键词。

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "asr/whisper_cpp_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "whisper.h"

namespace {

// Whisper 中文模型默认输出繁体。这里把 UTF-8 字符串强制转换为简体。
// 失败时退回原字符串。仅在 Windows 平台启用。
std::string ConvertTraditionalToSimplifiedUtf8(const std::string& input) {
  if (input.empty()) return input;

#ifdef _WIN32
  // UTF-8 -> UTF-16
  int wide_len = MultiByteToWideChar(CP_UTF8, 0, input.data(),
                                     static_cast<int>(input.size()),
                                     nullptr, 0);
  if (wide_len <= 0) return input;
  std::wstring wide(wide_len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, input.data(),
                      static_cast<int>(input.size()),
                      wide.data(), wide_len);

  // 繁体/异体 -> 简体
  int simp_len = LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE,
                               wide.data(),
                               static_cast<int>(wide.size()),
                               nullptr, 0, nullptr, nullptr, 0);
  if (simp_len <= 0) return input;
  std::wstring simplified(simp_len, L'\0');
  LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE,
                wide.data(),
                static_cast<int>(wide.size()),
                simplified.data(), simp_len,
                nullptr, nullptr, 0);

  // UTF-16 -> UTF-8
  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, simplified.data(),
                                     static_cast<int>(simplified.size()),
                                     nullptr, 0, nullptr, nullptr);
  if (utf8_len <= 0) return input;
  std::string output(utf8_len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, simplified.data(),
                      static_cast<int>(simplified.size()),
                      output.data(), utf8_len, nullptr, nullptr);
  return output;
#else
  return input;
#endif
}

// 极简能量阈值 VAD：按一帧（典型 10ms / 20ms）的平均绝对值 + 峰值判断是否
// 像在说话。这是为了避免静音段送进 whisper 后生成幻觉字幕。
// 阈值数值参考同组成员的工作实现，针对 16k mono int16 PCM。
bool HasSpeechLikeEnergy(const std::vector<int16_t>& samples) {
  if (samples.empty()) return false;

  int64_t sum_abs = 0;
  int peak = 0;
  for (int16_t s : samples) {
    const int v = std::abs(static_cast<int>(s));
    sum_abs += v;
    if (v > peak) peak = v;
  }
  const int avg_abs = static_cast<int>(sum_abs / samples.size());
  return avg_abs > 180 && peak > 230;
}

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

namespace audiosub::asr {

WhisperCppEngine::WhisperCppEngine(std::string model_path)
    : model_path_(std::move(model_path)) {}

WhisperCppEngine::~WhisperCppEngine() {
  if (ctx_) {
    whisper_free(ctx_);
    ctx_ = nullptr;
  }
}

bool WhisperCppEngine::Initialize() {
  if (ctx_) return true;

  whisper_context_params cparams = whisper_context_default_params();
  // 先固定走 CPU，避免没有 GPU 的机器接入阶段不稳定。
  cparams.use_gpu = false;

  ctx_ = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
  if (!ctx_) {
    std::cerr << "[asr] whisper_init_from_file failed: " << model_path_ << "\n";
    return false;
  }
  std::cerr << "[asr] whisper model loaded: " << model_path_ << "\n";
  return true;
}

void WhisperCppEngine::PushAudio(const core::PcmFrame& frame) {
  if (!ctx_ || !consumer_) return;
  // 上游必须传 16k / mono / 16bit。否则直接丢弃，避免 whisper 跑出乱七八糟的结果。
  if (frame.sample_rate != kTargetSampleRate ||
      frame.channels != 1 ||
      frame.bits_per_sample != 16 ||
      frame.samples.empty()) {
    return;
  }

  // 这一帧是不是有人在说话？仅用于驱动 VAD 状态机，**音频本身仍然要入缓冲**。
  // 不要直接丢静音帧，否则一句话说完后的停顿不会进入缓存，
  // 会导致上一句话要等下一句话补足缓存后才显示。
  const bool has_speech = HasSpeechLikeEnergy(frame.samples);

  // 一段新音频开始进入 ASR 缓冲时，记录现实时间。后续字幕起止时间
  // 用这两个 wall-clock 时间展示，而不是用 0s/4s/8s 这种相对段号。
  if (pending_pcm_.empty()) {
    pending_start_wall_ms_ = NowUnixMs();
  }

  pending_pcm_.reserve(pending_pcm_.size() + frame.samples.size());
  for (int16_t s : frame.samples) {
    pending_pcm_.push_back(static_cast<float>(s) / 32768.0f);
  }

  if (has_speech) {
    segment_has_speech_ = true;
    silence_frame_count_ = 0;
  } else if (segment_has_speech_) {
    ++silence_frame_count_;
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
    pending_start_wall_ms_ = 0;
    segment_has_speech_ = false;
    silence_frame_count_ = 0;
  }
}

void WhisperCppEngine::RunInference() {
  if (!ctx_ || !consumer_ || pending_pcm_.empty()) return;

  whisper_full_params params =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.print_realtime   = false;
  params.print_progress   = false;
  params.print_timestamps = false;
  params.print_special    = false;
  params.translate        = false;
  params.no_context       = true;
  params.single_segment   = true;
  params.n_threads        = 4;
  params.language         = "zh";
  params.initial_prompt   = nullptr;
  // 抑制非语音 token，并保留 whisper 内部的 no-speech 判定阈值，
  // 用来过滤静音/噪声下的幻觉字幕（如“谢谢观看”）。
  params.suppress_nst     = true;
  params.no_speech_thold  = 0.6f;

  const int sample_count =
      std::min(static_cast<int>(pending_pcm_.size()), kSegmentSamples);

  // 能量门限：没人说话时 B 端持续收到静音/底噪 PCM，Whisper 会每隔几秒
  // 幻觉出“谢谢 / 拜拜 / 谢谢再见”等高频词。这里用整段的平均振幅 + 峰值
  // 双重门限来判断“这一段到底有没有人真的在说话”：
  //   - 静音/底噪：平均振幅极低（约 0.0001~0.002），即使偶有小尖峰；
  //   - 真实人声：平均振幅明显更高（约 0.008+）。
  // 用平均振幅做主判据，比单看峰值更能挡住静音幻觉。
  {
    float max_abs = 0.f;
    double sum_abs = 0.0;
    for (int i = 0; i < sample_count; ++i) {
      const float v = std::fabs(pending_pcm_[i]);
      sum_abs += v;
      if (v > max_abs) max_abs = v;
    }
    const double avg_abs = sum_abs / std::max(1, sample_count);
    constexpr double kMinAvgAmplitude = 0.004;  // 平均振幅门限（主判据）
    constexpr float kMinPeakAmplitude = 0.02f;  // 峰值门限（兜底）
    if (avg_abs < kMinAvgAmplitude || max_abs < kMinPeakAmplitude) {
      return;
    }
  }

  // 测识别推理耗时，作为端到端字幕延迟指标（用 steady_clock，单端精确、
  // 不受墙钟跳变影响）。这段音频备齐到字幕产出的延迟主要就是推理耗时。
  const auto infer_t0 = std::chrono::steady_clock::now();
  const int ret =
      whisper_full(ctx_, params, pending_pcm_.data(), sample_count);
  const auto infer_t1 = std::chrono::steady_clock::now();
  const int64_t infer_latency_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(infer_t1 - infer_t0)
          .count();
  if (ret != 0) {
    std::cerr << "[asr] whisper_full failed, ret=" << ret << "\n";
    return;
  }

  const int n_segments = whisper_full_n_segments(ctx_);
  std::string text;
  float max_no_speech = 0.f;
  for (int i = 0; i < n_segments; ++i) {
    const float nsp = whisper_full_get_segment_no_speech_prob(ctx_, i);
    if (nsp > max_no_speech) max_no_speech = nsp;
    const char* seg_text = whisper_full_get_segment_text(ctx_, i);
    if (seg_text) text += seg_text;
  }

  // whisper 判定这一段大概率是无语音（静音/背景噪声），直接丢弃。
  // 这能通用地拦住“谢谢观看”等低能量输入下的幻觉字幕，且不误伤真实语音。
  constexpr float kNoSpeechThreshold = 0.6f;
  if (n_segments > 0 && max_no_speech > kNoSpeechThreshold) {
    std::cerr << "[asr] drop hallucination, no_speech_prob=" << max_no_speech
              << "\n";
    return;
  }
  if (text.empty()) return;

  // 强制繁体转简体。一定要在输出字幕前做。
  text = ConvertTraditionalToSimplifiedUtf8(text);

  // 过滤 Whisper 在低能量噪声下常见的幻觉字幕。
  // 注意：这里写简体词，因为上面已经统一转成简体。
  if (text.find("字幕") != std::string::npos ||
      text.find("制作") != std::string::npos ||
      text.find("贝尔") != std::string::npos ||
      text.find("叹气") != std::string::npos ||
      text.find("音乐") != std::string::npos ||
      text.find("掌声") != std::string::npos ||
      text.find("谢谢观看") != std::string::npos ||
      text.find("谢谢大家") != std::string::npos ||
      text.find("请订阅") != std::string::npos ||
      text.find("点赞") != std::string::npos) {
    return;
  }

  // 简单去重：完全相同的文本只输出一次。
  if (text == last_text_) return;
  last_text_ = text;

  const int64_t start_ms =
      pending_start_wall_ms_ > 0 ? pending_start_wall_ms_ : NowUnixMs();
  const int64_t end_ms = NowUnixMs();

  core::SubtitleSegment seg;
  seg.start_ms = start_ms;
  seg.end_ms = end_ms;
  seg.text = text;
  seg.is_final = true;
  seg.latency_ms = infer_latency_ms;
  consumer_->OnSubtitleSegment(seg);
}

}  // namespace audiosub::asr
