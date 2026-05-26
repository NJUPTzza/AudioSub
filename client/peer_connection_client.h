// peer_connection_client.h
// ========================
// WebRTC PeerConnection 的高层封装。把 WebRTC 那一堆"线程 + 工厂 + Observer +
// Promise 风格异步"的复杂 API 包装成 4 个高层方法 + 4 个回调，业务层（main.cc）
// 不需要直接接触 WebRTC 的细节。
//
// ============================================================================
// WebRTC 协商流程速览（你看代码前必须先理解的概念）
// ============================================================================
//
// 角色：
//   - Offerer (A 端)：主动发起连接的一方
//   - Answerer (B 端)：应答的一方
//
// 完整握手时序：
//
//   A 端                          信令服务器                   B 端
//   |                                |                          |
//   |  CreateOffer()                 |                          |
//   |  生成 SDP Offer                |                          |
//   |---- {type:"offer", sdp} ---->  |                          |
//   |                                |--- {type:"offer"} ---->  |
//   |                                |                          | SetRemoteDescription(offer)
//   |                                |                          | CreateAnswer()
//   |                                |                          | 生成 SDP Answer
//   |                                | <-- {type:"answer"} ---- |
//   |  <-- {type:"answer"} ---       |                          |
//   |  SetRemoteDescription(answer)  |                          |
//   |                                |                          |
//   |   ---- ICE candidates ----     |   --- ICE candidates --- |
//   |   双向交换网络地址候选          |   服务器纯转发            |
//   |                                |                          |
//   |   ICE 协商完成（NAT 穿透）     |                          |
//   |   DTLS 握手完成（加密）        |                          |
//   |   <===== P2P 通道建立 =====>   |  ===================>    |
//   |                                |                          |
//   |   <----- DataChannel 上下行直接走 P2P，绕过服务器 ----->  |
//
// 关键名词：
//   - SDP (Session Description Protocol)：一段文本，描述本端支持的编解码、
//     ICE 凭证、媒体轨道等。Offer 和 Answer 都是 SDP。
//   - ICE Candidate：一个"我可以在 IP:Port 这里被联系到"的候选地址。一端会
//     生成多条 candidate（公网 IP、私网 IP、relay 等），通过信令交换给对端，
//     最后两端各自选出一条能互通的 pair。
//   - DTLS：在 UDP 上做 TLS，让 P2P 通道加密。WebRTC 内部自动处理。
//   - DataChannel：基于 SCTP 的可靠/有序消息通道，类似 WebSocket，但走 P2P。
//
// ============================================================================
// 本类对外接口
// ============================================================================
//
// 4 个方法（由业务/信令侧调用）：
//   Initialize()                  启动 3 个内部线程，创建 Factory + PeerConnection
//   CreateOfferAndDataChannel()   A 端：开 channel + 创建 offer
//   CreateAnswer()                B 端：收到 offer 后回 answer
//   SetRemoteSdp() / AddRemoteIceCandidate()  接收对端 SDP / ICE
//   SendMessage()                 通过 DataChannel 发文字（P2P 直连）
//
// 4 个回调（由本类异步触发，业务侧设置）：
//   SdpReadyCallback        本端 SDP 生成好了，业务侧应该发给对端
//   IceCandidateCallback    本端发现一个 ICE candidate，业务侧应该发给对端
//   MessageCallback         对端通过 DataChannel 发来一条文字
//   StateCallback           连接状态变化（信令/ICE/DataChannel 等）
//
// 业务层只要"按提示办事"：回调里说要送出什么，就通过信令送出。
// ============================================================================

