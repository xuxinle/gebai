#pragma once

/// LSP 消息层（`Content-Length` 帧 + JSON-RPC 2.0 编解码）。
///
/// 这一层**只做两件事**，不碰进程、不碰 UI：
/// 1. **分帧**：LSP 走 `Content-Length: N\r\n\r\n<body>` 的字节流（与 HTTP 同源）。
///    读端要处理「一次读进来半条消息」「一次读进来三条消息」两种情形——
///    分帧层持有剩余缓冲，凑齐一条就交一条。
/// 2. **JSON-RPC**：请求（有 id，要应答）、响应（有 id，配请求）、通知（无 id，不等应答）
///    三种形态的构造与判别。
///
/// 为什么不塞进 `st/lsp/client`：分帧与语义是**两个变化率**——协议版本升级改语义，
/// 而分帧格式从 3.0 起没动过；混在一起以后没人敢动。而且分帧能脱离进程单测
/// （喂字节断言消息），这是本文件测试策略的基础。
///
/// 依赖纪律：只用 `st::ext::Json`（nlohmann）与标准库；不含进程/线程/UI 头。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"

namespace st::lsp {

/// JSON-RPC 错误码（协议标准值 + LSP 扩展）。
namespace error_code {
constexpr int kParseError{-32700};
constexpr int kInvalidRequest{-32600};
constexpr int kMethodNotFound{-32601};
constexpr int kInvalidParams{-32602};
constexpr int kInternalError{-32603};
/// LSP：请求被取消（`$/cancelRequest` 生效）。
constexpr int kRequestCancelled{-32800};
/// LSP：内容已修改（响应对应一个过期的文档版本）。
constexpr int kContentModified{-32801};
/// LSP：服务端未初始化完成。
constexpr int kServerNotInitialized{-32002};
/// LSP：未知错误码。
constexpr int kUnknownErrorCode{-32001};
}  // namespace error_code

/// 一条已分帧的消息。
struct Message {
  /// JSON-RPC 的 `id`（原样保留：数字或字符串都可能，LSP 允许两者）。
  /// 通知没有 id（`has_id == false`）。
  Json id{};
  bool has_id{false};
  /// 方法名（请求/通知）。响应消息里为空。
  std::string method{};
  /// `params`（请求/通知）或 `result`（成功响应）。缺省为 null。
  Json body{};
  /// 错误响应（协议里 `error` 与 `result` 互斥）。
  bool is_error{false};
  int error_code{0};
  std::string error_message{};

  /// 是否响应（有 id、无 method）。
  [[nodiscard]] auto is_response() const noexcept -> bool { return has_id && method.empty(); }
  /// 是否请求（有 id、有 method）。
  [[nodiscard]] auto is_request() const noexcept -> bool { return has_id && !method.empty(); }
  /// 是否通知（无 id、有 method）。
  [[nodiscard]] auto is_notification() const noexcept -> bool {
    return !has_id && !method.empty();
  }
};

/// 把一条消息编码成 LSP 线格式（含帧头）。
///
/// `body` 为 `params`（请求/通知）或 `result`（响应）。
/// `error_code != 0` 时按错误响应编码（`error_message` 必须有值）。
[[nodiscard]] auto encode_message(const Message& message) -> std::string;

/// 便捷构造：请求。
[[nodiscard]] auto make_request(Json id, std::string method, Json params = Json()) -> Message;
/// 便捷构造：通知。
[[nodiscard]] auto make_notification(std::string method, Json params = Json()) -> Message;
/// 便捷构造：成功响应。
[[nodiscard]] auto make_response(Json id, Json result) -> Message;
/// 便捷构造：错误响应。
[[nodiscard]] auto make_error_response(Json id, int code, std::string message) -> Message;

/// 增量分帧器：喂字节、吐消息。
///
/// 用法（读线程）：
/// ```cpp
/// FrameReader reader;
/// while (handle.read_some(chunk)) {
///   reader.feed(chunk);
///   while (auto message = reader.next()) { queue.push(*message); }
/// }
/// ```
/// 设计取舍：**不做流式部分解析**——LSP 消息都不大（毫秒级 server 响应），
/// 攒齐再解析换来的简单性远大于省下的内存峰值。
class FrameReader {
 public:
  /// 喂入一段字节（可任意切分）。
  void feed(std::string_view bytes);

  /// 取一条完整消息；不足一条返回 `nullopt`。
  ///
  /// 解析失败的帧被**丢弃并记入 `last_error()`**（不抛异常、不卡住后续消息）——
  /// 一条坏消息不该让整个会话瘫掉（server 有时会在 stderr 混入非协议输出，
  /// 合并读时尤其常见）。
  [[nodiscard]] auto next() -> std::optional<Message>;

  /// 最近一次解析失败的原因（空 = 无错）。成功解析一条后清空。
  [[nodiscard]] auto last_error() const -> const std::string& { return last_error_; }

  /// 已缓存但未凑成完整消息的字节数（诊断/测试用）。
  [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t { return buffer_.size(); }

  /// 丢弃缓冲（重连时调）。
  void reset();

 private:
  /// 从缓冲头部取一条：返回 false = 不够一条（`last_error_` 可能被填）。
  [[nodiscard]] auto take(Message& out) -> bool;

  std::string buffer_{};
  std::string last_error_{};
  /// 帧头解析上限：`Content-Length` 本身很短，超此长度必是协议错乱（不无限等）。
  static constexpr std::size_t kMaxHeaderBytes{4096};
  /// 单条消息体上限（64 MiB）：防一条坏帧头把内存吃光。
  static constexpr std::size_t kMaxBodyBytes{64U * 1024U * 1024U};
};

/// 把 JSON 转成 LSP 的 `Position`（0 基行、UTF-16 列）。
///
/// ⚠ **UTF-16 的坑**：LSP 的列以 UTF-16 码元计，而我们的文档是 UTF-8 字节。
/// BMP 外的字符（emoji 等）在 UTF-16 里占 2 个码元、UTF-8 里占 4 字节。
/// 本层提供两向换算，**不假设 ASCII**——中文注释（BMP 内，1 码元 3 字节）也走同一路径。
struct Position {
  std::size_t line{0};        ///< 0 基
  std::size_t character{0};   ///< 0 基，UTF-16 码元
};

/// `Position` ↔ UTF-8 字节偏移（相对于整篇文档）。
///
/// 越界的行/列**夹取**到文档末尾（不抛）——server 给的 range 有时超出当前文本
/// （它手里的版本比我们新一帧），夹取比崩掉好，且语义上就是「尽量到那附近」。
[[nodiscard]] auto position_to_offset(std::string_view text, const Position& position)
    -> std::size_t;
[[nodiscard]] auto offset_to_position(std::string_view text, std::size_t offset) -> Position;

/// 由 JSON `{"line":..,"character":..}` 解析 `Position`（缺字段按 0）。
[[nodiscard]] auto position_from_json(const Json& value) -> Position;
/// 转成 JSON `{"line":..,"character":..}`。
[[nodiscard]] auto position_to_json(const Position& position) -> Json;

/// 区间 `[start, end)`。
struct Range {
  Position start{};
  Position end{};
};

/// 由 JSON `{"start":..,"end":..}` 解析 `Range`。
[[nodiscard]] auto range_from_json(const Json& value) -> Range;
[[nodiscard]] auto range_to_json(const Range& range) -> Json;

}  // namespace st::lsp
