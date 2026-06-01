// main.cc
// =======
// audiosub_client.exe（命令行外壳）。
// 真正的接线逻辑都在 AudiosubEngine 里；这里只负责：
//   1. 解析命令行 --id A/B、--host、--port、--audio-path
//   2. 创建 AudiosubEngine，注册「把结构化事件打印到控制台」的回调
//   3. 主线程跑 std::getline 读命令（/talk on|off、/note、/quit）
//   4. 退出时打印指标汇总
//
// 这样同一套引擎逻辑可同时被 CLI 和 Qt 前端（经 C ABI DLL）复用。

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

#include "audiosub_engine.h"

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

void PrintUsage(const char* prog) {
  std::cout << "Usage: " << prog
            << " --id <A|B> [--host <host>] [--port <port>]\n"
            << "\n"
            << "Required:\n"
            << "  --id <A|B>\n"
            << "Optional:\n"
            << "  --host <host>    default: 127.0.0.1\n"
            << "  --port <port>    default: 8888\n"
            << "  --audio-path <wasapi|webrtc> default: wasapi\n";
}

struct Args {
  std::string id;
  std::string host = "127.0.0.1";
  int port = 8888;
  std::string audio_path = "wasapi";
};

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

// 控制台多线程打印加锁，保证一行消息原子输出。
std::mutex g_print_mutex;
void Println(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_print_mutex);
  std::cout << s << "\n" << std::flush;
}

// 把 Unix 毫秒格式化成 HH:MM:SS.mmm。
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

// 把指标格式化成 " [标签=值ms]"，超预算加 " OVER!"。
std::string FormatMetric(const std::string& label, int64_t value_ms,
                         int64_t budget_ms) {
  return " [" + label + "=" + std::to_string(value_ms) + "ms" +
         (value_ms <= budget_ms ? "]" : " OVER!]");
}

// 单项指标统计行。
std::string FormatStatLine(const std::string& name,
                           const audiosub::engine::MetricStat& s,
                           int64_t budget_ms) {
  if (s.count == 0) return "  " + name + ": (\u65e0\u6837\u672c)";
  const int64_t avg = s.sum / s.count;
  return "  " + name + ": \u6837\u672c=" + std::to_string(s.count) +
         " \u5e73\u5747=" + std::to_string(avg) + "ms \u5cf0\u503c=" +
         std::to_string(s.max) + "ms \u9884\u7b97=" +
         std::to_string(budget_ms) + "ms" +
         (avg <= budget_ms ? "" : " (\u5e73\u5747\u8d85\u9884\u7b97)");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    PrintUsage(argv[0]);
    return 1;
  }

  audiosub::engine::AudiosubEngine engine;

  // 状态/日志：直接打印（字符串本身已带 [state]/[pc]/[peer] 等前缀）。
  engine.SetStateCallback([](const std::string& s) { Println(s); });

  // 字幕事件：本端识别和对端回传统一格式打印。
  engine.SetSubtitleCallback([](const audiosub::engine::SubtitleEvent& ev) {
    Println("[sub] #" + std::to_string(ev.index) + " " +
            FormatWallClock(ev.start_ms) + " - " + FormatWallClock(ev.end_ms) +
            " " + ev.text + FormatMetric("lat", ev.latency_ms, 1500));
    for (const audiosub::engine::MarkInfo& mk : ev.marks) {
      Println("        \u2514\u2500 [\u6807\u6ce8#" + std::to_string(mk.seq) +
              "] " + mk.text + FormatMetric("err", mk.err_ms, 500));
    }
  });

  // 收到对端标注。
  engine.SetMarkCallback(
      [](std::uint64_t seq, const std::string& text, std::int64_t vis_ms) {
        Println("[mark #" + std::to_string(seq) + "] " + text +
                FormatMetric("vis", vis_ms, 300));
      });

  // 无归属标注。
  engine.SetOrphanCallback([](std::uint64_t seq, const std::string& text) {
    Println("[\u6807\u6ce8#" + std::to_string(seq) + " \u672a\u5bf9\u9f50] " +
            text);
  });

  audiosub::engine::AudiosubEngine::Config cfg;
  cfg.id = args.id;
  cfg.host = args.host;
  cfg.port = args.port;
  cfg.audio_path = args.audio_path;
  if (!engine.Start(cfg)) {
    std::cerr << "engine start failed\n";
    return 2;
  }

  const bool is_offerer = engine.IsOfferer();
  std::cout << "Role: " << (is_offerer ? "A (offerer)" : "B (answerer)") << "\n"
            << "Commands: /talk on, /talk off, /note <text>, /quit\n"
            << "Waiting for peer...\n"
            << std::flush;

  // 主线程命令循环。
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "/quit" || line == "/exit") break;
    if (line.empty()) continue;

    if (line == "/talk on") {
      if (!is_offerer) {
        Println("[audio] only A can control local microphone speech");
      } else if (!engine.SetTalking(true)) {
        Println("[audio] failed to start talking");
      } else {
        Println("[audio] talking enabled on A");
      }
      continue;
    }
    if (line == "/talk off") {
      if (!is_offerer) {
        Println("[audio] only A can control local microphone speech");
      } else if (!engine.SetTalking(false)) {
        Println("[audio] failed to stop talking");
      } else {
        Println("[audio] talking disabled on A");
      }
      continue;
    }
    if (line.rfind("/note ", 0) == 0) {
      const std::string note_text = line.substr(6);
      if (note_text.empty()) {
        Println("[note] usage: /note <text>");
        continue;
      }
      if (!engine.SendNote(note_text)) {
        Println("(annotation dropped: data channel not open yet)");
      } else {
        Println("[note] sent: " + note_text);
      }
      continue;
    }

    Println("(unknown command; use /talk on|off, /note <text>, /quit)");
  }

  // 退出汇总：放在引擎清理之前打印（避免清理流程卡住时看不到统计）。
  const audiosub::engine::MetricsSummary m = engine.GetMetrics();
  Println("==== \u6307\u6807\u6c47\u603b ====");
  Println(FormatStatLine("\u7aef\u5230\u7aef\u5b57\u5e55\u5ef6\u8fdf", m.lat, 1500));
  Println(FormatStatLine("\u6807\u6ce8\u5339\u914d\u8bef\u5dee", m.err, 500));
  Println(FormatStatLine("\u6807\u6ce8\u53ef\u89c1\u5ef6\u8fdf", m.vis, 300));

  engine.Stop();
  std::cout << "bye.\n";
  return 0;
}
