// audiosub_engine.cc
// ==================
// AudiosubEngine 实现：把 main.cc 原有的接线逻辑搬进来，所有原本 Println 的地方
// 改成调用结构化事件回调（state_cb_ / subtitle_cb_ / mark_cb_ / orphan_cb_）。

#include "audiosub_engine.h"

#include <chrono>
#include <iostream>
#include <utility>

#include <nlohmann/json.hpp>

#include "proto/dc_message.h"

namespace audiosub::engine {

namespace {

// 当前现实时间（Unix 毫秒）。
std::int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 标注匹配误差：标注发生时刻落在字幕 [start,end] 内记 0，否则取到最近边界距离。
std::int64_t MarkMatchError(std::int64_t event_ms, std::int64_t start_ms,
                            std::int64_t end_ms) {
  if (event_ms < start_ms) return start_ms - event_ms;
  if (event_ms > end_ms) return event_ms - end_ms;
  return 0;
}

// 把 ring buffer 抽干直到 Close()。原来的电平监视打印已停用，这里只负责
// 持续消费，避免缓冲区满后 Push 一直失败刷 cerr。
void DrainBuffer(audio::PcmRingBuffer& buffer) {
  while (buffer.WaitPop()) {
  }
}

}  // namespace

AudiosubEngine::AudiosubEngine() = default;

AudiosubEngine::~AudiosubEngine() { Stop(); }

void AudiosubEngine::AddLat(std::int64_t v) {
  std::lock_guard<std::mutex> g(metrics_mutex_);
  if (metrics_.lat.count == 0 || v > metrics_.lat.max) metrics_.lat.max = v;
  ++metrics_.lat.count;
  metrics_.lat.sum += v;
}

void AudiosubEngine::AddErr(std::int64_t v) {
  std::lock_guard<std::mutex> g(metrics_mutex_);
  if (metrics_.err.count == 0 || v > metrics_.err.max) metrics_.err.max = v;
  ++metrics_.err.count;
  metrics_.err.sum += v;
}

void AudiosubEngine::AddVis(std::int64_t v) {
  std::lock_guard<std::mutex> g(metrics_mutex_);
  if (metrics_.vis.count == 0 || v > metrics_.vis.max) metrics_.vis.max = v;
  ++metrics_.vis.count;
  metrics_.vis.sum += v;
}

MetricsSummary AudiosubEngine::GetMetrics() const {
  std::lock_guard<std::mutex> g(metrics_mutex_);
  return metrics_;
}

bool AudiosubEngine::Start(const Config& cfg) {
  if (started_.exchange(true)) return false;  // 只能启动一次
  cfg_ = cfg;
  is_offerer_ = (cfg_.id == "A");

  auto emit_state = [this](const std::string& s) {
    if (state_cb_) state_cb_(s);
  };

  // 初始化 WebRTC 核心对象。
  if (!pc_.Initialize()) {
    std::cerr << "PeerConnectionClient::Initialize() failed\n";
    emit_state("[error] PeerConnectionClient init failed");
    return false;
  }

  if (cfg_.audio_path == "wasapi") {
    pc_.SetAudioPath(PeerConnectionClient::AudioPath::kWasapiDataChannel);
  } else if (cfg_.audio_path == "webrtc") {
    pc_.SetAudioPath(PeerConnectionClient::AudioPath::kWebrtcTrack);
  } else {
    std::cerr << "invalid audio_path: " << cfg_.audio_path << "\n";
    emit_state("[error] invalid audio_path");
    return false;
  }

  // 两条本地/远端电平监视线程（仅用于消费缓冲）。
  local_audio_worker_ = std::thread([this]() { DrainBuffer(local_audio_buffer_); });
  remote_audio_worker_ =
      std::thread([this]() { DrainBuffer(remote_audio_buffer_); });

  // ASR 引擎（small 模型）。
  asr_engine_ = std::make_unique<asr::WhisperCppEngine>(
      "third_party/whisper.cpp/models/ggml-small.bin");
  if (!asr_engine_->Initialize()) {
    std::cerr << "[asr] whisper init failed\n";
    emit_state("[error] whisper init failed");
    return false;
  }
  asr_engine_->SetSubtitleConsumer(this);

  // ASR 消费线程：48k/stereo -> 16k/mono -> whisper。
  asr_worker_ = std::thread([this] {
    while (auto frame = remote_audio_asr_buffer_.WaitPop()) {
      core::PcmFrame asr_frame = asr_converter_.ToAsrFormat(*frame);
      if (asr_frame.samples.empty()) continue;
      asr_engine_->PushAudio(asr_frame);
      ++asr_sent_frames_;
    }
  });

  // === WebRTC / 信令回调接线 ===
  pc_.SetSdpReadyCallback(
      [this, emit_state](webrtc::SdpType type, const std::string& sdp) {
        std::string type_str =
            (type == webrtc::SdpType::kOffer) ? "offer" : "answer";
        nlohmann::json msg = {{"type", type_str}, {"sdp", sdp}};
        signaling_.Send(msg);
        emit_state(std::string("[pc] local ") + type_str + " sent (" +
                   std::to_string(sdp.size()) + " bytes)");
      });

  pc_.SetIceCandidateCallback([this](const std::string& candidate,
                                     const std::string& mid, int mline) {
    nlohmann::json msg = {{"type", "candidate"},
                          {"candidate", candidate},
                          {"sdpMid", mid},
                          {"sdpMLineIndex", mline}};
    signaling_.Send(msg);
  });

  pc_.SetMessageCallback([this](const std::string& text) { HandlePeerMessage(text); });

  pc_.SetLocalAudioFrameCallback([this](const core::PcmFrame& frame) {
    if (!local_audio_buffer_.Push(frame)) {
      std::cerr << "[audio] dropping local PCM frame: ring buffer closed\n";
    }
  });

  pc_.SetRemoteAudioFrameCallback([this](const core::PcmFrame& frame) {
    if (!remote_audio_buffer_.Push(frame)) {
      std::cerr << "[audio] dropping remote PCM frame: ring buffer closed\n";
    }
    if (!remote_audio_asr_buffer_.Push(frame)) {
      std::cerr << "[audio] dropping remote PCM frame: asr ring buffer closed\n";
    }
  });

  pc_.SetStateCallback(
      [emit_state](const std::string& state) { emit_state(std::string("[state] ") + state); });

  // A 端：提前挂上本地音频轨，默认静音。
  if (is_offerer_ && !pc_.EnableLocalAudio()) {
    std::cerr << "PeerConnectionClient::EnableLocalAudio() failed\n";
    emit_state("[error] EnableLocalAudio failed");
    return false;
  }
  if (is_offerer_ && !pc_.SetLocalAudioEnabled(false)) {
    std::cerr << "PeerConnectionClient::SetLocalAudioEnabled(false) failed\n";
    emit_state("[error] SetLocalAudioEnabled(false) failed");
    return false;
  }

  // 信令消息处理。
  signaling_.SetMessageHandler([this, emit_state](const nlohmann::json& msg) {
    std::string type = msg.value("type", "");
    if (type == "peer_ready") {
      emit_state(std::string("[peer] ") + msg.value("peer", "?") + " is online");
      if (is_offerer_) {
        emit_state("[pc] creating Offer + DataChannel...");
        pc_.CreateOfferAndDataChannel();
      }
    } else if (type == "peer_left") {
      emit_state(std::string("[peer] ") + msg.value("peer", "?") + " left");
    } else if (type == "offer") {
      emit_state("[pc] received Offer from peer");
      pc_.SetRemoteSdp(webrtc::SdpType::kOffer, msg.value("sdp", ""));
      pc_.CreateAnswer();
    } else if (type == "answer") {
      emit_state("[pc] received Answer from peer");
      pc_.SetRemoteSdp(webrtc::SdpType::kAnswer, msg.value("sdp", ""));
    } else if (type == "candidate") {
      pc_.AddRemoteIceCandidate(msg.value("sdpMid", ""),
                                msg.value("sdpMLineIndex", 0),
                                msg.value("candidate", ""));
    } else {
      emit_state(std::string("[signal] unhandled type=") + type);
    }
  });

  // 连接信令服务器。
  if (!signaling_.Connect(cfg_.host, cfg_.port, cfg_.id)) {
    emit_state("[error] signaling connect failed");
    return false;
  }

  emit_state("[engine] started");
  return true;
}

bool AudiosubEngine::SetTalking(bool on) {
  if (!is_offerer_) return false;  // 只有 A 能控制本地麦克风
  return pc_.SetLocalAudioEnabled(on);
}

bool AudiosubEngine::SendNote(const std::string& utf8_text) {
  proto::DcMessage m;
  m.type = "annotation";
  m.seq = ++annotation_seq_;
  m.event_time_ms = NowUnixMs();
  m.text = utf8_text;
  return pc_.SendMessage(proto::Serialize(m));
}

void AudiosubEngine::OnSubtitleSegment(const core::SubtitleSegment& seg) {
  if (seg.text.empty()) return;
  const int n = ++subtitle_count_;

  // 本端识别字幕 + 融合标注。
  const core::EnhancedSubtitleSegment enhanced = mark_fuser_.Fuse(seg);

  SubtitleEvent ev;
  ev.index = n;
  ev.start_ms = seg.start_ms;
  ev.end_ms = seg.end_ms;
  ev.text = seg.text;
  ev.latency_ms = seg.latency_ms;
  ev.remote = false;
  AddLat(seg.latency_ms);
  for (const core::MarkMessage& mark : enhanced.marks) {
    const std::int64_t err =
        MarkMatchError(mark.event_time_ms, seg.start_ms, seg.end_ms);
    ev.marks.push_back(MarkInfo{mark.seq, mark.text, err});
    AddErr(err);
  }
  if (subtitle_cb_) subtitle_cb_(ev);

  // 无归属标注：发生时刻早于本条字幕、且不可能再匹配未来字幕的，单独抛出。
  if (orphan_cb_) {
    for (const core::MarkMessage& orphan :
         mark_fuser_.CollectOrphansBefore(seg.start_ms)) {
      orphan_cb_(orphan.seq, orphan.text);
    }
  }

  // B -> A 字幕回传：把增强字幕打包成 "subtitle" 消息发回对端。
  nlohmann::json marks = nlohmann::json::array();
  for (const core::MarkMessage& mk : enhanced.marks) {
    const std::int64_t err =
        MarkMatchError(mk.event_time_ms, seg.start_ms, seg.end_ms);
    marks.push_back({{"seq", mk.seq}, {"text", mk.text}, {"err_ms", err}});
  }
  const nlohmann::json msg = {
      {"type", "subtitle"},
      {"index", n},
      {"start_ms", seg.start_ms},
      {"end_ms", seg.end_ms},
      {"latency_ms", seg.latency_ms},
      {"payload", {{"text", seg.text}}},
      {"marks", marks},
  };
  pc_.SendMessage(msg.dump());
}

void AudiosubEngine::HandlePeerMessage(const std::string& text) {
  const nlohmann::json j =
      nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_discarded() && j.is_object() && j.contains("type")) {
    const std::string type = j.value("type", "");

    if (type == "annotation") {
      core::MarkMessage mark;
      mark.seq = j.value("seq", std::uint64_t{0});
      mark.event_time_ms = j.value("event_time_ms", std::int64_t{0});
      if (j.contains("payload") && j["payload"].is_object()) {
        mark.text = j["payload"].value("text", std::string());
      }
      // 可见延迟：A 发出(event_time_ms) -> B 此刻收到。
      const std::int64_t vis_ms = NowUnixMs() - mark.event_time_ms;
      // 按 seq 去重；只有首次收到才上报 + 计入指标。
      if (mark_fuser_.AddMark(mark)) {
        AddVis(vis_ms);
        if (mark_cb_) mark_cb_(mark.seq, mark.text, vis_ms);
      }
      return;
    }

    if (type == "subtitle") {
      // 对端回传的增强字幕。
      SubtitleEvent ev;
      ev.index = j.value("index", 0);
      ev.start_ms = j.value("start_ms", std::int64_t{0});
      ev.end_ms = j.value("end_ms", std::int64_t{0});
      ev.latency_ms = j.value("latency_ms", std::int64_t{0});
      ev.remote = true;
      if (j.contains("payload") && j["payload"].is_object()) {
        ev.text = j["payload"].value("text", std::string());
      }
      AddLat(ev.latency_ms);
      if (j.contains("marks") && j["marks"].is_array()) {
        for (const auto& mk : j["marks"]) {
          const std::int64_t err = mk.value("err_ms", std::int64_t{0});
          ev.marks.push_back(MarkInfo{mk.value("seq", std::uint64_t{0}),
                                      mk.value("text", std::string()), err});
          AddErr(err);
        }
      }
      if (subtitle_cb_) subtitle_cb_(ev);
      return;
    }
  }
  // 普通聊天文本：通过 state 回调透出去。
  if (state_cb_) state_cb_(std::string("<peer> ") + text);
}

void AudiosubEngine::Stop() {
  if (stopped_.exchange(true)) return;
  if (!started_.load()) return;

  signaling_.Close();
  pc_.Close();
  local_audio_buffer_.Close();
  remote_audio_buffer_.Close();
  remote_audio_asr_buffer_.Close();
  if (asr_worker_.joinable()) asr_worker_.join();
  if (local_audio_worker_.joinable()) local_audio_worker_.join();
  if (remote_audio_worker_.joinable()) remote_audio_worker_.join();

  // 兜底：把剩余未归属标注抛出。
  if (orphan_cb_) {
    for (const core::MarkMessage& orphan : mark_fuser_.CollectRemainingOrphans()) {
      orphan_cb_(orphan.seq, orphan.text);
    }
  }
}

}  // namespace audiosub::engine
