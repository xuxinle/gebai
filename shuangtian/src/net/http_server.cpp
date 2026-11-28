/// HTTP/1.1 服务端实现（`st/net/http_server.hpp`）：请求增量解析器 + 序列化 + 监听服务。
///
/// 与客户端的对称性：解析器与 `HttpResponseParser` 同构（同样「跨 feed 续跑状态机」），
/// 序列化与 `serialize_request` 同构（同一份字节级真相给测试复用）。

#include "st/net/http_server.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>

namespace st::net::http {

namespace {

[[nodiscard]] auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

/// `METHOD` 文本 → 枚举（不认识的方法按 GET 处理？**不**——返回 nullopt 让上层 501）。
[[nodiscard]] auto method_from_name(std::string_view name) -> std::optional<Method> {
  if (name == "GET") return Method::Get;
  if (name == "POST") return Method::Post;
  if (name == "PUT") return Method::Put;
  if (name == "DELETE") return Method::Delete;
  if (name == "HEAD") return Method::Head;
  if (name == "PATCH") return Method::Patch;
  return std::nullopt;
}

/// 请求行上限 / 头上限（畸形流不给他无限攒）。
inline constexpr std::size_t kMaxHeaderBytes = 64U * 1024U;

}  // namespace

// ── HttpRequestParser ────────────────────────────────────────────────────

void HttpRequestParser::reset() {
  state_ = State::RequestLine;
  request_ = HttpRequest{};
  buffer_.clear();
  body_expected_ = 0;
  header_bytes_ = 0;
  chunk_size_remaining_ = 0;
  error_.clear();
}

auto HttpRequestParser::feed(std::string_view data) -> bool {
  if (state_ == State::Done || state_ == State::Failed) return state_ == State::Done;
  buffer_.append(data.data(), data.size());
  for (;;) {
    if (state_ == State::RequestLine) {
      if (!parse_request_line()) return state_ == State::Done;
    } else if (state_ == State::Headers) {
      if (!parse_headers()) return state_ == State::Done;
    } else if (state_ == State::BodyLength) {
      return parse_body_by_length();
    } else if (state_ == State::BodyChunked) {
      return parse_body_chunked();
    } else {
      return state_ == State::Done;
    }
  }
}

auto HttpRequestParser::parse_request_line() -> bool {
  const auto eol = buffer_.find("\r\n");
  if (eol == std::string::npos) {
    if (buffer_.size() > kMaxHeaderBytes) fail("请求行超长（无 CRLF）");
    return false;
  }
  std::string line = buffer_.substr(0, eol);
  // "GET /path HTTP/1.1"：三段以单空格分隔（前两段定界用第一个/最后一个空格，
  // 路径里允许出现空格吗？不允许（需百分号编码）——但畸形客户端会发，所以按「首个空格 + 末个空格」切，
  // 中间整段当 path，宁可接受超标路径也不把合法请求判死）。
  const auto first = line.find(' ');
  const auto last = line.rfind(' ');
  if (first == std::string::npos || first == 0 || last == std::string::npos || last <= first) {
    fail("请求行格式非法: " + line.substr(0, 40));
    return false;
  }
  const std::string_view method_text(line.data(), first);
  const std::string_view version_text(line.data() + last + 1, line.size() - last - 1);
  if (!version_text.starts_with("HTTP/1.")) {
    fail("非 HTTP/1.x 协议: " + std::string(version_text.substr(0, 16)));
    return false;
  }
  const auto method = method_from_name(method_text);
  if (!method.has_value()) {
    // 不认的方法：仍按「协议正确、语义不支持」处理——直接失败会让调用方拿不到 501 的机会。
    // 约定：置 failed 并带 501 提示，让服务端回 501 而不是 400。
    fail(std::string("不支持的请求方法: ").append(method_text));
    return false;
  }
  request_.method = *method;
  request_.path.assign(line.data() + first + 1, last - first - 1);
  if (request_.path.empty()) request_.path = "/";
  header_bytes_ += eol + 2;
  buffer_.erase(0, eol + 2);
  state_ = State::Headers;
  return true;
}

auto HttpRequestParser::parse_headers() -> bool {
  for (;;) {
    const auto eol = buffer_.find("\r\n");
    if (eol == std::string::npos) {
      if (header_bytes_ + buffer_.size() > kMaxHeaderBytes) fail("头部超过 64KiB 上限");
      return false;
    }
    header_bytes_ += eol + 2;
    if (eol == 0) {
      // 空行 = 头结束。请求体的定界：Content-Length / chunked / 都没有 = 无体。
      buffer_.erase(0, 2);
      const auto length = request_.headers.get("content-length");
      const auto encoding = request_.headers.get("transfer-encoding");
      const bool chunked =
          encoding.has_value() && lower(*encoding).find("chunked") != std::string::npos;
      if (chunked) {
        state_ = State::BodyChunked;
        return parse_body_chunked();
      }
      if (length.has_value()) {
        std::uint64_t value = 0;
        const auto parsed =
            std::from_chars(length->data(), length->data() + length->size(), value);
        if (parsed.ec != std::errc{}) {
          fail("Content-Length 非法: " + length->substr(0, 24));
          return false;
        }
        if (value > max_body_) {
          fail("请求体声明超过上限");
          return false;
        }
        body_expected_ = value;
        state_ = State::BodyLength;
        return parse_body_by_length();
      }
      // 无长度无 chunked：没有体（RFC 7230 §3.3.3——请求方向与响应方向不同，
      // 这里**不能**读关闭定界，否则服务端会把连接挂到对端超时）
      state_ = State::Done;
      return true;
    }
    const std::string line = buffer_.substr(0, eol);
    buffer_.erase(0, eol + 2);
    if (const auto colon = line.find(':'); colon != std::string::npos) {
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
      }
      request_.headers.set(name, value);
      // Host 顺手落到结构字段（handler 里常要用；不必自己回头查头）
      if (lower(name) == "host") {
        request_.host = value;
        if (const auto c = value.rfind(':'); c != std::string::npos) {
          std::uint16_t port = 0;
          const std::string_view tail(value.data() + c + 1, value.size() - c - 1);
          const auto parsed = std::from_chars(tail.data(), tail.data() + tail.size(), port);
          if (parsed.ec == std::errc{}) {
            request_.port = port;
            request_.host = value.substr(0, c);
          }
        }
      }
    }
  }
}

