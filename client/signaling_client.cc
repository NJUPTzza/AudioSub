// signaling_client.cc
// ===================
// 信令客户端的 Windows 实现，用 Winsock2 直接做阻塞式 TCP。
//
// 关键设计点：
//   - 主线程 send，后台线程 recv（一收一发分离）
//   - 用 ::recv() 阻塞读取 4096 字节块，自己维护"行缓冲区"按 '\n' 拆消息
//   - WSAStartup 用一个 static 单例确保整个进程只初始化一次

#include "signaling_client.h"

// Winsock 必须在 windows.h 之前 include，否则会和老版 WinSock 1.x 头冲突。
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <iostream>
#include <string>

namespace audiosub {

namespace {

// Winsock 用 INVALID_SOCKET 表示无效 socket，存到 uintptr_t 里时
// 用这个常量比较。
constexpr uintptr_t kInvalidSocket = static_cast<uintptr_t>(INVALID_SOCKET);

// RAII 包装 WSAStartup/WSACleanup：
// Windows 上用 socket 前必须 WSAStartup，进程退出前要 WSACleanup。
// 用一个 static 局部变量让初始化只发生一次，且程序退出时自动清理。
struct WinsockInitializer {
  WinsockInitializer() {
    WSADATA wsa{};
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);  // 请求 Winsock 2.2
    if (rc != 0) {
      std::cerr << "[signaling] WSAStartup failed: " << rc << "\n";
    }
  }
  ~WinsockInitializer() { WSACleanup(); }
};

WinsockInitializer& EnsureWinsock() {
  // C++11 起，函数内 static 变量初始化是线程安全的（"magic statics"）。
  static WinsockInitializer instance;
  return instance;
}

// 循环 send 直到全部发完，处理 send 一次没发完的情况（部分写）。
bool SendAll(SOCKET s, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
    if (n <= 0) {
      // 0 表示对端关闭，<0 表示错误，两种情况都视为失败。
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

SignalingClient::SignalingClient() : sock_(kInvalidSocket) {
  // 构造时就保证 Winsock 初始化好了。
  EnsureWinsock();
}

SignalingClient::~SignalingClient() { Close(); }

bool SignalingClient::Connect(const std::string& host,
                              int port,
                              const std::string& peer_id) {
  EnsureWinsock();

  if (running_.load()) {
    std::cerr << "[signaling] already connected\n";
    return false;
  }

  // 用 getaddrinfo 而不是 inet_addr，这样支持传 hostname（"localhost"）也支持 IP。
  addrinfo hints{};
  hints.ai_family = AF_INET;       // IPv4（暂不支持 IPv6）
  hints.ai_socktype = SOCK_STREAM; // TCP
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* result = nullptr;
  std::string port_str = std::to_string(port);
  int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
  if (rc != 0 || result == nullptr) {
    std::cerr << "[signaling] getaddrinfo failed: " << rc << "\n";
    return false;
  }

  // 一个 hostname 可能对应多个 IP，挨个试，第一个 connect 成功的就用。
  SOCKET s = INVALID_SOCKET;
  for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
    s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    if (::connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
    ::closesocket(s);
    s = INVALID_SOCKET;
  }
  freeaddrinfo(result);  // 必须释放，否则内存泄漏

  if (s == INVALID_SOCKET) {
    std::cerr << "[signaling] connect to " << host << ":" << port
              << " failed (WSAError=" << WSAGetLastError() << ")\n";
    return false;
  }

  sock_ = static_cast<uintptr_t>(s);
  running_.store(true);

  // 协议约定：第一条消息必须是 hello，告诉服务器自己是谁。
  nlohmann::json hello = {{"type", "hello"}, {"id", peer_id}};
  Send(hello);

  // 起 recv 线程异步收消息，主线程立刻返回继续干别的（比如读 stdin）。
  recv_thread_ = std::thread(&SignalingClient::RecvLoop, this);
  std::cout << "[signaling] connected as '" << peer_id << "' to " << host
            << ":" << port << "\n";
  return true;
}

void SignalingClient::Send(const nlohmann::json& msg) {
  if (sock_ == kInvalidSocket) {
    std::cerr << "[signaling] Send called but socket is invalid\n";
    return;
  }
  // dump() 把 JSON 对象序列化为字符串，然后我们补一个换行作为消息分隔符。
  std::string line = msg.dump();
  line.push_back('\n');

  // 加锁，避免多线程同时 send 导致一条消息被另一条插入。
  std::lock_guard<std::mutex> lock(send_mutex_);
  if (!SendAll(static_cast<SOCKET>(sock_), line.data(), line.size())) {
    std::cerr << "[signaling] send failed (WSAError=" << WSAGetLastError()
              << ")\n";
  }
}

void SignalingClient::Close() {
  // running_.exchange(false) 原子地拿到旧值并设为 false，
  // 这样可以保证 join 只发生一次。
  bool was_running = running_.exchange(false);

  if (sock_ != kInvalidSocket) {
    // shutdown 让 recv 线程的阻塞 ::recv() 立刻返回（=0 或错误）。
    // 不 shutdown 直接 closesocket 也能用，但 shutdown 更"礼貌"。
    ::shutdown(static_cast<SOCKET>(sock_), SD_BOTH);
    ::closesocket(static_cast<SOCKET>(sock_));
    sock_ = kInvalidSocket;
  }
  if (was_running && recv_thread_.joinable()) {
    recv_thread_.join();  // 等 recv 线程真正退出，避免野指针
  }
}

void SignalingClient::SetMessageHandler(MessageHandler handler) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  handler_ = std::move(handler);
}

void SignalingClient::RecvLoop() {
  // 行缓冲：TCP 是"字节流"，不保证 send 一次对应 recv 一次。可能：
  //   - 一次 recv 收到半条 + 一条完整 + 半条
  //   - 一条消息分多次 recv 才能收齐
  // 所以要自己维护一个 buf，每次 recv 后追加，再用 '\n' 切出完整的行。
  std::string buf;
  buf.reserve(4096);
  char chunk[4096];

  while (running_.load()) {
    int n = ::recv(static_cast<SOCKET>(sock_), chunk, sizeof(chunk), 0);
    if (n == 0) {
      std::cout << "[signaling] peer closed connection\n";
      break;
    }
    if (n < 0) {
      // Close() 时 socket 被关闭会让 recv 返回错误，这是预期的，不报错。
      if (running_.load()) {
        std::cerr << "[signaling] recv failed (WSAError="
                  << WSAGetLastError() << ")\n";
      }
      break;
    }

    // 追加这次新收到的字节
    buf.append(chunk, static_cast<size_t>(n));

    // 切出所有完整的行（剩下的"半行"留在 buf 里等下次 recv）
    size_t pos = 0;
    for (;;) {
      size_t nl = buf.find('\n', pos);
      if (nl == std::string::npos) break;
      DispatchLine(buf.substr(pos, nl - pos));
      pos = nl + 1;
    }
    if (pos > 0) buf.erase(0, pos);  // 把已处理掉的部分扔了
  }
  running_.store(false);
}

void SignalingClient::DispatchLine(const std::string& line) {
  if (line.empty()) return;

  // nlohmann/json 默认遇错抛异常，但我们项目用 _HAS_EXCEPTIONS=0 关闭了
  // 异常，所以这里用 allow_exceptions=false：解析失败返回 discarded 对象。
  auto msg =
      nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (msg.is_discarded()) {
    std::cerr << "[signaling] bad json from server: " << line << "\n";
    return;
  }

  // 复制一份 handler，避免持锁回调（用户的 handler 里可能调 Send，需要别的锁）。
  MessageHandler handler_copy;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_copy = handler_;
  }
  if (handler_copy) {
    handler_copy(msg);
  } else {
    // 没设置 handler 时打到控制台，方便调试。
    std::cout << "[signaling] (no handler) <- " << msg.dump() << "\n";
  }
}

}  // namespace audiosub
