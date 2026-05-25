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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "audiosub/audio/pcm_ring_buffer.h"
#include "audiosub/audio/wav_writer.h"
#include "peer_connection_client.h"
#include "signaling_client.h"

namespace {

void PrintUsage(const char* prog) {
  std::cout
      << "Usage: " << prog
      << " --id <A|B> [--host 127.0.0.1] [--port 8888] [--mic-test]\n"
      << "       [--dump-wav <path>]\n"
      << "\n"
      << "Stage 2 demo: A captures microphone audio, WebRTC sends it to B,\n"
      << "and B logs decoded PCM frames while both peers can still chat over\n"
      << "the DataChannel.\n"
      << "\n"
      << "Role:\n"
      << "  A: offerer (adds microphone track, creates DataChannel and Offer)\n"
      << "  B: answerer (waits for Offer, receives remote audio track)\n"
      << "\n"
      << "Commands:\n"
      << "  /talk on   start sending microphone audio from A\n"
      << "  /talk off  stop sending microphone audio from A\n"
      << "  /quit      exit\n"
      << "\n"
      << "Standalone mic test (no peer / no signaling needed):\n"
      << "  --mic-test           drive ADM directly, print live mic level\n"
      << "  --dump-wav <path>    dump captured PCM to a 16-bit WAV file\n"
      << "                       (works in --mic-test and in normal A mode)\n"
      << "\n"
      << "Any other text is still sent over the DataChannel.\n";
}

// 命令行参数。--id 必填。
struct Args {
  std::string id;            // "A" 或 "B"
  std::string host = "127.0.0.1";
  int port = 8888;
  bool mic_test = false;
  std::string dump_wav_path;   // 非空时把采到的本地 PCM 写到这个 WAV 里
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
    } else if (a == "--mic-test") {
      out->mic_test = true;
    } else if (a == "--dump-wav") {
      if (const char* v = next("--dump-wav")) out->dump_wav_path = v;
      else return false;
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

int main(int argc, char** argv) {
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

  audiosub::audio::PcmRingBuffer local_audio_buffer(/*capacity_frames=*/128);
  audiosub::audio::PcmRingBuffer remote_audio_buffer(/*capacity_frames=*/128);
  std::thread local_audio_worker([&local_audio_buffer]() {
    RunAudioLevelMonitor(local_audio_buffer, "local");
  });
  std::thread remote_audio_worker([&remote_audio_buffer]() {
    RunAudioLevelMonitor(remote_audio_buffer, "remote");
  });

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

  // 可选：把本地采到的 PCM 同步写一份到 WAV 文件，便于事后用任意播放器
  // 回放确认"麦克风真的把声音收进来了"。WavWriter 自己内部加锁，可以从
  // WebRTC 的音频线程直接调用。
  audiosub::audio::WavWriter local_wav;
  if (!args.dump_wav_path.empty()) {
    if (!local_wav.Open(args.dump_wav_path)) {
      std::cerr << "[audio] failed to open WAV dump: " << args.dump_wav_path
                << "\n";
    } else {
      std::cout << "[audio] dumping local PCM to " << args.dump_wav_path
                << "\n";
    }
  }

  pc.SetLocalAudioFrameCallback([&local_audio_buffer, &local_wav](
                                    const audiosub::core::PcmFrame& frame) {
    if (!local_audio_buffer.Push(frame)) {
      std::cerr << "[audio] dropping local PCM frame: ring buffer closed\n";
    }
    if (local_wav.is_open()) {
      local_wav.Append(frame);
    }
  });
  pc.SetRemoteAudioFrameCallback([&remote_audio_buffer](
                                     const audiosub::core::PcmFrame& frame) {
    if (!remote_audio_buffer.Push(frame)) {
      std::cerr << "[audio] dropping remote PCM frame: ring buffer closed\n";
    }
  });

  // 各种状态变化 -> 打印。仅辅助调试，不影响业务。
  pc.SetStateCallback([](const std::string& state) {
    Println(std::string("[state] ") + state);
  });

  // 本地麦克风自测模式：完全不依赖信令和 PeerConnection 协商，直接驱动
  // AudioDeviceModule 录音。这是验证"麦克风到底能不能采到声音"最干净的
  // 方式——和 WebRTC P2P 链路任何环节都解耦。
  if (args.mic_test) {
    if (!is_offerer) {
      std::cerr << "--mic-test only supports --id A\n";
      local_audio_buffer.Close();
      remote_audio_buffer.Close();
      if (local_audio_worker.joinable()) local_audio_worker.join();
      if (remote_audio_worker.joinable()) remote_audio_worker.join();
      local_wav.Close();
      pc.Close();
      return 6;
    }

    pc.LogRecordingDevices();
    if (!pc.StartLocalCaptureSelfTest()) {
      std::cerr << "Failed to start microphone self-test\n";
      local_audio_buffer.Close();
      remote_audio_buffer.Close();
      if (local_audio_worker.joinable()) local_audio_worker.join();
      if (remote_audio_worker.joinable()) remote_audio_worker.join();
      local_wav.Close();
      pc.Close();
      return 7;
    }

    std::cout << "Mic test mode: ADM recording is running.\n"
              << "Speak to your microphone and watch [audio] local ...\n"
              << "  avg/peak should rise when you talk, drop in silence.\n"
              << (args.dump_wav_path.empty()
                      ? "  (tip: pass --dump-wav out.wav to save captured PCM)\n"
                      : "  PCM is being saved to the WAV file above.\n")
              << "Type /quit to stop.\n"
              << "[you] " << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "/quit" || line == "/exit") break;
      std::cout << "[you] " << std::flush;
    }

    pc.StopLocalCaptureSelfTest();
    pc.Close();
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    if (local_audio_worker.joinable()) local_audio_worker.join();
    if (remote_audio_worker.joinable()) remote_audio_worker.join();
    local_wav.Close();
    std::cout << "bye.\n";
    return 0;
  }

  // A 端在协商前就把音频轨道挂进去，但默认先置为静音。
  // 这样首个 Offer 就已经带上音频 m= section，后续只靠命令切换讲话状态，
  // 不需要再做二次协商。
  if (is_offerer && !pc.EnableLocalAudio()) {
    std::cerr << "PeerConnectionClient::EnableLocalAudio() failed\n";
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    local_audio_worker.join();
    remote_audio_worker.join();
    local_wav.Close();
    pc.Close();
    return 4;
  }
  if (is_offerer && !pc.SetLocalAudioEnabled(false)) {
    std::cerr << "PeerConnectionClient::SetLocalAudioEnabled(false) failed\n";
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    local_audio_worker.join();
    remote_audio_worker.join();
    local_wav.Close();
    pc.Close();
    return 5;
  }

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
            << (is_offerer
                    ? "A starts muted. Use /talk on and /talk off to control speech.\n"
                    : "B will show remote audio level as silent/speaking.\n")
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
  local_audio_buffer.Close();
  remote_audio_buffer.Close();
  if (local_audio_worker.joinable()) {
    local_audio_worker.join();
  }
  if (remote_audio_worker.joinable()) {
    remote_audio_worker.join();
  }
  local_wav.Close();
  std::cout << "bye.\n";
  return 0;
}
