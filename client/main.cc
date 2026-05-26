// main.cc
// =======
// audiosub_client.exe 的入口。这个文件做的事：
//   1. 解析命令行 --id A/B、--host、--port
//   2. 创建 PeerConnectionClient + SignalingClient
//   3. 把"WebRTC 生成的 SDP/ICE"转发到信令通道
//      把"信令通道收到的 SDP/ICE"喂回 WebRTC
//   4. 主线程跑 std::getline 读用户输入，调 SendMessage 发到 P2P
//
// 这里就是阶段 1b 的全部业务逻辑——大约 100 行。
//
// 流程示意：
//
//   stdin (用户输入)                stdout (打印)
//        |                              ^
//        v                              |
//   ┌────────────────────────────────────────┐
//   │ main 主线程：getline 循环              │
//   │   收到字符串 -> pc.SendMessage()       │
//   └────────────────────────────────────────┘
//                  |   ^
//                  v   |   (P2P 数据)
//   ┌──────────────────────────────────────────┐
//   │ PeerConnectionClient (内部 3 线程)       │
//   └──────────────────────────────────────────┘
//      ^回调                  | 回调
//      | SDP/ICE              v
//   ┌──────────────────────────────────────────┐
//   │ SignalingClient (1 后台 recv 线程)        │
//   └──────────────────────────────────────────┘
//          ^                      |
//          | TCP/JSON 上行         v 下行
//          +---- 信令服务器 -------+
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <utility>
#include <nlohmann/json.hpp>

#include "peer_connection_client.h"
#include "signaling_client.h"
#include "audiosub/audio/pcm_ring_buffer.h"
#include "audiosub/core/interfaces.h"
#include "audiosub/asr/null_asr_engine.h"
#include "audiosub/asr/whisper_asr_engine.h"

namespace {

void PrintUsage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " --id <A|B> [--host 127.0.0.1] [--port 8888]\n"
      << "\n"
      << "Stage 1b demo: two peers establish a WebRTC DataChannel through\n"
      << "the signaling server, then exchange text messages over P2P.\n"
      << "\n"
      << "Role:\n"
      << "  A: offerer (creates DataChannel and sends Offer)\n"
      << "  B: answerer (waits for Offer, then sends Answer)\n"
      << "\n"
      << "Type any text + Enter to send. /quit to exit.\n";
}

// 命令行参数。--id 必填。
struct Args {
  std::string id;            // "A" 或 "B"
  std::string host = "127.0.0.1";
  int port = 8888;
};

// 简易命令行解析：循环识别 --xxx <value>，碰到 -h/--help 返回 false 触发用法说明。
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

// 控制台多线程打印很容易把"[you]"提示行打乱。这里加锁保证：
//   - 一行消息原子打印
//   - 总是补回 "[you] " 提示行（用 \r 回到行首再覆盖）
std::mutex g_print_mutex;

void Println(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_print_mutex);
  std::cout << "\r" << s << "\n[you] " << std::flush;
}
class LoggingAudioConsumer : public audiosub::core::IAudioFrameConsumer {
public:
    void OnPcmFrame(const audiosub::core::PcmFrame& frame) override {
        const int n = ++frame_count_;

        // 只提示前 1 次，证明 PCM 已进入音频处理接口，之后不再刷屏。
        if (n <= 1) {
            Println("[audio-pipeline] pcm accepted #" + std::to_string(n) +
                ": sample_rate=" + std::to_string(frame.sample_rate) +
                ", channels=" + std::to_string(frame.channels) +
                ", samples=" + std::to_string(frame.samples.size()));

            if (n == 1) {
                Println("[audio-pipeline] PCM pipeline verification done.");
            }
        }
    }

private:
    std::atomic<int> frame_count_{ 0 };
};

class CompositeAudioConsumer : public audiosub::core::IAudioFrameConsumer {
public:
    void AddConsumer(audiosub::core::IAudioFrameConsumer* consumer) {
        if (consumer) {
            consumers_.push_back(consumer);
        }
    }

