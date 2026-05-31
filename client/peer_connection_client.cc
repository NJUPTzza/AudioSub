#include "peer_connection_client.h"

#include <iostream>
#include <utility>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/audio_options.h"
#include "api/media_stream_interface.h"
#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"

#ifdef _WIN32
#include "rtc_base/win/scoped_com_initializer.h"
#endif

namespace audiosub {

namespace {

const char kStunServer[] = "stun:stun.l.google.com:19302";
const char kDataChannelLabel[] = "chat";

const char* SignalingStateName(
    webrtc::PeerConnectionInterface::SignalingState s) {
  using S = webrtc::PeerConnectionInterface::SignalingState;
  switch (s) {
    case S::kStable: return "stable";
    case S::kHaveLocalOffer: return "have-local-offer";
    case S::kHaveLocalPrAnswer: return "have-local-pranswer";
    case S::kHaveRemoteOffer: return "have-remote-offer";
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
    case S::kIceConnectionChecking: return "ice:checking";
    case S::kIceConnectionConnected: return "ice:connected";
    case S::kIceConnectionCompleted: return "ice:completed";
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
    case S::kConnecting: return "pc:connecting";
    case S::kConnected: return "pc:connected";
    case S::kDisconnected: return "pc:disconnected";
    case S::kFailed: return "pc:failed";
    case S::kClosed: return "pc:closed";
  }
  return "pc:?";
}

}  // namespace

class PeerConnectionClient::CreateSdpObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateSdpObserver(PeerConnectionClient* parent) : parent_(parent) {}

  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    parent_->OnLocalSdpReady(
        std::unique_ptr<webrtc::SessionDescriptionInterface>(desc));
  }

  void OnFailure(webrtc::RTCError error) override {
    parent_->OnSdpFailure("CreateSdp", std::move(error));
  }

 private:
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
    }
  }

 private:
  PeerConnectionClient* const parent_;
};

PeerConnectionClient::PeerConnectionClient() = default;

PeerConnectionClient::~PeerConnectionClient() { Close(); }

bool PeerConnectionClient::Initialize() {
  webrtc::InitializeSSL();

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

#ifdef _WIN32
  com_initializer_ = std::make_unique<webrtc::ScopedCOMInitializer>(
      webrtc::ScopedCOMInitializer::kMTA);
  if (!com_initializer_->Succeeded()) {
    std::cerr << "[pc] COM initialization failed\n";
    return false;
  }
#endif

  webrtc::Environment env = webrtc::CreateEnvironment();
  adm_ = webrtc::CreateAudioDeviceModule(
      env, webrtc::AudioDeviceModule::kWindowsCoreAudio);
  if (!adm_) {
    std::cerr << "CreateAudioDeviceModule failed\n";
    return false;
  }

  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(),
      worker_thread_.get(),
      signaling_thread_.get(),
      adm_,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      nullptr,
      nullptr,
      nullptr,
      nullptr);
  if (!factory_) {
    std::cerr << "[pc] CreatePeerConnectionFactory failed\n";
    return false;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  webrtc::PeerConnectionInterface::IceServer ice;
  ice.urls.push_back(kStunServer);
  config.servers.push_back(std::move(ice));

  webrtc::PeerConnectionDependencies deps(this);

  auto pc_or = factory_->CreatePeerConnectionOrError(config, std::move(deps));
  if (!pc_or.ok()) {
    std::cerr << "[pc] CreatePeerConnection failed: "
              << pc_or.error().message() << "\n";
    return false;
  }
  pc_ = pc_or.MoveValue();
  return true;
}

void PeerConnectionClient::Close() {
  {
    std::lock_guard<std::mutex> lock(dc_mutex_);
    if (dc_) {
      dc_->UnregisterObserver();
      dc_->Close();
      dc_ = nullptr;
    }
  }
  audio_track_ = nullptr;
  audio_source_ = nullptr;
  if (pc_) {
    pc_->Close();
    pc_ = nullptr;
  }
  factory_ = nullptr;

  if (signaling_thread_) signaling_thread_->Stop();
  if (worker_thread_) worker_thread_->Stop();
  if (network_thread_) network_thread_->Stop();
  signaling_thread_.reset();
  worker_thread_.reset();
  network_thread_.reset();
}

