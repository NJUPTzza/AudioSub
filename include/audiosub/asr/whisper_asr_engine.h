#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audiosub/core/interfaces.h"
#include "audiosub/core/types.h"

struct whisper_context;

namespace audiosub::asr {

class WhisperASREngine : public core::IASREngine {
 public:
  explicit WhisperASREngine(const std::string& model_path,
                            const std::string& language = "auto",
                            int chunk_duration_ms = 5000);
  ~WhisperASREngine() override;

  bool Initialize();

  void PushAudio(const core::PcmFrame& frame) override;
  void SetSubtitleConsumer(core::ISubtitleConsumer* consumer) override;

  void Stop();

 private:
  void WorkerLoop();
  bool ProcessChunk(const std::vector<float>& pcm, int64_t start_ms);
  static float ComputeRms(const std::vector<float>& pcm);

  std::string model_path_;
  std::string language_;
  int chunk_duration_ms_;
  int chunk_samples_;

  whisper_context* ctx_ = nullptr;
  core::ISubtitleConsumer* consumer_ = nullptr;

  std::mutex buffer_mutex_;
  std::vector<float> audio_buffer_;
  int64_t buffer_start_ms_ = 0;

  std::atomic<bool> running_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_cv_;
  std::thread worker_thread_;
};

}  // namespace audiosub::asr