    void OnPcmFrame(const audiosub::core::PcmFrame& frame) override {
        for (auto* consumer : consumers_) {
            consumer->OnPcmFrame(frame);
        }
    }

private:
    std::vector<audiosub::core::IAudioFrameConsumer*> consumers_;
};

audiosub::core::PcmFrame ConvertTo16kMono(
    const audiosub::core::PcmFrame& input) {
    audiosub::core::PcmFrame output;
    output.sample_rate = 16000;
    output.channels = 1;
    output.bits_per_sample = 16;
    output.timestamp_ms = input.timestamp_ms;

    if (input.samples.empty() || input.channels <= 0 || input.sample_rate <= 0) {
        return output;
    }

    const int channels = input.channels;
    const std::size_t input_frames = input.samples.size() / channels;

    std::vector<int16_t> mono;
    mono.reserve(input_frames);

    for (std::size_t i = 0; i < input_frames; ++i) {
        int sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
            sum += input.samples[i * channels + ch];
        }
        mono.push_back(static_cast<int16_t>(sum / channels));
    }

    if (input.sample_rate == 16000) {
        output.samples = std::move(mono);
        return output;
    }

    if (input.sample_rate == 48000) {
        output.samples.reserve(mono.size() / 3 + 1);
        for (std::size_t i = 0; i < mono.size(); i += 3) {
            output.samples.push_back(mono[i]);
        }
        return output;
    }

    // 简单通用重采样：最近邻抽样。
    // 当前阶段用于打通处理链路，后续可替换为高质量 resampler。
    const double step = static_cast<double>(input.sample_rate) / 16000.0;
    for (double pos = 0.0; pos < static_cast<double>(mono.size()); pos += step) {
        output.samples.push_back(mono[static_cast<std::size_t>(pos)]);
    }

    return output;
}

class BufferedAudioConsumer : public audiosub::core::IAudioFrameConsumer {
public:
    explicit BufferedAudioConsumer(audiosub::audio::PcmRingBuffer* buffer)
        : buffer_(buffer) {
    }

    void OnPcmFrame(const audiosub::core::PcmFrame& frame) override {
        const int n = ++received_count_;

        if (!buffer_) {
            return;
        }

        const bool ok = buffer_->Push(frame);

        // 只提示前 5 次，证明 PCM 已经进入 RingBuffer。
        if (n <= 5) {
            Println("[audio-buffer] pushed #" + std::to_string(n) +
                ": sample_rate=" + std::to_string(frame.sample_rate) +
                ", channels=" + std::to_string(frame.channels) +
                ", samples=" + std::to_string(frame.samples.size()) +
                ", ok=" + std::string(ok ? "true" : "false"));

            if (n == 5) {
                Println("[audio-process] PCM conversion verification done.");
                Println("************************这里是语宙工坊！**************************");
                Println("【B端字幕识别已就绪】");
                Println("现在请等待 A 端文字提示就绪，就绪后可以在 A 端开始说话，B 端将显示相应字幕。");
                Println("==================================================");
            }
        }
    }

private:
    audiosub::audio::PcmRingBuffer* buffer_ = nullptr;
    audiosub::core::IASREngine* asr_ = nullptr;
    std::atomic<int> received_count_{ 0 };
};


class ConsoleSubtitleConsumer : public audiosub::core::ISubtitleConsumer {
public:
    void OnSubtitleSegment(const audiosub::core::SubtitleSegment& segment) override {
        const int n = ++subtitle_count_;

        Println("[subtitle] #" + std::to_string(n) + " " +
            FormatTime(segment.start_ms) + " - " +
            FormatTime(segment.end_ms) + "  " + segment.text);
    }

private:
    static std::string FormatTime(int64_t ms) {
        const int64_t total_seconds = ms / 1000;
        const int64_t minutes = total_seconds / 60;
        const int64_t seconds = total_seconds % 60;
        const int64_t millis = ms % 1000;

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02lld:%02lld.%03lld",
            static_cast<long long>(minutes),
            static_cast<long long>(seconds),
            static_cast<long long>(millis));
        return std::string(buffer);
    }

    std::atomic<int> subtitle_count_{ 0 };
};


