// main.cc
// =======
// audiosub_client.exe 的入口。
//
// Stage 2: WebRTC 音频链路
//   A端: 麦克风采集 → AudioTrack → WebRTC P2P → B端
//   B端: AudioTrackSink → PcmRingBuffer → 控制台打印PCM帧信息
//
// 数据流:
//
//   A端: 麦克风 → ADM → AudioSource → AudioTrack → PeerConnection
//                                                              ↓ P2P
//   B端: PeerConnection → AudioTrack → RemoteAudioSink::OnData()
//              → PcmFrame → PcmRingBuffer → 打印线程

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "api/media_stream_interface.h"
#include "audiosub/audio/pcm_ring_buffer.h"
#include "peer_connection_client.h"
#include "remote_audio_sink.h"
#include "signaling_client.h"

namespace {

void PrintUsage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " --id <A|B> [--host 127.0.0.1] [--port 8888]\n"
      << "\n"
      << "Stage 2: WebRTC audio link demo.\n"
      << "  A: captures microphone audio and sends via WebRTC\n"
      << "  B: receives audio, prints PCM frame info to console\n"
      << "\n"
      << "Role:\n"
      << "  A: offerer (creates DataChannel + AudioTrack, sends Offer)\n"
      << "  B: answerer (waits for Offer, receives audio)\n"
      << "\n"
      << "Type any text + Enter to send over DataChannel. /quit to exit.\n";
}

struct Args {
  std::string id;
  std::string host = "127.0.0.1";
  int port = 8888;
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
  std::cout << "\r" << s << "\n[you] " << std::flush;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 1;
  }

  const bool is_offerer = (args.id == "A");

  // === Step 1: Initialize WebRTC ===
  audiosub::PeerConnectionClient pc;
  if (!pc.Initialize()) {
    std::cerr << "PeerConnectionClient::Initialize() failed\n";
    return 2;
  }

  // === Step 2: Set up audio pipeline ===
  // A端: add audio track for microphone capture
  // B端: set up audio receive pipeline (RingBuffer + Sink + print thread)
  std::unique_ptr<audiosub::audio::PcmRingBuffer> ring_buffer;
  std::unique_ptr<audiosub::RemoteAudioSink> remote_sink;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> remote_audio_track;
  std::thread audio_print_thread;
  std::atomic<bool> audio_running{false};

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

    pc.SetAudioTrackCallback([&](webrtc::AudioTrackInterface* track) {
      remote_audio_track = track;
      track->AddSink(remote_sink.get());
      Println("[audio] remote audio track received, sink attached (B)");
    });

    audio_running = true;
    audio_print_thread = std::thread([&]() {
      int64_t frame_count = 0;
      while (audio_running.load()) {
        auto frame = ring_buffer->WaitPop();
        if (!frame) break;
        frame_count++;
        if (frame_count <= 5 || frame_count % 100 == 0) {
          double duration_ms =
              static_cast<double>(frame->samples.size() / frame->channels) *
              1000.0 / frame->sample_rate;
          Println("[audio] frame #" + std::to_string(frame_count) +
                  ": " + std::to_string(frame->sample_rate) + "Hz " +
                  std::to_string(frame->channels) + "ch " +
                  std::to_string(frame->samples.size()) + "samples " +
                  "ts=" + std::to_string(frame->timestamp_ms) + "ms " +
                  "(" + std::to_string(duration_ms).substr(0, 5) +
                  "ms/frame)");
        }
      }
      Println("[audio] print thread stopped, total frames: " +
              std::to_string(frame_count));
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

  pc.SetMessageCallback([](const std::string& text) {
    Println(std::string("<peer> ") + text);
  });

  pc.SetStateCallback([](const std::string& state) {
    Println(std::string("[state] ") + state);
  });

  // === Step 4: Wire signaling callbacks to WebRTC ===
  signaling.SetMessageHandler(
      [&pc, is_offerer](const nlohmann::json& msg) {
        std::string type = msg.value("type", "");

        if (type == "peer_ready") {
          Println(std::string("[peer] ") + msg.value("peer", "?") +
                  " is online");
          if (is_offerer) {
            Println("[pc] creating Offer + DataChannel...");
            pc.CreateOfferAndDataChannel();
          }

        } else if (type == "peer_left") {
          Println(std::string("[peer] ") + msg.value("peer", "?") + " left");

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

  std::cout << "Role: " << (is_offerer ? "A (offerer, mic capture)" : "B (answerer, audio receive)")
            << "\n"
            << "Waiting for peer. Once both peers are online, the offerer "
               "will start.\n"
            << "Type messages and Enter to send. /quit to exit.\n"
            << "[you] " << std::flush;

  // === Step 6: Main loop (stdin) ===
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") break;
    if (line.empty()) {
      std::cout << "[you] " << std::flush;
      continue;
    }
    if (!pc.SendMessage(line)) {
      Println("(message dropped: data channel not open yet)");
    } else {
      std::cout << "[you] " << std::flush;
    }
  }

  // === Step 7: Cleanup ===
  if (remote_audio_track) {
    remote_audio_track->RemoveSink(remote_sink.get());
    remote_audio_track = nullptr;
  }
  audio_running = false;
  if (ring_buffer) ring_buffer->Close();
  if (audio_print_thread.joinable()) audio_print_thread.join();
  signaling.Close();
  pc.Close();
  std::cout << "bye.\n";
  return 0;
}