#pragma once
#include "audiosub/core/interfaces.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "api/audio/audio_device.h"
#include "api/environment/environment_factory.h"
#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"
#include "wasapi_mic_capture.h"
namespace audiosub {

// PeerConnectionClient 同时实现两个 WebRTC 回调接口：
//   PeerConnectionObserver   PeerConnection 级别的事件（ICE、状态变化、收到 DataChannel）
//   DataChannelObserver      DataChannel 级别的事件（状态变化、收到消息）
// 这两个接口都是普通 C++ 接口（不是引用计数），可以直接继承。
class PeerConnectionClient : public webrtc::PeerConnectionObserver,
                             public webrtc::DataChannelObserver {
 public:
  // === 业务侧需要实现的 4 个回调签名 ===

  // 本端生成了一段 SDP（Offer 或 Answer），业务侧需要通过信令送给对端。
  using SdpReadyCallback =
      std::function<void(webrtc::SdpType type, const std::string& sdp)>;

  // 本端发现了一个 ICE candidate，业务侧需要通过信令送给对端。
  // sdp_mid / sdp_mline_index 是这个 candidate 属于哪个 m= section（业务上当 cookie 传回即可）。
  using IceCandidateCallback =
      std::function<void(const std::string& candidate,
                         const std::string& sdp_mid,
                         int sdp_mline_index)>;

  // 收到对端通过 DataChannel 发来的一条文字消息。
  using MessageCallback = std::function<void(const std::string& text)>;

  // 状态变化通知（信令状态、ICE 状态、PeerConnection 状态、DataChannel 状态等）。
  // 仅用于打印/UI 显示，不影响逻辑。
  using StateCallback = std::function<void(const std::string& state)>;

  PeerConnectionClient();
  ~PeerConnectionClient() override;

  // 含 thread / socket / refcount，禁止拷贝。
  PeerConnectionClient(const PeerConnectionClient&) = delete;
  PeerConnectionClient& operator=(const PeerConnectionClient&) = delete;

  // 启动 WebRTC：建 3 个线程、创建 Factory、创建 PeerConnection。
  // 必须先调它，再调其他业务方法。失败返回 false。
  bool Initialize();

  // A 端入口：创建一个名为 "chat" 的 DataChannel，然后生成 Offer。
  // Offer 生成完会通过 SdpReadyCallback 通知业务侧。
  bool CreateOfferAndDataChannel();

  // B 端入口：在 SetRemoteSdp(kOffer, ...) 之后调用，生成 Answer。
  // Answer 生成完通过 SdpReadyCallback 通知。
  bool CreateAnswer();

  // 设置对端的 SDP。A 端用 type=kAnswer，B 端用 type=kOffer。
  bool SetRemoteSdp(webrtc::SdpType type, const std::string& sdp);

  // 添加对端发来的 ICE candidate。可以被调用多次（一端可能有 N 个 candidate）。
  bool AddRemoteIceCandidate(const std::string& sdp_mid,
                             int sdp_mline_index,
                             const std::string& sdp);

  // 通过 DataChannel 发送文字。返回 false 表示 channel 还没 open。
  bool SendMessage(const std::string& text);

  // 优雅关闭：停掉所有线程、释放 WebRTC 资源。析构时也会自动调用。
  void Close();

  // === 业务侧设置回调 ===
  // 这些 setter 不是线程安全的：约定在 Initialize() 之后、协商开始之前调用一次。
  void SetSdpReadyCallback(SdpReadyCallback cb) { sdp_ready_cb_ = std::move(cb); }
  void SetIceCandidateCallback(IceCandidateCallback cb) { ice_cb_ = std::move(cb); }
  void SetMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
  void SetStateCallback(StateCallback cb) { state_cb_ = std::move(cb); }
  void SetAudioFrameConsumer(core::IAudioFrameConsumer* consumer)
  {audio_consumer_ = consumer;}
  // === PeerConnectionObserver 接口实现（WebRTC 触发） ===
  // 这些方法都是 WebRTC 内部线程回调过来的，不能阻塞太久。

  // 信令状态变化（stable / have-local-offer / have-remote-offer / closed 等）
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;

  // B 端：对端创建的 DataChannel 到达本端时被调用。
  // 我们在这里 RegisterObserver，绑定到 this 上接收消息。
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
  // 收到远端媒体轨道。P0 音频链路在这里接收远端 AudioTrack。
  void OnTrack(
      webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;

  // 协商需要重新走一遍（比如改了媒体配置）。我们暂时不处理。
  void OnRenegotiationNeeded() override {}

  // ICE 候选收集状态变化（new / gathering / complete）
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;

  // 关键：发现了一个本端的 ICE candidate。要通过信令送给对端。
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;

  // ICE 整体连接状态变化（checking / connected / failed 等）
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;

  // 端到端连接状态变化（new / connecting / connected / disconnected / failed / closed）
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

  // === DataChannelObserver 接口实现 ===

  // DataChannel 状态变化（connecting -> open -> closing -> closed）
  void OnStateChange() override;

  // 收到一条消息。我们当成 UTF-8 文字交给业务回调。
  void OnMessage(const webrtc::DataBuffer& buffer) override;

  // 缓冲区水位变化。文字 demo 用不到。
  void OnBufferedAmountChange(uint64_t /*sent_data_size*/) override {}

 private:
  // === 三个 SDP Observer 嵌套类（前向声明，定义在 .cc）===
  // 这三个是 WebRTC 的 RefCounted 接口，必须用 webrtc::scoped_refptr 持有，
  // 不能让 PeerConnectionClient 直接继承（因为外部用裸指针管理本类）。
  class CreateSdpObserver;       // 接 CreateOffer/CreateAnswer 的异步结果
  class SetLocalDescObserver;    // 接 SetLocalDescription 的异步结果
  class SetRemoteDescObserver;   // 接 SetRemoteDescription 的异步结果
  class RemoteAudioSink;
  // CreateOffer/CreateAnswer 成功后被嵌套类调用。
  void OnLocalSdpReady(std::unique_ptr<webrtc::SessionDescriptionInterface> desc);

  // 任何 SDP 操作失败后被嵌套类调用，统一打日志。
  void OnSdpFailure(const std::string& where, webrtc::RTCError error);

  // 把 DataChannel 绑定到本类（注册 observer + 保存指针）。
  // A 端在 CreateOfferAndDataChannel 里调一次（主动创建）。
  // B 端在 OnDataChannel 里调一次（被动接收）。
  void AttachDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> ch);
  // A 端：创建并添加本地音频轨道，让 offer SDP 包含 audio。
  bool AddLocalAudioTrack();
  // === 状态 ===
  bool SendPcmDataChannel(const int16_t* samples,
      size_t sample_count,
      int sample_rate,
      int channels);

  void HandlePcmDataChannel(const webrtc::DataBuffer& buffer);
  // WebRTC 的 3 个内部线程，规范要求：
  //   network_thread   跑 socket I/O（必须用 CreateWithSocketServer）
  //   worker_thread    跑编解码、媒体处理
  //   signaling_thread 跑 PeerConnection 内部状态机
  // 多线程模型让 WebRTC 不阻塞业务线程。
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;
  WasapiMicCapture wasapi_mic_;
  // PeerConnectionFactory 是创建 PeerConnection 的工厂。整个进程一般共享一个，
  // 这里为了简化每个 PeerConnectionClient 自己持有一份。
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  bool com_initialized_ = false;
  // 真正的 P2P 连接对象。
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

  // DataChannel 可能被多个线程访问（业务线程 SendMessage、WebRTC 内部线程
  // 触发 OnMessage），用 mutex 保护它的指针读写。
  std::mutex dc_mutex_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> dc_;
  bool is_offer_side_ = false;
  bool a_ready_prompt_printed_ = false;
  // A 端本地音频轨道。持有引用，避免被提前释放。
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> local_audio_track_;
  core::IAudioFrameConsumer* audio_consumer_ = nullptr;
  // B 端远端音频轨道和 PCM sink。
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> remote_audio_track_;
  std::unique_ptr<RemoteAudioSink> remote_audio_sink_;
  // === 业务回调 ===
  SdpReadyCallback sdp_ready_cb_;
  IceCandidateCallback ice_cb_;
  MessageCallback message_cb_;
  StateCallback state_cb_;
};

}  // namespace audiosub