class AudioProcessingWorker {
public:
    AudioProcessingWorker(audiosub::audio::PcmRingBuffer* buffer,
        audiosub::core::IASREngine* asr)
        : buffer_(buffer), asr_(asr) {
    }

    ~AudioProcessingWorker() {
        Stop();
    }

    void Start() {
        worker_ = std::thread([this] { Run(); });
    }

    void Stop() {
        if (buffer_) {
            buffer_->Close();
        }

        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void Run() {
        while (buffer_) {
            auto frame = buffer_->WaitPop();
            if (!frame.has_value()) {
                break;
            }

            audiosub::core::PcmFrame converted = ConvertTo16kMono(*frame);
            const int n = ++processed_count_;

            if (asr_) {
                asr_->PushAudio(converted);
            }

            if (n <= 1) {
                Println("[audio-process] converted #" + std::to_string(n) +
                    ": input_rate=" + std::to_string(frame->sample_rate) +
                    ", input_samples=" + std::to_string(frame->samples.size()) +
                    " -> output_rate=" + std::to_string(converted.sample_rate) +
                    ", output_channels=" + std::to_string(converted.channels) +
                    ", output_samples=" + std::to_string(converted.samples.size()));

                if (n == 1) {
                    Println("[audio-process] PCM conversion verification done.");
                }
            }
        }
    }

    audiosub::audio::PcmRingBuffer* buffer_ = nullptr;
    audiosub::core::IASREngine* asr_ = nullptr;
    std::thread worker_;
    std::atomic<int> processed_count_{ 0 };
};



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

  // 角色判定：本项目约定 A 是 offerer，B 是 answerer。
  // 这只是一个客户端层面的约定，信令服务器不关心谁先发 offer。
  const bool is_offerer = (args.id == "A");

  // === 第 1 步：先把 WebRTC 准备好（启动线程 + 创建 PeerConnection）===
  audiosub::PeerConnectionClient pc;
  if (!pc.Initialize()) {
      std::cerr << "PeerConnectionClient::Initialize() failed\n";
      return 2;
  }

  audiosub::audio::PcmRingBuffer pcm_buffer(500);

  LoggingAudioConsumer logging_consumer;
  BufferedAudioConsumer buffered_consumer(&pcm_buffer);

  CompositeAudioConsumer audio_consumer;
  audio_consumer.AddConsumer(&logging_consumer);
  audio_consumer.AddConsumer(&buffered_consumer);

  ConsoleSubtitleConsumer subtitle_consumer;

  audiosub::asr::WhisperASREngine whisper_engine(
      "D:/webrtc_work/whisper.cpp/ggml-small.bin");

  audiosub::asr::NullASREngine null_engine;
  audiosub::core::IASREngine* asr_engine = nullptr;

  if (whisper_engine.Initialize()) {
      asr_engine = &whisper_engine;
      Println("[asr] using whisper.cpp engine");
      // 只作为控制台提示打印一次，不要放进 whisper initial_prompt。
      
  }
  else {
      asr_engine = &null_engine;
      Println("[asr] whisper init failed, fallback to NullASREngine");
  }

  asr_engine->SetSubtitleConsumer(&subtitle_consumer);

  AudioProcessingWorker audio_worker(&pcm_buffer, asr_engine);
  audio_worker.Start();

  pc.SetAudioFrameConsumer(&audio_consumer);

  audiosub::SignalingClient signaling;

  // === 第 2 步：把 WebRTC 的 4 个回调接到"通过信令送出"或"打到控制台"===

