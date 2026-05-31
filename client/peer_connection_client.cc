// 对 WebRTC PeerConnection 封装的实现
// 将 WebRTC 复杂的机制包装成 main 中容易调用的接口

#include "peer_connection_client.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

#include "rtc_base/copy_on_write_buffer.h"

#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "modules/audio_device/include/audio_device_data_observer.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win/scoped_com_initializer.h"

// 核心方法
// Initialize()                         初始化 WebRTC，创建线程、Factory、PeerConnection 等
// CreateOfferAndDataChannel()          A 端发起协商，开 channel + 创建 offer
// EnableLocalAudio()                   启用本地音频轨道，开始发送音频数据
// SetLocalAudioEnabled()               控制语音讲话开关
// CreateAnswer()                       B 端收到 offer 后， 生成 answer 并回 channel
// SetRemoteSdp()                       接收对端 SDP / ICE
// AddRemoteIceCandidate()              接收对端 ICE candidate
// SendMessage()                        通过 DataChannel 发文字（P2P 直连）

namespace audiosub {

// ===========================================================================
// DataChannel PCM 协议
// ===========================================================================
// A 端 WASAPI 直采的 raw int16 mono PCM 通过 DataChannel 以 binary 帧发送。
// 每个包前面带一个固定 16 字节 header，描述 PCM 元信息。B 端收到后按 header
// 解包，原样喂给 ASR（避免 WebRTC 编解码/降噪管线"洗"掉人声）。
#pragma pack(push, 1)
struct PcmDcHeader {
  char     magic[4];          // "PCM1"
  uint32_t sample_rate;       // 采样率，如 48000
  uint16_t channels;          // 通道数，当前固定为 1（mono）
  uint16_t bits_per_sample;   // 位深，固定 16
  uint32_t sample_count;      // int16 样本个数（不是字节数）
};
#pragma pack(pop)
static_assert(sizeof(PcmDcHeader) == 16, "PcmDcHeader must be 16 bytes");

namespace {

// Google 公开的 STUN 服务器。STUN 用来帮助本端发现自己的公网 IP，
// 让两个不同 NAT 后面的端能互连。同机/同局域网测试其实用不到。
const char kStunServer[] = "stun:stun.l.google.com:19302";

// DataChannel 的标签名（双方约定一致即可，作为通道的标识）。
const char kDataChannelLabel[] = "chat";
const char kAudioTrackLabel[] = "mic";
const char kAudioStreamId[] = "audiosub-audio";

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

const char* AudioLayerName(webrtc::AudioDeviceModule::AudioLayer layer) {
  using L = webrtc::AudioDeviceModule::AudioLayer;
  switch (layer) {
    case L::kPlatformDefaultAudio: return "PlatformDefault";
    case L::kWindowsCoreAudio: return "WindowsCoreAudio";
    case L::kWindowsCoreAudio2: return "WindowsCoreAudio2";
    default: return "OtherLayer";
  }
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

class PeerConnectionClient::RemoteAudioSink
    : public webrtc::AudioTrackSinkInterface {
 public:
  enum class Route {
    kLocal,
    kRemote,
  };

  explicit RemoteAudioSink(PeerConnectionClient* parent, Route route)
      : parent_(parent), route_(route) {}

  // WebRTC 解码完一帧远端音频后会回调到这里。我们只做轻量复制与格式整理，
  // 让真正的耗时处理留给后面的 ring buffer / pipeline / ASR。
  void OnData(const void* audio_data,
              int bits_per_sample,
              int sample_rate,
              size_t number_of_channels,
              size_t number_of_frames,
              std::optional<int64_t> absolute_capture_timestamp_ms) override {
    if (!parent_ || !audio_data || bits_per_sample != 16) {
      return;
    }

    core::PcmFrame frame;
    frame.bits_per_sample = bits_per_sample;
    frame.sample_rate = sample_rate;
    frame.channels = static_cast<int>(number_of_channels);
    frame.timestamp_ms = absolute_capture_timestamp_ms.value_or(
        static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count()));

    const auto* samples = static_cast<const int16_t*>(audio_data);
    const size_t sample_count = number_of_channels * number_of_frames;
    frame.samples.assign(samples, samples + sample_count);
    if (route_ == Route::kLocal) {
      parent_->DeliverLocalAudioFrame(frame);
    } else {
      parent_->DeliverRemoteAudioFrame(frame);
    }
  }

 private:
  PeerConnectionClient* const parent_;
  const Route route_;
};

class PeerConnectionClient::AdmCaptureObserver
    : public webrtc::AudioDeviceDataObserver {
 public:
  explicit AdmCaptureObserver(PeerConnectionClient* parent) : parent_(parent) {}

  void OnCaptureData(const void* audio_samples,
                     size_t num_samples,
                     size_t bytes_per_sample,
                     size_t num_channels,
                     uint32_t samples_per_sec) override {
    if (!parent_ || !audio_samples || bytes_per_sample != sizeof(int16_t)) {
      return;
    }

    core::PcmFrame frame;
    frame.bits_per_sample = static_cast<int>(bytes_per_sample * 8);
    frame.sample_rate = static_cast<int>(samples_per_sec);
    frame.channels = static_cast<int>(num_channels);
    frame.timestamp_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto* samples = static_cast<const int16_t*>(audio_samples);
    frame.samples.assign(samples, samples + num_samples);
    parent_->DeliverLocalAudioFrame(frame);
  }

  void OnRenderData(const void*,
                    size_t,
                    size_t,
                    size_t,
                    uint32_t) override {}

 private:
  PeerConnectionClient* const parent_;
};

// ===========================================================================
// 生命周期：构造 / Initialize / Close / 析构
// ===========================================================================

PeerConnectionClient::PeerConnectionClient() = default;

PeerConnectionClient::~PeerConnectionClient() { Close(); }

bool PeerConnectionClient::Initialize() {
  // [L3] 初始化链路：SSL -> ADM -> 三线程 -> Factory -> PeerConnection。
  // 第一步：初始化 OpenSSL/BoringSSL。WebRTC 内部用到 SSL（DTLS 加密 P2P 数据），
  // 必须在创建 PeerConnection 前调用一次。重复调用也安全。
  webrtc::InitializeSSL();

  // 第二步：准备音频设备模块（ADM），用于采集本地麦克风、播放远端音频。
  {
    com_initializer_ =
        std::make_unique<webrtc::ScopedCOMInitializer>(
            webrtc::ScopedCOMInitializer::kMTA);
    if (!com_initializer_->Succeeded()) {
      std::cerr << "[pc] warning: COM init failed, local mic capture disabled\n";
      com_initializer_.reset();
    } else {
      env_ = std::make_unique<webrtc::Environment>(webrtc::CreateEnvironment());
      // Windows 下某些驱动在某个 ADM layer 能枚举设备但 StartRecording 会失败。
      // 这里按顺序尝试多个 layer，优先选一个能成功创建的 ADM。
      const webrtc::AudioDeviceModule::AudioLayer candidate_layers[] = {
          webrtc::AudioDeviceModule::kPlatformDefaultAudio,
          webrtc::AudioDeviceModule::kWindowsCoreAudio,
      };
      for (auto layer : candidate_layers) {
        adm_ = webrtc::CreateAudioDeviceModule(*env_, layer);
        if (adm_) {
          std::cerr << "[pc] ADM created with layer=" << AudioLayerName(layer)
                    << "\n";
          break;
        }
        std::cerr << "[pc] ADM create failed with layer=" << AudioLayerName(layer)
                  << "\n";
      }
      if (!adm_) {
        std::cerr << "[pc] warning: failed to create audio device module\n";
      } else {
        adm_ = webrtc::CreateAudioDeviceWithDataObserver(
            adm_, std::make_unique<AdmCaptureObserver>(this));
      }
    }
  }

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
      /*default_adm=*/adm_,
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
  // [L8] 清理链路：按固定顺序释放，避免回调野指针/线程析构卡住。
  // 关闭顺序很重要：DataChannel -> PeerConnection -> Factory -> 线程
  // 反过来会出问题（线程停了之后 PeerConnection 析构会卡）。

  // 先停 WASAPI，避免它在 DataChannel 关掉之后还往 dc_ 发包。
  wasapi_talking_.store(false, std::memory_order_relaxed);
  wasapi_mic_.Stop();

  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (remote_audio_track_ && remote_audio_sink_) {
      remote_audio_track_->RemoveSink(remote_audio_sink_.get());
    }
    remote_audio_track_ = nullptr;
    local_audio_track_ = nullptr;
    local_audio_source_ = nullptr;
    remote_audio_sink_.reset();
  }

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
  // 兜底停掉 ADM 的录音/播放，避免线程关闭时还有音频回调在跑。
  if (adm_ && worker_thread_) {
    worker_thread_->BlockingCall([this] {
      if (adm_->Recording()) {
        adm_->StopRecording();
      }
      if (adm_->Playing()) {
        adm_->StopPlayout();
      }
    });
  }
  factory_ = nullptr;
  adm_ = nullptr;
  env_.reset();
  com_initializer_.reset();

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
  // [L4] 协商链路（A）：先开 DataChannel，再 CreateOffer。
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

// A 端采集音频核心函数
// 创建 AudioSource
// 创建 AudioTrack
// 把这条音频轨道 AddTrack 到 PeerConnection
bool PeerConnectionClient::EnableLocalAudio() {
  // [L5] 发送音频链路：创建并挂载本地音频轨道。
  if (!pc_ || !factory_) {
    return false;
  }
  if (!adm_) {
    std::cerr << "[pc] local audio capture unavailable: ADM not initialized\n";
    return false;
  }

  std::lock_guard<std::mutex> lock(audio_mutex_);
  if (local_audio_track_) {
    return true;
  }

  // 这里先用默认 AudioOptions，后续阶段再按项目需要细调回声消除、降噪等。
  webrtc::AudioOptions options;
  local_audio_source_ = factory_->CreateAudioSource(options);
  if (!local_audio_source_) {
    std::cerr << "[pc] CreateAudioSource failed\n";
    return false;
  }

  local_audio_track_ =
      factory_->CreateAudioTrack(kAudioTrackLabel, local_audio_source_.get());
  if (!local_audio_track_) {
    std::cerr << "[pc] CreateAudioTrack failed\n";
    local_audio_source_ = nullptr;
    return false;
  }

  auto add_result = pc_->AddTrack(local_audio_track_, {kAudioStreamId});
  if (!add_result.ok()) {
    std::cerr << "[pc] AddTrack(audio) failed: "
              << add_result.error().message() << "\n";
    local_audio_track_ = nullptr;
    local_audio_source_ = nullptr;
    return false;
  }

  if (state_cb_) {
    state_cb_("audio:local-track-added");
  }

  // WASAPI 模式下，仍然走旧链路：
  //   A 端 WASAPI 直采 raw PCM -> DataChannel binary -> B 端 ASR。
  // WebRTC 模式下，不启动 WASAPI，音频由 local_audio_track_ 通过 WebRTC 媒体链路发送。
  if (audio_path_.load(std::memory_order_relaxed) ==
      AudioPath::kWasapiDataChannel) {
    wasapi_talking_ = false;
    wasapi_mic_.Start([this](const int16_t* samples,
                             std::size_t sample_count,
                             int sample_rate,
                             int channels) {
      if (!wasapi_talking_.load(std::memory_order_relaxed)) {
        return;  // /talk off 状态下不向对端发送
      }
      SendPcmDataChannel(samples, sample_count, sample_rate, channels);
    });
    if (state_cb_) {
      state_cb_("audio:path-wasapi-datachannel");
    }
  } else {
    if (state_cb_) {
      state_cb_("audio:path-webrtc-track");
    }
  }

  return true;
}

bool PeerConnectionClient::SetLocalAudioEnabled(bool enabled) {
  // [L5] 讲话开关：确保录音线程可用，再切本地音轨 enabled 状态。
  std::lock_guard<std::mutex> lock(audio_mutex_);
  if (!local_audio_track_) {
    std::cerr << "[pc] local audio track not ready\n";
    return false;
  }

  // 某些 Windows 设备在 built-in AEC 路径下，仅 set_enabled(true) 不足以让
  // 采集线程真正跑起来。这里显式确保 ADM 已经进入 Recording 状态。
  if (enabled && adm_) {
    if (adm_->InitRecording() != 0) {
      std::cerr << "[pc] local audio: InitRecording failed\n";
    }
    if (!adm_->Recording() && adm_->StartRecording() != 0) {
      // 经典失败场景：要求先启动 playout，再重试 StartRecording。
      bool playout_started = false;
      if (!adm_->Playing()) {
        if (adm_->InitPlayout() == 0 && adm_->StartPlayout() == 0) {
          playout_started = true;
          std::cerr << "[pc] local audio: playout fallback started\n";
        } else {
          std::cerr << "[pc] local audio: playout fallback failed\n";
        }
      }
      if (adm_->StartRecording() != 0) {
        std::cerr << "[pc] local audio: StartRecording still failed after fallback\n";
      } else {
        std::cerr << "[pc] local audio: recording started after fallback\n";
      }
      (void)playout_started;
    } else if (adm_->Recording()) {
      std::cerr << "[pc] local audio: recording already active\n";
    }
  }
  // 重复输入 /talk on 或 /talk off 不应该算错误。
  // 如果当前轨道状态已经和目标状态一致，直接视为成功。
  if (local_audio_track_->enabled() == enabled) {
    if (state_cb_) {
      state_cb_(enabled ? "audio:talking" : "audio:silent");
    }
    return true;
  }
  if (!local_audio_track_->set_enabled(enabled)) {
    std::cerr << "[pc] failed to change local audio track state\n";
    return false;
  }
  wasapi_talking_.store(enabled, std::memory_order_relaxed);
  if (state_cb_) {
    state_cb_(enabled ? "audio:talking" : "audio:silent");
  }
  return true;
}

bool PeerConnectionClient::CreateAnswer() {
  // [L4] 协商链路（B）：收到 offer 后创建 answer。
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
  // [L4] CreateOffer/Answer 异步完成后，设置本地描述并回调应用层发信令。
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

void PeerConnectionClient::DeliverLocalAudioFrame(const core::PcmFrame& frame) {
  if (local_audio_frame_cb_) {
    local_audio_frame_cb_(frame);
  }
}

void PeerConnectionClient::DeliverRemoteAudioFrame(const core::PcmFrame& frame) {
  // WASAPI 模式：远端真实 PCM 来自 DataChannel，WebRTC AudioTrack 上的帧不送 ASR，
  // 避免静音帧/处理后音频干扰识别。
  if (audio_path_.load(std::memory_order_relaxed) ==
      AudioPath::kWasapiDataChannel) {
    return;
  }
  // WebRTC 模式：远端 PCM 来自 AudioTrackSinkInterface，交给 main.cc 里的
  // remote_audio_frame_cb_，后面会进入 ring buffer -> AsrAudioConverter -> Whisper。
  if (remote_audio_frame_cb_) {
    remote_audio_frame_cb_(frame);
  }
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

void PeerConnectionClient::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  // [L6] 远端音轨到达：绑定 AudioSink，把 PCM 帧回调给应用层。
  if (!transceiver) {
    return;
  }
  auto receiver = transceiver->receiver();
  if (!receiver) {
    return;
  }
  auto track = receiver->track();
  if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) {
    return;
  }

  auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(track.get());
  std::lock_guard<std::mutex> lock(audio_mutex_);
  if (!remote_audio_sink_) {
    remote_audio_sink_ =
        std::make_unique<RemoteAudioSink>(this, RemoteAudioSink::Route::kRemote);
  }
  if (remote_audio_track_ && remote_audio_track_ != track) {
    remote_audio_track_->RemoveSink(remote_audio_sink_.get());
  }
  remote_audio_track_ = audio_track;
  remote_audio_track_->AddSink(remote_audio_sink_.get());

  if (state_cb_) {
    state_cb_("audio:remote-track-attached");
  }
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
  //   - binary=true  -> WASAPI 路径的 raw PCM 数据包（PcmDcHeader + samples）
  //   - binary=false -> 普通文字（聊天用）
  if (buffer.binary) {
    HandlePcmDataChannel(buffer);
    return;
  }
  if (!message_cb_) return;
  std::string text(buffer.data.data<char>(), buffer.data.size());
  message_cb_(text);
}

// ===========================================================================
// WASAPI PCM 通过 DataChannel 收发
// ===========================================================================

bool PeerConnectionClient::SendPcmDataChannel(const int16_t* samples,
                                              std::size_t sample_count,
                                              int sample_rate,
                                              int channels) {
  if (!samples || sample_count == 0) return false;

  // 取一份 DataChannel 智能指针，避免在持锁期间走网络栈。
  webrtc::scoped_refptr<webrtc::DataChannelInterface> dc;
  {
    std::lock_guard<std::mutex> lock(dc_mutex_);
    dc = dc_;
  }
  if (!dc || dc->state() != webrtc::DataChannelInterface::kOpen) {
    return false;
  }

  PcmDcHeader header{};
  header.magic[0] = 'P';
  header.magic[1] = 'C';
  header.magic[2] = 'M';
  header.magic[3] = '1';
  header.sample_rate     = static_cast<uint32_t>(sample_rate);
  header.channels        = static_cast<uint16_t>(channels);
  header.bits_per_sample = 16;
  header.sample_count    = static_cast<uint32_t>(sample_count);

  const std::size_t pcm_bytes = sample_count * sizeof(int16_t);
  const std::size_t total = sizeof(PcmDcHeader) + pcm_bytes;
  std::vector<uint8_t> packet(total);
  std::memcpy(packet.data(), &header, sizeof(PcmDcHeader));
  std::memcpy(packet.data() + sizeof(PcmDcHeader), samples, pcm_bytes);

  webrtc::CopyOnWriteBuffer cow;
  cow.AppendData(packet.data(), packet.size());
  webrtc::DataBuffer data_buffer(cow, /*binary=*/true);
  return dc->Send(data_buffer);
}

void PeerConnectionClient::HandlePcmDataChannel(
    const webrtc::DataBuffer& buffer) {
  if (buffer.data.size() < sizeof(PcmDcHeader)) return;

  PcmDcHeader header{};
  std::memcpy(&header, buffer.data.data(), sizeof(PcmDcHeader));
  if (header.magic[0] != 'P' || header.magic[1] != 'C' ||
      header.magic[2] != 'M' || header.magic[3] != '1') {
    return;
  }
  if (header.bits_per_sample != 16 || header.channels == 0 ||
      header.sample_count == 0) {
    return;
  }

  const std::size_t pcm_bytes =
      static_cast<std::size_t>(header.sample_count) * sizeof(int16_t);
  if (buffer.data.size() < sizeof(PcmDcHeader) + pcm_bytes) return;

  const int16_t* samples = reinterpret_cast<const int16_t*>(
      buffer.data.data() + sizeof(PcmDcHeader));

  // 投递给业务侧。直接复用 remote_audio_frame_cb_：业务侧 main.cc 里早就接好
  // 了这个回调把 PCM 推进 ASR 缓冲。
  if (!remote_audio_frame_cb_) return;
  core::PcmFrame frame;
  frame.sample_rate     = static_cast<int>(header.sample_rate);
  frame.channels        = static_cast<int>(header.channels);
  frame.bits_per_sample = 16;
  frame.timestamp_ms    = 0;
  frame.samples.assign(samples, samples + header.sample_count);
  remote_audio_frame_cb_(frame);
}

}  // namespace audiosub
