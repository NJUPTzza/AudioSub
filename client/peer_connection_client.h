// WebRTC PeerConnection 的高层封装

// 核心方法
// Initialize()                         初始化 WebRTC，创建线程、Factory、PeerConnection 等
// CreateOfferAndDataChannel()          A 端发起协商，开 channel + 创建 offer
// EnableLocalAudio()                   启用本地音频轨道，开始发送音频数据
// SetLocalAudioEnabled()               控制语音讲话开关
// CreateAnswer()                       B 端收到 offer 后， 生成 answer 并回 channel
// SetRemoteSdp()                       接收对端 SDP / ICE
// AddRemoteIceCandidate()              接收对端 ICE candidate
// SendMessage()                        通过 DataChannel 发文字（P2P 直连）



#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "core/types.h"
#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

namespace webrtc {
class ScopedCOMInitializer;
class AudioDeviceModule;
class Environment;
}

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

  // 收到远端音频轨道解码后的 PCM 数据。用于把 WebRTC 音频链路接到后续
  // ring buffer / audio pipeline / ASR。
  using RemoteAudioFrameCallback =
      std::function<void(const core::PcmFrame& frame)>;

  // 收到本地麦克风轨道上的 PCM 数据。用于确认 A 端本地采集是否真的有声音。
  using LocalAudioFrameCallback =
      std::function<void(const core::PcmFrame& frame)>;

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

  // 在本端挂一条来自系统麦克风的音频轨道。
  // 约定在 A 端创建 Offer 之前调用一次，让首个 Offer 就带上音频 m= section。
  bool EnableLocalAudio();

  // 控制本地音频轨道是否向对端发送有效语音。
  // true  表示 A 端开始讲话；
  // false 表示 A 端结束讲话（轨道仍保留，但发送静音）。
  bool SetLocalAudioEnabled(bool enabled);

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
  void SetRemoteAudioFrameCallback(RemoteAudioFrameCallback cb) {
    remote_audio_frame_cb_ = std::move(cb);
  }
  void SetLocalAudioFrameCallback(LocalAudioFrameCallback cb) {
    local_audio_frame_cb_ = std::move(cb);
  }
  void SetStateCallback(StateCallback cb) { state_cb_ = std::move(cb); }

  // === PeerConnectionObserver 接口实现（WebRTC 触发） ===
  // 这些方法都是 WebRTC 内部线程回调过来的，不能阻塞太久。

  // 信令状态变化（stable / have-local-offer / have-remote-offer / closed 等）
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;

  // B 端：对端创建的 DataChannel 到达本端时被调用。
  // 我们在这里 RegisterObserver，绑定到 this 上接收消息。
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;

  // Unified Plan 下，远端媒体轨道会在 SetRemoteDescription 过程中从这里到达。
  // 第一个需求需要在这里抓到远端音频轨道并注册 PCM sink。
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
  class RemoteAudioSink;         // 把 WebRTC AudioTrack 回调转成统一 PcmFrame
  class AdmCaptureObserver;      // 直接从音频设备层拿本地麦克风 PCM

  // CreateOffer/CreateAnswer 成功后被嵌套类调用。
  void OnLocalSdpReady(std::unique_ptr<webrtc::SessionDescriptionInterface> desc);

  // 任何 SDP 操作失败后被嵌套类调用，统一打日志。
  void OnSdpFailure(const std::string& where, webrtc::RTCError error);

  // 把 DataChannel 绑定到本类（注册 observer + 保存指针）。
  // A 端在 CreateOfferAndDataChannel 里调一次（主动创建）。
  // B 端在 OnDataChannel 里调一次（被动接收）。
  void AttachDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> ch);

  // 供 RemoteAudioSink 回调，把 PCM 转交给业务侧。
  void DeliverLocalAudioFrame(const core::PcmFrame& frame);
  void DeliverRemoteAudioFrame(const core::PcmFrame& frame);

  // === 状态 ===

  // WebRTC 的 3 个内部线程，规范要求：
  //   network_thread   跑 socket I/O（必须用 CreateWithSocketServer）
  //   worker_thread    跑编解码、媒体处理
  //   signaling_thread 跑 PeerConnection 内部状态机
  // 多线程模型让 WebRTC 不阻塞业务线程。
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;

  // 本地麦克风采集依赖平台音频设备模块。Windows 下需要先把当前线程初始化为 COM。
  std::unique_ptr<webrtc::ScopedCOMInitializer> com_initializer_;
  std::unique_ptr<webrtc::Environment> env_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;

  // PeerConnectionFactory 是创建 PeerConnection 的工厂。整个进程一般共享一个，
  // 这里为了简化每个 PeerConnectionClient 自己持有一份。
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;

  // 真正的 P2P 连接对象。
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

  // DataChannel 可能被多个线程访问（业务线程 SendMessage、WebRTC 内部线程
  // 触发 OnMessage），用 mutex 保护它的指针读写。
  std::mutex dc_mutex_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> dc_;

  // 本地发送的麦克风轨道，以及远端到达后绑定 sink 的音频轨道。
  std::mutex audio_mutex_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> local_audio_source_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> local_audio_track_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> remote_audio_track_;
  std::unique_ptr<RemoteAudioSink> remote_audio_sink_;

  // === 业务回调 ===
  SdpReadyCallback sdp_ready_cb_;
  IceCandidateCallback ice_cb_;
  MessageCallback message_cb_;
  LocalAudioFrameCallback local_audio_frame_cb_;
  RemoteAudioFrameCallback remote_audio_frame_cb_;
  StateCallback state_cb_;
};

}  // namespace audiosub
