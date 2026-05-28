// main.cc
// =======
// audiosub_client.exe 的入口。这个文件做的事：
//   1. 解析命令行 --id A/B、--host、--port
//   2. 创建 PeerConnectionClient + SignalingClient
//   3. 把"WebRTC 生成的 SDP/ICE"转发到信令通道
//      把"信令通道收到的 SDP/ICE"喂回 WebRTC
//   4. 主线程跑 std::getline 读用户输入，调 SendMessage 发到 P2P


#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "audio/pcm_ring_buffer.h"
#include "peer_connection_client.h"
#include "signaling_client.h"

int main(int argc, char** argv) {
  // 解析命令行参数
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 1;
  }

  // 角色判定：约定 A 是 offerer，B 是 answerer。
  // 后面逻辑基于 A 主动创建 offer，B 被动等待 offer
  const bool is_offerer = (args.id == "A");

  // 初始化 WebRTC 核心对象
  // 创建 PeerConnectionClient
  // 启动 WebRTC 三个线程
  // 创建 PeerConnectionFactory 和 PeerConnection
  audiosub::PeerConnectionClient pc;
  if (!pc.Initialize()) {
    std::cerr << "PeerConnectionClient::Initialize() failed\n";
    return 2;
  }

  // 创建两个 PCM 缓冲区，分别用于本地采集和对端接收
  audiosub::audio::PcmRingBuffer local_audio_buffer(/*capacity_frames=*/128);
  audiosub::audio::PcmRingBuffer remote_audio_buffer(/*capacity_frames=*/128);
  // 启动两个监控线程
  std::thread local_audio_worker([&local_audio_buffer]() {
    RunAudioLevelMonitor(local_audio_buffer, "local");
  });
  std::thread remote_audio_worker([&remote_audio_buffer]() {
    RunAudioLevelMonitor(remote_audio_buffer, "remote");
  });

  // 创建信令客户端对象
  audiosub::SignalingClient signaling;

  // 应用层接线。把底层回调映射到业务动作。
  // 当本地生成 offer 或 answer 时，把它包装成 JSON，通过 signaling.Send() 发出去
  pc.SetSdpReadyCallback(
      [&signaling](webrtc::SdpType type, const std::string& sdp) {
        std::string type_str =
            (type == webrtc::SdpType::kOffer) ? "offer" : "answer";
        nlohmann::json msg = {{"type", type_str}, {"sdp", sdp}};
        signaling.Send(msg);
        Println(std::string("[pc] local ") + type_str + " sent (" +
                std::to_string(sdp.size()) + " bytes)");
      });

  // 当本地产生一个 ICE candidate 时，通过信令发给对端
  pc.SetIceCandidateCallback(
      [&signaling](const std::string& candidate, const std::string& mid,
                   int mline) {
        nlohmann::json msg = {{"type", "candidate"},
                              {"candidate", candidate},
                              {"sdpMid", mid},
                              {"sdpMLineIndex", mline}};
        signaling.Send(msg);
      });

  // 通过 DataChannel 收到对端发来的文本消息，打印出来
  pc.SetMessageCallback([](const std::string& text) {
    Println(std::string("<peer> ") + text);
  });

  // 本地 PCM 来了以后，放进 local_audio_buffer
  pc.SetLocalAudioFrameCallback([&local_audio_buffer](
                                    const audiosub::core::PcmFrame& frame) {
    if (!local_audio_buffer.Push(frame)) {
      std::cerr << "[audio] dropping local PCM frame: ring buffer closed\n";
    }
  });
  
  // 远端 PCM 来了以后，放进 remote_audio_buffer
  pc.SetRemoteAudioFrameCallback([&remote_audio_buffer](
                                     const audiosub::core::PcmFrame& frame) {
    if (!remote_audio_buffer.Push(frame)) {
      std::cerr << "[audio] dropping remote PCM frame: ring buffer closed\n";
    }
  });

  // 把各种状态变化打印出来，比如 pc:connected 、 dc:open
  pc.SetStateCallback([](const std::string& state) {
    Println(std::string("[state] ") + state);
  });

  // 如果是 A 端（offerer），提前把本地音频轨加进 PeerConnection，默认先静音
  // 这样后续切换成讲话状态时不需要做二次协商
  if (is_offerer && !pc.EnableLocalAudio()) {
    std::cerr << "PeerConnectionClient::EnableLocalAudio() failed\n";
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    local_audio_worker.join();
    remote_audio_worker.join();
    pc.Close();
    return 4;
  }
  if (is_offerer && !pc.SetLocalAudioEnabled(false)) {
    std::cerr << "PeerConnectionClient::SetLocalAudioEnabled(false) failed\n";
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    local_audio_worker.join();
    remote_audio_worker.join();
    pc.Close();
    return 5;
  }

  // 这里给 信令服务器 设置消息处理函数，处理对端发来的不同类型信令消息
  // 信令消息类型有：peer_ready、peer_left、offer、answer、candidate
  signaling.SetMessageHandler(
      [&pc, is_offerer](const nlohmann::json& msg) {
        std::string type = msg.value("type", "");
        // peer_ready 表示对端上线了
        if (type == "peer_ready") {
          Println(std::string("[peer] ") + msg.value("peer", "?") +
                  " is online");
          // 如果自己是 A 端（offerer），则主动发起 DataChannel + Offer
          if (is_offerer) {
            Println("[pc] creating Offer + DataChannel...");
            pc.CreateOfferAndDataChannel();
          }
          // 如果自己是 B 端（answerer），则不做任何事，等收到 offer。

        // peer_left 表示对端下线了
        } else if (type == "peer_left") {
          Println(std::string("[peer] ") + msg.value("peer", "?") + " left");

        // offer 表示 B 收到 A 的 offer
        } else if (type == "offer") {
          //  B 端接收 A 端发来的 offer(sdp)，将对端提出的连接方案告诉本地 WebRTC
          //  再调 CreateAnswer 生成应答
          Println("[pc] received Offer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kOffer, msg.value("sdp", ""));
          pc.CreateAnswer();

        // answer 表示 A 收到 B 的 answer
        } else if (type == "answer") {
          // A 端接收 B 端发来的 answer(sdp)，将对端提出的连接方案告诉本地 WebRTC
          Println("[pc] received Answer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kAnswer, msg.value("sdp", ""));

        // candidate 表示 收到对端的 ICE candidate
        } else if (type == "candidate") {
          // 直接喂给 WebRTC
          pc.AddRemoteIceCandidate(msg.value("sdpMid", ""),
                                   msg.value("sdpMLineIndex", 0),
                                   msg.value("candidate", ""));

        } else {
          Println(std::string("[signal] unhandled type=") + type);
        }
      });

  // 连接信令服务器，发送 hello 并等待 peer_ready
  // 这一步会发出 hello。如果对端已经在线，会立刻收到 peer_ready
  // 触发上面的逻辑去 CreateOffer / 等待 offer
  if (!signaling.Connect(args.host, args.port, args.id)) {
    return 3;
  }

  // 提示语，告诉用户当前角色和可用命令
  std::cout << "Role: " << (is_offerer ? "A (offerer)" : "B (answerer)")
            << "\n"
            << "Commands: /talk on, /talk off, /quit\n"
            << "Type text to send after DataChannel opens.\n"
            << "Waiting for peer...\n"
            << "[you] " << std::flush;

  // 主线程 stdin 循环（文本消息 + /talk 命令）。
  // 对用户输入的文本消息，直接通过 DataChannel 发送
  // 对 talk 相关命令，根据是否是 A 端（offerer），分别触发本地音频轨的静音/取消静音
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") break;
    if (line.empty()) {
      std::cout << "[you] " << std::flush;
      continue;
    }
    // talk on: 触发 A 端开始送麦克风音频。
    if (line == "/talk on") {
      if (!is_offerer) {
        Println("[audio] only A can control local microphone speech");
      } else if (!pc.SetLocalAudioEnabled(true)) {
        Println("[audio] failed to start talking");
      } else {
        Println("[audio] talking enabled on A");
      }
      continue;
    }
    // talk off: 停止发送有效语音（保留轨道）。
    if (line == "/talk off") {
      if (!is_offerer) {
        Println("[audio] only A can control local microphone speech");
      } else if (!pc.SetLocalAudioEnabled(false)) {
        Println("[audio] failed to stop talking");
      } else {
        Println("[audio] talking disabled on A");
      }
      continue;
    }
    // 普通文本消息：直接通过 DataChannel 发送
    if (!pc.SendMessage(line)) {
      Println("(message dropped: data channel not open yet)");
    } else {
      std::cout << "[you] " << std::flush;
    }
  }

  // 清理：先关信令（这样不会再有新 SDP/ICE 进来），再关 WebRTC（停所有线程）。
  signaling.Close();
  pc.Close();
  local_audio_buffer.Close();
  remote_audio_buffer.Close();
  if (local_audio_worker.joinable()) {
    local_audio_worker.join();
  }
  if (remote_audio_worker.joinable()) {
    remote_audio_worker.join();
  }
  std::cout << "bye.\n";
  return 0;
}

