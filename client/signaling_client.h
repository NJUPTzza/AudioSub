// signaling_client.h
// ===================
// C++ 端的信令客户端，用 Winsock 跑 TCP 连接，发/收"一行 JSON"协议消息。
//
// 它对外的核心能力：
//   - Connect()  发送 hello 注册自己到信令服务器
//   - Send()     发送任意 JSON 给对端（服务器原样转发）
//   - 设置一个回调，收到对端消息时被调用
//
// 内部细节：
//   - 一个独立的后台线程跑 recv（不能占用主线程，不然就阻塞用户输入了）
//   - 用一行缓冲 + '\n' 分隔，处理 TCP 粘包/拆包
//
// 设计原则：这层完全不懂 WebRTC，它只负责"在网络上传字典"。

#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace audiosub {

class SignalingClient {
 public:
  // 收到对端/服务器消息时被调用的回调类型。
  // 参数 msg 是解析好的 JSON 对象，由调用方自行判断 type 字段。
  using MessageHandler = std::function<void(const nlohmann::json& msg)>;

  SignalingClient();
  ~SignalingClient();

  // 禁用拷贝：内部有 thread 和 socket，不能被拷贝。
  SignalingClient(const SignalingClient&) = delete;
  SignalingClient& operator=(const SignalingClient&) = delete;

  // 连接信令服务器并发送 hello。
  // peer_id: 自己的身份标识（本项目用 "A" 或 "B"）。
  // 返回 true 表示 TCP 握手成功且 hello 已发出。
  bool Connect(const std::string& host, int port, const std::string& peer_id);

  // 发送一条 JSON 消息（线程安全，会自动追加 '\n'）。
  // 服务器会原样转发给对端。
  void Send(const nlohmann::json& msg);

  // 主动关闭。会停掉 recv 线程并关闭 socket。析构时也会自动调用。
  void Close();

  // 设置收到消息时的回调。可以在 Connect 之前或之后调用。
  void SetMessageHandler(MessageHandler handler);

  // 是否还活着（recv 线程还在跑、socket 没关）。
  bool IsConnected() const { return running_.load(); }

 private:
  // recv 线程主循环：不停地读 socket、按 '\n' 切分、解析 JSON、调回调。
  void RecvLoop();

  // 把一行 JSON 字符串解析后调用 handler_。
  void DispatchLine(const std::string& line);

  // === 状态 ===
  // 注意：Winsock 的 SOCKET 类型在 <winsock2.h> 里定义，但为了不在头文件
  // 里污染整个项目（include <winsock2.h> 会拉一堆 Windows 宏），这里用
  // uintptr_t 存 socket handle，.cc 里再 cast 成 SOCKET。
  uintptr_t sock_;

  // recv 后台线程。Close() 时会被 join。
  std::thread recv_thread_;

  // 是否在跑。Close() 会设成 false，recv 循环检测到就退出。
  // 用 atomic 是因为 RecvLoop 是后台线程，主线程会改它。
  std::atomic<bool> running_{false};

  // Send() 可能被多个线程并发调用（比如 WebRTC 各种 observer 回调），
  // 这个锁保证一条 JSON 写到一半不会被另一条切断。
  std::mutex send_mutex_;

  // handler_ 的读写要同步：主线程可能 SetMessageHandler，recv 线程会读。
  std::mutex handler_mutex_;
  MessageHandler handler_;
};

}  // namespace audiosub
