#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/audio/audio_device.h"
#include "rtc_base/thread.h"

#ifdef _WIN32
namespace webrtc { class ScopedCOMInitializer; }
#endif

namespace audiosub {

class PeerConnectionClient : public webrtc::PeerConnectionObserver,
                             public webrtc::DataChannelObserver {
 public:
  using SdpReadyCallback =
      std::function<void(webrtc::SdpType type, const std::string& sdp)>;

  using IceCandidateCallback =
      std::function<void(const std::string& candidate,
                         const std::string& sdp_mid,
                         int sdp_mline_index)>;

  using StateCallback = std::function<void(const std::string& state)>;

  using AudioTrackCallback = std::function<void(webrtc::AudioTrackInterface* track)>;

  PeerConnectionClient();
  ~PeerConnectionClient() override;

  PeerConnectionClient(const PeerConnectionClient&) = delete;
  PeerConnectionClient& operator=(const PeerConnectionClient&) = delete;

  bool Initialize();
  bool CreateOfferAndDataChannel();
  bool CreateAnswer();
  bool SetRemoteSdp(webrtc::SdpType type, const std::string& sdp);
  bool AddRemoteIceCandidate(const std::string& sdp_mid,
                             int sdp_mline_index,
                             const std::string& sdp);
  bool AddAudioTrack();
  void Close();

  void SetSdpReadyCallback(SdpReadyCallback cb) { sdp_ready_cb_ = std::move(cb); }
  void SetIceCandidateCallback(IceCandidateCallback cb) { ice_cb_ = std::move(cb); }
  void SetStateCallback(StateCallback cb) { state_cb_ = std::move(cb); }
  void SetAudioTrackCallback(AudioTrackCallback cb) { audio_track_cb_ = std::move(cb); }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
  void OnRenegotiationNeeded() override {}
  void OnTrack(
      webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState new_state) override;

  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override {}
  void OnBufferedAmountChange(uint64_t) override {}

 private:
  class CreateSdpObserver;
  class SetLocalDescObserver;
  class SetRemoteDescObserver;

  void OnLocalSdpReady(std::unique_ptr<webrtc::SessionDescriptionInterface> desc);
  void OnSdpFailure(const std::string& where, webrtc::RTCError error);
  void AttachDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> ch);

  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;

  std::mutex dc_mutex_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> dc_;

  SdpReadyCallback sdp_ready_cb_;
  IceCandidateCallback ice_cb_;
  StateCallback state_cb_;
  AudioTrackCallback audio_track_cb_;

  webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
#ifdef _WIN32
  std::unique_ptr<webrtc::ScopedCOMInitializer> com_initializer_;
#endif
};

}  // namespace audiosub
