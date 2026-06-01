#include "wasapi_mic_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace audiosub {

using Microsoft::WRL::ComPtr;

WasapiMicCapture::WasapiMicCapture() = default;

WasapiMicCapture::~WasapiMicCapture() {
  Stop();
}

bool WasapiMicCapture::Start(PcmCallback callback) {
  if (running_) {
    return true;
  }
  callback_ = std::move(callback);
  running_ = true;
  thread_ = std::thread(&WasapiMicCapture::CaptureThreadMain, this);
  return true;
}

void WasapiMicCapture::Stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void WasapiMicCapture::CaptureThreadMain() {
  // WASAPI 必须在 COM 初始化后的线程上调用。
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_initialized = SUCCEEDED(hr);

  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    std::cerr << "[wasapi] CoInitializeEx failed, hr=0x"
              << std::hex << static_cast<unsigned long>(hr)
              << std::dec << "\n";
    running_ = false;
    return;
  }

  // 1. 拿到设备枚举器
  ComPtr<IMMDeviceEnumerator> enumerator;
  hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator),
      nullptr,
      CLSCTX_ALL,
      __uuidof(IMMDeviceEnumerator),
      reinterpret_cast<void**>(enumerator.GetAddressOf()));
  if (FAILED(hr)) {
    std::cerr << "[wasapi] CoCreateInstance(MMDeviceEnumerator) failed\n";
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }

  // 2. 拿到默认录音设备（eCapture = 录音 endpoint）
  ComPtr<IMMDevice> device;
  hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                           device.GetAddressOf());
  if (FAILED(hr)) {
    std::cerr << "[wasapi] GetDefaultAudioEndpoint failed\n";
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }

  // 3. Activate IAudioClient
  ComPtr<IAudioClient> audio_client;
  hr = device->Activate(
      __uuidof(IAudioClient),
      CLSCTX_ALL,
      nullptr,
      reinterpret_cast<void**>(audio_client.GetAddressOf()));
  if (FAILED(hr)) {
    std::cerr << "[wasapi] Activate(IAudioClient) failed\n";
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }

  // 4. 获取系统混音格式（通常是 48kHz / 2ch / 32-bit float）
  WAVEFORMATEX* mix_format = nullptr;
  hr = audio_client->GetMixFormat(&mix_format);
  if (FAILED(hr) || !mix_format) {
    std::cerr << "[wasapi] GetMixFormat failed\n";
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }
  std::cout << "[wasapi] mix format: sample_rate=" << mix_format->nSamplesPerSec
            << ", channels=" << mix_format->nChannels
            << ", bits=" << mix_format->wBitsPerSample
            << ", format_tag=" << mix_format->wFormatTag
            << "\n";

  // 5. 初始化共享模式，1 秒缓冲
  REFERENCE_TIME buffer_duration = 10000000;  // 1 秒 (100ns 单位)
  hr = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      0,
      buffer_duration,
      0,
      mix_format,
      nullptr);
  if (FAILED(hr)) {
    std::cerr << "[wasapi] IAudioClient::Initialize failed, hr=0x"
              << std::hex << static_cast<unsigned long>(hr)
              << std::dec << "\n";
    CoTaskMemFree(mix_format);
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }

  // 6. 拿到 capture client（用来 poll PCM 数据）
  ComPtr<IAudioCaptureClient> capture_client;
  hr = audio_client->GetService(
      __uuidof(IAudioCaptureClient),
      reinterpret_cast<void**>(capture_client.GetAddressOf()));
  if (FAILED(hr)) {
    std::cerr << "[wasapi] GetService(IAudioCaptureClient) failed\n";
    CoTaskMemFree(mix_format);
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }

  hr = audio_client->Start();
  if (FAILED(hr)) {
    std::cerr << "[wasapi] audio_client->Start failed\n";
    CoTaskMemFree(mix_format);
    running_ = false;
    if (com_initialized) CoUninitialize();
    return;
  }

  std::cout << "[wasapi] capture started\n";

  // 7. 主循环：轮询 packet -> 转 mono int16 -> 加增益 -> 回调
  while (running_) {
    UINT32 packet_frames = 0;
    hr = capture_client->GetNextPacketSize(&packet_frames);
    if (FAILED(hr)) {
      std::cerr << "[wasapi] GetNextPacketSize failed\n";
      break;
    }
    if (packet_frames == 0) {
      Sleep(5);
      continue;
    }

    BYTE* data = nullptr;
    UINT32 frames_available = 0;
    DWORD flags = 0;
    hr = capture_client->GetBuffer(&data, &frames_available, &flags,
                                   nullptr, nullptr);
    if (FAILED(hr)) {
      std::cerr << "[wasapi] GetBuffer failed\n";
      break;
    }

    const int channels = static_cast<int>(mix_format->nChannels);
    const int sample_rate = static_cast<int>(mix_format->nSamplesPerSec);

    std::vector<int16_t> pcm_mono;
    pcm_mono.reserve(frames_available);

    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
      // 系统标记这块是静音 -> 写 0
      pcm_mono.assign(frames_available, 0);
    } else if (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
               (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                mix_format->wBitsPerSample == 32)) {
      // float32 多通道 -> 平均混 mono -> int16
      const float* input = reinterpret_cast<const float*>(data);
      for (UINT32 f = 0; f < frames_available; ++f) {
        float sum = 0.0f;
        for (int ch = 0; ch < channels; ++ch) {
          sum += input[f * channels + ch];
        }
        float mono = sum / static_cast<float>(channels);
        mono = std::clamp(mono, -1.0f, 1.0f);
        pcm_mono.push_back(static_cast<int16_t>(mono * 32767.0f));
      }
    } else if (mix_format->wBitsPerSample == 16) {
      // 已经是 int16 -> 直接平均混 mono
      const int16_t* input = reinterpret_cast<const int16_t*>(data);
      for (UINT32 f = 0; f < frames_available; ++f) {
        int sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
          sum += input[f * channels + ch];
        }
        pcm_mono.push_back(static_cast<int16_t>(sum / channels));
      }
    } else {
      std::cerr << "[wasapi] unsupported format: bits="
                << mix_format->wBitsPerSample
                << ", tag=" << mix_format->wFormatTag << "\n";
    }

    // 软增益 + 防削波。
    // 参考仓库用的是 8x，但那是按他们的麦克风调的；本机系统采集音量本来就不低，
    // 8x 会把说话整段削顶到 ±32767（log 里 max_abs 一直=1.0 → 失真方波），
    // Whisper 听到的就不是语音而是噪声，所以会蒙出"拜拜""问问问人能否听到"这种幻觉。
    // 这里改成温和的 2x，并且在中间过程用 int 防溢出，最后才 clamp 到 int16。
    const float gain = 2.0f;
    for (int16_t& s : pcm_mono) {
      int v = static_cast<int>(std::lround(static_cast<float>(s) * gain));
      v = std::clamp(v, -32768, 32767);
      s = static_cast<int16_t>(v);
    }

    if (!pcm_mono.empty() && callback_) {
      callback_(pcm_mono.data(), pcm_mono.size(), sample_rate, /*channels=*/1);
    }

    capture_client->ReleaseBuffer(frames_available);
  }

  audio_client->Stop();
  CoTaskMemFree(mix_format);
  if (com_initialized) CoUninitialize();
  std::cout << "[wasapi] capture stopped\n";
}

}  // namespace audiosub
