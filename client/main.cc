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
#include <iostream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "peer_connection_client.h"
#include "signaling_client.h"

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
  std::cout << "bye.\n";
  return 0;
}
