#include "audiosub/asr/whisper_asr_engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "ggml.h"
#include "whisper.h"

namespace {

void WhisperLogCallback(ggml_log_level level, const char* text, void*) {
  if (level >= GGML_LOG_LEVEL_WARN && text && text[0] != '\0') {
    std::cerr << text;
  }
}

}  // namespace

namespace audiosub::asr {

WhisperASREngine::WhisperASREngine(const std::string& model_path,
                                   const std::string& language,
                                   int chunk_duration_ms)
    : model_path_(model_path),
      language_(language),
      chunk_duration_ms_(chunk_duration_ms),
      chunk_samples_(16000 * chunk_duration_ms / 1000) {}

WhisperASREngine::~WhisperASREngine() {
  Stop();
  if (ctx_) {
    whisper_free(ctx_);
    ctx_ = nullptr;
  }
}

bool WhisperASREngine::Initialize() {
  whisper_log_set(WhisperLogCallback, nullptr);

  whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = false;

  ctx_ = whisper_init_from_file_with_params(model_path_.c_str(), cparams);
  if (!ctx_) {
    std::cerr << "failed to load whisper model: " << model_path_ << "\n";
    return false;
  }

  running_ = true;
  worker_thread_ = std::thread(&WhisperASREngine::WorkerLoop, this);

  return true;
}

void WhisperASREngine::PushAudio(const core::PcmFrame& frame) {
  if (!running_ || frame.samples.empty()) return;

  std::vector<float> float_samples;
  float_samples.reserve(frame.samples.size());
  for (auto s : frame.samples) {
    float_samples.push_back(static_cast<float>(s) / 32768.0f);
  }

  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (audio_buffer_.empty()) {
      buffer_start_ms_ = frame.timestamp_ms;
    }
    audio_buffer_.insert(audio_buffer_.end(), float_samples.begin(),
                         float_samples.end());
    if (static_cast<int>(audio_buffer_.size()) > kMaxBufferSamples) {
      int excess = static_cast<int>(audio_buffer_.size()) - kMaxBufferSamples;
      audio_buffer_.erase(audio_buffer_.begin(),
                          audio_buffer_.begin() + excess);
      buffer_start_ms_ += excess * 1000 / 16000;
    }
  }

  wake_cv_.notify_one();
}

void WhisperASREngine::SetSubtitleConsumer(core::ISubtitleConsumer* consumer) {
  consumer_ = consumer;
}

void WhisperASREngine::Stop() {
  running_ = false;
  wake_cv_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

float WhisperASREngine::ComputeRms(const std::vector<float>& pcm) {
  if (pcm.empty()) return 0.0f;
  double sum = 0.0;
  for (float s : pcm) {
    sum += static_cast<double>(s) * s;
  }
  return static_cast<float>(std::sqrt(sum / pcm.size()));
}

void WhisperASREngine::WorkerLoop() {
  while (running_.load()) {
    std::vector<float> chunk;
    int64_t chunk_start_ms = 0;

    {
      std::unique_lock<std::mutex> lock(wake_mutex_);
      wake_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] {
        std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
        return !running_ || static_cast<int>(audio_buffer_.size()) >= chunk_samples_;
      });
    }

    {
      std::lock_guard<std::mutex> lock(buffer_mutex_);
      if (static_cast<int>(audio_buffer_.size()) >= chunk_samples_) {
        chunk.assign(audio_buffer_.begin(),
                     audio_buffer_.begin() + chunk_samples_);
        chunk_start_ms = buffer_start_ms_;
        audio_buffer_.erase(audio_buffer_.begin(),
                            audio_buffer_.begin() + chunk_samples_);
        buffer_start_ms_ += chunk_duration_ms_;
      }
    }

    if (!chunk.empty()) {
      float rms = ComputeRms(chunk);
      if (rms < 0.002f) {
        continue;
      }
      ProcessChunk(chunk, chunk_start_ms);
    }
  }

  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!audio_buffer_.empty()) {
      float rms = ComputeRms(audio_buffer_);
      if (rms >= 0.002f) {
        ProcessChunk(audio_buffer_, buffer_start_ms_);
      }
      audio_buffer_.clear();
    }
  }
}

bool WhisperASREngine::ProcessChunk(const std::vector<float>& pcm,
                                    int64_t start_ms) {
  if (!ctx_ || pcm.empty()) return false;

  whisper_full_params wparams =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

  wparams.print_progress = false;
  wparams.print_special = false;
  wparams.print_realtime = false;
  wparams.print_timestamps = false;
  wparams.single_segment = false;
  wparams.no_timestamps = false;
  wparams.language = language_.c_str();
  wparams.suppress_blank = true;
  wparams.suppress_nst = true;
  wparams.no_speech_thold = 0.6f;

  if (whisper_full(ctx_, wparams, pcm.data(), pcm.size()) != 0) {
    return false;
  }

  int n_segments = whisper_full_n_segments(ctx_);

  for (int i = 0; i < n_segments; ++i) {
    const char* text = whisper_full_get_segment_text(ctx_, i);
    if (!text || text[0] == '\0') continue;

    int64_t t0 = whisper_full_get_segment_t0(ctx_, i);
    int64_t t1 = whisper_full_get_segment_t1(ctx_, i);

    int64_t seg_start_ms = start_ms + t0 * 10;
    int64_t seg_end_ms = start_ms + t1 * 10;

    std::string segment_text(text);
    while (!segment_text.empty() &&
           (segment_text.back() == ' ' || segment_text.back() == '\n')) {
      segment_text.pop_back();
    }
    if (segment_text.empty()) continue;

    core::SubtitleSegment seg;
    seg.start_ms = seg_start_ms;
    seg.end_ms = seg_end_ms;
    seg.text = segment_text;
    seg.is_final = true;

    if (consumer_) {
      consumer_->OnSubtitleSegment(seg);
    }
  }

  return true;
}

}  // namespace audiosub::asr