auto HttpRequestParser::parse_body_by_length() -> bool {
  if (body_expected_ == 0) {
    state_ = State::Done;
    return true;
  }
  const std::size_t have = request_.body.size();
  const std::uint64_t need = body_expected_ - have;
  if (buffer_.size() < need) {
    request_.body.append(buffer_);
    buffer_.clear();
    return false;
  }
  request_.body.append(buffer_, 0, static_cast<std::size_t>(need));
  buffer_.erase(0, static_cast<std::size_t>(need));
  state_ = State::Done;
  return true;
}

auto HttpRequestParser::parse_body_chunked() -> bool {
  for (;;) {
    if (chunk_size_remaining_ > 0) {
      const std::size_t take =
          static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size_remaining_, buffer_.size()));
      request_.body.append(buffer_, 0, take);
      buffer_.erase(0, take);
      chunk_size_remaining_ -= take;
      if (chunk_size_remaining_ > 0) return false;
      continue;   // 本块吃完，等块尾 CRLF
    }
    const auto eol = buffer_.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string line = buffer_.substr(0, eol);
    buffer_.erase(0, eol + 2);
    if (line.empty()) continue;   // 上一块的尾 CRLF
    if (const auto semi = line.find(';'); semi != std::string::npos) {
      line = line.substr(0, semi);   // chunk extension 不解析
    }
    std::uint64_t size = 0;
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), size, 16);
    if (parsed.ec != std::errc{}) {
      fail("chunk 大小非法: " + line.substr(0, 32));
      return false;
    }
    if (size == 0) {
      // 尾块：后面的 trailer 头忽略（读到下一段头结束即可——这里简化：直接完成，
      // 残余 trailer 字节留在 buffer（连接随响应关闭，不再复用）
      state_ = State::Done;
      return true;
    }
    if (request_.body.size() + size > max_body_) {
      fail("chunked 请求体超过上限");
      return false;
    }
    chunk_size_remaining_ = size;
  }
}

// ── 序列化 ───────────────────────────────────────────────────────────────

