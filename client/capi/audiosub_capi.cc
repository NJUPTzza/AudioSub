// audiosub_capi.cc
// ================
// C ABI 实现：把 AudiosubEngine 的 std::function 事件回调适配成 C 函数指针。
// 本文件编译进 audiosub_capi.dll（/MT），内部可自由使用 STL；只有导出的
// extern "C" 函数构成边界，边界上不传 C++ 对象、不抛异常。

#include "audiosub_capi.h"

#include <string>
#include <vector>

#include "audiosub_engine.h"

namespace {

// 句柄实现：持有引擎 + 各回调的 C 函数指针与 user 指针。
struct EngineImpl {
  audiosub::engine::AudiosubEngine engine;

  AudiosubStateFn state_fn = nullptr;
  void* state_user = nullptr;
  AudiosubSubtitleFn subtitle_fn = nullptr;
  void* subtitle_user = nullptr;
  AudiosubMarkFn mark_fn = nullptr;
  void* mark_user = nullptr;
  AudiosubOrphanFn orphan_fn = nullptr;
  void* orphan_user = nullptr;
};

EngineImpl* Cast(AudiosubEngineHandle* h) {
  return reinterpret_cast<EngineImpl*>(h);
}

}  // namespace

extern "C" {

AUDIOSUB_API AudiosubEngineHandle* audiosub_create(void) {
  return reinterpret_cast<AudiosubEngineHandle*>(new EngineImpl());
}

AUDIOSUB_API void audiosub_destroy(AudiosubEngineHandle* h) {
  if (!h) return;
  delete Cast(h);
}

AUDIOSUB_API void audiosub_set_state_cb(AudiosubEngineHandle* h,
                                        AudiosubStateFn fn, void* user) {
  if (!h) return;
  EngineImpl* impl = Cast(h);
  impl->state_fn = fn;
  impl->state_user = user;
  impl->engine.SetStateCallback([impl](const std::string& s) {
    if (impl->state_fn) impl->state_fn(impl->state_user, s.c_str());
  });
}

AUDIOSUB_API void audiosub_set_subtitle_cb(AudiosubEngineHandle* h,
                                           AudiosubSubtitleFn fn, void* user) {
  if (!h) return;
  EngineImpl* impl = Cast(h);
  impl->subtitle_fn = fn;
  impl->subtitle_user = user;
  impl->engine.SetSubtitleCallback(
      [impl](const audiosub::engine::SubtitleEvent& ev) {
        if (!impl->subtitle_fn) return;
        // 把 marks 摊平成 C 数组；c_str() 指向 ev 内部字符串，回调期间有效。
        std::vector<AudiosubMark> c_marks;
        c_marks.reserve(ev.marks.size());
        for (const audiosub::engine::MarkInfo& m : ev.marks) {
          c_marks.push_back(AudiosubMark{m.seq, m.text.c_str(), m.err_ms});
        }
        AudiosubSubtitle sub;
        sub.index = ev.index;
        sub.start_ms = ev.start_ms;
        sub.end_ms = ev.end_ms;
        sub.text = ev.text.c_str();
        sub.latency_ms = ev.latency_ms;
        sub.remote = ev.remote ? 1 : 0;
        sub.marks = c_marks.empty() ? nullptr : c_marks.data();
        sub.mark_count = static_cast<int>(c_marks.size());
        impl->subtitle_fn(impl->subtitle_user, &sub);
      });
}

AUDIOSUB_API void audiosub_set_mark_cb(AudiosubEngineHandle* h,
                                       AudiosubMarkFn fn, void* user) {
  if (!h) return;
  EngineImpl* impl = Cast(h);
  impl->mark_fn = fn;
  impl->mark_user = user;
  impl->engine.SetMarkCallback([impl](std::uint64_t seq, const std::string& text,
                                      std::int64_t vis_ms) {
    if (impl->mark_fn) impl->mark_fn(impl->mark_user, seq, text.c_str(), vis_ms);
  });
}

AUDIOSUB_API void audiosub_set_orphan_cb(AudiosubEngineHandle* h,
                                         AudiosubOrphanFn fn, void* user) {
  if (!h) return;
  EngineImpl* impl = Cast(h);
  impl->orphan_fn = fn;
  impl->orphan_user = user;
  impl->engine.SetOrphanCallback(
      [impl](std::uint64_t seq, const std::string& text) {
        if (impl->orphan_fn)
          impl->orphan_fn(impl->orphan_user, seq, text.c_str());
      });
}

AUDIOSUB_API int audiosub_start(AudiosubEngineHandle* h, const char* id,
                                const char* host, int port,
                                const char* audio_path) {
  if (!h) return 0;
  audiosub::engine::AudiosubEngine::Config cfg;
  cfg.id = id ? id : "";
  cfg.host = host ? host : "127.0.0.1";
  cfg.port = port;
  cfg.audio_path = audio_path ? audio_path : "wasapi";
  return Cast(h)->engine.Start(cfg) ? 1 : 0;
}

AUDIOSUB_API int audiosub_is_offerer(AudiosubEngineHandle* h) {
  if (!h) return 0;
  return Cast(h)->engine.IsOfferer() ? 1 : 0;
}

AUDIOSUB_API int audiosub_set_talking(AudiosubEngineHandle* h, int on) {
  if (!h) return 0;
  return Cast(h)->engine.SetTalking(on != 0) ? 1 : 0;
}

AUDIOSUB_API int audiosub_send_note(AudiosubEngineHandle* h,
                                    const char* utf8_text) {
  if (!h || !utf8_text) return 0;
  return Cast(h)->engine.SendNote(utf8_text) ? 1 : 0;
}

AUDIOSUB_API void audiosub_get_metrics(AudiosubEngineHandle* h,
                                       AudiosubMetrics* out) {
  if (!h || !out) return;
  const audiosub::engine::MetricsSummary m = Cast(h)->engine.GetMetrics();
  out->lat_count = m.lat.count;
  out->lat_sum = m.lat.sum;
  out->lat_max = m.lat.max;
  out->err_count = m.err.count;
  out->err_sum = m.err.sum;
  out->err_max = m.err.max;
  out->vis_count = m.vis.count;
  out->vis_sum = m.vis.sum;
  out->vis_max = m.vis.max;
}

AUDIOSUB_API void audiosub_stop(AudiosubEngineHandle* h) {
  if (!h) return;
  Cast(h)->engine.Stop();
}

}  // extern "C"
