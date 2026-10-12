/// HTTP/1.1 客户端实现（`st/net/http.hpp`）：消息模型 + 解析器 + 序列化。
/// 传输执行（连接/超时/重定向）在 `HttpClient`（http_client.hpp，组合 TcpStream/TlsStream）。

#include "st/net/http.hpp"

#include <algorithm>
#include <charconv>

namespace st::net::http {

// —— Headers ——

namespace {

[[nodiscard]] auto lower(std::string_view text) -> std::string {
  std::string out(text);
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

void Headers::set(std::string_view name, std::string_view value) {
  const std::string key = lower(name);
  for (auto& [existing, existing_value] : entries_) {
    if (lower(existing) == key) {
      existing_value = std::string(value);
      return;
    }
  }
  entries_.emplace_back(std::string(name), std::string(value));
}

auto Headers::get(std::string_view name) const -> std::optional<std::string> {
  const std::string key = lower(name);
  for (const auto& [existing, value] : entries_) {
    if (lower(existing) == key) return value;
  }
  return std::nullopt;
}

// —— Url ——

auto Url::parse(std::string_view url) -> std::optional<Url> {
  Url out;
  std::string_view rest = url;
  // scheme
  if (const auto sep = rest.find("://"); sep != std::string_view::npos) {
    out.scheme = lower(rest.substr(0, sep));
    if (out.scheme != "http" && out.scheme != "https" && out.scheme != "ws" &&
        out.scheme != "wss") {
      return std::nullopt;
    }
    rest = rest.substr(sep + 3);
  }
  out.port = out.scheme == "https" || out.scheme == "wss" ? 443 : 80;
  // path（首个 '/' 或 '?' 之后）
  std::string_view host_port = rest;
  if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
    host_port = rest.substr(0, slash);
    out.path = std::string(rest.substr(slash));
  } else if (const auto mark = rest.find('?'); mark != std::string_view::npos) {
    host_port = rest.substr(0, mark);
    out.path = "/" + std::string(rest.substr(mark));
  } else {
    out.path = "/";
  }
  // userinfo（user:pass@——基本不用，但解析不能崩）：**先剥离**再找端口，
  // 顺序不能反——userinfo 里就带 ':'，先找 colon 会把端口解析成 "pass@host:81"。
  std::string_view authority = host_port;
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    authority = authority.substr(at + 1);
  }
  // host:port（[IPv6] 形态：']' 后找 ':'）
  std::string_view host = authority;
  std::string_view port_text;
  const auto close = authority.find(']');
  const auto colon = close == std::string_view::npos ? authority.find(':')
                                                     : authority.find(':', close);
  if (colon != std::string_view::npos) {
    host = authority.substr(0, colon);
    port_text = authority.substr(colon + 1);
  }
  if (host.empty()) return std::nullopt;
  if (!port_text.empty()) {
    std::uint16_t port = 0;
    const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} || port == 0) return std::nullopt;
    out.port = port;
  }
  out.host = std::string(host);
  return out;
}

auto Url::to_string() const -> std::string {
  const bool default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
  return scheme + "://" + host + (default_port ? "" : ":" + std::to_string(port)) + path;
}

// —— serialize ——

auto serialize_request(const HttpRequest& request) -> std::string {
  std::string head;
  head.reserve(256);
  head += std::string(method_name(request.method));
  head += ' ';
  head += request.path;
  head += " HTTP/1.1\r\n";
  head += "Host: " + request.host + "\r\n";
  bool has_connection = false;
  bool has_length = false;
  for (const auto& [name, value] : request.headers.all()) {
    if (lower(name) == "host") continue;   // 上面已写（Host 唯一）
    if (lower(name) == "connection") has_connection = true;
    if (lower(name) == "content-length") has_length = true;
    head += name + ": " + value + "\r\n";
  }
  if (!has_connection) head += "Connection: close\r\n";   // 单次语义（不管理 keep-alive 池）
  if (!has_length && (!request.body.empty() || request.method == Method::Post ||
                      request.method == Method::Put || request.method == Method::Patch)) {
    head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  }
  head += "\r\n";
  return head + request.body;
}

