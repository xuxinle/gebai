#pragma once

/// WebSocket（RFC 6455，自研）：帧编解码 + 握手 + 会话。
///
/// 分层：
/// - `Frame`/`frame_encode`/`FrameDecoder`：协议层（与传输无关——单测直测）
/// - `handshake_*`：开手（HTTP Upgrade + Sec-WebSocket-Accept 校验）
/// - `WsStream`：会话层（跑在 `TcpStream` 或 `TlsStream` 上；发/收消息、ping/pong）
///
/// SHA-1 与 base64 复用 codec 层既有实现。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/net.hpp"

namespace st::net::ws {

/// 操作码（RFC 6455 §5.2）。
enum class Opcode : std::uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

/// 一帧（解析后的结构形态）。
struct Frame {
  bool fin{true};                 ///< 最后一片（分片消息的中间片为 false）
  Opcode opcode{Opcode::Text};
  std::vector<std::uint8_t> payload{};
};

/// 客户端→服务器的帧必须掩码；服务器→客户端必须不掩码（RFC §5.1）。
/// `mask_key` 为空 = 不掩码（服务器方向）；32 位 key 全零也合法（掩码=异或 0）。
[[nodiscard]] auto frame_encode(const Frame& frame, std::uint32_t mask_key = 0,
                                bool masked = false) -> std::vector<std::uint8_t>;

/// 增量帧解码器：feed 字节流，产出完整帧（一次可能多帧；内部保未消费字节）。
class FrameDecoder {
 public:
  /// 喂数据；返回本批解析出的帧（可能空——半帧）。
  auto feed(std::span<const std::uint8_t> data) -> std::vector<Frame>;
  /// 协议错误（置位后不再解析；调用方应关连接——RFC 要求 fail the connection）。
  [[nodiscard]] auto failed() const noexcept -> bool { return !error_.empty(); }
  [[nodiscard]] auto error() const -> std::string { return error_; }

 private:
  auto try_parse_one() -> std::optional<Frame>;

  std::vector<std::uint8_t> buffer_{};
  std::string error_{};
};

/// 握手键（客户端生成随机 16 字节 → base64；每次连接必须不同）。
[[nodiscard]] auto make_handshake_key() -> std::string;
/// `Sec-WebSocket-Accept` 的期望值（key + 固定 GUID 的 SHA-1 → base64）。
/// 握手校验与服务器计算共用（同一份真相）。
[[nodiscard]] auto expected_accept(std::string_view client_key) -> std::string;

/// 客户端握手请求（整段写出——HTTP/1.1 Upgrade）。
[[nodiscard]] auto client_handshake_request(std::string_view host, std::uint16_t port,
                                            std::string_view path,
                                            std::string_view client_key) -> std::string;
/// 服务器握手应答（对请求的 101 响应整段）。
[[nodiscard]] auto server_handshake_response(std::string_view client_key) -> std::string;
/// 客户端校验 101 应答（状态行 + Upgrade + Sec-WebSocket-Accept）。
[[nodiscard]] auto validate_server_handshake(std::string_view response,
                                             std::string_view client_key) -> bool;

/// 应用层消息（分片重组后的形态）。
struct Message {
  Opcode opcode{Opcode::Text};    ///< Text / Binary / Close
  std::string text{};             ///< Text 的 UTF-8（Close 时是关闭原因）
  std::vector<std::uint8_t> binary{};
  /// Close 的状态码（Close 帧才有；0 = 无）。
  std::uint16_t close_code{0};
};

/// 会话层：跑在既有流上（TcpStream 或 TlsStream——模板构造点注入）。
///
/// 线程模型：**单线程**（帧解码有状态）；需要并发时由调用方在独立线程跑
/// `read_message` 循环（配合 `st::app::Tasks`）。
class WsSession {
 public:
  /// 流接口的最小抽象（TcpStream/TlsStream 都满足；测试可注入假流）。
  class Stream {
   public:
    virtual ~Stream() = default;
    /// 写全部（阻塞至写完；失败返回 false）。
    virtual auto write_all(std::span<const std::uint8_t> data) -> bool = 0;
    /// 读一次（返回读到的字节数；0 = 对端关闭；错误抛出由实现决定——这里返回 -1）。
    virtual auto read_some(std::span<std::uint8_t> buffer) -> int = 0;
  };

  explicit WsSession(Stream& stream, bool client_side)
      : stream_(stream), client_(client_side) {}

  /// 发一条消息（自动分片由调用方自理——本层单帧发送；文本/二进制）。
  auto send_text(std::string_view text) -> bool;
  auto send_binary(std::span<const std::uint8_t> data) -> bool;
  /// 发 Ping（对端应回 Pong——RFC 要求自动，不透给应用）。
  auto send_ping(std::string_view payload = {}) -> bool;
  /// 发 Close（优雅关闭：对端回 Close 后连接结束）。
  auto send_close(std::uint16_t code = 1000, std::string_view reason = {}) -> bool;

  /// 收下一条**应用**消息（阻塞；Ping 自动回 Pong、Pong 静默吞——它们不是应用消息）。
  /// 连接关闭（收到 Close 且已回）返回 nullopt。
  auto read_message() -> std::optional<Message>;
  /// **非阻塞**收下一条应用消息：从 socket 读一次（无数据/would-block 不阻塞），
  /// 再取一条已解析消息。返回 nullopt = 「这次没有」（可能数据未凑齐，也可能已关闭）——
  /// 调用方用 `close_received()`/`close_sent()` 区分。适合主循环每帧轮询。
  auto read_message_nb() -> std::optional<Message>;
  /// 非阻塞探测：有完整消息立返（内部解析；无则 nullopt）。read_message 前用。
  auto try_read_message() -> std::optional<Message>;

  [[nodiscard]] auto close_sent() const noexcept -> bool { return close_sent_; }
  [[nodiscard]] auto close_received() const noexcept -> bool { return close_received_; }

 private:
  auto send_frame(Opcode opcode, std::span<const std::uint8_t> payload) -> bool;
  auto pump_frames() -> bool;

  Stream& stream_;
  bool client_;
  FrameDecoder decoder_{};
  /// 分片重组缓冲（Continuation 到 FIN 才成消息）。
  Opcode fragment_opcode_{Opcode::Text};
  std::vector<std::uint8_t> fragment_buffer_{};
  bool fragmenting_{false};
  bool close_sent_{false};
  bool close_received_{false};
  std::vector<std::uint8_t> read_buffer_{};
  /// 已解析待交付的帧（pump 产出；try_read 消费——FrameDecoder 不存帧）。
  std::vector<Frame> pending_frames_{};
};

}  // namespace st::net::ws
