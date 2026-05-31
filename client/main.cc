// main.cc
// =======
// audiosub_client.exe 的入口。
//
// Stage 4: WebRTC 音频链路 + 音频处理 + 实时语音转写
//   A端: 麦克风采集 → AudioTrack → WebRTC P2P → B端
//   B端: AudioTrackSink → PcmRingBuffer → AudioPipeline(重采样+声道转换)
//              → WhisperASREngine → ConsoleSubtitleConsumer
//
// 数据流:
//
//   A端: 麦克风 → ADM → AudioSource → AudioTrack → PeerConnection
//                                                              ↓ P2P
//   B端: PeerConnection → AudioTrack → RemoteAudioSink::OnData()
//              → PcmFrame(48kHz/stereo) → PcmRingBuffer
//              → AudioPipeline(48kHz→16kHz, stereo→mono)
//              → WhisperASREngine(工作线程, 5秒块式识别)
//              → SubtitleSegment → ConsoleSubtitleConsumer(控制台)
//
// 退出机制:
//   - 用户输入 /quit
//   - 对端离线（收到 peer_left 信令）
//   - WebRTC 连接断开（pc:disconnected / pc:failed / pc:closed）

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#endif

#include <nlohmann/json.hpp>

#include "api/media_stream_interface.h"
#include "audiosub/asr/whisper_asr_engine.h"
#include "audiosub/audio/audio_pipeline.h"
#include "audiosub/audio/pcm_ring_buffer.h"
#include "audiosub/ui/console_subtitle_consumer.h"
#include "peer_connection_client.h"
#include "remote_audio_sink.h"
#include "signaling_client.h"

namespace {

void PrintUsage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " --id <A|B> [--host 127.0.0.1] [--port 8888] "
         "[--model <path>] [--lang <auto|zh|en|...>]\n"
      << "\n"
      << "Stage 4: WebRTC audio link + real-time ASR subtitles.\n"
      << "  A: captures microphone audio and sends via WebRTC\n"
      << "  B: receives audio, resamples to 16kHz/mono, runs ASR, prints subtitles\n"
      << "\n"
      << "Role:\n"
      << "  A: offerer (creates AudioTrack, sends Offer)\n"
      << "  B: answerer (waits for Offer, receives + processes audio + ASR)\n"
      << "\n"
      << "Options:\n"
      << "  --model <path>  whisper model file (default: models/ggml-small.bin)\n"
      << "  --lang <code>   ASR language (default: auto, try zh for Chinese)\n"
      << "\n"
      << "Type /quit to exit. Program also exits when peer disconnects.\n";
}

struct Args {
  std::string id;
  std::string host = "127.0.0.1";
  int port = 8888;
  std::string model_path;
  std::string language = "auto";
};

bool ParseArgs(int argc, char** argv, Args* out) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--id") {
      if (const char* v = next("--id")) out->id = v; else return false;
    } else if (a == "--host") {
      if (const char* v = next("--host")) out->host = v; else return false;
    } else if (a == "--port") {
      if (const char* v = next("--port")) out->port = std::atoi(v); else return false;
    } else if (a == "--model") {
      if (const char* v = next("--model")) out->model_path = v; else return false;
    } else if (a == "--lang") {
      if (const char* v = next("--lang")) out->language = v; else return false;
    } else if (a == "-h" || a == "--help") {
      return false;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      return false;
    }
  }
  return !out->id.empty();
}

std::mutex g_print_mutex;

void Println(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_print_mutex);
  std::cout << "\r" << s << "\n> " << std::flush;
}

