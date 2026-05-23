// peer_connection_client.cc
// =========================
// WebRTC PeerConnection 封装的实现。
//
// 主要复杂度集中在 3 个地方：
//   1. 三个 WebRTC 内部线程的启动和销毁
//   2. SDP 异步流程：CreateOffer/Answer -> SetLocalDescription -> 信令送出
//   3. 各种 Observer 嵌套类（WebRTC 要求 SDP observer 必须是 RefCounted）

#include "peer_connection_client.h"

#include <iostream>
#include <utility>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"

namespace audiosub {

namespace {

// Google 公开的 STUN 服务器。STUN 用来帮助本端发现自己的公网 IP，
// 让两个不同 NAT 后面的端能互连。同机/同局域网测试其实用不到。
const char kStunServer[] = "stun:stun.l.google.com:19302";

// DataChannel 的标签名（双方约定一致即可，作为通道的标识）。
const char kDataChannelLabel[] = "chat";

// === 一些枚举到字符串的辅助函数，仅用于打日志 ===

const char* SignalingStateName(
    webrtc::PeerConnectionInterface::SignalingState s) {
  using S = webrtc::PeerConnectionInterface::SignalingState;
  switch (s) {
    case S::kStable: return "stable";                       // 没有未完成的 offer/answer
    case S::kHaveLocalOffer: return "have-local-offer";     // 本端发出了 offer，等对端 answer
    case S::kHaveLocalPrAnswer: return "have-local-pranswer";
    case S::kHaveRemoteOffer: return "have-remote-offer";   // 收到了对端 offer，准备 answer
    case S::kHaveRemotePrAnswer: return "have-remote-pranswer";
    case S::kClosed: return "closed";
  }
  return "?";
}

const char* IceConnectionStateName(
    webrtc::PeerConnectionInterface::IceConnectionState s) {
  using S = webrtc::PeerConnectionInterface::IceConnectionState;
  switch (s) {
    case S::kIceConnectionNew: return "ice:new";
    case S::kIceConnectionChecking: return "ice:checking";   // 正在试 candidate pair
    case S::kIceConnectionConnected: return "ice:connected"; // 至少一对 pair 通了
    case S::kIceConnectionCompleted: return "ice:completed"; // 所有 pair 都试完了
    case S::kIceConnectionFailed: return "ice:failed";
    case S::kIceConnectionDisconnected: return "ice:disconnected";
    case S::kIceConnectionClosed: return "ice:closed";
    default: return "ice:?";
  }
}

const char* PeerConnectionStateName(
    webrtc::PeerConnectionInterface::PeerConnectionState s) {
  using S = webrtc::PeerConnectionInterface::PeerConnectionState;
  switch (s) {
    case S::kNew: return "pc:new";
    case S::kConnecting: return "pc:connecting"; // ICE + DTLS 进行中
    case S::kConnected: return "pc:connected";   // 通了！这是你最想看到的状态
    case S::kDisconnected: return "pc:disconnected";
    case S::kFailed: return "pc:failed";
    case S::kClosed: return "pc:closed";
  }
  return "pc:?";
}

}  // namespace

// ===========================================================================
// 三个 SDP Observer 嵌套类
// ===========================================================================
// 为什么要单独搞嵌套类，而不是让 PeerConnectionClient 直接继承？
//   - 这些 Observer 接口继承自 webrtc::RefCountInterface（引用计数对象），
//     必须用 webrtc::scoped_refptr 持有。
//   - 但 PeerConnectionClient 是普通对象（外部用裸指针/局部变量管理），
//     不能让它"被引用计数管理"。
// 所以做法是：嵌套类做"皮"，只把回调转发给 parent_ 指针。

class PeerConnectionClient::CreateSdpObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateSdpObserver(PeerConnectionClient* parent) : parent_(parent) {}

  // CreateOffer/CreateAnswer 成功完成
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    // WebRTC 通过裸指针给我们传过来 SessionDescriptionInterface，文档说"我
    // 们拿到所有权"。立刻 wrap 成 unique_ptr 避免泄漏。
    parent_->OnLocalSdpReady(
        std::unique_ptr<webrtc::SessionDescriptionInterface>(desc));
  }

  void OnFailure(webrtc::RTCError error) override {
    parent_->OnSdpFailure("CreateSdp", std::move(error));
  }

 private:
  // 注意是裸指针：parent 的生命周期 >= 本 observer。这点由 PeerConnectionClient
  // 在 Close() 中先停掉 WebRTC 再析构来保证。
  PeerConnectionClient* const parent_;
};