namespace {
// 打印命令行帮助。
// 提示文本，不参与任何音频/信令逻辑。
void PrintUsage(const char* prog) {
  std::cout
      << "Usage: " << prog << " --id <A|B> [--host <host>] [--port <port>]\n"
      << "\n"
      << "Required:\n"
      << "  --id <A|B>\n"
      << "Optional:\n"
      << "  --host <host>    default: 127.0.0.1\n"
      << "  --port <port>    default: 8888\n";
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

std::string DescribePcmFrame(const audiosub::core::PcmFrame& frame,
                             std::size_t frame_index) {
  return "[audio] remote pcm #" + std::to_string(frame_index) +
         " ts_ms=" + std::to_string(frame.timestamp_ms) +
         " rate=" + std::to_string(frame.sample_rate) +
         " ch=" + std::to_string(frame.channels) +
         " samples=" + std::to_string(frame.samples.size());
}

int ComputeAverageAmplitude(const audiosub::core::PcmFrame& frame) {
  if (frame.samples.empty()) {
    return 0;
  }
  int64_t sum = 0;
  for (int16_t sample : frame.samples) {
    sum += (sample >= 0) ? sample : -static_cast<int>(sample);
  }
  return static_cast<int>(sum / static_cast<int64_t>(frame.samples.size()));
}

// 电平监视：聚合最近一段时间内的多帧，再按固定时间间隔打印一次"窗口内
// 平均电平"。这样既能让用户看到麦克风活动持续刷新（验证采集真的在跑），
// 又不会被 10ms/帧 的密度刷屏。
//
// 同时保留状态变化即时打印（silent <-> speaking）以便观察 /talk 切换效果。
void RunAudioLevelMonitor(audiosub::audio::PcmRingBuffer& buffer,
                          const std::string& label) {
  using clock = std::chrono::steady_clock;
  constexpr auto kPrintInterval = std::chrono::milliseconds(500);
  constexpr int kSpeakingThreshold = 80;

  std::size_t frame_index = 0;
  std::size_t window_frames = 0;
  int64_t window_level_sum = 0;
  int window_level_max = 0;
  auto last_print = clock::now();
  bool has_last_state = false;
  bool last_speaking = false;

  while (auto frame = buffer.WaitPop()) {
    ++frame_index;
    const int level = ComputeAverageAmplitude(*frame);
    window_level_sum += level;
    window_level_max = std::max(window_level_max, level);
    ++window_frames;

    const bool speaking = level >= kSpeakingThreshold;
    const bool state_changed = !has_last_state || speaking != last_speaking;

    const auto now = clock::now();
    const bool time_due = (now - last_print) >= kPrintInterval;

    if (state_changed || time_due) {
      const int avg = window_frames > 0
                          ? static_cast<int>(window_level_sum /
                                             static_cast<int64_t>(window_frames))
                          : 0;
      Println("[audio] " + label + " frames=" + std::to_string(frame_index) +
              " avg=" + std::to_string(avg) +
              " peak=" + std::to_string(window_level_max) +
              " state=" + (speaking ? "speaking" : "silent"));
      has_last_state = true;
      last_speaking = speaking;
      last_print = now;
      window_frames = 0;
      window_level_sum = 0;
      window_level_max = 0;
    }
  }
}

}  // namespace


