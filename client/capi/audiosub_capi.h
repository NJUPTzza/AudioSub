// audiosub_capi.h
// ===============
// AudiosubEngine 的 C ABI 边界。这是 DLL 对外暴露的唯一接口，专门用来跨越
// /MT(DLL 内, 含 WebRTC) 与 /MD(Qt 前端) 两套 C++ 运行时。
//
// 边界纪律（务必遵守）：
//   - 只用 C 类型：POD 结构、指针、const char*(UTF-8)、函数指针；
//   - 回调里传出的 const char* / 数组指针仅在「该回调调用期间」有效，
//     调用方若要保留必须立即拷贝（例如 Qt 侧转成 QString）；
//   - 不跨边界 malloc/free 对方的内存，不让异常穿过边界；
//   - 回调发生在 DLL 内部的后台线程，Qt 侧必须自行切回 UI 线程再刷界面。

#ifndef AUDIOSUB_CAPI_H
#define AUDIOSUB_CAPI_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(AUDIOSUB_CAPI_BUILD)
#define AUDIOSUB_API __declspec(dllexport)
#else
#define AUDIOSUB_API __declspec(dllimport)
#endif
#else
#define AUDIOSUB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 不透明引擎句柄。
typedef struct AudiosubEngineHandle AudiosubEngineHandle;

// 一条对齐到字幕的标注。
typedef struct {
  uint64_t seq;
  const char* text;  // UTF-8，仅回调期间有效
  int64_t err_ms;    // 标注匹配误差
} AudiosubMark;

// 一条「带标注增强」的字幕。
typedef struct {
  int index;
  int64_t start_ms;
  int64_t end_ms;
  const char* text;  // UTF-8，仅回调期间有效
  int64_t latency_ms;
  int remote;  // 0=本端识别, 1=对端回传
  const AudiosubMark* marks;
  int mark_count;
} AudiosubSubtitle;

// 指标快照（次数/总和/峰值）。
typedef struct {
  int64_t lat_count, lat_sum, lat_max;  // 端到端字幕延迟
  int64_t err_count, err_sum, err_max;  // 标注匹配误差
  int64_t vis_count, vis_sum, vis_max;  // 标注可见延迟
} AudiosubMetrics;

// 回调类型（均在后台线程被调用）。
typedef void (*AudiosubStateFn)(void* user, const char* state_utf8);
typedef void (*AudiosubSubtitleFn)(void* user, const AudiosubSubtitle* sub);
typedef void (*AudiosubMarkFn)(void* user, uint64_t seq, const char* text_utf8,
                               int64_t visible_ms);
typedef void (*AudiosubOrphanFn)(void* user, uint64_t seq,
                                 const char* text_utf8);

// 创建 / 销毁引擎实例。
AUDIOSUB_API AudiosubEngineHandle* audiosub_create(void);
AUDIOSUB_API void audiosub_destroy(AudiosubEngineHandle* h);

// 注册回调（约定在 audiosub_start 之前调用）。
AUDIOSUB_API void audiosub_set_state_cb(AudiosubEngineHandle* h,
                                        AudiosubStateFn fn, void* user);
AUDIOSUB_API void audiosub_set_subtitle_cb(AudiosubEngineHandle* h,
                                           AudiosubSubtitleFn fn, void* user);
AUDIOSUB_API void audiosub_set_mark_cb(AudiosubEngineHandle* h,
                                       AudiosubMarkFn fn, void* user);
AUDIOSUB_API void audiosub_set_orphan_cb(AudiosubEngineHandle* h,
                                         AudiosubOrphanFn fn, void* user);

// 启动。id="A"/"B"，audio_path="wasapi"/"webrtc"。返回 1 成功 / 0 失败。
AUDIOSUB_API int audiosub_start(AudiosubEngineHandle* h, const char* id,
                                const char* host, int port,
                                const char* audio_path);

// 是否 A 端（offerer）。返回 1/0。
AUDIOSUB_API int audiosub_is_offerer(AudiosubEngineHandle* h);

// A 端讲话开关（on=1 开始讲话）。返回 1 成功 / 0 失败。
AUDIOSUB_API int audiosub_set_talking(AudiosubEngineHandle* h, int on);

// 发送一条标注（UTF-8）。返回 1 成功 / 0 失败（DataChannel 未就绪）。
AUDIOSUB_API int audiosub_send_note(AudiosubEngineHandle* h,
                                    const char* utf8_text);

// 取指标快照。
AUDIOSUB_API void audiosub_get_metrics(AudiosubEngineHandle* h,
                                       AudiosubMetrics* out);

// 停止引擎（关闭 WebRTC/信令/线程）。
AUDIOSUB_API void audiosub_stop(AudiosubEngineHandle* h);

#ifdef __cplusplus
}
#endif

#endif  // AUDIOSUB_CAPI_H