auto reason_phrase(int status) -> std::string_view {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

auto serialize_response(const HttpResponse& response) -> std::string {
  const int status = response.status > 0 ? response.status : 200;
  const std::string reason =
      response.reason.empty() ? std::string(reason_phrase(status)) : response.reason;
  std::string head;
  head.reserve(256);
  head += std::format("HTTP/1.1 {} {}\r\n", status, reason);
  bool has_length = false;
  bool has_connection = false;
  bool has_type = false;
  for (const auto& [name, value] : response.headers.all()) {
    if (lower(name) == "content-length") has_length = true;
    if (lower(name) == "connection") has_connection = true;
    if (lower(name) == "content-type") has_type = true;
    head += name + ": " + value + "\r\n";
  }
  // 204/304 按 RFC 不得带体（也不该带 Content-Length 的矛盾值）
  const bool bodiless = status == 204 || status == 304 || (status >= 100 && status < 200);
  if (!has_length && !bodiless) {
    head += std::format("Content-Length: {}\r\n", response.body.size());
  }
  if (!has_type && !bodiless && !response.body.empty()) {
    head += "Content-Type: text/plain; charset=utf-8\r\n";
  }
  if (!has_connection) head += "Connection: close\r\n";
  head += "\r\n";
  if (bodiless) return head;
  return head + response.body;
}

// ── HttpServer ───────────────────────────────────────────────────────────

auto HttpServer::bind(std::string_view host, std::uint16_t port, int backlog)
    -> Result<HttpServer> {
  auto listener = TcpListener::bind(host, port, backlog);
  if (!listener.has_value()) return forward_error(listener.error());
  HttpServer server;
  server.listener_ = std::move(*listener);
  return server;
}

auto HttpServer::read_request(TcpStream& stream) -> Result<HttpRequest> {
  HttpRequestParser parser;
  parser.set_max_body(max_body_);
  std::array<std::uint8_t, 16384> chunk{};
  while (!parser.complete()) {
    // 先等可读（慢客户端不占死线程）：等不到就是超时错误
    auto ready = stream.wait_readable(timeout_ms_);
    if (!ready.has_value()) return forward_error(ready.error());
    if (!*ready) return unexpected(ErrorCode::Timeout, "等待请求超时");
    const auto got = stream.read_some(chunk);
    if (!got.has_value()) return forward_error(got.error());
    if (*got == 0) return unexpected(ErrorCode::Io, "对端在请求完成前关闭连接");
    (void)parser.feed(std::string_view(reinterpret_cast<const char*>(chunk.data()), *got));   // lint-allow: L6 入站字节的自然类型是 char，与解析器签名一致
    if (parser.failed()) {
      return unexpected(ErrorCode::Parse, "HTTP 请求解析失败: " + parser.error());
    }
  }
  return parser.take();
}

void HttpServer::serve_connection(TcpStream& stream) {
  auto request = read_request(stream);
  HttpResponse response;
  if (!request.has_value()) {
    // 解析失败也回一个响应（客户端才知道为什么失败）——超时/对端关闭则不回
    const bool silent = request.error().code == ErrorCode::Timeout ||
                        request.error().code == ErrorCode::Io;
    if (silent) return;
    const bool method_issue = request.error().message.find("不支持的请求方法") != std::string::npos;
    response.status = method_issue ? 501 : 400;
    response.body = request.error().message;
    const std::string wire = serialize_response(response);
    (void)stream.write_all(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));   // lint-allow: L6 出站字节的 char→uint8_t 视图
    return;
  }
  if (handler_) {
    response = handler_(*request);
  } else {
    response.status = 500;
    response.body = "未设置请求处理器（HttpServer::set_handler）";
  }
  if (!server_header_.empty() && !response.headers.has("Server")) {
    response.headers.set("Server", server_header_);
  }
  const std::string wire = serialize_response(response);
  (void)stream.write_all(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));   // lint-allow: L6 同上
}

auto HttpServer::poll_once(int timeout_ms) -> Result<bool> {
  auto accepted = listener_.accept_for(timeout_ms);
  if (!accepted.has_value()) return forward_error(accepted.error());
  if (!accepted->has_value()) return false;   // 超时：没有连接
  serve_connection(**accepted);
  return true;
}

auto HttpServer::run(const std::function<bool()>& stop) -> Status {
  if (!listener_.is_valid()) return unexpected(ErrorCode::Invalid, "服务端未绑定");
  for (;;) {
    if (stop && stop()) return ok();
    auto tick = poll_once(200);
    if (!tick.has_value()) {
      // 监听器没了（被 close）→ 正常收工；别的错误上报
      if (!listener_.is_valid()) return ok();
      return forward_error(tick.error());
    }
  }
}

}  // namespace st::net::http