  // 本端 SDP 生成完成 -> 通过信令送给对端。
  // type 是 kOffer 或 kAnswer，根据当前角色而定。
  pc.SetSdpReadyCallback(
      [&signaling](webrtc::SdpType type, const std::string& sdp) {
        std::string type_str =
            (type == webrtc::SdpType::kOffer) ? "offer" : "answer";
        nlohmann::json msg = {{"type", type_str}, {"sdp", sdp}};
        signaling.Send(msg);
        Println(std::string("[pc] local ") + type_str + " sent (" +
                std::to_string(sdp.size()) + " bytes)");
      });

  // 发现一个本端 ICE candidate -> 也送给对端。
  // 注意 sdpMid 和 sdpMLineIndex 必须原样回传，对端解析时要用。
  pc.SetIceCandidateCallback(
      [&signaling](const std::string& candidate, const std::string& mid,
                   int mline) {
        nlohmann::json msg = {{"type", "candidate"},
                              {"candidate", candidate},
                              {"sdpMid", mid},
                              {"sdpMLineIndex", mline}};
        signaling.Send(msg);
      });

  // P2P 通道上收到对端文字 -> 直接打印到控制台。
  pc.SetMessageCallback([](const std::string& text) {
    Println(std::string("<peer> ") + text);
  });

  // 各种状态变化 -> 打印。仅辅助调试，不影响业务。
  pc.SetStateCallback([](const std::string& state) {
    Println(std::string("[state] ") + state);
  });

  // === 第 3 步：把信令的回调接到"喂给 WebRTC"或"决定下一步动作"===
  signaling.SetMessageHandler(
      [&pc, is_offerer](const nlohmann::json& msg) {
        std::string type = msg.value("type", "");

        if (type == "peer_ready") {
          // 对端上线。A 端在这一刻"主动发起"：创建 DataChannel + Offer。
          Println(std::string("[peer] ") + msg.value("peer", "?") +
                  " is online");
          if (is_offerer) {
            Println("[pc] creating Offer + DataChannel...");
            pc.CreateOfferAndDataChannel();
          }
          // B 端不主动做任何事，等收到 offer。

        } else if (type == "peer_left") {
          Println(std::string("[peer] ") + msg.value("peer", "?") + " left");

        } else if (type == "offer") {
          // B 端收到 A 端的 Offer：
          //   先把 offer 设置为远端描述（这一步会触发 OnDataChannel）
          //   再调 CreateAnswer 生成应答
          Println("[pc] received Offer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kOffer, msg.value("sdp", ""));
          pc.CreateAnswer();

        } else if (type == "answer") {
          // A 端收到 B 端的 Answer：设置为远端描述就行，握手完成。
          Println("[pc] received Answer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kAnswer, msg.value("sdp", ""));

        } else if (type == "candidate") {
          // 收到对端的 ICE candidate：直接喂给 WebRTC。
          pc.AddRemoteIceCandidate(msg.value("sdpMid", ""),
                                   msg.value("sdpMLineIndex", 0),
                                   msg.value("candidate", ""));

        } else {
          Println(std::string("[signal] unhandled type=") + type);
        }
      });

  // === 第 4 步：连接信令服务器 ===
  // 这一步会发出 hello。如果对端已经在线，会立刻收到 peer_ready，触发上面
  // 的逻辑去 CreateOffer / 等待 offer。
  if (!signaling.Connect(args.host, args.port, args.id)) {
    return 3;
  }

  std::cout << "Role: " << (is_offerer ? "A (offerer)" : "B (answerer)")
            << "\n"
            << "Waiting for peer. Once both peers are online, the offerer "
               "will start.\n"
            << "Type messages and Enter to send. /quit to exit.\n"
            << "[you] " << std::flush;

  // === 第 5 步：主线程进入 stdin 读循环 ===
  // 用户每输一行就尝试通过 DataChannel 发出。dc 没 open 之前会被拒绝。
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

  // === 第 6 步：清理 ===
  // 注意顺序：先关信令（这样不会再有新 SDP/ICE 进来），再关 WebRTC（停所有线程）。
  signaling.Close();
  pc.Close();
  audio_worker.Stop();

  std::cout << "bye.\n";
  return 0;
}