// —— HttpResponseParser ——

void HttpResponseParser::reset() {
  state_ = State::StatusLine;
  response_ = HttpResponse{};
  buffer_.clear();
  body_expected_ = 0;
  body_received_ = 0;
  header_bytes_ = 0;
  error_.clear();
}

auto HttpResponseParser::feed(std::string_view data) -> bool {
  if (state_ == State::Done || state_ == State::Failed) return state_ == State::Done;
  buffer_.append(data.data(), data.size());
  // 依次推进状态机（每次 feed 都从当前状态续跑——增量性的全部意义）
  for (;;) {
    if (state_ == State::StatusLine) {
      if (!parse_status_line()) return state_ == State::Done;
    } else if (state_ == State::Headers) {
      if (!parse_headers()) return state_ == State::Done;
    } else if (state_ == State::BodyLength) {
      return parse_body_by_length(buffer_);
    } else if (state_ == State::BodyChunked) {
      return parse_body_chunked(buffer_);
    } else {
      return state_ == State::Done;
    }
  }
}

auto HttpResponseParser::parse_status_line() -> bool {
  const auto eol = buffer_.find("\r\n");
  if (eol == std::string::npos) {
    // 头部上限含状态行：畸形流不给他无限攒
    if (buffer_.size() > max_header_default_hint()) {
      state_ = State::Failed;
      error_ = "状态行超长（无 CRLF）";
    }
    return false;
  }
  const std::string line = buffer_.substr(0, eol);
  // "HTTP/1.1 200 OK"
  const auto sp1 = line.find(' ');
  if (sp1 == std::string::npos || sp1 + 1 >= line.size()) {
    state_ = State::Failed;
    error_ = "状态行格式非法: " + line.substr(0, 40);
    return false;
  }
  if (!line.starts_with("HTTP/1.")) {
    state_ = State::Failed;
    error_ = "非 HTTP/1.x 协议: " + line.substr(0, 24);
    return false;
  }
  const auto sp2 = line.find(' ', sp1 + 1);
  const std::string status_text =
      sp2 == std::string::npos ? line.substr(sp1 + 1) : line.substr(sp1 + 1, sp2 - sp1 - 1);
  int status = 0;
  const auto parsed =
      std::from_chars(status_text.data(), status_text.data() + status_text.size(), status);
  if (parsed.ec != std::errc{} || status < 100 || status > 599) {
    state_ = State::Failed;
    error_ = "状态码非法: " + status_text;
    return false;
  }
  response_.status = status;
  response_.reason = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
  header_bytes_ += eol + 2;
  buffer_.erase(0, eol + 2);
  state_ = State::Headers;
  return true;
}

auto HttpResponseParser::parse_headers() -> bool {
  for (;;) {
    const auto eol = buffer_.find("\r\n");
    if (eol == std::string::npos) {
      if (header_bytes_ + buffer_.size() > 64U * 1024U) {
        state_ = State::Failed;
        error_ = "头部超过 64KiB 上限";
      }
      return false;
    }
    header_bytes_ += eol + 2;
    if (eol == 0) {
      // 空行 = 头结束。体定界：Content-Length / chunked / 读到关闭。
      buffer_.erase(0, 2);
      if (head_only_ || response_.status == 204 || response_.status == 304 ||
          (response_.status >= 100 && response_.status < 200)) {
        state_ = State::Done;
        return true;
      }
      const auto length = response_.headers.get("Content-Length");
      const auto encoding = response_.headers.get("Transfer-Encoding");
      if (encoding.has_value() && lower(*encoding).find("chunked") != std::string::npos) {
        state_ = State::BodyChunked;
        return true;
      }
      if (length.has_value()) {
        std::uint64_t expected = 0;
        const auto parsed = std::from_chars(length->data(), length->data() + length->size(), expected);
        if (parsed.ec != std::errc{}) {
          state_ = State::Failed;
          error_ = "Content-Length 非法: " + *length;
          return false;
        }
        if (expected > max_body_cap()) {
          state_ = State::Failed;
          error_ = "声明体长超过上限";
          return false;
        }
        body_expected_ = expected;
        state_ = State::BodyLength;
        return true;
      }
      // 无长度无 chunked：读到连接关闭（由调用方在 EOF 时调 `finish_by_eof`——
      // 这里先把状态置 BodyLength 且 expected 未知（0 但 received 也不封顶），
      // 以剩余 buffer 全部当体、后续 feed 全进体。为简化：标记 0 = 读关闭模式。
      body_expected_ = 0;
      state_ = State::BodyLength;
      eof_delimited_ = true;
      return true;
    }
    const std::string line = buffer_.substr(0, eol);
    buffer_.erase(0, eol + 2);
    if (const auto colon = line.find(':'); colon != std::string::npos) {
      std::string name = line.substr(0, colon);
      std::string value = line.substr(colon + 1);
      // 折叠起始空白（RFC 7230 OWS）
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
      }
      response_.headers.set(name, value);
    }
    // 无冒号行：忽略（稳健——不规范服务器存在，比报错好）
  }
}

