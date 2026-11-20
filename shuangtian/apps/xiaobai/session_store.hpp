/// 小白的**单会话持久化**（自动保存 + 启动恢复）。
///
/// ## 为什么只存一个会话
///
/// 小白是“一个模型、一条对话”的应用——没有会话列表，也没有切换概念。
/// 所以持久化就是**一条对话的存档**：退出前写到固定文件，启动时读回来。
/// 多会话是歌白（会话管理）的事，不该混进这里。
///
/// ## 为什么是“连续保存”而不是“退出时保存”
///
/// 退出时保存有三个洞（都实测过）：
/// · 强杀/崩溃 → 一个字都没存；
/// · 关窗口时若正在流式生成，保存的是半截状态；
/// · 测试时要先正常退出才能看文件，来回很慢。
///
/// 改成**内容变了就存**（主循环每帧比对一次序列号，变了才写），
/// 上面三条全消失；代价只是一次小文件写入，而对话文本本身很小。
///
/// ## 存到哪
///
/// `$HOME/.xiaobai/session.json`（Windows 即 `%USERPROFILE%\.xiaobai\`）。
/// 不放工作目录：那是“模型操作的对象”，用户可能在任意目录启动小白，
/// 存档不该跟着乱跑。
///
/// `XIAOBAI_SESSION` 环境变量可覆盖路径（测试/多份存档用）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"

