#pragma once

/// WebSocket 服务端接入（`DESIGN.md` §net）：把「已接受的 TCP 连接」变成可用会话。
///
/// `ws.hpp` 的协议面已经双向通用（帧编解码、握手串生成与校验、`WsSession` 的 client/server 两态），
/// 缺的只是**服务端那一段接线**：读 Upgrade 请求 → 校验 key → 回 101 → 交出一条 server 侧会话。
/// 本头只补这一层，不引新概念。
///
/// 用法（WS echo 服务端，十行）：
/// ```cpp
/// auto listener = st::net::TcpListener::bind("127.0.0.1", 0);
/// for (;;) {
///   auto conn = listener->accept_for(200);
///   if (!conn || !conn->has_value()) continue;
///   auto session = st::net::ws::accept_handshake(std::move(**conn), 3000);
///   if (!session) continue;
///   while (auto message = session->read_message()) {
///     if (message->opcode == st::net::ws::Opcode::Text) session->send_text(message->text);
///   }
/// }
/// ```
///
/// 连接模型与 HTTP 服务端一致：**单线程、一连接一会话**，并发由调用方起线程。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/core/net.hpp"
#include "st/net/ws.hpp"

namespace st::net::ws {

/// `WsSession::Stream` 对 `TcpStream` 的标准适配器（客户端与服务端共用）。
///
/// 为何要这一层：`WsSession` 的流面用 `int`（-1 = 读不到/出错），而 `TcpStream::read_some`
/// 返回 `Result<size_t>`（错误可携带原因）——把后者压成 -1 是**倒退**，所以适配放在这里，
/// 让协议层不受传输层错误模型的牵连。
///
/// 两个能力都不是可选项，而是踩过的坑（实测见 `apps/xiangqi/main.cpp` 的同名类注释）：
/// - `preload`：握手时可能多读到首帧（客户端把 Upgrade 与第一帧一起发出），
///   丢掉它解码器就从帧中间开始，从此再也解不出消息；
/// - `timeout_ms`：负 = 阻塞到底（服务端单连接线程的常见选择）；非负时先
///   `wait_readable` 再一次真读，避免慢客户端把线程冻在 `recv` 里。
class TcpStreamAdapter : public WsSession::Stream {
 public:
  explicit TcpStreamAdapter(TcpStream& stream, int timeout_ms = -1)
      : stream_(stream), timeout_ms_(timeout_ms) {}

  /// 预置字节（握手后多读到的帧数据）。
  void preload(std::string bytes) { pending_ = std::move(bytes); }
  [[nodiscard]] auto has_pending() const noexcept -> bool { return !pending_.empty(); }
  /// 已交付给解码器的累计字节数（「这一轮有没有真读到东西」的判定量）。
  [[nodiscard]] auto delivered() const noexcept -> std::size_t { return delivered_; }
  /// 是否已断开（读到 EOF 或传输层错误）。
  ///
  /// 为何需要：`read_some` 返 -1 的两种语义（「这次没数据」与「已断」）必须能分开——
  /// 服务端主循环靠它决定「继续等」还是「收尾关连接」。
  [[nodiscard]] auto eof() const noexcept -> bool { return eof_; }
  void set_timeout_ms(int timeout_ms) { timeout_ms_ = timeout_ms; }

  auto write_all(std::span<const std::uint8_t> data) -> bool override {
    return static_cast<bool>(stream_.write_all(data));
  }

  auto read_some(std::span<std::uint8_t> buffer) -> int override {
    if (!pending_.empty()) {
      const std::size_t take = buffer.size() < pending_.size() ? buffer.size() : pending_.size();
      for (std::size_t i = 0; i < take; ++i) buffer[i] = static_cast<std::uint8_t>(pending_[i]);
      pending_.erase(0, take);
      delivered_ += take;
      return static_cast<int>(take);
    }
    if (timeout_ms_ >= 0) {
      const auto ready = stream_.wait_readable(timeout_ms_);
      // 没数据 ≠ 断开：返 -1（不返 0）。0 会被上层读成「对端关闭」，
      // 而慢客户端在同一时刻只是「还没发」。调用方用 `close_received()` 区分两者。
      if (!ready.has_value() || !*ready) return -1;
    }
    const auto got = stream_.read_some(buffer);
    if (!got.has_value()) {
      eof_ = true;
      return -1;
    }
    if (*got == 0) eof_ = true;
    delivered_ += *got;
    return static_cast<int>(*got);   // 0 = 对端关闭
  }

 private:
  TcpStream& stream_;
  std::string pending_{};
  std::size_t delivered_{0};
  int timeout_ms_{-1};
  bool eof_{false};
};

/// 服务端会话：拥有连接 + 适配器 + `WsSession`（三者生命周期一起，调用方只管用）。
///
/// 为何要这层包装：`WsSession` 只**借用**流（与客户端一致，协议层不掺所有权），
/// 而服务端每次 `accept` 出来的连接需要有人持有——否则出了作用域就是悬垂。
///
/// **不可移动/拷贝**：适配器持的是 `TcpStream&`，移动会把它指向已空的对象。
/// 要在容器/参数里携带它，用 `std::unique_ptr<ServerSession>`（`accept_handshake` 就返回这个）。
class ServerSession {
 public:
  ServerSession(std::unique_ptr<TcpStream> stream, std::string preload, int timeout_ms = -1);
  ~ServerSession();
  ServerSession(const ServerSession&) = delete;
  auto operator=(const ServerSession&) -> ServerSession& = delete;
  ServerSession(ServerSession&&) = delete;
  auto operator=(ServerSession&&) -> ServerSession& = delete;

  [[nodiscard]] auto session() -> WsSession& { return *session_; }
  [[nodiscard]] auto adapter() -> TcpStreamAdapter& { return *adapter_; }
  /// 连接的对端地址（诊断/日志用）。
  [[nodiscard]] auto peer_address() const -> std::string;

  // 转发常用动作（免调用方写 `session().`）
  auto send_text(std::string_view text) -> bool { return session_->send_text(text); }
  auto send_binary(std::span<const std::uint8_t> data) -> bool { return session_->send_binary(data); }
  auto send_ping(std::string_view payload = {}) -> bool { return session_->send_ping(payload); }
  auto send_close(std::uint16_t code = 1000, std::string_view reason = {}) -> bool {
    return session_->send_close(code, reason);
  }
  auto read_message() -> std::optional<Message> { return session_->read_message(); }
  auto read_message_nb() -> std::optional<Message> { return session_->read_message_nb(); }

 private:
  std::unique_ptr<TcpStream> stream_{};         // 必须最先声明：适配器借它
  std::unique_ptr<TcpStreamAdapter> adapter_{};
  std::unique_ptr<WsSession> session_{};
};

/// 服务端握手：读一整个 HTTP Upgrade 请求，校验 `Sec-WebSocket-Key`，回 `101`。
///
/// 成功返回服务端侧会话（发出帧不掩码；收到的帧必须掩码——RFC §5.1）。
/// 失败分类：超时（`Timeout`）/ 对端关闭（`Io`）/ 不是 WS 握手（`Invalid`，已尽量回 400）。
///
/// `timeout_ms` 同时用于握手与后续会话读（负 = 阻塞到底）。
/// 握手后**多读到的字节**（客户端把首帧与 Upgrade 一起发来的情况）会预置给会话，不丢弃。
[[nodiscard]] auto accept_handshake(std::unique_ptr<TcpStream> stream, int timeout_ms = 3000)
    -> Result<std::unique_ptr<ServerSession>>;

}  // namespace st::net::ws
