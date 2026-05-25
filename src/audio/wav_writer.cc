#include "audiosub/audio/wav_writer.h"

#include <cstdio>
#include <cstring>

namespace audiosub::audio {

namespace {

void WriteU32LE(unsigned char* dst, uint32_t value) {
  dst[0] = static_cast<unsigned char>(value & 0xff);
  dst[1] = static_cast<unsigned char>((value >> 8) & 0xff);
  dst[2] = static_cast<unsigned char>((value >> 16) & 0xff);
  dst[3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

void WriteU16LE(unsigned char* dst, uint16_t value) {
  dst[0] = static_cast<unsigned char>(value & 0xff);
  dst[1] = static_cast<unsigned char>((value >> 8) & 0xff);
}

}  // namespace

WavWriter::~WavWriter() { Close(); }

bool WavWriter::Open(const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ != nullptr) {
    return false;
  }
#if defined(_MSC_VER)
  // MSVC 推荐 fopen_s。
  if (fopen_s(&file_, path.c_str(), "wb") != 0) {
    file_ = nullptr;
  }
#else
  file_ = std::fopen(path.c_str(), "wb");
#endif
  if (file_ == nullptr) {
    std::fprintf(stderr, "[wav] open failed: %s\n", path.c_str());
    return false;
  }
  // 先占位 44 字节，header 真正参数在第一帧到达时再回填。
  unsigned char placeholder[44] = {0};
  std::fwrite(placeholder, 1, sizeof(placeholder), file_);
  return true;
}

bool WavWriter::is_open() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_ != nullptr;
}

void WavWriter::WriteHeader(int sample_rate, int channels) {
  const uint16_t bits_per_sample = 16;
  const uint16_t block_align =
      static_cast<uint16_t>(channels * bits_per_sample / 8);
  const uint32_t byte_rate =
      static_cast<uint32_t>(sample_rate) * block_align;

  unsigned char header[44];
  std::memcpy(header + 0, "RIFF", 4);
  WriteU32LE(header + 4, 0);  // 整体长度，先占位
  std::memcpy(header + 8, "WAVE", 4);
  std::memcpy(header + 12, "fmt ", 4);
  WriteU32LE(header + 16, 16);                                    // fmt chunk size
  WriteU16LE(header + 20, 1);                                     // PCM
  WriteU16LE(header + 22, static_cast<uint16_t>(channels));
  WriteU32LE(header + 24, static_cast<uint32_t>(sample_rate));
  WriteU32LE(header + 28, byte_rate);
  WriteU16LE(header + 32, block_align);
  WriteU16LE(header + 34, bits_per_sample);
  std::memcpy(header + 36, "data", 4);
  WriteU32LE(header + 40, 0);  // data chunk size，先占位

  // 回到文件开头，覆盖之前占位的 44 字节。
  long cur = std::ftell(file_);
  std::fseek(file_, 0, SEEK_SET);
  std::fwrite(header, 1, sizeof(header), file_);
  if (cur > 0) {
    std::fseek(file_, cur, SEEK_SET);
  }
}

bool WavWriter::Append(const core::PcmFrame& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ == nullptr) {
    return false;
  }
  if (frame.bits_per_sample != 16) {
    std::fprintf(stderr, "[wav] only 16-bit PCM supported, got %d\n",
                 frame.bits_per_sample);
    return false;
  }
  if (!header_written_) {
    sample_rate_ = frame.sample_rate;
    channels_ = frame.channels;
    WriteHeader(sample_rate_, channels_);
    header_written_ = true;
  } else if (frame.sample_rate != sample_rate_ ||
             frame.channels != channels_) {
    std::fprintf(stderr,
                 "[wav] dropped frame: format changed from %dHz/%dch to "
                 "%dHz/%dch\n",
                 sample_rate_, channels_, frame.sample_rate, frame.channels);
    return false;
  }
  if (frame.samples.empty()) {
    return true;
  }
  const std::size_t bytes = frame.samples.size() * sizeof(int16_t);
  std::fwrite(frame.samples.data(), 1, bytes, file_);
  data_bytes_ += static_cast<uint32_t>(bytes);
  return true;
}

void WavWriter::PatchSizes() {
  if (file_ == nullptr || !header_written_) {
    return;
  }
  unsigned char tmp[4];
  WriteU32LE(tmp, data_bytes_ + 36);
  std::fseek(file_, 4, SEEK_SET);
  std::fwrite(tmp, 1, 4, file_);
  WriteU32LE(tmp, data_bytes_);
  std::fseek(file_, 40, SEEK_SET);
  std::fwrite(tmp, 1, 4, file_);
}

void WavWriter::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_ == nullptr) {
    return;
  }
  PatchSizes();
  std::fflush(file_);
  std::fclose(file_);
  file_ = nullptr;
}

}  // namespace audiosub::audio