auto HttpResponseParser::parse_body_by_length(std::string_view data) -> bool {
  // data 就是 buffer_ 本身（feed 的调用约定）——不再重复追加
  (void)data;
  if (eof_delimited_) {
    // 读关闭模式：全部进体，由 finish_by_eof 收尾
    response_.body += buffer_;
    buffer_.clear();
    return false;
  }
  if (buffer_.size() < body_expected_ - body_received_) return false;
  const std::size_t take_size =
      static_cast<std::size_t>(body_expected_ - body_received_);
  response_.body.append(buffer_, 0, take_size);
  buffer_.erase(0, take_size);
  state_ = State::Done;
  return true;
}

auto HttpResponseParser::parse_body_chunked(std::string_view data) -> bool {
  (void)data;   // data 即 buffer_（feed 调用约定）——不重复追加
  for (;;) {
    if (chunk_size_remaining_ > 0) {
      const std::size_t take = static_cast<std::size_t>(
          std::min<std::uint64_t>(chunk_size_remaining_, buffer_.size()));
      response_.body.append(buffer_, 0, take);
      buffer_.erase(0, take);
      chunk_size_remaining_ -= take;
      if (chunk_size_remaining_ > 0) return false;
      continue;   // 本块吃完，等块尾 CRLF
    }
    // 期待："<hex>\r\n" 或块尾 "\r\n" 或终止 "0\r\n\r\n"
    const auto eol = buffer_.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string line = buffer_.substr(0, eol);
    buffer_.erase(0, eol + 2);
    // 块尾的空行（上一块数据后的 CRLF）
    if (line.empty()) continue;
    // 分号后是 chunk extension（不解析）
    if (const auto semi = line.find(';'); semi != std::string::npos) line = line.substr(0, semi);
    std::uint64_t size = 0;
    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), size, 16);
    if (parsed.ec != std::errc{}) {
      state_ = State::Failed;
      error_ = "chunk 大小非法: " + line.substr(0, 32);
      return false;
    }
    if (size == 0) {
      state_ = State::Done;   // 尾块（其后的 trailer 忽略到关闭即可）
      return true;
    }
    if (response_.body.size() + size > max_body_cap()) {
      state_ = State::Failed;
      error_ = "chunked 体超过上限";
      return false;
    }
    chunk_size_remaining_ = size;
  }
}

auto HttpResponseParser::finish_by_eof() -> bool {
  if (state_ == State::Done) return true;
  if (state_ != State::BodyLength || !eof_delimited_) {
    state_ = State::Failed;
    error_ = "EOF 到来时响应不完整（状态 " + std::to_string(static_cast<int>(state_)) + "）";
    return false;
  }
  // 剩余 buffer 已在 parse_body_by_length 并入 body——直接完
  state_ = State::Done;
  return true;
}

auto HttpResponseParser::body_remaining() const -> std::optional<std::uint64_t> {
  if (state_ != State::BodyLength || eof_delimited_) return std::nullopt;
  return body_expected_ - body_received_ - std::min<std::uint64_t>(
                                               response_.body.size(), body_expected_);
}

}  // namespace st::net::http
