#include <atomic>
#include <chrono>
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
#include "audiosub/audio/audio_resampler.h"
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
      << "  A: captures microphone audio and sends via WebRTC\n"
      << "  B: receives audio, runs ASR, prints subtitles\n"
      << "\n"
      << "Options:\n"
      << "  --model <path>  whisper model file (default: models/ggml-small.bin)\n"
      << "  --lang <code>   ASR language (default: auto, try zh for Chinese)\n"
      << "\n"
      << "Type /quit to exit.\n";
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

  audiosub::PeerConnectionClient pc;
  if (!pc.Initialize()) {
    std::cerr << "Initialize() failed\n";
    return 2;
  }

  std::unique_ptr<audiosub::audio::PcmRingBuffer> ring_buffer;
  std::unique_ptr<audiosub::RemoteAudioSink> remote_sink;
  std::unique_ptr<audiosub::audio::AudioResampler> resampler;
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
  } else {
    ring_buffer = std::make_unique<audiosub::audio::PcmRingBuffer>(200);
    remote_sink =
        std::make_unique<audiosub::RemoteAudioSink>(*ring_buffer);
    resampler = std::make_unique<audiosub::audio::AudioResampler>(16000, 1);

    subtitle_consumer =
        std::make_unique<audiosub::ui::ConsoleSubtitleConsumer>(&g_print_mutex);

    asr_engine =
        std::make_unique<audiosub::asr::WhisperASREngine>(args.model_path,
                                                           args.language);
    asr_engine->SetSubtitleConsumer(subtitle_consumer.get());
    if (!asr_engine->Initialize()) {
      std::cerr << "ASR initialize failed\n";
      return 2;
    }

    pc.SetAudioTrackCallback([&](webrtc::AudioTrackInterface* track) {
      remote_audio_track = track;
      track->AddSink(remote_sink.get());
    });

    audio_running = true;
    audio_consume_thread = std::thread(
        [&ring_buffer, &resampler, &asr_engine, &audio_running]() {
          while (audio_running.load()) {
            auto frame = ring_buffer->WaitPop();
            if (!frame) break;

            auto converted = resampler->Process(*frame);
            asr_engine->PushAudio(converted);
          }
        });
  }

  audiosub::SignalingClient signaling;

  pc.SetSdpReadyCallback(
      [&signaling](webrtc::SdpType type, const std::string& sdp) {
        std::string type_str =
            (type == webrtc::SdpType::kOffer) ? "offer" : "answer";
        nlohmann::json msg = {{"type", type_str}, {"sdp", sdp}};
        signaling.Send(msg);
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
    if (state == "pc:connected") {
      Println("connected");
#ifdef _WIN32
      MuteProcessAudioOutput();
#endif
    }
    if (state == "pc:disconnected" || state == "pc:failed" ||
        state == "pc:closed") {
      should_exit = true;
      audio_running = false;
      if (ring_buffer) ring_buffer->Close();
    }
  });

  signaling.SetMessageHandler(
      [&pc, is_offerer, &should_exit, &ring_buffer, &audio_running](
          const nlohmann::json& msg) {
        std::string type = msg.value("type", "");

        if (type == "peer_ready") {
          Println(std::string("peer ") + msg.value("peer", "?") + " online");
          if (is_offerer) {
            pc.CreateOfferAndDataChannel();
          }

        } else if (type == "peer_left") {
          Println(std::string("peer ") + msg.value("peer", "?") + " left");
          should_exit = true;
          audio_running = false;
          if (ring_buffer) ring_buffer->Close();

        } else if (type == "offer") {
          pc.SetRemoteSdp(webrtc::SdpType::kOffer, msg.value("sdp", ""));
          pc.CreateAnswer();

        } else if (type == "answer") {
          pc.SetRemoteSdp(webrtc::SdpType::kAnswer, msg.value("sdp", ""));

        } else if (type == "candidate") {
          pc.AddRemoteIceCandidate(msg.value("sdpMid", ""),
                                   msg.value("sdpMLineIndex", 0),
                                   msg.value("candidate", ""));
        }
      });

  if (!signaling.Connect(args.host, args.port, args.id)) {
    return 3;
  }

  std::cout << "Role: " << (is_offerer ? "A (mic)" : "B (asr)")
            << "  /quit to exit\n> " << std::flush;

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
