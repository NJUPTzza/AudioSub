// peer_connection_client.cc
// =========================
// WebRTC PeerConnection 封装的实现。
//
// 主要复杂度集中在 3 个地方：
//   1. 三个 WebRTC 内部线程的启动和销毁
//   2. SDP 异步流程：CreateOffer/Answer -> SetLocalDescription -> 信令送出
//   3. 各种 Observer 嵌套类（WebRTC 要求 SDP observer 必须是 RefCounted）
#include "api/environment/environment_factory.h"
#include "modules/audio_device/include/audio_device_factory.h"
#include <mutex>
#ifdef _WIN32
#include <objbase.h>
#endif
#include <cstdlib>
#include "peer_connection_client.h"
#include <cstdint>
#include <atomic>
#include <iostream>
#include <utility>
#include "media/base/media_channel.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/audio_options.h"
#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"
#include <chrono>
#include <thread>
#include "wasapi_mic_capture.h"
#include <cstring>
#include <vector>
#include "rtc_base/copy_on_write_buffer.h"
#include "api/data_channel_interface.h"
namespace audiosub {
#pragma pack(push, 1)
    struct PcmDcHeader {
        char magic[4];              // "PCM1"
        uint32_t sample_rate;       // 例如 48000
        uint16_t channels;          // 当前先发 1
        uint16_t bits_per_sample;   // 16
        uint32_t sample_count;      // int16_t 样本数量
    };
#pragma pack(pop)

    static_assert(sizeof(PcmDcHeader) == 16, "PcmDcHeader must be 16 bytes");
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

class AudioEnergyDebugSink : public webrtc::AudioTrackSinkInterface {
public:
    explicit AudioEnergyDebugSink(const char* tag) : tag_(tag) {}

    void OnData(const void* audio_data,
        int bits_per_sample,
        int sample_rate,
        size_t number_of_channels,
        size_t number_of_frames,
        absl::optional<int64_t> absolute_capture_timestamp_ms) override {
        if (!audio_data || bits_per_sample != 16) {
            return;
        }

        const auto* pcm = static_cast<const int16_t*>(audio_data);
        const size_t sample_count = number_of_channels * number_of_frames;

        int64_t sum_abs = 0;
        int peak = 0;

        for (size_t i = 0; i < sample_count; ++i) {
            const int v = std::abs(static_cast<int>(pcm[i]));
            sum_abs += v;
            if (v > peak) {
                peak = v;
            }
        }

        const int avg_abs = sample_count > 0
            ? static_cast<int>(sum_abs / sample_count)
            : 0;

        const int n = ++count_;

        // 只打印前 50 次，避免刷屏。
        if (n <= 50) {
            std::cout << tag_
                << " #" << n
                << ": sample_rate=" << sample_rate
                << ", channels=" << number_of_channels
                << ", frames=" << number_of_frames
                << ", avg_abs=" << avg_abs
                << ", peak=" << peak
                << std::endl;
        }
    }

private:
    const char* tag_ = "";
    std::atomic<int> count_{ 0 };
};

class PeerConnectionClient::RemoteAudioSink
    : public webrtc::AudioTrackSinkInterface {
public:
    explicit RemoteAudioSink(core::IAudioFrameConsumer* consumer)
        : consumer_(consumer) {
    }

    void OnData(const void* audio_data,
        int bits_per_sample,
        int sample_rate,
        size_t number_of_channels,
        size_t number_of_frames,
        absl::optional<int64_t> absolute_capture_timestamp_ms) override {
        // 音频回调线程：这里只做轻量日志和轻量复制，后续不要在这里直接跑 ASR。
        static std::atomic<int> frame_count{ 0 };
        const int n = ++frame_count;

        // 只打印前 5 帧，用于证明 B 端已经成功收到 PCM。
        if (n <= 5) {
            std::cout << "[audio] pcm frame #" << n
                << ": sample_rate=" << sample_rate
                << ", channels=" << number_of_channels
                << ", frames=" << number_of_frames
                << ", bits=" << bits_per_sample;

            if (absolute_capture_timestamp_ms.has_value()) {
                std::cout << " ts_ms=" << *absolute_capture_timestamp_ms;
            }

            std::cout << std::endl;

            if (n == 5) {
                std::cout << "[audio] PCM verification done, input is available."
                    << std::endl;
                std::cout << "[you] " << std::flush;
            }
        }

        // 当前阶段只处理 16-bit PCM。其他格式先忽略，避免误投递。
        if (!consumer_ || !audio_data || bits_per_sample != 16) {
            return;
        }

        const auto* pcm = static_cast<const int16_t*>(audio_data);
        const size_t sample_count = number_of_channels * number_of_frames;
        int64_t sum_abs = 0;
        int peak = 0;

        for (size_t i = 0; i < sample_count; ++i) {
            const int v = std::abs(static_cast<int>(pcm[i]));
            sum_abs += v;
            if (v > peak) {
                peak = v;
            }
        }

        const int avg_abs = sample_count > 0
            ? static_cast<int>(sum_abs / sample_count)
            : 0;

        static std::atomic<int> energy_log_count{ 0 };
        const int energy_n = ++energy_log_count;

        if (energy_n <= 30) {
            std::cout << "[audio-energy] avg_abs=" << avg_abs
                << ", peak=" << peak << std::endl;
        }
        core::PcmFrame frame;
        frame.sample_rate = sample_rate;
        frame.channels = static_cast<int>(number_of_channels);
        frame.bits_per_sample = bits_per_sample;
        frame.timestamp_ms = absolute_capture_timestamp_ms.value_or(0);
        frame.samples.assign(pcm, pcm + sample_count);

        // 后续可接入 PassThroughAudioPipeline / RingBuffer / ASR。
        // 注意：这里仍然只是轻量投递，不在音频回调线程里做重处理。
        consumer_->OnPcmFrame(frame);
    }

private:
    core::IAudioFrameConsumer* consumer_ = nullptr;
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
#ifdef _WIN32
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) {
      com_initialized_ = true;
      std::cout << "[audio] COM initialized for Core Audio ADM\n";
  }
  else if (hr == RPC_E_CHANGED_MODE) {
      // 当前线程已经用另一种 COM 模式初始化过。先继续尝试创建 ADM。
      std::cout << "[audio] COM already initialized with different mode\n";
  }
  else {
      std::cerr << "[audio] CoInitializeEx failed, hr=0x"
          << std::hex << static_cast<unsigned long>(hr)
          << std::dec << "\n";
      return false;
  }
#endif

