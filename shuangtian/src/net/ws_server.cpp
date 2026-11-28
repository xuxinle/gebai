/// WebSocket 服务端接入实现（`st/net/ws_server.hpp`）：握手 + 会话装配。
///
/// 握手就是一段 HTTP/1.1 Upgrade 请求——这里**复用** `HttpRequestParser`（服务端 HTTP 那套），
/// 不再写第三遍「请求行 + 头」解析。这也是把请求解析器放在 net 层而非服务端私有的收益：
/// WS 服务端白得一份经过测试的解析器。

#include "st/net/ws_server.hpp"

#include <algorithm>
#include <array>

#include "st/net/http_server.hpp"

namespace st::net::ws {

namespace {

[[nodiscard]] auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

[[nodiscard]] auto contains_ci(std::string_view haystack, std::string_view needle) -> bool {
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

[[nodiscard]] auto send_text(TcpStream& stream, std::string_view text) -> bool {
  return static_cast<bool>(stream.write_all(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));   // lint-allow: L6 出站字节视图
}

/// 回一个 HTTP 错误（握手失败），尽力而为（对端已断就算了）。
void reply_error(TcpStream& stream, int status, std::string_view text) {
  http::HttpResponse response;
  response.status = status;
  response.body = std::string(text);
  (void)send_text(stream, http::serialize_response(response));
}

}  // namespace

auto accept_handshake(std::unique_ptr<TcpStream> stream, int timeout_ms)
    -> Result<std::unique_ptr<ServerSession>> {
  if (stream == nullptr || !stream->is_valid()) {
    return unexpected(ErrorCode::Invalid, "WS 握手需要一条已连接且有效的 TCP 流");
  }
  // 逐块读到头结束（\r\n\r\n）：结构判定交给 HttpRequestParser，
  // 但**保留原始字节**以便把「头之后剩下的」交给会话（那是首帧，丢了帧流就永远错位）。
  std::string raw;
  raw.reserve(1024);
  http::HttpRequestParser probe;
  probe.set_max_body(0);   // 握手请求没有体
  std::array<std::uint8_t, 4096> buffer{};
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const auto ready = stream->wait_readable(timeout_ms);
    if (!ready.has_value()) return forward_error(ready.error());
    if (!*ready) return unexpected(ErrorCode::Timeout, "等待 WS 握手请求超时");
    const auto got = stream->read_some(buffer);
    if (!got.has_value()) return forward_error(got.error());
    if (*got == 0) return unexpected(ErrorCode::Io, "对端在 WS 握手完成前关闭连接");
    raw.append(reinterpret_cast<const char*>(buffer.data()), *got);   // lint-allow: L6 入站字节视图
    if (raw.size() > 64U * 1024U) {
      reply_error(*stream, 431, "握手请求头超过 64KiB");
      return unexpected(ErrorCode::Invalid, "WS 握手请求头超过上限");
    }
    header_end = raw.find("\r\n\r\n");
    (void)probe.feed(std::string_view(raw));
    if (probe.failed()) {
      reply_error(*stream, 400, probe.error());
      return unexpected(ErrorCode::Parse, "WS 握手请求解析失败: " + probe.error());
    }
  }
  // 头 = 到 \r\n\r\n 含；其余是首帧数据（必须原样交给会话）
  std::string remainder;
  if (header_end + 4 < raw.size()) remainder = raw.substr(header_end + 4);
  const std::string headers = raw.substr(0, header_end + 4);

  // 校验四项：GET + Upgrade: websocket + Connection: Upgrade + Sec-WebSocket-Key（Version 13）
  http::HttpRequestParser check;
  check.set_max_body(0);
  (void)check.feed(headers);
  if (check.failed() || !check.complete()) {
    reply_error(*stream, 400, "握手请求不是合法 HTTP/1.1 请求");
    return unexpected(ErrorCode::Invalid, "WS 握手请求不完整或非法");
  }
  const http::HttpRequest request = check.take();
  if (request.method != http::Method::Get) {
    reply_error(*stream, 405, "WS 握手必须是 GET");
    return unexpected(ErrorCode::Invalid, "WS 握手方法不是 GET");
  }
  const auto upgrade = request.headers.get("upgrade");
  if (!upgrade.has_value() || !contains_ci(*upgrade, "websocket")) {
    reply_error(*stream, 400, "缺少 Upgrade: websocket");
    return unexpected(ErrorCode::Invalid, "WS 握手缺少 Upgrade: websocket");
  }
  const auto connection = request.headers.get("connection");
  if (!connection.has_value() || !contains_ci(*connection, "upgrade")) {
    reply_error(*stream, 400, "缺少 Connection: Upgrade");
    return unexpected(ErrorCode::Invalid, "WS 握手缺少 Connection: Upgrade");
  }
  const auto key = request.headers.get("sec-websocket-key");
  if (!key.has_value() || key->empty()) {
    reply_error(*stream, 400, "缺少 Sec-WebSocket-Key");
    return unexpected(ErrorCode::Invalid, "WS 握手缺少 Sec-WebSocket-Key");
  }
  const auto version = request.headers.get("sec-websocket-version");
  if (version.has_value() && *version != "13") {
    // 只支持 13（RFC 6455）；其它版本按规范回 426 + 声明支持的版本
    (void)send_text(*stream,
                    "HTTP/1.1 426 Upgrade Required\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 0\r\n\r\n");
    return unexpected(ErrorCode::Unsupported, "只支持 Sec-WebSocket-Version: 13");
  }

  // 回 101（复用协议层同一份真相：server_handshake_response 用的就是 expected_accept）
  if (!send_text(*stream, server_handshake_response(*key))) {
    return unexpected(ErrorCode::Io, "WS 握手应答发送失败");
  }

  // 装配：连接 → 适配器（预置剩余字节）→ 会话（服务端方向，发出帧不掩码）
  return std::make_unique<ServerSession>(std::move(stream), std::move(remainder), timeout_ms);
}

// ── ServerSession ────────────────────────────────────────────────────────

ServerSession::ServerSession(std::unique_ptr<TcpStream> stream, std::string preload,
                             int timeout_ms)
    : stream_(std::move(stream)),
      adapter_(std::make_unique<TcpStreamAdapter>(*stream_, timeout_ms)) {
  adapter_->preload(std::move(preload));
  session_ = std::make_unique<WsSession>(*adapter_, /*client_side=*/false);
}

ServerSession::~ServerSession() = default;

auto ServerSession::peer_address() const -> std::string {
  return stream_ != nullptr ? stream_->peer_address() : std::string{};
}

}  // namespace st::net::ws