class PeerConnectionClient::SetLocalDescObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit SetLocalDescObserver(PeerConnectionClient* parent)
      : parent_(parent) {}

  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) {
      parent_->OnSdpFailure("SetLocal", std::move(error));
    } else {
      RTC_LOG(LS_INFO) << "[pc] SetLocalDescription OK";
    }
  }

 private:
  PeerConnectionClient* const parent_;
};

class PeerConnectionClient::SetRemoteDescObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit SetRemoteDescObserver(PeerConnectionClient* parent)
      : parent_(parent) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    if (!error.ok()) {
      parent_->OnSdpFailure("SetRemote", std::move(error));
    } else {
      RTC_LOG(LS_INFO) << "[pc] SetRemoteDescription OK";
    }
  }

 private:
  PeerConnectionClient* const parent_;
};

// ===========================================================================
// 生命周期：构造 / Initialize / Close / 析构
// ===========================================================================

PeerConnectionClient::PeerConnectionClient() = default;

PeerConnectionClient::~PeerConnectionClient() { Close(); }

bool PeerConnectionClient::Initialize() {
  // 第一步：初始化 OpenSSL/BoringSSL。WebRTC 内部用到 SSL（DTLS 加密 P2P 数据），
  // 必须在创建 PeerConnection 前调用一次。重复调用也安全。
  webrtc::InitializeSSL();

  // 第二步：启动三个 WebRTC 内部线程。
  //
  // 三个线程都是 webrtc::Thread（不是 std::thread），它们内部有事件循环，
  // 可以接收 PostTask 调度的任务。
  //
  // 关键区别：
  //   network_thread 必须用 CreateWithSocketServer()，里面带一个 SocketServer，
  //   能跑 socket select/poll；其他两个用普通 Create()。

  network_thread_ = webrtc::Thread::CreateWithSocketServer();
  network_thread_->SetName("pc_network", nullptr);
  if (!network_thread_->Start()) {
    std::cerr << "[pc] failed to start network thread\n";
    return false;
  }

  worker_thread_ = webrtc::Thread::Create();
  worker_thread_->SetName("pc_worker", nullptr);
  if (!worker_thread_->Start()) {
    std::cerr << "[pc] failed to start worker thread\n";
    return false;
  }

  signaling_thread_ = webrtc::Thread::Create();
  signaling_thread_->SetName("pc_signaling", nullptr);
  if (!signaling_thread_->Start()) {
    std::cerr << "[pc] failed to start signaling thread\n";
    return false;
  }

  // 第三步：创建 PeerConnectionFactory。这是创建 PeerConnection 的工厂。
  //
  // 参数比较多，但本项目阶段 1 只用 DataChannel，没用音频/视频媒体轨道：
  //   - audio_*_factory: 传 builtin 实现，后续阶段 2 加麦克风时会用到
  //   - video_*_factory: nullptr，本项目不做视频
  //   - audio_mixer / audio_processing: nullptr，用默认
  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(),
      worker_thread_.get(),
      signaling_thread_.get(),
      /*default_adm=*/nullptr,
      /*audio_encoder_factory=*/webrtc::CreateBuiltinAudioEncoderFactory(),
      /*audio_decoder_factory=*/webrtc::CreateBuiltinAudioDecoderFactory(),
      /*video_encoder_factory=*/nullptr,
      /*video_decoder_factory=*/nullptr,
      /*audio_mixer=*/nullptr,
      /*audio_processing=*/nullptr);
  if (!factory_) {
    std::cerr << "[pc] CreatePeerConnectionFactory failed\n";
    return false;
  }

  // 第四步：配置并创建 PeerConnection。
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  // 新版 WebRTC 推荐用 UnifiedPlan（兼容浏览器、规范一致）。
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  // 加一个 STUN 服务器。同机调试用不到，但加上能让真实跨网络场景能跑。
  webrtc::PeerConnectionInterface::IceServer ice;
  ice.urls.push_back(kStunServer);
  config.servers.push_back(std::move(ice));

  // PeerConnectionDependencies 必须传一个 PeerConnectionObserver。
  // 我们把 this 传进去（本类就实现了 PeerConnectionObserver 接口）。
  webrtc::PeerConnectionDependencies deps(this);

  auto pc_or = factory_->CreatePeerConnectionOrError(config, std::move(deps));
  if (!pc_or.ok()) {
    std::cerr << "[pc] CreatePeerConnection failed: "
              << pc_or.error().message() << "\n";
    return false;
  }
  pc_ = pc_or.MoveValue();
  RTC_LOG(LS_INFO) << "[pc] initialized";
  return true;
}