  webrtc::Environment env = webrtc::CreateEnvironment();

  audio_device_module_ =
      webrtc::CreateWindowsCoreAudioAudioDeviceModule(env);

  if (!audio_device_module_) {
      std::cerr << "[audio] CreateWindowsCoreAudioAudioDeviceModule failed\n";
      return false;
  }

  std::cout << "[audio] Windows Core Audio ADM created\n";
  const int adm_init_ret = audio_device_module_->Init();
  std::cout << "[audio] ADM Init ret=" << adm_init_ret << "\n";

  // 打印 WebRTC ADM 能看到的录音设备列表
  const int16_t rec_device_count = audio_device_module_->RecordingDevices();
  std::cout << "[audio] RecordingDevices count=" << rec_device_count << "\n";

  for (int16_t i = 0; i < rec_device_count; ++i) {
      char name[256] = {};
      char guid[256] = {};

      const int name_ret = audio_device_module_->RecordingDeviceName(
          static_cast<uint16_t>(i),
          name,
          guid);

      std::cout << "[audio] rec device #" << i
          << ", ret=" << name_ret
          << ", name=" << name
          << ", guid=" << guid
          << "\n";
  }

  // 先继续用默认设备，等看到设备编号后，再改成 SetRecordingDevice(编号)
  const int set_rec_ret = audio_device_module_->SetRecordingDevice(1);

  std::cout << "[audio] SetRecordingDevice(index 1) ret="
      << set_rec_ret << "\n";

  const int stereo_rec_ret = audio_device_module_->SetStereoRecording(false);
  std::cout << "[audio] SetStereoRecording(false) ret="
      << stereo_rec_ret << "\n";

  bool rec_available = false;
  const int rec_available_ret =
      audio_device_module_->RecordingIsAvailable(&rec_available);

  std::cout << "[audio] RecordingIsAvailable ret="
      << rec_available_ret
      << ", available=" << (rec_available ? "true" : "false")
      << "\n";

  const int init_rec_ret = audio_device_module_->InitRecording();
  std::cout << "[audio] InitRecording ret=" << init_rec_ret << "\n";

  const bool rec_initialized =
      audio_device_module_->RecordingIsInitialized();