namespace xiaobai::session {

/// 存档路径：`XIAOBAI_SESSION` 优先，否则 `~/.xiaobai/session.json`。
[[nodiscard]] inline auto store_path() -> std::string {
  if (const auto custom = st::fs::read_env("XIAOBAI_SESSION");
      custom.has_value() && !custom->empty()) {
    return *custom;
  }
  const std::string home = st::fs::home_dir();
  if (home.empty()) return "xiaobai-session.json";   // 极端兜底：当前目录
  return st::fs::join(st::fs::join(home, ".xiaobai"), "session.json");
}

/// 存档格式版本。读档时版本不符就**丢弃**（而不是尽力解析）——
/// 旧格式硬解出来的半截对话比没有更糟：用户会以为历史真的只剩这些。
inline constexpr int kFormatVersion = 1;

/// 一条待存/待读的消息（与应用的 `Message` 逐字段对应，但**去掉了
/// UI 专属的瞬时状态**，如 `reasoning_streaming`）。
///
/// 为什么要单独一个结构而不是直接序列化 `Message`：`Message` 加了新字段时，
/// 这里会**编译报错**提醒同步——比“静默少存一个字段”强得多。
struct StoredMessage {
  std::string role;
  std::string content;
  std::string stamp;
  std::string reasoning;
  /// `{id, name, arguments}` 三元组（`arguments` 是 JSON 字符串原样）
  struct StoredCall {
    std::string id;
    std::string name;
    std::string arguments;
  };
  std::vector<StoredCall> tool_calls;
  std::string tool_call_id;
  std::string tool_name;
  std::string tool_arguments;
  std::string output;
  bool tool_failed{false};
};

struct Snapshot {
  std::vector<StoredMessage> messages;
  std::string base_url;
  std::string api_key;
  std::string model;
};

// ————————————————————————————————————————————————————————————————
// 序列化
// ————————————————————————————————————————————————————————————————

[[nodiscard]] inline auto to_json(const Snapshot& snap) -> st::Json {
  st::Json root = st::Json::object();   // ⚠ 必须显式 object（花括号初始化会变数组）
  root["version"] = kFormatVersion;

  st::Json cfg = st::Json::object();
  cfg["base_url"] = snap.base_url;
  cfg["api_key"] = snap.api_key;
  cfg["model"] = snap.model;
  root["config"] = std::move(cfg);

  st::Json arr = st::Json::array();
  for (const auto& m : snap.messages) {
    st::Json item = st::Json::object();
    item["role"] = m.role;
    item["content"] = m.content;
    if (!m.stamp.empty()) item["stamp"] = m.stamp;
    if (!m.reasoning.empty()) item["reasoning"] = m.reasoning;
    if (!m.tool_calls.empty()) {
      st::Json calls = st::Json::array();
      for (const auto& c : m.tool_calls) {
        st::Json call = st::Json::object();
        call["id"] = c.id;
        call["name"] = c.name;
        call["arguments"] = c.arguments;
        calls.push_back(std::move(call));
      }
      item["tool_calls"] = std::move(calls);
    }
    if (!m.tool_call_id.empty()) item["tool_call_id"] = m.tool_call_id;
    if (!m.tool_name.empty()) item["tool_name"] = m.tool_name;
    if (!m.tool_arguments.empty()) item["tool_arguments"] = m.tool_arguments;
    if (!m.output.empty()) item["output"] = m.output;
    if (m.tool_failed) item["tool_failed"] = true;
    arr.push_back(std::move(item));
  }
  root["messages"] = std::move(arr);
  return root;
}

/// 读字符串字段的简写（缺省返回空串）。
[[nodiscard]] inline auto str(const st::Json& obj, const char* key) -> std::string {
  const st::Json* value = st::json_find(obj, key);
  if (value == nullptr || !value->is_string()) return {};
  return st::json_as_string(*value);
}

/// 解析存档。**任何异常都返回空 optional**（宁可当作没有存档，也不半解）。
[[nodiscard]] inline auto from_json(const st::Json& root) -> std::optional<Snapshot> {
  if (!root.is_object()) return std::nullopt;
  const st::Json* ver = st::json_find(root, "version");
  if (ver == nullptr || !ver->is_number()) return std::nullopt;
  if (static_cast<int>(st::json_as_i64(*ver)) != kFormatVersion) return std::nullopt;

  Snapshot snap;
  if (const st::Json* cfg = st::json_find(root, "config"); cfg != nullptr && cfg->is_object()) {
    snap.base_url = str(*cfg, "base_url");
    snap.api_key = str(*cfg, "api_key");
    snap.model = str(*cfg, "model");
  }
  const st::Json* arr = st::json_find(root, "messages");
  if (arr == nullptr || !arr->is_array()) return std::nullopt;
  for (const auto& item : *arr) {
    if (!item.is_object()) continue;
    StoredMessage m;
    m.role = str(item, "role");
    if (m.role.empty()) continue;   // 没有角色的消息无意义（更可能是坏档）
    m.content = str(item, "content");
    m.stamp = str(item, "stamp");
    m.reasoning = str(item, "reasoning");
    if (const st::Json* calls = st::json_find(item, "tool_calls");
        calls != nullptr && calls->is_array()) {
      for (const auto& call : *calls) {
        if (!call.is_object()) continue;
        StoredMessage::StoredCall c;
        c.id = str(call, "id");
        c.name = str(call, "name");
        c.arguments = str(call, "arguments");
        m.tool_calls.push_back(std::move(c));
      }
    }
    m.tool_call_id = str(item, "tool_call_id");
    m.tool_name = str(item, "tool_name");
    m.tool_arguments = str(item, "tool_arguments");
    m.output = str(item, "output");
    if (const st::Json* failed = st::json_find(item, "tool_failed");
        failed != nullptr) {
      // 用 `json_as_bool` 的 fallback 语义：类型不符就当 false，
      // 不要求显式 `is_bool`（nlohmann 没有这个名字；对布尔求 `is_boolean`）。
      m.tool_failed = st::json_as_bool(*failed, false);
    }
    snap.messages.push_back(std::move(m));
  }
  return snap;
}

// ————————————————————————————————————————————————————————————————
// 读写
// ————————————————————————————————————————————————————————————————

/// 保存（失败不抛：存不下来不该影响聊天）。返回是否真的写成了。
[[nodiscard]] inline auto save(const Snapshot& snap) -> bool {
  if (auto made = st::fs::ensure_parent(store_path()); !made) return false;
  const std::string text = st::json_dump(to_json(snap), /*pretty=*/false);
  return st::fs::write_text(store_path(), text).has_value();
}

/// 读取（不存在/损坏都返回空）。
[[nodiscard]] inline auto load() -> std::optional<Snapshot> {
  auto text = st::fs::read_text(store_path());
  if (!text.has_value()) return std::nullopt;
  auto parsed = st::json_parse(*text);
  if (!parsed.has_value()) return std::nullopt;
  return from_json(*parsed);
}

/// 内容指纹：用来判断“这一帧要不要写”。只统计**会进存档的字段**的长度与
/// 轮数——比逐字段比较便宜，而流式追加时长度必然变化。
[[nodiscard]] inline auto fingerprint(const Snapshot& snap) -> std::uint64_t {
  std::uint64_t h = 1469598103934665603ULL;   // FNV-1a offset
  const auto mix = [&h](std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  mix(snap.messages.size());
  for (const auto& m : snap.messages) {
    mix(m.content.size());
    mix(m.reasoning.size());
    mix(m.output.size());
    mix(m.tool_calls.size());
    mix(m.tool_failed ? 1 : 0);
  }
  return h;
}

}  // namespace xiaobai::session
