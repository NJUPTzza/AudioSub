#include "wasapi_mic_capture.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#define NOMINMAX
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <objbase.h>
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
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_initialized = SUCCEEDED(hr);

        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            std::cerr << "[wasapi] CoInitializeEx failed, hr=0x"
                << std::hex << static_cast<unsigned long>(hr)
                << std::dec << "\n";
            running_ = false;
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(enumerator.GetAddressOf()));

        if (FAILED(hr)) {
            std::cerr << "[wasapi] CoCreateInstance(MMDeviceEnumerator) failed, hr=0x"
                << std::hex << static_cast<unsigned long>(hr)
                << std::dec << "\n";
            running_ = false;
            if (com_initialized) CoUninitialize();
            return;
        }

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.GetAddressOf());

        if (FAILED(hr)) {
            std::cerr << "[wasapi] GetDefaultAudioEndpoint failed, hr=0x"
                << std::hex << static_cast<unsigned long>(hr)
                << std::dec << "\n";
            running_ = false;
            if (com_initialized) CoUninitialize();
            return;
        }

        ComPtr<IAudioClient> audio_client;
        hr = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(audio_client.GetAddressOf()));

        if (FAILED(hr)) {
            std::cerr << "[wasapi] Activate(IAudioClient) failed, hr=0x"
                << std::hex << static_cast<unsigned long>(hr)
                << std::dec << "\n";
            running_ = false;
            if (com_initialized) CoUninitialize();
            return;
        }

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

        REFERENCE_TIME buffer_duration = 10000000;  // 1 second

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

        ComPtr<IAudioCaptureClient> capture_client;
        hr = audio_client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(capture_client.GetAddressOf()));

        if (FAILED(hr)) {
            std::cerr << "[wasapi] GetService(IAudioCaptureClient) failed, hr=0x"
                << std::hex << static_cast<unsigned long>(hr)
                << std::dec << "\n";
            CoTaskMemFree(mix_format);
            running_ = false;
            if (com_initialized) CoUninitialize();
            return;
        }

        hr = audio_client->Start();
        if (FAILED(hr)) {
            std::cerr << "[wasapi] audio_client->Start failed, hr=0x"
                << std::hex << static_cast<unsigned long>(hr)
                << std::dec << "\n";
            CoTaskMemFree(mix_format);
            running_ = false;
            if (com_initialized) CoUninitialize();
            return;
        }

        //std::cout << "[wasapi] capture started\n";

        int debug_count = 0;

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

            hr = capture_client->GetBuffer(
                &data,
                &frames_available,
                &flags,
                nullptr,
                nullptr);

            if (FAILED(hr)) {
                std::cerr << "[wasapi] GetBuffer failed\n";
                break;
            }

            const int channels = static_cast<int>(mix_format->nChannels);
            const int sample_rate = static_cast<int>(mix_format->nSamplesPerSec);

            std::vector<int16_t> pcm_mono;
            pcm_mono.reserve(frames_available);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pcm_mono.assign(frames_available, 0);
            }
            else if (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    mix_format->wBitsPerSample == 32)) {
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
            }
            else if (mix_format->wBitsPerSample == 16) {
                const int16_t* input = reinterpret_cast<const int16_t*>(data);

                for (UINT32 f = 0; f < frames_available; ++f) {
                    int sum = 0;
                    for (int ch = 0; ch < channels; ++ch) {
                        sum += input[f * channels + ch];
                    }

                    pcm_mono.push_back(static_cast<int16_t>(sum / channels));
                }
            }
            else {
                if (debug_count < 10) {
                    std::cerr << "[wasapi] unsupported format: bits="
                        << mix_format->wBitsPerSample
                        << ", tag=" << mix_format->wFormatTag << "\n";
                }
            }
            const float gain = 8.0f;

            for (int16_t& s : pcm_mono) {
                int v = static_cast<int>(std::lround(static_cast<float>(s) * gain));
                v = std::clamp(v, -32768, 32767);
                s = static_cast<int16_t>(v);
            }
            if (!pcm_mono.empty()) {
                int64_t sum_abs = 0;
                int peak = 0;

                for (int16_t s : pcm_mono) {
                    int v = std::abs(static_cast<int>(s));
                    sum_abs += v;
                    peak = std::max(peak, v);
                }

                int avg_abs = static_cast<int>(sum_abs / pcm_mono.size());

                //if (debug_count < 200) {
                //    std::cout << "[wasapi] avg_abs=" << avg_abs
                //        << ", peak=" << peak
                //        << ", samples=" << pcm_mono.size()
                //        << ", input_rate=" << sample_rate
                //        << "\n";
                //    ++debug_count;
                //}

                if (callback_) {
                    callback_(pcm_mono.data(),
                        pcm_mono.size(),
                        sample_rate,
                        1);
                }
            }

            capture_client->ReleaseBuffer(frames_available);
        }

        audio_client->Stop();

        CoTaskMemFree(mix_format);

        if (com_initialized) {
            CoUninitialize();
        }

        std::cout << "[wasapi] capture stopped\n";
    }

}  // namespace audiosub