  std::cout << "[audio] RecordingIsInitialized="
      << (rec_initialized ? "true" : "false")
      << "\n";

  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(),
      worker_thread_.get(),
      signaling_thread_.get(),
      /*default_adm=*/audio_device_module_,
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
    if (remote_audio_track_ && remote_audio_sink_) {
        remote_audio_track_->RemoveSink(remote_audio_sink_.get());
        remote_audio_track_ = nullptr;
        remote_audio_sink_.reset();
    }

    local_audio_track_ = nullptr;
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

#ifdef _WIN32
  if (com_initialized_) {
      CoUninitialize();
      com_initialized_ = false;
  }
#endif
}

// ===========================================================================
// 业务侧调用的高层方法
// ===========================================================================
bool PeerConnectionClient::AddLocalAudioTrack() {
    if (!factory_ || !pc_) return false;

    webrtc::AudioOptions audio_options;
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source =
        factory_->CreateAudioSource(audio_options);

    local_audio_track_ =
        factory_->CreateAudioTrack("audiosub_audio", audio_source.get());
    
    if (!local_audio_track_) {
        std::cerr << "[audio] CreateAudioTrack failed\n";
        return false;
    }
    local_audio_track_->set_enabled(true);
    static AudioEnergyDebugSink s_local_audio_debug_sink("[local-audio]");
    local_audio_track_->AddSink(&s_local_audio_debug_sink);

    auto result = pc_->AddTrack(local_audio_track_, { "audiosub_stream" });
    if (!result.ok()) {
        std::cerr << "[audio] AddTrack failed: "
            << result.error().message() << "\n";
        local_audio_track_ = nullptr;
        return false;
    }

    std::cout << "[audio] local audio track added\n";

    if (audio_device_module_) {
        std::cout << "[audio] ADM Recording after AddTrack="
            << (audio_device_module_->Recording() ? "true" : "false")
            << "\n";

        auto adm = audio_device_module_;

        std::thread([adm]() {
            for (int i = 1; i <= 8; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                std::cout << "[audio] ADM Recording delayed check #"
                    << i << "="
                    << (adm->Recording() ? "true" : "false")
                    << "\n";
            }
            }).detach();
    }
    return true;
}
bool PeerConnectionClient::CreateOfferAndDataChannel() {
    if (!pc_) return false;
    is_offer_side_ = true;
    // A 端：先添加本地音频轨道。必须在 CreateOffer 之前添加，
    // 这样 offer SDP 中才会包含 audio m-section。
  /*  if (!AddLocalAudioTrack()) {
        std::cerr << "[audio] failed to add local audio track\n";
        return false;
    }*/

    // 第一阶段：A 端直接用 WASAPI 采集麦克风，只打印能量，不发送
    wasapi_mic_.Start([this](const int16_t* samples,
        size_t sample_count,
        int sample_rate,
        int channels) {
            SendPcmDataChannel(samples, sample_count, sample_rate, channels);
        });


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

bool PeerConnectionClient::SendPcmDataChannel(const int16_t* samples,
    size_t sample_count,
    int sample_rate,
    int channels) {
    if (!samples || sample_count == 0) {
        return false;
    }

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
    header.sample_rate = static_cast<uint32_t>(sample_rate);
    header.channels = static_cast<uint16_t>(channels);
    header.bits_per_sample = 16;
    header.sample_count = static_cast<uint32_t>(sample_count);

    const size_t pcm_bytes = sample_count * sizeof(int16_t);
    const size_t total_bytes = sizeof(PcmDcHeader) + pcm_bytes;

    std::vector<uint8_t> packet(total_bytes);

    std::memcpy(packet.data(),
        &header,
        sizeof(PcmDcHeader));

    std::memcpy(packet.data() + sizeof(PcmDcHeader),
        samples,
        pcm_bytes);

    webrtc::CopyOnWriteBuffer cow;
    cow.AppendData(packet.data(), packet.size());

    webrtc::DataBuffer data_buffer(cow, true);
    return dc->Send(data_buffer);
}

void PeerConnectionClient::HandlePcmDataChannel(const webrtc::DataBuffer& buffer) {
    if (buffer.data.size() < sizeof(PcmDcHeader)) {
        return;
    }

    PcmDcHeader header{};
    std::memcpy(&header, buffer.data.data<uint8_t>(), sizeof(PcmDcHeader));

    if (header.magic[0] != 'P' ||
        header.magic[1] != 'C' ||
        header.magic[2] != 'M' ||
        header.magic[3] != '1') {
        return;
    }

    if (header.bits_per_sample != 16 ||
        header.channels == 0 ||
        header.sample_count == 0) {
        return;
    }

    const size_t pcm_bytes =
        static_cast<size_t>(header.sample_count) * sizeof(int16_t);

    if (buffer.data.size() < sizeof(PcmDcHeader) + pcm_bytes) {
        return;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(
        buffer.data.data<uint8_t>() + sizeof(PcmDcHeader));

    int64_t sum_abs = 0;
    int peak = 0;

    for (uint32_t i = 0; i < header.sample_count; ++i) {
        const int v = std::abs(static_cast<int>(samples[i]));
        sum_abs += v;
        peak = std::max(peak, v);
    }

    const int avg_abs =
        static_cast<int>(sum_abs / static_cast<int64_t>(header.sample_count));

  /*  static int debug_count = 0;
    if (debug_count < 200) {
        std::cout << "[dc-pcm] sample_rate=" << header.sample_rate
            << ", channels=" << header.channels
            << ", samples=" << header.sample_count
            << ", avg_abs=" << avg_abs
            << ", peak=" << peak
            << "\n";
        ++debug_count;
    }*/
    if (!audio_consumer_) {
        return;
    }

    core::PcmFrame frame;
    frame.sample_rate = static_cast<int>(header.sample_rate);
    frame.channels = static_cast<int>(header.channels);
    frame.bits_per_sample = 16;
    frame.timestamp_ms = 0;
    frame.samples.assign(samples, samples + header.sample_count);

    audio_consumer_->OnPcmFrame(frame);
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

void PeerConnectionClient::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (!transceiver || !transceiver->receiver()) {
        return;
    }

    auto track = transceiver->receiver()->track();
    if (!track) {
        return;
    }

    std::cout << "[pc] OnTrack: kind=" << track->kind() << std::endl;

    if (track->kind() != webrtc::MediaStreamTrackInterface::kAudioKind) {
        return;
    }

    auto* audio_track_raw =
        static_cast<webrtc::AudioTrackInterface*>(track.get());

    remote_audio_track_ = audio_track_raw;
    //remote_audio_sink_ = std::make_unique<RemoteAudioSink>();
    remote_audio_sink_ = std::make_unique<RemoteAudioSink>(audio_consumer_);
    remote_audio_track_->AddSink(remote_audio_sink_.get());

    std::cout << "[audio] remote audio track received, sink attached"
        << std::endl;
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
    if (state_cb_) {
        state_cb_(IceConnectionStateName(new_state));
    }

    // 只在 A 端，并且 ICE completed 后，打印一次语音输入提示。
    if (is_offer_side_ &&
        !a_ready_prompt_printed_ &&
        new_state == webrtc::PeerConnectionInterface::kIceConnectionCompleted) {
        a_ready_prompt_printed_ = true;

        if (state_cb_) {
            state_cb_("************************这里是语宙工坊！**************************");
            state_cb_("【A端语音发送已就绪】");
            state_cb_("为避免噪音，现在请插上耳机对着 A 端麦克风大声说话，语音将发送到 B 端。");
            state_cb_("B 端将显示语音识别字幕，会有短暂延迟。当B端字幕显示当前语句后再开始下一句语音");
            state_cb_("==================================================");
        }
    }
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

        // 只在 A 端 DataChannel 打开后提示一次，B 端不显示。
        //if (is_offer_side_ &&
        //    !a_ready_prompt_printed_ &&
        //    dc->state() == webrtc::DataChannelInterface::kOpen) {
        //    a_ready_prompt_printed_ = true;

        //    state_cb_("========================这里是语宙工坊！==========================");
        //    state_cb_("【A端语音发送已就绪】");
        //    state_cb_("现在请对着 A 端麦克风大声说话，语音将发送到 B 端。");
        //    state_cb_("B 端将显示语音识别字幕，可能会有短暂延迟。");
        //    state_cb_("==================================================");
        //}
    }
}

void PeerConnectionClient::OnMessage(const webrtc::DataBuffer& buffer) {
    if (buffer.binary) {
        HandlePcmDataChannel(buffer);
        return;
    }

    if (!message_cb_) return;

    std::string text(buffer.data.data<char>(), buffer.data.size());
    message_cb_(text);
}

}  // namespace audiosub
