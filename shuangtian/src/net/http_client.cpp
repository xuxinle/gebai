/// HTTP 客户端传输执行（`st/net/http.hpp` 的 HttpClient）：连接（TCP/TLS 自动分流）+
/// 收发（增量解析）+ 重定向（Location 跟随）。

#include "st/net/http.hpp"

#include "st/net/tls.hpp"

namespace st::net::http {

namespace {

/// 统一的「可读写字节流」窄面（TcpStream / TlsStream 二选一持有）。
struct Transport {
  TcpStream tcp{};
  std::optional<TlsStream> tls{};
  [[nodiscard]] auto valid() const noexcept -> bool {
    return tls.has_value() ? tls->is_valid() : tcp.is_valid();
  }
  auto write_all(std::span<const std::uint8_t> data) -> Status {
    return tls.has_value() ? tls->write_all(data) : tcp.write_all(data);
  }
  auto read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t> {
    return tls.has_value() ? tls->read_some(buffer) : tcp.read_some(buffer);
  }
};

}  // namespace

auto HttpClient::get(const std::string& url) -> Result<HttpResponse> {
  return get(url, Headers{});
}

auto HttpClient::get(const std::string& url, const Headers& headers) -> Result<HttpResponse> {
  return request(url, Method::Get, headers, {});
}

auto HttpClient::post(const std::string& url, std::string_view content_type,
                      std::string_view body) -> Result<HttpResponse> {
  Headers headers;
  headers.set("Content-Type", content_type);
  return request(url, Method::Post, headers, std::string(body));
}

auto HttpClient::request(const std::string& url, Method method, const Headers& headers,
                         std::string body) -> Result<HttpResponse> {
  const auto parsed = Url::parse(url);
  if (!parsed.has_value()) {
    return Err{Error{ErrorCode::Invalid, "URL 非法: " + url.substr(0, 128)}};
  }
  std::string current = url;
  HttpRequest spec{};
  spec.method = method;
  spec.headers = headers;
  spec.body = std::move(body);
  HttpResponse response{};
  for (int hop = 0;; ++hop) {
    const auto result = fetch_once(current, spec);
    if (!result.has_value()) return result;
    response = std::move(*result);
    // 3xx + Location + 还有额度 → 跟
    const auto location = response.headers.get("Location");
    const bool redirectable =
        response.status >= 300 && response.status < 400 && location.has_value();
    if (!redirectable || hop >= max_redirects) {
      response.redirected = hop > 0;
      return response;
    }
    // 相对 Location 解析（RFC 7231：可相对当前 URL）
    const auto next = Url::parse(*location);
    if (next.has_value()) {
      current = next->to_string();
    } else if (!location->empty() && (*location)[0] == '/') {
      const auto base = Url::parse(current);
      if (base.has_value()) {
        Url absolute = *base;
        absolute.path = *location;
        current = absolute.to_string();
      } else {
        current = *location;
      }
    } else {
      return Err{Error{ErrorCode::Invalid, "重定向 Location 非法: " + location->substr(0, 128)}};
    }
    response.hop_chain.push_back(current);
    spec.method = Method::Get;   // 302/303 的语义：后续改 GET（301/307 保守处理为 GET——POST 重放少见）
    spec.body.clear();
    if (hop + 1 <= max_redirects) response.redirected = true;
  }
}

auto HttpClient::fetch_once(const std::string& url, const HttpRequest& request_template)
    -> Result<HttpResponse> {
  const auto parsed = Url::parse(url);
  if (!parsed.has_value()) {
    return Err{Error{ErrorCode::Invalid, "URL 非法: " + url.substr(0, 128)}};
  }
  const bool encrypted = parsed->scheme == "https" || parsed->scheme == "wss";
  if (encrypted && !TlsStream::platform_available()) {
    return Err{Error{ErrorCode::Unsupported, "HTTPS 需要 TLS：" + TlsStream::unavailable_reason()}};
  }
  // 组装请求（host/port 来自 URL；头合并 User-Agent）
  HttpRequest spec = request_template;
  spec.host = parsed->host;
  spec.port = parsed->port;
  spec.path = parsed->path;
  if (!user_agent.empty()) spec.headers.set("User-Agent", user_agent);
  spec.headers.set("Accept", "*/*");

  const std::string wire = serialize_request(spec);
  Transport transport;
  if (encrypted) {
    auto tls = TlsStream::connect(parsed->host, parsed->port, timeout_ms);
    if (!tls.has_value()) {
      return Err{Error{tls.error().code, "HTTPS 连接失败: " + tls.error().message}};
    }
    transport.tls = std::move(*tls);
  } else {
    auto tcp = connect_tcp(parsed->host, parsed->port, timeout_ms);
    if (!tcp.has_value()) {
      return Err{Error{tcp.error().code, "TCP 连接失败: " + tcp.error().message}};
    }
    transport.tcp = std::move(*tcp);
  }
  const auto written = transport.write_all(
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(wire.data()),   // lint-allow: L6 报文发送的字节视图（char→uint8_t，布局即字节）
                                    wire.size()));
  if (!written.has_value()) {
    return Err{Error{ErrorCode::Io, "HTTP 发送失败: " + written.error().to_string()}};
  }

  HttpResponseParser parser;
  parser.set_head_only(spec.method == Method::Head);
  parser.set_max_body(spec.max_body_bytes);
  std::uint8_t chunk[16384];
  while (!parser.complete()) {
    const auto got = transport.read_some(chunk);
    if (!got.has_value()) {
      return Err{Error{ErrorCode::Io, "HTTP 读取失败: " + got.error().to_string()}};
    }
    if (*got == 0) {
      // 连接关闭：读关闭定界的响应在此收尾
      if (!parser.finish_by_eof()) {
        return Err{Error{ErrorCode::Io, "HTTP 响应不完整: " + parser.error()}};
      }
      break;
    }
    (void)parser.feed(std::string_view(reinterpret_cast<const char*>(chunk), *got));   // lint-allow: L6 响应块的字节视图（uint8_t→char，同一内存两种解读）
    if (parser.failed()) {
      return Err{Error{ErrorCode::Io, "HTTP 解析失败: " + parser.error()}};
    }
  }
  return parser.take();
}

}  // namespace st::net::http