void PeerConnectionClient::Close() {
  // 关闭顺序很重要：DataChannel -> PeerConnection -> Factory -> 线程
  // 反过来会出问题（线程停了之后 PeerConnection 析构会卡）。

  {
    std::lock_guard<std::mutex> lock(dc_mutex_);
    if (dc_) {
      dc_->UnregisterObserver();  // 一定要先注销 observer，否则析构时回调到野指针
      dc_->Close();
      dc_ = nullptr;
    }
  }
  if (pc_) {
    pc_->Close();
    pc_ = nullptr;
  }
  factory_ = nullptr;

  // 停三个内部线程。注意 reset 之前要 Stop()，让线程退出事件循环。
  if (signaling_thread_) signaling_thread_->Stop();
  if (worker_thread_) worker_thread_->Stop();
  if (network_thread_) network_thread_->Stop();
  signaling_thread_.reset();
  worker_thread_.reset();
  network_thread_.reset();
}

// ===========================================================================
// 业务侧调用的高层方法
// ===========================================================================

bool PeerConnectionClient::CreateOfferAndDataChannel() {
  if (!pc_) return false;

  // 1. 先创建 DataChannel。注意：必须在 CreateOffer 之前创建，
  //    这样 offer 里才有 data 的 m= section，B 端 SetRemoteSdp(offer) 时
  //    才会触发 OnDataChannel 回调。
  webrtc::DataChannelInit init;
  init.ordered = true;  // 有序送达。文字消息适合用 ordered+reliable（默认）。
  auto dc_or = pc_->CreateDataChannelOrError(kDataChannelLabel, &init);
  if (!dc_or.ok()) {
    std::cerr << "[pc] CreateDataChannel failed: "
              << dc_or.error().message() << "\n";
    return false;
  }
  AttachDataChannel(dc_or.MoveValue());

  // 2. 创建 Offer。WebRTC 异步生成，结果通过 CreateSdpObserver 回调。
  //    完成后会触发 OnLocalSdpReady -> 调用业务方的 SdpReadyCallback。
  auto observer = webrtc::make_ref_counted<CreateSdpObserver>(this);
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  pc_->CreateOffer(observer.get(), opts);
  return true;
}

bool PeerConnectionClient::CreateAnswer() {
  if (!pc_) return false;
  // B 端的 CreateAnswer。前提：已经 SetRemoteSdp(kOffer, ...) 过了。
  // 同样是异步，结果通过 OnLocalSdpReady 触发。
  auto observer = webrtc::make_ref_counted<CreateSdpObserver>(this);
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  pc_->CreateAnswer(observer.get(), opts);
  return true;
}

bool PeerConnectionClient::SetRemoteSdp(webrtc::SdpType type,
                                       const std::string& sdp) {
  if (!pc_) return false;
  // 把字符串 SDP 反序列化为 SessionDescriptionInterface 对象。
  auto desc = webrtc::CreateSessionDescription(type, sdp);
  if (!desc) {
    std::cerr << "[pc] CreateSessionDescription failed for SDP\n";
    return false;
  }
  // 异步设置远端描述，结果通过 SetRemoteDescObserver 回调。
  auto observer = webrtc::make_ref_counted<SetRemoteDescObserver>(this);
  pc_->SetRemoteDescription(std::move(desc), observer);
  return true;
}

bool PeerConnectionClient::AddRemoteIceCandidate(const std::string& sdp_mid,
                                                 int sdp_mline_index,
                                                 const std::string& sdp) {
  if (!pc_) return false;

  // 从对端送来的 SDP 字符串重建 IceCandidate 对象。
  webrtc::SdpParseError err;
  auto candidate = webrtc::IceCandidate::Create(sdp_mid, sdp_mline_index, sdp,
                                                &err);
  if (!candidate) {
    std::cerr << "[pc] bad ice candidate: " << err.description << "\n";
    return false;
  }

  // 异步加入到 PeerConnection。失败时通过 lambda 接收错误。
  pc_->AddIceCandidate(
      std::move(candidate),
      [](webrtc::RTCError e) {
        if (!e.ok()) {
          std::cerr << "[pc] AddIceCandidate failed: " << e.message() << "\n";
        }
      });
  return true;
}