bool PeerConnectionClient::CreateOfferAndDataChannel() {
  if (!pc_) return false;

  webrtc::DataChannelInit init;
  init.ordered = true;
  auto dc_or = pc_->CreateDataChannelOrError(kDataChannelLabel, &init);
  if (!dc_or.ok()) {
    std::cerr << "[pc] CreateDataChannel failed: "
              << dc_or.error().message() << "\n";
    return false;
  }
  AttachDataChannel(dc_or.MoveValue());

  auto observer = webrtc::make_ref_counted<CreateSdpObserver>(this);
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  pc_->CreateOffer(observer.get(), opts);
  return true;
}

bool PeerConnectionClient::CreateAnswer() {
  if (!pc_) return false;
  auto observer = webrtc::make_ref_counted<CreateSdpObserver>(this);
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  pc_->CreateAnswer(observer.get(), opts);
  return true;
}

bool PeerConnectionClient::SetRemoteSdp(webrtc::SdpType type,
                                       const std::string& sdp) {
  if (!pc_) return false;
  auto desc = webrtc::CreateSessionDescription(type, sdp);
  if (!desc) {
    std::cerr << "[pc] CreateSessionDescription failed for SDP\n";
    return false;
  }
  auto observer = webrtc::make_ref_counted<SetRemoteDescObserver>(this);
  pc_->SetRemoteDescription(std::move(desc), observer);
  return true;
}

bool PeerConnectionClient::AddRemoteIceCandidate(const std::string& sdp_mid,
                                                 int sdp_mline_index,
                                                 const std::string& sdp) {
  if (!pc_) return false;

  webrtc::SdpParseError err;
  auto candidate = webrtc::IceCandidate::Create(sdp_mid, sdp_mline_index, sdp,
                                                &err);
  if (!candidate) {
    std::cerr << "[pc] bad ice candidate: " << err.description << "\n";
    return false;
  }

  pc_->AddIceCandidate(
      std::move(candidate),
      [](webrtc::RTCError e) {
        if (!e.ok()) {
          std::cerr << "[pc] AddIceCandidate failed: " << e.message() << "\n";
        }
      });
  return true;
}

bool PeerConnectionClient::AddAudioTrack() {
  if (!pc_ || !factory_) return false;

  webrtc::AudioOptions options;
  options.echo_cancellation = false;
  options.auto_gain_control = true;
  options.noise_suppression = true;
  options.highpass_filter = true;
  audio_source_ = factory_->CreateAudioSource(options);
  if (!audio_source_) {
    std::cerr << "[pc] CreateAudioSource failed\n";
    return false;
  }

  audio_track_ =
      factory_->CreateAudioTrack("audio_label", audio_source_.get());
  if (!audio_track_) {
    std::cerr << "[pc] CreateAudioTrack failed\n";
    audio_source_ = nullptr;
    return false;
  }

  auto result = pc_->AddTrack(audio_track_, {"stream_id"});
  if (!result.ok()) {
    std::cerr << "[pc] AddTrack failed: " << result.error().message() << "\n";
    audio_track_ = nullptr;
    audio_source_ = nullptr;
    return false;
  }

  return true;
}

void PeerConnectionClient::OnLocalSdpReady(
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
  webrtc::SdpType type = desc->GetType();
  std::string sdp;
  desc->ToString(&sdp);

  auto observer = webrtc::make_ref_counted<SetLocalDescObserver>(this);
  pc_->SetLocalDescription(std::move(desc), observer);

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
  dc_->RegisterObserver(this);
}

void PeerConnectionClient::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  if (state_cb_) state_cb_(SignalingStateName(new_state));
}

void PeerConnectionClient::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  AttachDataChannel(std::move(data_channel));
}

void PeerConnectionClient::OnTrack(
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  if (!transceiver) return;
  auto track = transceiver->receiver()->track();
  if (track && track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    auto* audio_track =
        static_cast<webrtc::AudioTrackInterface*>(track.get());
    if (audio_track_cb_) audio_track_cb_(audio_track);
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

void PeerConnectionClient::OnStateChange() {
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

}  // namespace audiosub
