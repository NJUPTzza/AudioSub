// main.cc
// =======
// audiosub_client.exe 的入口。这个文件做的事：
//   1. 解析命令行 --id A/B、--host、--port
//   2. 创建 PeerConnectionClient + SignalingClient
//   3. 把"WebRTC 生成的 SDP/ICE"转发到信令通道
//      把"信令通道收到的 SDP/ICE"喂回 WebRTC
//   4. 主线程跑 std::getline 读用户输入，调 SendMessage 发到 P2P
//   5. 远端 PCM 链路：WebRTC 回调 -> remote_audio_asr_buffer
//        -> asr_worker(后台线程)
//          -> AsrAudioConverter(48k/stereo -> 16k/mono)
//          -> WhisperCppEngine.PushAudio()
//          -> ConsoleSubtitleConsumer 打字幕
//
// 重要：所有重逻辑（重采样、whisper 推理）都跑在 asr_worker 这个后台线程上，
//      WebRTC 的音频回调线程只负责把帧塞进 ring buffer，不做任何阻塞操作。


#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "audio/asr_audio_converter.h"
#include "audio/pcm_ring_buffer.h"
#include "asr/whisper_cpp_engine.h"
#include "peer_connection_client.h"
#include "signaling_client.h"
#include "proto/dc_message.h"
#include "fusion/subtitle_mark_fuser.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
  // 打印命令行帮助。
  // 提示文本，不参与任何音频/信令逻辑。
  void PrintUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " --id <A|B> [--host <host>] [--port <port>]\n"
        << "\n"
        << "Required:\n"
        << "  --id <A|B>\n"
        << "Optional:\n"
        << "  --host <host>    default: 127.0.0.1\n"
      << "  --port <port>    default: 8888\n"
      << "  --audio-path <wasapi|webrtc> default: wasapi\n";
  }
  
  // 命令行参数。--id 必填。
  struct Args {
    std::string id;            // "A" 或 "B"
    std::string host = "127.0.0.1";
    int port = 8888;
    std::string audio_path = "wasapi"; // "wasapi" 或 "webrtc"
  };
  
  // 简易命令行解析：循环识别 --xxx <value>，碰到 -h/--help 返回 false 触发用法说明。
  bool ParseArgs(int argc, char** argv, Args* out) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto next = [&](const char* name) -> const char* {
        if (i + 1 >= argc) {
          std::cerr << "missing value for " << name << "\n";
          return nullptr;
        }
        return argv[++i];
      };
      if (a == "--id") {
        if (const char* v = next("--id")) out->id = v; else return false;
      } else if (a == "--host") {
        if (const char* v = next("--host")) out->host = v; else return false;
      } else if (a == "--port") {
        if (const char* v = next("--port")) out->port = std::atoi(v); else return false;
      } else if (a == "-h" || a == "--help") {
        return false;
      } else if (a == "--audio-path") {
        if (const char* v = next("--audio-path")) out->audio_path = v; else return false;
      } else {
        std::cerr << "unknown arg: " << a << "\n";
        return false;
      }
    }
    return !out->id.empty();
  }
  
  // 控制台多线程打印加锁，保证一行消息原子输出，不被其它线程的输出穿插。
  std::mutex g_print_mutex;
  
  void Println(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::cout << s << "\n" << std::flush;
  }

  // 把 Unix 毫秒格式化成 HH:MM:SS.mmm。字幕本地打印和 A 端回传打印都用它，
  // 保证两端显示格式一致。
  std::string FormatWallClock(int64_t unix_ms) {
    const std::time_t seconds_part = static_cast<std::time_t>(unix_ms / 1000);
    const int64_t millis = unix_ms % 1000;
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &seconds_part);
#else
    localtime_r(&seconds_part, &local_time);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03lld",
                  local_time.tm_hour, local_time.tm_min, local_time.tm_sec,
                  static_cast<long long>(millis));
    return std::string(buffer);
  }

  // 标注匹配误差：标注发生时刻落在字幕 [start,end] 内记 0，
  // 否则取到最近边界的距离（毫秒）。越小说明标注和字幕在时间上对得越准。
  int64_t MarkMatchError(int64_t event_ms, int64_t start_ms, int64_t end_ms) {
    if (event_ms < start_ms) return start_ms - event_ms;
    if (event_ms > end_ms) return event_ms - end_ms;
    return 0;
  }

  // 把指标格式化成 " [标签=值ms]"，超过阈值时加 " OVER!" 提示，方便对照评估。
  std::string FormatMetric(const std::string& label, int64_t value_ms,
                           int64_t budget_ms) {
    return " [" + label + "=" + std::to_string(value_ms) + "ms" +
           (value_ms <= budget_ms ? "]" : " OVER!]");
  }

  // 当前现实时间（Unix 毫秒）。用于计算标注消息从 A 发出到 B 收到的可见延迟。
  int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // 指标汇总累加器：运行期间线程安全地累计各指标的样本数/总和/峰值，
  // 退出时打印平均值与峰值，方便整体评估（数据来自字幕、标注两条线程）。
  struct MetricStats {
    struct Stat {
      int64_t count = 0;
      int64_t sum = 0;
      int64_t max = 0;
      void Add(int64_t v) {
        if (count == 0 || v > max) max = v;
        ++count;
        sum += v;
      }
    };
    std::mutex mu;
    Stat lat;  // 端到端字幕延迟（识别耗时）
    Stat err;  // 标注匹配误差
    Stat vis;  // DataChannel 标注可见延迟
    void AddLat(int64_t v) { std::lock_guard<std::mutex> g(mu); lat.Add(v); }
    void AddErr(int64_t v) { std::lock_guard<std::mutex> g(mu); err.Add(v); }
    void AddVis(int64_t v) { std::lock_guard<std::mutex> g(mu); vis.Add(v); }
  };
  MetricStats g_metrics;

  // 把单个指标的统计格式化成一行：样本数 / 平均 / 峰值 / 预算（平均超预算时标注）。
  std::string FormatStatLine(const std::string& name,
                             const MetricStats::Stat& s, int64_t budget_ms) {
    if (s.count == 0) return "  " + name + ": (\u65e0\u6837\u672c)";
    const int64_t avg = s.sum / s.count;
    return "  " + name + ": \u6837\u672c=" + std::to_string(s.count) +
           " \u5e73\u5747=" + std::to_string(avg) + "ms \u5cf0\u503c=" +
           std::to_string(s.max) + "ms \u9884\u7b97=" +
           std::to_string(budget_ms) + "ms" +
           (avg <= budget_ms ? "" : " (\u5e73\u5747\u8d85\u9884\u7b97)");
  }

  // 退出时打印三项指标的整体统计。
  void PrintMetricSummary() {
    std::lock_guard<std::mutex> g(g_metrics.mu);
    Println("==== \u6307\u6807\u6c47\u603b ====");
    Println(FormatStatLine("\u7aef\u5230\u7aef\u5b57\u5e55\u5ef6\u8fdf",
                           g_metrics.lat, 1500));
    Println(FormatStatLine("\u6807\u6ce8\u5339\u914d\u8bef\u5dee",
                           g_metrics.err, 500));
    Println(FormatStatLine("\u6807\u6ce8\u53ef\u89c1\u5ef6\u8fdf",
                           g_metrics.vis, 300));
  }

  class ConsoleSubtitleConsumer : public audiosub::core::ISubtitleConsumer {
    public:
     // 传入融合器：字幕产生时用它找出对应的标注。
     explicit ConsoleSubtitleConsumer(audiosub::fusion::SubtitleMarkFuser* fuser)
         : fuser_(fuser) {}

     // 字幕回传回调类型：每产出一条字幕调用一次（用于 B->A 把字幕发回对端）。
     // 参数：本端字幕序号 + 增强字幕（含时间范围、正文、对齐到的标注）。
     using RemoteSubtitleSink = std::function<void(
         int index, const audiosub::core::EnhancedSubtitleSegment&)>;
     void SetRemoteSubtitleSink(RemoteSubtitleSink sink) {
       remote_sink_ = std::move(sink);
     }
 
     void OnSubtitleSegment(const audiosub::core::SubtitleSegment& seg) override {
       if (seg.text.empty()) return;
       const int n = ++subtitle_count_;
 
       // 字幕正文 + 时间范围 + 端到端字幕延迟指标（识别耗时，预算 1500ms）。
      Println("[sub] #" + std::to_string(n) + " " +
              FormatTime(seg.start_ms) + " - " +
              FormatTime(seg.end_ms) + " " + seg.text +
              FormatMetric("lat", seg.latency_ms, 1500));
      g_metrics.AddLat(seg.latency_ms);

      // 融合：找出"发生时刻落在这条字幕时间范围内"的标注，逐条附加到下方，
      // 并附上标注匹配误差指标（预算 500ms）。
      const audiosub::core::EnhancedSubtitleSegment enhanced = fuser_->Fuse(seg);
      for (const audiosub::core::MarkMessage& mark : enhanced.marks) {
        const int64_t err =
            MarkMatchError(mark.event_time_ms, seg.start_ms, seg.end_ms);
        Println("        \u2514\u2500 [\u6807\u6ce8#" + std::to_string(mark.seq) +
                "] " + mark.text + FormatMetric("err", err, 500));
        g_metrics.AddErr(err);
      }

       // 无归属标注：发生时刻早于这条字幕、一直没被任何字幕认领的标注，
       // 已不可能再匹配未来字幕，单独打印一行，保证标注不会"消失"。
       for (const audiosub::core::MarkMessage& orphan :
            fuser_->CollectOrphansBefore(seg.start_ms)) {
         Println("[\u6807\u6ce8#" + std::to_string(orphan.seq) +
                 " \u672a\u5bf9\u9f50] " + orphan.text);
       }

       // B -> A 字幕回传：把"序号 + 增强字幕（含对齐标注）"交给外部回调发回对端。
       if (remote_sink_) {
         remote_sink_(n, enhanced);
       }
     }
 
    private:
     static std::string FormatTime(int64_t unix_ms) {
       const std::time_t seconds_part =
           static_cast<std::time_t>(unix_ms / 1000);
       const int64_t millis = unix_ms % 1000;
       std::tm local_time{};
 
 #ifdef _WIN32
       localtime_s(&local_time, &seconds_part);
 #else
       localtime_r(&seconds_part, &local_time);
 #endif
 
       char buffer[32];
       std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03lld",
                     local_time.tm_hour,
                     local_time.tm_min,
                     local_time.tm_sec,
                     static_cast<long long>(millis));
       return std::string(buffer);
     }
 
     audiosub::fusion::SubtitleMarkFuser* fuser_ = nullptr;
     RemoteSubtitleSink remote_sink_;
     std::atomic<int> subtitle_count_{0};
   };
  
  std::string DescribePcmFrame(const audiosub::core::PcmFrame& frame,
                               std::size_t frame_index) {
    return "[audio] remote pcm #" + std::to_string(frame_index) +
           " ts_ms=" + std::to_string(frame.timestamp_ms) +
           " rate=" + std::to_string(frame.sample_rate) +
           " ch=" + std::to_string(frame.channels) +
           " samples=" + std::to_string(frame.samples.size());
  }
  
  int ComputeAverageAmplitude(const audiosub::core::PcmFrame& frame) {
    if (frame.samples.empty()) {
      return 0;
    }
    int64_t sum = 0;
    for (int16_t sample : frame.samples) {
      sum += (sample >= 0) ? sample : -static_cast<int>(sample);
    }
    return static_cast<int>(sum / static_cast<int64_t>(frame.samples.size()));
  }
  
  // 电平监视：聚合最近一段时间内的多帧，再按固定时间间隔打印一次"窗口内
  // 平均电平"。这样既能让用户看到麦克风活动持续刷新（验证采集真的在跑），
  // 又不会被 10ms/帧 的密度刷屏。
  //
  // 同时保留状态变化即时打印（silent <-> speaking）以便观察 /talk 切换效果。
  void RunAudioLevelMonitor(audiosub::audio::PcmRingBuffer& buffer,
                            const std::string& label) {
    using clock = std::chrono::steady_clock;
    constexpr auto kPrintInterval = std::chrono::milliseconds(500);
    constexpr int kSpeakingThreshold = 80;
  
    std::size_t frame_index = 0;
    std::size_t window_frames = 0;
    int64_t window_level_sum = 0;
    int window_level_max = 0;
    auto last_print = clock::now();
    bool has_last_state = false;
    bool last_speaking = false;
  
    while (auto frame = buffer.WaitPop()) {
      ++frame_index;
      const int level = ComputeAverageAmplitude(*frame);
      window_level_sum += level;
      window_level_max = std::max(window_level_max, level);
      ++window_frames;
  
      const bool speaking = level >= kSpeakingThreshold;
      const bool state_changed = !has_last_state || speaking != last_speaking;
  
      const auto now = clock::now();
      const bool time_due = (now - last_print) >= kPrintInterval;
  
      if (state_changed || time_due) {
        const int avg = window_frames > 0
                            ? static_cast<int>(window_level_sum /
                                               static_cast<int64_t>(window_frames))
                            : 0;
      // 暂时关闭音频电平刷屏日志，避免控制台被 avg/peak/state 淹没。
      // Println("[audio] " + label + " frames=" + std::to_string(frame_index) +
      //         " avg=" + std::to_string(avg) +
      //         " peak=" + std::to_string(window_level_max) +
      //         " state=" + (speaking ? "speaking" : "silent"));
        has_last_state = true;
        last_speaking = speaking;
        last_print = now;
        window_frames = 0;
        window_level_sum = 0;
        window_level_max = 0;
      }
    }
  }
  
  }  // namespace
  
