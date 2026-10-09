#include "st/lsp/protocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

namespace st::lsp {

namespace {

/// 一个 UTF-8 字符的字节长度（非法字节按 1 计，不崩）。
[[nodiscard]] auto utf8_length(unsigned char lead) -> std::size_t {
  if ((lead & 0x80U) == 0U) return 1;
  if ((lead & 0xE0U) == 0xC0U) return 2;
  if ((lead & 0xF0U) == 0xE0U) return 3;
  if ((lead & 0xF8U) == 0xF0U) return 4;
  return 1;   // 非法起始字节：当单字节走（宁可位置略偏，不要死循环）
}

/// 一个 UTF-8 字符对应的 **UTF-16 码元数**：BMP 内 1，BMP 外（4 字节）2。
[[nodiscard]] auto utf16_units(unsigned char lead) -> std::size_t {
  const std::size_t bytes = utf8_length(lead);
  return bytes == 4 ? 2U : 1U;
}

}  // namespace

// ———————————————————————————— 消息构造与编码 ————————————————————————————

auto make_request(Json id, std::string method, Json params) -> Message {
  Message message{};
  message.id = std::move(id);
  message.has_id = true;
  message.method = std::move(method);
  message.body = std::move(params);
  return message;
}

auto make_notification(std::string method, Json params) -> Message {
  Message message{};
  message.method = std::move(method);
  message.body = std::move(params);
  return message;
}

auto make_response(Json id, Json result) -> Message {
  Message message{};
  message.id = std::move(id);
  message.has_id = true;
  message.body = std::move(result);
  return message;
}

auto make_error_response(Json id, int code, std::string message_text) -> Message {
  Message message{};
  message.id = std::move(id);
  message.has_id = true;
  message.is_error = true;
  message.error_code = code;
  message.error_message = std::move(message_text);
  return message;
}

auto encode_message(const Message& message) -> std::string {
  Json payload = Json::object();
  payload["jsonrpc"] = "2.0";
  if (message.has_id) {
    payload["id"] = message.id;
  }
  if (message.is_error) {
    Json error = Json::object();
    error["code"] = message.error_code;
    error["message"] = message.error_message;
    payload["error"] = std::move(error);
  } else if (!message.method.empty()) {
    payload["method"] = message.method;
    // 请求/通知的 params：null 也要带上（server 按存在性判断的少见但合法）。
    payload["params"] = message.body.is_null() ? Json::object() : message.body;
  } else {
    payload["result"] = message.body.is_null() ? Json() : message.body;
  }
  const std::string body = json_dump(payload);
  return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body);
}

// ———————————————————————————— 分帧 ————————————————————————————

void FrameReader::feed(std::string_view bytes) {
  buffer_.append(bytes);
}

void FrameReader::reset() {
  buffer_.clear();
  last_error_.clear();
}

auto FrameReader::take(Message& out) -> bool {
  // 1) 找帧头结束（`\r\n\r\n`）。协议允许只有 `\n\n`（少数 server 如此），两者都认。
  std::size_t header_end = buffer_.find("\r\n\r\n");
  std::size_t separator = 4;
  if (header_end == std::string::npos) {
    header_end = buffer_.find("\n\n");
    separator = 2;
  }
  if (header_end == std::string::npos) {
    if (buffer_.size() > kMaxHeaderBytes) {
      // 帧头长到不可能：必是协议错乱（不是 LSP 流）。
      last_error_ = "帧头超长（不是 LSP 流？）";
      buffer_.clear();
    }
    return false;   // 还不够一条
  }

  // 2) 解析 `Content-Length`。其余头字段（`Content-Type` 等）忽略但必须容忍。
  std::size_t content_length = 0;
  bool found_length = false;
  std::string_view headers(buffer_.data(), header_end);
  while (!headers.empty()) {
    const std::size_t line_end = headers.find('\n');
    std::string_view line = headers.substr(0, line_end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    constexpr std::string_view kKey = "content-length:";
    if (line.size() > kKey.size()) {
      std::string lowered(line.substr(0, kKey.size()));
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (lowered == kKey) {
        const std::string_view value = line.substr(kKey.size());
        // 手写解析（`strtoull` 对前导空白宽容，这里也要）。
        std::size_t at = 0;
        while (at < value.size() && (value[at] == ' ' || value[at] == '\t')) ++at;
        std::size_t parsed = 0;
        bool digits = false;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
          parsed = parsed * 10U + static_cast<std::size_t>(value[at] - '0');
          digits = true;
          ++at;
        }
        if (digits) {
          content_length = parsed;
          found_length = true;
        }
      }
    }
    if (line_end == std::string_view::npos) break;
    headers.remove_prefix(line_end + 1);
  }
  if (!found_length) {
    last_error_ = "帧头缺 Content-Length";
    buffer_.erase(0, header_end + separator);
    return false;   // 丢弃这个坏帧头，让后续消息还能解析
  }
  if (content_length > kMaxBodyBytes) {
    last_error_ = std::format("消息体超上限（{} 字节）", content_length);
    buffer_.clear();
    return false;
  }

  // 3) 消息体齐了吗？
  const std::size_t body_start = header_end + separator;
  if (buffer_.size() < body_start + content_length) return false;   // 还差字节

  // ⚠ **先解析再改缓冲**：`body` 是指向 `buffer_` 的 view，`erase` 之后立即悬垂
  //（实测：先 erase 再 parse 会把随后读到的字节当消息体，前两条消息被静默吃掉，
  // 第三次 `next()` 才吐出一条不应期的消息）。
  const std::string_view body(buffer_.data() + body_start, content_length);

  // 4) 解析 JSON。失败：如实记录并**跳过这条**（不让一条坏消息卡死会话）。
  auto parsed = json_parse(body);
  buffer_.erase(0, body_start + content_length);
  if (!parsed.has_value()) {
    last_error_ = std::format("消息体不是合法 JSON: {}", parsed.error().to_string());
    return false;
  }
  const Json& doc = *parsed;
  if (!doc.is_object()) {
    last_error_ = "消息体不是 JSON 对象";
    return false;
  }

  last_error_.clear();
  out = Message{};
  const Json* id = json_find(doc, "id");
  if (id != nullptr && !id->is_null()) {
    out.id = *id;
    out.has_id = true;
  }
  if (const Json* method = json_find(doc, "method"); method != nullptr && method->is_string()) {
    out.method = method->get<std::string>();
  }
  if (const Json* params = json_find(doc, "params"); params != nullptr) {
    out.body = *params;
  } else if (const Json* result = json_find(doc, "result"); result != nullptr) {
    out.body = *result;
  }
  if (const Json* error = json_find(doc, "error"); error != nullptr && error->is_object()) {
    out.is_error = true;
    out.error_code = static_cast<int>(json_get_i64(*error, "code", 0));
    out.error_message = json_get_string(*error, "message");
  }
  return true;
}

