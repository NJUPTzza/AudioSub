// DataChannel 应用层消息协议
//
// 这条 DataChannel 上跑两类东西：
//   - 二进制帧 (binary=true)  : WASAPI 原始 PCM，跟本协议无关
//   - 文本帧   (binary=false) : 统一用本协议的 JSON，靠 "type" 字段区分种类
//
// 目前定义两种 type：
//   "annotation"  A -> B 的标注文本
//   "subtitle"    B -> A 回传的字幕文本
// 以后要扩展，只需新增 type，老接收端遇到不认识的 type 直接忽略即可。

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace audiosub::proto {

// 一条结构化消息的内存表示。字段刻意拍平，方便业务侧使用；
// 序列化时再组织成 { type, seq, event_time_ms, payload:{text} } 的嵌套结构。
struct DcMessage {
  std::string type;            // "annotation" / "subtitle" / ...
  std::uint64_t seq = 0;       // 发送端自增序号，用于去重 / 排序
  std::int64_t event_time_ms = 0;  // 事件发生的现实时间（Unix 毫秒）
  std::string text;            // 对应 JSON 里的 payload.text
};

// 把 DcMessage 编码成要通过 DataChannel 发送的 JSON 字符串。
inline std::string Serialize(const DcMessage& m) {
  const nlohmann::json j = {
      {"type", m.type},
      {"seq", m.seq},
      {"event_time_ms", m.event_time_ms},
      {"payload", {{"text", m.text}}},
  };
  return j.dump();  // 紧凑单行 JSON，适合走网络
}

// 尝试把收到的文本解析成 DcMessage。
//   返回 true  : 是一条合法的结构化消息，结果写入 out
//   返回 false : 不是本协议消息（比如普通聊天文本），调用方可按旧逻辑处理
inline bool TryParse(const std::string& s, DcMessage& out) {
  // parse 第二参数传 nullptr、第三参数传 false：解析失败不抛异常，返回 discarded。
  const nlohmann::json j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) return false;

  // 没有 type 字段就不当作结构化消息，交回普通文本逻辑。
  if (!j.contains("type") || !j["type"].is_string()) return false;

  out.type = j.value("type", "");
  out.seq = j.value("seq", std::uint64_t{0});
  out.event_time_ms = j.value("event_time_ms", std::int64_t{0});

  // payload 可能不存在或字段缺失，做容错。
  if (j.contains("payload") && j["payload"].is_object()) {
    out.text = j["payload"].value("text", "");
  }
  return true;
}

}  // namespace audiosub::proto