#ifdef _WIN32
void MuteProcessAudioOutput() {
  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&enumerator);
  if (FAILED(hr)) return;

  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(hr)) {
    enumerator->Release();
    return;
  }

  IAudioSessionManager* manager = nullptr;
  hr = device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                        (void**)&manager);
  if (FAILED(hr)) {
    device->Release();
    enumerator->Release();
    return;
  }

  ISimpleAudioVolume* volume = nullptr;
  hr = manager->GetSimpleAudioVolume(nullptr, 0, &volume);
  if (FAILED(hr)) {
    manager->Release();
    device->Release();
    enumerator->Release();
    return;
  }

  volume->SetMute(TRUE, nullptr);
  std::cerr << "[audio] process audio session muted\n";

  volume->Release();
  manager->Release();
  device->Release();
  enumerator->Release();
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 1;
  }

  if (args.model_path.empty()) {
    args.model_path = "models/ggml-small.bin";
  }

  const bool is_offerer = (args.id == "A");

  // === Step 1: Initialize WebRTC ===
  audiosub::PeerConnectionClient pc;
  if (!pc.Initialize()) {
    std::cerr << "PeerConnectionClient::Initialize() failed\n";
    return 2;
  }

  // === Step 2: Set up audio pipeline ===
  std::unique_ptr<audiosub::audio::PcmRingBuffer> ring_buffer;
  std::unique_ptr<audiosub::RemoteAudioSink> remote_sink;
  std::unique_ptr<audiosub::audio::AudioPipeline> pipeline;
  std::unique_ptr<audiosub::asr::WhisperASREngine> asr_engine;
  std::unique_ptr<audiosub::ui::ConsoleSubtitleConsumer> subtitle_consumer;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> remote_audio_track;
  std::thread audio_consume_thread;
  std::atomic<bool> audio_running{false};
  std::atomic<bool> should_exit{false};

  if (is_offerer) {
    if (!pc.AddAudioTrack()) {
      std::cerr << "AddAudioTrack() failed\n";
      return 2;
    }
    Println("[audio] microphone audio track added (A)");
  } else {
    ring_buffer = std::make_unique<audiosub::audio::PcmRingBuffer>(200);
    remote_sink =
        std::make_unique<audiosub::RemoteAudioSink>(*ring_buffer);
    pipeline = std::make_unique<audiosub::audio::AudioPipeline>(16000, 1);

    subtitle_consumer =
        std::make_unique<audiosub::ui::ConsoleSubtitleConsumer>(&g_print_mutex);

    asr_engine =
        std::make_unique<audiosub::asr::WhisperASREngine>(args.model_path,
                                                           args.language);
    asr_engine->SetSubtitleConsumer(subtitle_consumer.get());
    if (!asr_engine->Initialize()) {
      std::cerr << "[asr] failed to initialize whisper engine\n";
      return 2;
    }
    Println("[asr] whisper engine initialized (lang=" + args.language + ")");

    pc.SetAudioTrackCallback([&](webrtc::AudioTrackInterface* track) {
      remote_audio_track = track;
      track->AddSink(remote_sink.get());
      Println("[audio] remote audio track received, sink attached (B)");
    });

    audio_running = true;
    audio_consume_thread = std::thread(
        [&ring_buffer, &pipeline, &asr_engine, &audio_running]() {
          while (audio_running.load()) {
            auto frame = ring_buffer->WaitPop();
            if (!frame) break;

            auto converted = pipeline->resampler().Process(*frame);
            asr_engine->PushAudio(converted);
          }
        });
  }

  audiosub::SignalingClient signaling;

  // === Step 3: Wire WebRTC callbacks to signaling ===

  pc.SetSdpReadyCallback(
      [&signaling](webrtc::SdpType type, const std::string& sdp) {
        std::string type_str =
            (type == webrtc::SdpType::kOffer) ? "offer" : "answer";
        nlohmann::json msg = {{"type", type_str}, {"sdp", sdp}};
        signaling.Send(msg);
        Println(std::string("[pc] local ") + type_str + " sent (" +
                std::to_string(sdp.size()) + " bytes)");
      });

  pc.SetIceCandidateCallback(
      [&signaling](const std::string& candidate, const std::string& mid,
                   int mline) {
        nlohmann::json msg = {{"type", "candidate"},
                              {"candidate", candidate},
                              {"sdpMid", mid},
                              {"sdpMLineIndex", mline}};
        signaling.Send(msg);
      });

  pc.SetStateCallback([&](const std::string& state) {
    Println(std::string("[state] ") + state);
    if (state == "pc:connected") {
#ifdef _WIN32
      MuteProcessAudioOutput();
#endif
    }
    if (state == "pc:disconnected" || state == "pc:failed" ||
        state == "pc:closed") {
      Println("[pc] peer connection lost, exiting...");
      should_exit = true;
      audio_running = false;
      if (ring_buffer) ring_buffer->Close();
    }
  });

  // === Step 4: Wire signaling callbacks to WebRTC ===
  signaling.SetMessageHandler(
      [&pc, is_offerer, &should_exit, &ring_buffer, &audio_running](
          const nlohmann::json& msg) {
        std::string type = msg.value("type", "");

        if (type == "peer_ready") {
          Println(std::string("[peer] ") + msg.value("peer", "?") +
                  " is online");
          if (is_offerer) {
            Println("[pc] creating Offer + DataChannel...");
            pc.CreateOfferAndDataChannel();
          }

        } else if (type == "peer_left") {
          Println(std::string("[peer] ") + msg.value("peer", "?") +
                  " left, exiting...");
          should_exit = true;
          audio_running = false;
          if (ring_buffer) ring_buffer->Close();

        } else if (type == "offer") {
          Println("[pc] received Offer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kOffer, msg.value("sdp", ""));
          pc.CreateAnswer();

        } else if (type == "answer") {
          Println("[pc] received Answer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kAnswer, msg.value("sdp", ""));

        } else if (type == "candidate") {
          pc.AddRemoteIceCandidate(msg.value("sdpMid", ""),
                                   msg.value("sdpMLineIndex", 0),
                                   msg.value("candidate", ""));

        } else {
          Println(std::string("[signal] unhandled type=") + type);
        }
      });

  // === Step 5: Connect to signaling server ===
  if (!signaling.Connect(args.host, args.port, args.id)) {
    return 3;
  }

  std::cout << "Role: " << (is_offerer ? "A (offerer, mic capture)" : "B (answerer, audio receive + ASR)")
            << "\n"
            << "Waiting for peer. Once both peers are online, the offerer "
               "will start.\n"
            << "Type /quit to exit. Program also exits when peer disconnects.\n"
            << "> " << std::flush;

  // === Step 6: stdin reader thread + main wait loop ===
  std::thread stdin_thread([&]() {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "/quit" || line == "/exit") {
        should_exit = true;
        break;
      }
      std::lock_guard<std::mutex> lock(g_print_mutex);
      std::cout << "> " << std::flush;
    }
  });

  while (!should_exit.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // === Step 7: Cleanup ===
  if (asr_engine) asr_engine->Stop();
  if (remote_audio_track) {
    remote_audio_track->RemoveSink(remote_sink.get());
    remote_audio_track = nullptr;
  }
  audio_running = false;
  if (ring_buffer) ring_buffer->Close();
  if (audio_consume_thread.joinable()) audio_consume_thread.join();

  signaling.Close();
  pc.Close();

  if (stdin_thread.joinable()) stdin_thread.detach();

  std::cout << "bye.\n";
  return 0;
}
