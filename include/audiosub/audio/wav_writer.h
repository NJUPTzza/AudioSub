#pragma once

// wav_writer.h
// ============
// 一个极简的 16-bit PCM WAV 文件写入器，用于：
//   1. 麦克风自测时把采到的 PCM 落盘，便于用任意播放器回放确认
//   2. 把对端解码出的 PCM 落盘做调试
//
// 设计目标：
//   - 不依赖外部库，只依赖标准 C++ 文件 I/O
//   - 调用方在音频线程上 push 帧，本类内部自己加锁
//   - 第一帧到达时根据 sample_rate / channels 确定头部参数，之后
//     不允许变更（防止文件结构错乱）
//   - 析构 / Close 时回写 WAV 头里的 size 字段
//
// 注意：这是给调试用的工具，不追求高性能；如果将来要扩展更多格式，
//       可以独立成单独的 audio_io 模块。

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "audiosub/core/types.h"

namespace audiosub::audio {

// 故意用 C 的 FILE* 而不是 std::ofstream，避免拉进异常支持代码
// （项目用 /EHs-c- 禁用异常，std::ofstream 会触发 C4530 警告）。
class WavWriter {
 public:
  WavWriter() = default;
  ~WavWriter();

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  // 打开输出文件并预留 44 字节 WAV 头，真正的 sample_rate / channels
  // 在第一帧到达时确定。返回 false 表示打不开文件。
  bool Open(const std::string& path);

  // 追加一帧。帧必须是 16-bit PCM。第一帧之后的帧若 sample_rate /
  // channels 不一致会被丢弃（同时打印一行 warn）。
  bool Append(const core::PcmFrame& frame);

  // 显式关闭，回写 WAV 头。析构会兜底再调一次。
  void Close();

  bool is_open() const;

 private:
  void WriteHeader(int sample_rate, int channels);
  void PatchSizes();

  mutable std::mutex mutex_;
  std::FILE* file_ = nullptr;
  bool header_written_ = false;
  int sample_rate_ = 0;
  int channels_ = 0;
  uint32_t data_bytes_ = 0;  // 写出的纯 PCM 字节数
};

}  // namespace audiosub::audio