auto FrameReader::next() -> std::optional<Message> {
  // 循环理由：一次 `feed` 里可能前后混着坏帧——`take` 会丢掉坏帧头/坏体并
  // 继续往下找，直到拿到一条干净消息或"字节不够一条"。
  //
  // 旧写法用 `last_error_` 是否被设置来区分"不够"与"丢了坏帧"，但要临时
  // 清/恢复错误标记——既难读又真出过 bug（重试路径把一条好消息吃掉后再返回
  // 下一条，测试实测：“期望 a，实际 b”）。现在让 `take` 自己把"丢弃了坏帧"
  // 这件事通过**缓冲变短**表达：变短就再试，没变就是字节不够。
  while (true) {
    const std::size_t before = buffer_.size();
    Message message{};
    if (take(message)) return message;
    if (buffer_.size() >= before) return std::nullopt;   // 没变化：等更多字节
    if (buffer_.empty()) return std::nullopt;
  }
}

// ———————————————————————————— 位置换算 ————————————————————————————

auto position_to_offset(std::string_view text, const Position& position) -> std::size_t {
  std::size_t line = 0;
  std::size_t offset = 0;
  // 先走行：找第 `position.line` 行的起始（0 基）。
  while (line < position.line && offset < text.size()) {
    const std::size_t newline = text.find('\n', offset);
    if (newline == std::string_view::npos) return text.size();   // 行数不足：夹到末尾
    offset = newline + 1;
    ++line;
  }
  if (offset >= text.size()) return text.size();
  // 再走列：按 UTF-16 码元前进。
  std::size_t units = 0;
  while (offset < text.size() && units < position.character) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead == '\n') break;   // 不跨行
    const std::size_t step = utf8_length(lead);
    const std::size_t advance = std::min(step, text.size() - offset);
    units += utf16_units(lead);
    offset += advance;
  }
  return offset;
}

auto offset_to_position(std::string_view text, std::size_t offset) -> Position {
  const std::size_t clamped = std::min(offset, text.size());
  Position position{};
  std::size_t at = 0;
  while (at < clamped) {
    if (text[at] == '\n') {
      ++position.line;
      position.character = 0;
      ++at;
      continue;
    }
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    const std::size_t step = std::min(utf8_length(lead), clamped - at);
    position.character += utf16_units(lead);
    at += step;
  }
  return position;
}

auto position_from_json(const Json& value) -> Position {
  Position position{};
  if (!value.is_object()) return position;
  const auto line = json_get_i64(value, "line", 0);
  const auto character = json_get_i64(value, "character", 0);
  position.line = line > 0 ? static_cast<std::size_t>(line) : 0;
  position.character = character > 0 ? static_cast<std::size_t>(character) : 0;
  return position;
}

auto position_to_json(const Position& position) -> Json {
  Json value = Json::object();
  value["line"] = position.line;
  value["character"] = position.character;
  return value;
}

auto range_from_json(const Json& value) -> Range {
  Range range{};
  if (!value.is_object()) return range;
  range.start = position_from_json(json_at(value, "start"));
  range.end = position_from_json(json_at(value, "end"));
  return range;
}

auto range_to_json(const Range& range) -> Json {
  Json value = Json::object();
  value["start"] = position_to_json(range.start);
  value["end"] = position_to_json(range.end);
  return value;
}

}  // namespace st::lsp
