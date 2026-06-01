// WASAPI 麦克风采集器
// =====================
// 用 Windows Core Audio API 直接采集默认麦克风的 raw PCM，
// 绕开 WebRTC 内置的 AudioProcessingModule（NS/AEC/AGC/HPF）。
//
// 为什么不走 WebRTC AudioTrack？
//   WebRTC 默认会对采集音频做强降噪。同机调试时，远端没有真实的扬声器
//   回声参考信号，NS 会把弱人声也削成底噪，导致 ASR 听不到清晰人声、
//   只能根据训练集先验幻觉成"谢谢收看"之类的高频短语。
//
// 数据流：WASAPI -> int16 mono -> callback -> 业务侧通过 DataChannel
//        把 raw PCM 发到对端 -> 对端原样喂给 ASR。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

namespace audiosub {

class WasapiMicCapture {
 public:
  // PCM 回调参数：
  //   samples       int16 mono PCM 缓冲
  //   sample_count  缓冲中样本数（不是字节数）
  //   sample_rate   采集采样率（通常 48000，跟系统 mix format 走）
  //   channels      回调里固定为 1（已经混成 mono）
  using PcmCallback = std::function<void(const int16_t* samples,
                                         std::size_t sample_count,
                                         int sample_rate,
                                         int channels)>;

  WasapiMicCapture();
  ~WasapiMicCapture();

  WasapiMicCapture(const WasapiMicCapture&) = delete;
  WasapiMicCapture& operator=(const WasapiMicCapture&) = delete;

  // 启动一个后台线程持续从默认麦克风读 PCM，每个 packet 调一次 callback。
  // 同一个对象重复 Start 会被忽略（保持当前 callback）。
  bool Start(PcmCallback callback);

  // 停止采集并 join 线程。析构时也会自动调用。
  void Stop();

 private:
  void CaptureThreadMain();

  std::atomic<bool> running_{false};
  std::thread thread_;
  PcmCallback callback_;
};

}  // namespace audiosub