int main(int argc, char** argv) {
#ifdef _WIN32
  // Whisper / 日志文本统一按 UTF-8 输出，避免中文在 PowerShell 里显示成乱码。
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  // 解析命令行参数
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 1;
  }

  // 角色判定：约定 A 是 offerer，B 是 answerer。
  // 后面逻辑基于 A 主动创建 offer，B 被动等待 offer
  const bool is_offerer = (args.id == "A");

  // 初始化 WebRTC 核心对象
  // 创建 PeerConnectionClient
  // 启动 WebRTC 三个线程
  // 创建 PeerConnectionFactory 和 PeerConnection
  audiosub::PeerConnectionClient pc;
  if (!pc.Initialize()) {
    std::cerr << "PeerConnectionClient::Initialize() failed\n";
    return 2;
  }

  if (args.audio_path == "wasapi") {
    pc.SetAudioPath(audiosub::PeerConnectionClient::AudioPath::kWasapiDataChannel);
  } else if (args.audio_path == "webrtc") {
    pc.SetAudioPath(audiosub::PeerConnectionClient::AudioPath::kWebrtcTrack);
  } else {
    std::cerr << "invalid --audio-path: " << args.audio_path
              << " (expected wasapi or webrtc)\n";
    return 1;
  }

  // 创建两个 PCM 缓冲区，分别用于本地采集和对端接收
  audiosub::audio::PcmRingBuffer local_audio_buffer(/*capacity_frames=*/128);
  audiosub::audio::PcmRingBuffer remote_audio_buffer(/*capacity_frames=*/128);
  // 启动两个监控线程
  std::thread local_audio_worker([&local_audio_buffer]() {
    RunAudioLevelMonitor(local_audio_buffer, "local");
  });
  std::thread remote_audio_worker([&remote_audio_buffer]() {
    RunAudioLevelMonitor(remote_audio_buffer, "remote");
  });

  // 创建一个 ASR 缓冲区，用于存储远端音频帧
  audiosub::audio::PcmRingBuffer remote_audio_asr_buffer(/*capacity_frames=*/256);
  audiosub::audio::AsrAudioConverter asr_converter;

  // 使用 small 模型。WASAPI 路径下接收到的是干净的原始 PCM，small 已经够准；
  // medium 反而在边界场景上更容易把弱信号幻觉成训练集里高频短语（"谢谢收看"
  // "我们下期再见" 等）。如需更高识别率可手动改成 ggml-medium.bin。
  audiosub::asr::WhisperCppEngine asr_engine(
    "third_party/whisper.cpp/models/ggml-small.bin");
  if (!asr_engine.Initialize()) {
    std::cerr << "[asr] whisper init failed\n";
    return 6;
  }

  // 字幕与标注融合器：网络线程往里存标注，ASR 线程从里取标注做对齐。
  audiosub::fusion::SubtitleMarkFuser mark_fuser;

  ConsoleSubtitleConsumer subtitle_consumer(&mark_fuser);

  asr_engine.SetSubtitleConsumer(&subtitle_consumer);

  // 单线程消费 ring buffer：
  //   1) 把 48k/stereo 原始帧转成 16k/mono；
  //   2) 整帧直接喂给 whisper engine（whisper engine 内部按"攒满 4s 或 VAD"触发推理）。
  // 这里不再做 20ms 切块——whisper.cpp 一次接受多大块都行，
  // 切碎反而让 PushAudio + VAD 计算成本变高，没有好处。
  std::atomic<uint64_t> asr_sent_frames{0};
  std::thread asr_worker([&] {
    while (auto frame = remote_audio_asr_buffer.WaitPop()) {
      audiosub::core::PcmFrame asr_frame = asr_converter.ToAsrFormat(*frame);
      if (asr_frame.samples.empty()) continue;
      asr_engine.PushAudio(asr_frame);
      ++asr_sent_frames;
    }
  });

  // 创建信令客户端对象
  audiosub::SignalingClient signaling;

  // 应用层接线。把底层回调映射到业务动作。
  // 当本地生成 offer 或 answer 时，把它包装成 JSON，通过 signaling.Send() 发出去
  pc.SetSdpReadyCallback(
      [&signaling](webrtc::SdpType type, const std::string& sdp) {
        std::string type_str =
            (type == webrtc::SdpType::kOffer) ? "offer" : "answer";
        nlohmann::json msg = {{"type", type_str}, {"sdp", sdp}};
        signaling.Send(msg);
        Println(std::string("[pc] local ") + type_str + " sent (" +
                std::to_string(sdp.size()) + " bytes)");
      });

  // 当本地产生一个 ICE candidate 时，通过信令发给对端
  pc.SetIceCandidateCallback(
      [&signaling](const std::string& candidate, const std::string& mid,
                   int mline) {
        nlohmann::json msg = {{"type", "candidate"},
                              {"candidate", candidate},
                              {"sdpMid", mid},
                              {"sdpMLineIndex", mline}};
        signaling.Send(msg);
      });

  // 通过 DataChannel 收到对端发来的文本消息，按 type 分流：
  //   - "annotation" 标注（A->B）-> 去重后存入融合器（留给字幕对齐用）；
  //   - "subtitle"   字幕（B->A）-> 用和本地一致的格式打印增强字幕；
  //   - 其它/解析失败            -> 当普通聊天文本打印。
  pc.SetMessageCallback([&mark_fuser](const std::string& text) {
    const nlohmann::json j =
        nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded() && j.is_object() && j.contains("type")) {
      const std::string type = j.value("type", "");

      if (type == "annotation") {
        audiosub::core::MarkMessage mark;
        mark.seq = j.value("seq", std::uint64_t{0});
        mark.event_time_ms = j.value("event_time_ms", std::int64_t{0});
        if (j.contains("payload") && j["payload"].is_object()) {
          mark.text = j["payload"].value("text", std::string());
        }
        // DataChannel 标注消息可见延迟：A 发出(event_time_ms) -> B 此刻收到。
        // 预算 300ms。注意它依赖 A/B 时钟一致（同机测试满足）。
        const std::int64_t vis_ms = NowUnixMs() - mark.event_time_ms;
        // AddMark 内部按 seq 去重：返回 false 表示这条之前收到过。
        if (mark_fuser.AddMark(mark)) {
          Println("[mark #" + std::to_string(mark.seq) + "] " + mark.text +
                  FormatMetric("vis", vis_ms, 300));
          g_metrics.AddVis(vis_ms);
        } else {
          Println("[mark #" + std::to_string(mark.seq) + "] (duplicate, ignored)");
        }
        return;
      }

      if (type == "subtitle") {
        // 对端（B 端）回传的增强字幕，用和本地完全一致的格式打印：
        //   [sub] #序号 起 - 止 正文
        //           └─ [标注#seq] 内容
        const int index = j.value("index", 0);
        const std::int64_t start_ms = j.value("start_ms", std::int64_t{0});
        const std::int64_t end_ms = j.value("end_ms", std::int64_t{0});
        const std::int64_t latency_ms = j.value("latency_ms", std::int64_t{0});
        std::string sub_text;
        if (j.contains("payload") && j["payload"].is_object()) {
          sub_text = j["payload"].value("text", std::string());
        }
        Println("[sub] #" + std::to_string(index) + " " +
                FormatWallClock(start_ms) + " - " + FormatWallClock(end_ms) +
                " " + sub_text + FormatMetric("lat", latency_ms, 1500));
        g_metrics.AddLat(latency_ms);
        if (j.contains("marks") && j["marks"].is_array()) {
          for (const auto& mk : j["marks"]) {
            const std::int64_t err = mk.value("err_ms", std::int64_t{0});
            Println("        \u2514\u2500 [\u6807\u6ce8#" +
                    std::to_string(mk.value("seq", std::uint64_t{0})) + "] " +
                    mk.value("text", std::string()) +
                    FormatMetric("err", err, 500));
            g_metrics.AddErr(err);
          }
        }
        return;
      }
    }
    // 不是结构化消息，按普通聊天文本处理。
    Println(std::string("<peer> ") + text);
  });

  // B -> A 字幕回传：本端每产出一条字幕，就把"序号 + 时间范围 + 正文 + 对齐标注"
  // 打包成 "subtitle" 消息通过 DataChannel 发回对端。A 端没有远端音频不产字幕，
  // 所以实际是 B 端把带标注的识别结果回传给 A 端，让两端看到一致的展示。
  subtitle_consumer.SetRemoteSubtitleSink(
      [&pc](int index, const audiosub::core::EnhancedSubtitleSegment& en) {
        nlohmann::json marks = nlohmann::json::array();
        for (const audiosub::core::MarkMessage& mk : en.marks) {
          const int64_t err = MarkMatchError(
              mk.event_time_ms, en.subtitle.start_ms, en.subtitle.end_ms);
          marks.push_back({{"seq", mk.seq}, {"text", mk.text}, {"err_ms", err}});
        }
        const nlohmann::json msg = {
            {"type", "subtitle"},
            {"index", index},
            {"start_ms", en.subtitle.start_ms},
            {"end_ms", en.subtitle.end_ms},
            {"latency_ms", en.subtitle.latency_ms},
            {"payload", {{"text", en.subtitle.text}}},
            {"marks", marks},
        };
        pc.SendMessage(msg.dump());
      });

  // 本地 PCM 来了以后，放进 local_audio_buffer
  pc.SetLocalAudioFrameCallback([&local_audio_buffer](
                                    const audiosub::core::PcmFrame& frame) {
    if (!local_audio_buffer.Push(frame)) {
      std::cerr << "[audio] dropping local PCM frame: ring buffer closed\n";
    }
  });
  
  // 远端 PCM 来了以后，放进 remote_audio_buffer
  pc.SetRemoteAudioFrameCallback([&remote_audio_buffer, &remote_audio_asr_buffer](
                                     const audiosub::core::PcmFrame& frame) {
    if (!remote_audio_buffer.Push(frame)) {
      std::cerr << "[audio] dropping remote PCM frame: ring buffer closed\n";
    }
    if (!remote_audio_asr_buffer.Push(frame)) {
      std::cerr << "[audio] dropping remote PCM frame: asr ring buffer closed\n";
    }
  });

  // 把各种状态变化打印出来，比如 pc:connected 、 dc:open
  pc.SetStateCallback([](const std::string& state) {
    Println(std::string("[state] ") + state);
  });

  // 如果是 A 端（offerer），提前把本地音频轨加进 PeerConnection，默认先静音
  // 这样后续切换成讲话状态时不需要做二次协商
  if (is_offerer && !pc.EnableLocalAudio()) {
    std::cerr << "PeerConnectionClient::EnableLocalAudio() failed\n";
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    local_audio_worker.join();
    remote_audio_worker.join();
    pc.Close();
    return 4;
  }
  if (is_offerer && !pc.SetLocalAudioEnabled(false)) {
    std::cerr << "PeerConnectionClient::SetLocalAudioEnabled(false) failed\n";
    local_audio_buffer.Close();
    remote_audio_buffer.Close();
    local_audio_worker.join();
    remote_audio_worker.join();
    pc.Close();
    return 5;
  }

  // 这里给 信令服务器 设置消息处理函数，处理对端发来的不同类型信令消息
  // 信令消息类型有：peer_ready、peer_left、offer、answer、candidate
  signaling.SetMessageHandler(
      [&pc, is_offerer](const nlohmann::json& msg) {
        std::string type = msg.value("type", "");
        // peer_ready 表示对端上线了
        if (type == "peer_ready") {
          Println(std::string("[peer] ") + msg.value("peer", "?") +
                  " is online");
          // 如果自己是 A 端（offerer），则主动发起 DataChannel + Offer
          if (is_offerer) {
            Println("[pc] creating Offer + DataChannel...");
            pc.CreateOfferAndDataChannel();
          }
          // 如果自己是 B 端（answerer），则不做任何事，等收到 offer。

        // peer_left 表示对端下线了
        } else if (type == "peer_left") {
          Println(std::string("[peer] ") + msg.value("peer", "?") + " left");

        // offer 表示 B 收到 A 的 offer
        } else if (type == "offer") {
          //  B 端接收 A 端发来的 offer(sdp)，将对端提出的连接方案告诉本地 WebRTC
          //  再调 CreateAnswer 生成应答
          Println("[pc] received Offer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kOffer, msg.value("sdp", ""));
          pc.CreateAnswer();

        // answer 表示 A 收到 B 的 answer
        } else if (type == "answer") {
          // A 端接收 B 端发来的 answer(sdp)，将对端提出的连接方案告诉本地 WebRTC
          Println("[pc] received Answer from peer");
          pc.SetRemoteSdp(webrtc::SdpType::kAnswer, msg.value("sdp", ""));

        // candidate 表示 收到对端的 ICE candidate
        } else if (type == "candidate") {
          // 直接喂给 WebRTC
          pc.AddRemoteIceCandidate(msg.value("sdpMid", ""),
                                   msg.value("sdpMLineIndex", 0),
                                   msg.value("candidate", ""));

        } else {
          Println(std::string("[signal] unhandled type=") + type);
        }
      });

  // 连接信令服务器，发送 hello 并等待 peer_ready
  // 这一步会发出 hello。如果对端已经在线，会立刻收到 peer_ready
  // 触发上面的逻辑去 CreateOffer / 等待 offer
  if (!signaling.Connect(args.host, args.port, args.id)) {
    return 3;
  }

  // 提示语，告诉用户当前角色和可用命令
  std::cout << "Role: " << (is_offerer ? "A (offerer)" : "B (answerer)")
            << "\n"
            << "Commands: /talk on, /talk off, /note <text>, /quit\n"
            << "Type text to send after DataChannel opens.\n"
            << "Waiting for peer...\n" << std::flush;


  // A 端标注的自增序号。stdin 循环在主线程单线程读取，普通变量即可。
  std::uint64_t annotation_seq = 0;

  // 取当前现实时间（Unix 毫秒），作为标注的 event_time_ms。
  auto now_unix_ms = []() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
        .count();
  };

  // 主线程 stdin 循环（文本消息 + /talk 命令）。
  // 对用户输入的文本消息，直接通过 DataChannel 发送
  // 对 talk 相关命令，根据是否是 A 端（offerer），分别触发本地音频轨的静音/取消静音
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") break;
    if (line.empty()) {
      continue;
    }
    // talk on: 触发 A 端开始送麦克风音频。
    if (line == "/talk on") {
      if (!is_offerer) {
        Println("[audio] only A can control local microphone speech");
      } else if (!pc.SetLocalAudioEnabled(true)) {
        Println("[audio] failed to start talking");
      } else {
        Println("[audio] talking enabled on A");
      }
      continue;
    }
    // talk off: 停止发送有效语音（保留轨道）。
    if (line == "/talk off") {
      if (!is_offerer) {
        Println("[audio] only A can control local microphone speech");
      } else if (!pc.SetLocalAudioEnabled(false)) {
        Println("[audio] failed to stop talking");
      } else {
        Println("[audio] talking disabled on A");
      }
      continue;
    }

    // /note <文本>：发送一条结构化标注消息给对端。
    // 用 rfind(prefix, 0) == 0 判断是不是以 "/note " 开头。
    if (line.rfind("/note ", 0) == 0) {
      const std::string note_text = line.substr(6);  // 去掉前缀 "/note "
      if (note_text.empty()) {
        Println("[note] usage: /note <text>");
        continue;
      }
      // 组装结构化消息：seq 自增，event_time_ms 用当前现实时间。
      audiosub::proto::DcMessage m;
      m.type = "annotation";
      m.seq = ++annotation_seq;
      m.event_time_ms = now_unix_ms();
      m.text = note_text;
      // 序列化成 JSON 字符串，复用现有的文字通道发送。
      if (!pc.SendMessage(audiosub::proto::Serialize(m))) {
        Println("(annotation dropped: data channel not open yet)");
      } else {
        Println("[note #" + std::to_string(m.seq) + "] sent: " + note_text);
      }
      continue;
    }

    // 普通文本消息：直接通过 DataChannel 发送
    if (!pc.SendMessage(line)) {
      Println("(message dropped: data channel not open yet)");
    }
  }

  std::cout << "[asr] sent frames=" << asr_sent_frames.load() << "\n";

  // 退出汇总：放在清理之前打印，避免 WebRTC 关闭流程卡住时看不到统计结果。
  PrintMetricSummary();

  // 清理：先关信令（这样不会再有新 SDP/ICE 进来），再关 WebRTC（停所有线程）。
  signaling.Close();
  pc.Close();
  local_audio_buffer.Close();
  remote_audio_buffer.Close();
  remote_audio_asr_buffer.Close();
  if (asr_worker.joinable()) {
    asr_worker.join();
  }
  if (local_audio_worker.joinable()) {
    local_audio_worker.join();
  }
  if (remote_audio_worker.joinable()) {
    remote_audio_worker.join();
  }

  // 退出兜底：字幕已不再产生，把仍没归属、也没上报过的标注全部打印出来，
  // 确保任何标注都不会"消失"（比如说完最后一句后才补的标注）。
  for (const audiosub::core::MarkMessage& orphan :
       mark_fuser.CollectRemainingOrphans()) {
    Println("[\u6807\u6ce8#" + std::to_string(orphan.seq) +
            " \u672a\u5bf9\u9f50] " + orphan.text);
  }

  std::cout << "bye.\n";
  return 0;
}