bool PeerConnectionClient::SendMessage(const std::string& text) {
  // 拿一份 dc_ 的快照（多线程下避免长时间持锁）。
  webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
  {
    std::lock_guard<std::mutex> lock(dc_mutex_);
    dc = dc_;
  }
  if (!dc) {
    std::cerr << "[pc] no data channel yet\n";
    return false;
  }
  // 必须是 open 状态才能发。kConnecting 时 send 会被丢弃。
  if (dc->state() != webrtc::DataChannelInterface::kOpen) {
    std::cerr << "[pc] data channel not open (state="
              << webrtc::DataChannelInterface::DataStateString(dc->state())
              << ")\n";
    return false;
  }

  // DataBuffer(string) 会构造一个 binary=false 的"文字"buffer。对端 OnMessage
  // 收到时也会知道这是文字。
  webrtc::DataBuffer buf(text);
  return dc->Send(buf);
}

// ===========================================================================
// 内部辅助：处理 SDP 异步结果
// ===========================================================================

void PeerConnectionClient::OnLocalSdpReady(
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
  // 走到这里表示 CreateOffer 或 CreateAnswer 异步完成了。

  // 拷一份信息出来，因为下一步 SetLocalDescription 会把 desc 的所有权拿走。
  webrtc::SdpType type = desc->GetType();
  std::string sdp;
  desc->ToString(&sdp);

  // 把这份 SDP 设置成本端描述（必做，否则 PeerConnection 状态机不动）。
  auto observer = webrtc::make_ref_counted<SetLocalDescObserver>(this);
  pc_->SetLocalDescription(std::move(desc), observer);

  // 通知业务侧"快把这段 SDP 通过信令送给对端"。
  if (sdp_ready_cb_) sdp_ready_cb_(type, sdp);
}

void PeerConnectionClient::OnSdpFailure(const std::string& where,
                                        webrtc::RTCError error) {
  std::cerr << "[pc] " << where << " failed: " << error.message() << "\n";
}

void PeerConnectionClient::AttachDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> ch) {
  std::lock_guard<std::mutex> lock(dc_mutex_);
  dc_ = std::move(ch);
  // 把"this"作为 observer 注册到 channel 上，这样 OnMessage / OnStateChange
  // 会被回调到本类。
  dc_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "[pc] data channel attached: " << dc_->label();
}

// ===========================================================================
// PeerConnectionObserver 接口实现（WebRTC 内部线程回调过来）
// ===========================================================================

void PeerConnectionClient::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  // 仅打印，业务无关。
  if (state_cb_) state_cb_(SignalingStateName(new_state));
}

void PeerConnectionClient::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  // B 端：A 端创建的 DataChannel 通过 SDP 协商后，在这里到达本端。
  // 我们立刻 Attach（注册 observer + 保存指针）。
  RTC_LOG(LS_INFO) << "[pc] OnDataChannel: " << data_channel->label();
  AttachDataChannel(std::move(data_channel));
}

void PeerConnectionClient::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  const char* names[] = {"gather:new", "gather:gathering", "gather:complete"};
  if (state_cb_ && new_state >= 0 && new_state < 3) {
    state_cb_(names[new_state]);
  }
}

void PeerConnectionClient::OnIceCandidate(
    const webrtc::IceCandidate* candidate) {
  // 关键：每次 ICE 发现一个本端地址候选，都通过这里通知我们。
  // 业务侧必须把它通过信令送给对端，让对端 AddRemoteIceCandidate。
  if (!candidate || !ice_cb_) return;
  std::string sdp;
  candidate->ToString(&sdp);
  ice_cb_(sdp, candidate->sdp_mid(), candidate->sdp_mline_index());
}

void PeerConnectionClient::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  if (state_cb_) state_cb_(IceConnectionStateName(new_state));
}

void PeerConnectionClient::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  if (state_cb_) state_cb_(PeerConnectionStateName(new_state));
}

// ===========================================================================
// DataChannelObserver 接口实现
// ===========================================================================

void PeerConnectionClient::OnStateChange() {
  // 拿一份当前状态。
  webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
  {
    std::lock_guard<std::mutex> lock(dc_mutex_);
    dc = dc_;
  }
  if (!dc) return;
  if (state_cb_) {
    state_cb_(std::string("dc:") +
              webrtc::DataChannelInterface::DataStateString(dc->state()));
  }
}

void PeerConnectionClient::OnMessage(const webrtc::DataBuffer& buffer) {
  // 收到对端发来的一条 DataChannel 消息。
  // buffer.data 是字节数组（CopyOnWriteBuffer），buffer.binary 表示是不是
  // 二进制（对面发字符串时是 false）。我们一律按文字处理。
  if (!message_cb_) return;
  std::string text(buffer.data.data<char>(), buffer.data.size());
  message_cb_(text);
}

}  // namespace audiosub
