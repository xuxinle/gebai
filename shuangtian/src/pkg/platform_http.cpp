// lint-allow: L6 本文件是 CONVENTIONS §2 R6 / §8 L6 登记的受控例外：系统 socket 边界
// （getaddrinfo / sockaddr / setsockopt / recv / send 的缓冲区还原）必须以 reinterpret_cast 完成，
// 且仅限本文件（`platform_*.cpp`），其余代码一律禁 reinterpret_cast。

/// 最小 HTTP/1.1 客户端（明文 `http://`）：POSIX socket 直连 + Windows winsock 分支。
/// 约束：只支持 GET；超时（连接 / 收发）避免永久阻塞；响应体上限（默认 512 MiB）；
/// 支持 Content-Length、chunked 与 Connection: close；跟随最多 N 次重定向。
/// `https://` 返回 `Unsupported`（TLS 需运行时 dlopen 系统库，本期未接入）。

#include "pkg_internal.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "st/core/string.hpp"

namespace st::pkg::detail {
namespace {

#if defined(_WIN32)
using socket_type = SOCKET;
using socket_length_type = int;
inline constexpr socket_type invalid_socket = INVALID_SOCKET;
#else
using socket_type = int;
using socket_length_type = socklen_t;
inline constexpr socket_type invalid_socket = -1;
#endif

inline constexpr std::size_t read_chunk_bytes = 16384;
inline constexpr std::uint64_t header_slack_bytes = 64ULL * 1024ULL;

[[nodiscard]] auto last_socket_error() noexcept -> int {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

[[nodiscard]] auto socket_error_text(int code) -> std::string {
#if defined(_WIN32)
  return std::format("winsock 错误 {}", code);
#else
  return std::string(::strerror(code));
#endif
}

[[nodiscard]] auto would_block(int code) noexcept -> bool {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK || code == WSAETIMEDOUT;
#else
  return code == EAGAIN || code == EWOULDBLOCK || code == EINTR || code == ETIMEDOUT;
#endif
}

[[nodiscard]] auto connect_in_progress(int code) noexcept -> bool {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEINVAL;
#else
  return code == EINPROGRESS || code == EWOULDBLOCK || code == EAGAIN;
#endif
}

/// socket 句柄 RAII（无裸 new/delete；跨平台统一为值语义移动）。
class SocketHandle {
 public:
  SocketHandle() noexcept = default;
  explicit SocketHandle(socket_type handle) noexcept : handle_(handle) {}
  ~SocketHandle() { close_now(); }
  SocketHandle(const SocketHandle&) = delete;
  auto operator=(const SocketHandle&) -> SocketHandle& = delete;
  SocketHandle(SocketHandle&& other) noexcept : handle_(other.handle_) {
    other.handle_ = invalid_socket;
  }
  auto operator=(SocketHandle&& other) noexcept -> SocketHandle& {
    if (this != &other) {
      close_now();
      handle_ = other.handle_;
      other.handle_ = invalid_socket;
    }
    return *this;
  }

  [[nodiscard]] auto valid() const noexcept -> bool { return handle_ != invalid_socket; }
  [[nodiscard]] auto get() const noexcept -> socket_type { return handle_; }
  void close_now() noexcept {
    if (handle_ == invalid_socket) return;
#if defined(_WIN32)
    ::closesocket(handle_);
#else
    ::close(handle_);
#endif
    handle_ = invalid_socket;
  }

 private:
  socket_type handle_{invalid_socket};
};

#if defined(_WIN32)
/// winsock 生命周期（替代可变全局状态：每次调用进入/离开成对初始化与清理）。
class WinsockGuard {
 public:
  WinsockGuard() noexcept {
    WSADATA data{};
    started_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~WinsockGuard() {
    if (started_) ::WSACleanup();
  }
  WinsockGuard(const WinsockGuard&) = delete;
  auto operator=(const WinsockGuard&) -> WinsockGuard& = delete;
  [[nodiscard]] auto started() const noexcept -> bool { return started_; }

 private:
  bool started_{false};
};
#endif

struct AddrInfoDeleter {
  void operator()(addrinfo* list) const noexcept {
    if (list != nullptr) ::freeaddrinfo(list);
  }
};

using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

/// 解析后的 http URL。
struct Url {
  std::string host{};
  std::uint16_t port{80};
  std::string path{"/"};
};

[[nodiscard]] auto invalid_path(std::string_view path) -> bool {
  if (path.empty()) return true;
  for (const char character : path) {
    const unsigned char code = static_cast<unsigned char>(character);
    if (code <= 0x20U || code == 0x7FU) return true;  // 空白/控制字符
  }
  return false;
}

[[nodiscard]] auto parse_http_url(std::string_view url) -> Result<Url> {
  if (has_prefix(url, "https://")) {
    return unexpected(ErrorCode::Unsupported,
                      std::string("HTTPS 需系统 TLS 支持（运行时 dlopen，本期未接入），"
                                  "请改用 http:// 或本地路径：")
                          .append(url));
  }
  if (!has_prefix(url, "http://")) {
    return unexpected(ErrorCode::Invalid,
                      std::string("URL 必须以 http:// 开头: ").append(url));
  }
  Url parsed;
  std::string_view rest = url.substr(7);
  const std::size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);
  const std::size_t slash = rest.find('/');
  const std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
  parsed.path = slash == std::string_view::npos ? std::string("/") : std::string(rest.substr(slash));
  if (authority.empty()) {
    return unexpected(ErrorCode::Invalid,
                      std::string("URL 缺少主机名: ").append(url));
  }
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos) {
    const std::string_view port_text = authority.substr(colon + 1);
    const auto port = st::parse_u64(port_text);
    if (!port || *port == 0 || *port > 65535) {
      return unexpected(ErrorCode::Invalid,
                        std::format("URL 端口非法: {}", std::string(port_text)));
    }
    parsed.port = static_cast<std::uint16_t>(*port);
    parsed.host = std::string(authority.substr(0, colon));
  } else {
    parsed.host = std::string(authority);
  }
  if (parsed.host.empty() || invalid_path(parsed.path)) {
    return unexpected(ErrorCode::Invalid, std::string("URL 非法: ").append(url));
  }
  return parsed;
}

[[nodiscard]] auto set_nonblocking(socket_type handle, bool enabled) -> Status {
#if defined(_WIN32)
  u_long mode = enabled ? 1UL : 0UL;
  if (::ioctlsocket(handle, FIONBIO, &mode) != 0) {
    return unexpected(ErrorCode::Io, "无法切换非阻塞模式（ioctlsocket）");
  }
#else
  const int flags = ::fcntl(handle, F_GETFL, 0);
  if (flags < 0) return unexpected(ErrorCode::Io, "无法读取 socket 标志（fcntl）");
  const int updated = enabled ? (flags | O_NONBLOCK) : (flags & (~O_NONBLOCK));
  if (::fcntl(handle, F_SETFL, updated) < 0) {
    return unexpected(ErrorCode::Io, "无法切换非阻塞模式（fcntl）");
  }
#endif
  return ok();
}

void set_socket_timeouts(socket_type handle, int timeout_ms) {
  timeval timeout{};
  timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
#if defined(_WIN32)
  ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               static_cast<int>(sizeof(timeout)));
  ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               static_cast<int>(sizeof(timeout)));
#else
  ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, static_cast<socket_length_type>(sizeof(timeout)));
  ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, static_cast<socket_length_type>(sizeof(timeout)));
#endif
}

[[nodiscard]] auto wait_writable(socket_type handle, int timeout_ms) -> Result<bool> {
#if defined(_WIN32)
  fd_set write_set;
  FD_ZERO(&write_set);
  FD_SET(handle, &write_set);
  fd_set error_set;
  FD_ZERO(&error_set);
  FD_SET(handle, &error_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>((timeout_ms % 1000) * 1000);
  const int rc = ::select(0, nullptr, &write_set, &error_set, &timeout);
  if (rc < 0) return unexpected(ErrorCode::Io, "select 失败");
  return rc > 0;
#else
  pollfd descriptor{};
  descriptor.fd = handle;
  descriptor.events = POLLOUT;
  const int rc = ::poll(&descriptor, 1, timeout_ms);
  if (rc < 0) return unexpected(ErrorCode::Io, std::string("poll 失败: ").append(::strerror(errno)));
  return rc > 0;
#endif
}

/// 建立连接（非阻塞 connect + 超时等待；随后恢复阻塞并设收发超时）。
[[nodiscard]] auto connect_socket(const Url& url, const HttpOptions& options) -> Result<SocketHandle> {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const std::string service = st::to_string(static_cast<std::uint64_t>(url.port));

  addrinfo* raw_list = nullptr;
  const int resolved = ::getaddrinfo(url.host.c_str(), service.c_str(), &hints, &raw_list);
  if (resolved != 0 || raw_list == nullptr) {
    return unexpected(ErrorCode::Io,
                      std::format("域名解析失败: {}:{}", url.host, service));
  }
  const AddrInfoPtr list(raw_list);

  std::string last_error = "无可用的地址";
  for (addrinfo* item = list.get(); item != nullptr; item = item->ai_next) {
    SocketHandle handle(::socket(item->ai_family, item->ai_socktype, item->ai_protocol));
    if (!handle.valid()) {
      last_error = "无法创建 socket";
      continue;
    }
    if (const Status status = set_nonblocking(handle.get(), true); !status) {
      last_error = status.error().message;
      continue;
    }
    const int connected = ::connect(handle.get(), reinterpret_cast<const sockaddr*>(item->ai_addr),
                                    static_cast<socket_length_type>(item->ai_addrlen));
    if (connected != 0) {
      const int code = last_socket_error();
      if (!connect_in_progress(code)) {
        last_error = std::format("connect 失败: {}", socket_error_text(code));
        continue;
      }
      const auto writable = wait_writable(handle.get(), options.connect_timeout_ms);
      if (!writable) {
        last_error = writable.error().message;
        continue;
      }
      if (!*writable) {
        last_error = std::format("连接 {}:{} 超时", url.host, service);
        continue;
      }
      int pending = 0;
      socket_length_type length = static_cast<socket_length_type>(sizeof(pending));
#if defined(_WIN32)
      const int probed = ::getsockopt(handle.get(), SOL_SOCKET, SO_ERROR,
                                      reinterpret_cast<char*>(&pending), &length);
#else
      const int probed = ::getsockopt(handle.get(), SOL_SOCKET, SO_ERROR, &pending, &length);
#endif
      if (probed != 0 || pending != 0) {
        last_error = std::format("connect 失败: {}", socket_error_text(pending));
        continue;
      }
    }
    if (const Status status = set_nonblocking(handle.get(), false); !status) {
      last_error = status.error().message;
      continue;
    }
    set_socket_timeouts(handle.get(), options.io_timeout_ms);
    return handle;
  }
  return unexpected(ErrorCode::Io,
                    std::format("连接 {}:{} 失败（{}）", url.host, service, last_error));
}

[[nodiscard]] auto send_all(socket_type handle, std::span<const std::uint8_t> data) -> Status {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::span<const std::uint8_t> remaining = data.subspan(sent);
    const std::size_t chunk = std::min<std::size_t>(remaining.size(), 65536);
    const std::span<const std::uint8_t> slice = remaining.first(chunk);
#if defined(_WIN32)
    const int written =
        ::send(handle, reinterpret_cast<const char*>(slice.data()), static_cast<int>(chunk), 0);
    const bool failed = written < 0;
    const std::ptrdiff_t count = written < 0 ? 0 : static_cast<std::ptrdiff_t>(written);
#else
    const ssize_t written = ::send(handle, slice.data(), chunk, 0);
    const bool failed = written < 0;
    const std::ptrdiff_t count = written < 0 ? 0 : static_cast<std::ptrdiff_t>(written);
#endif
    if (failed) {
      const int code = last_socket_error();
      if (would_block(code)) return unexpected(ErrorCode::Timeout, "发送超时");
      return unexpected(ErrorCode::Io, std::format("发送失败: {}", socket_error_text(code)));
    }
    sent += static_cast<std::size_t>(count);
  }
  return ok();
}

[[nodiscard]] auto read_all(socket_type handle, std::uint64_t limit) -> Result<std::vector<std::uint8_t>> {
  std::vector<std::uint8_t> data;
  std::array<std::uint8_t, read_chunk_bytes> buffer{};
  while (true) {
#if defined(_WIN32)
    const int received =
        ::recv(handle, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
    const bool failed = received < 0;
    const bool closed = received == 0;
    const std::ptrdiff_t count = received <= 0 ? 0 : static_cast<std::ptrdiff_t>(received);
#else
    const ssize_t received = ::recv(handle, buffer.data(), buffer.size(), 0);
    const bool failed = received < 0;
    const bool closed = received == 0;
    const std::ptrdiff_t count = received <= 0 ? 0 : static_cast<std::ptrdiff_t>(received);
#endif
    if (failed) {
      const int code = last_socket_error();
      if (would_block(code)) return unexpected(ErrorCode::Timeout, "接收超时");
      return unexpected(ErrorCode::Io, std::format("接收失败: {}", socket_error_text(code)));
    }
    if (closed) break;
    const std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(count));
    data.insert(data.end(), chunk.begin(), chunk.end());
    if (static_cast<std::uint64_t>(data.size()) > limit) {
      return unexpected(ErrorCode::Overflow,
                        std::format("响应体超过上限（{} 字节）", limit));
    }
  }
  return data;
}

[[nodiscard]] auto build_request(const Url& url) -> std::string {
  std::string request;
  request.append("GET ").append(url.path).append(" HTTP/1.1\r\n");
  request.append("Host: ").append(url.host);
  if (url.port != 80) {
    request.push_back(':');
    request.append(st::to_string(static_cast<std::uint64_t>(url.port)));
  }
  request.append("\r\n");
  request.append("User-Agent: stpm/0.1 (shuangtian self-hosted package manager)\r\n");
  request.append("Accept: */*\r\n");
  request.append("Connection: close\r\n\r\n");
  return request;
}

[[nodiscard]] auto header_value_lower(std::string_view name) -> std::string {
  std::string lowered;
  lowered.reserve(name.size());
  for (const char character : name) {
    lowered.push_back((character >= 'A' && character <= 'Z')
                          ? static_cast<char>(character - 'A' + 'a')
                          : character);
  }
  return lowered;
}

struct RawResponse {
  int status{0};
  std::string location{};
  std::string transfer_encoding{};
  std::int64_t content_length{-1};
  std::vector<std::uint8_t> body{};
};

[[nodiscard]] auto decode_chunked(std::span<const std::uint8_t> payload) -> Result<std::vector<std::uint8_t>> {
  const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
  std::vector<std::uint8_t> body;
  std::size_t offset = 0;
  while (true) {
    const std::size_t line_end = text.find("\r\n", offset);
    if (line_end == std::string_view::npos) {
      return unexpected(ErrorCode::Parse, "chunked 响应缺少块长度行");
    }
    std::string_view size_text = text.substr(offset, line_end - offset);
    if (const std::size_t extension = size_text.find(';'); extension != std::string_view::npos) {
      size_text = size_text.substr(0, extension);
    }
    const auto size = st::parse_u64(st::trim(size_text));
    if (!size) return unexpected(ErrorCode::Parse, "chunked 块长度非法");
    offset = line_end + 2;
    if (*size == 0) return body;  // 结束块（忽略 trailer）
    const std::size_t chunk_size = static_cast<std::size_t>(*size);
    if (offset + chunk_size > payload.size()) {
      return unexpected(ErrorCode::Parse, "chunked 块数据不完整");
    }
    const std::span<const std::uint8_t> chunk = payload.subspan(offset, chunk_size);
    body.insert(body.end(), chunk.begin(), chunk.end());
    offset += chunk_size;
    if (offset + 2 > payload.size() || text.substr(offset, 2) != "\r\n") {
      return unexpected(ErrorCode::Parse, "chunked 块数据缺少 CRLF 终止");
    }
    offset += 2;
  }
}

[[nodiscard]] auto parse_response(std::span<const std::uint8_t> payload, std::uint64_t max_body_bytes,
                                  int* status_out, std::string* location_out) -> Result<std::vector<std::uint8_t>> {
  const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
  std::size_t header_end = text.find("\r\n\r\n");
  std::size_t body_start = 0;
  if (header_end == std::string_view::npos) {
    header_end = text.find("\n\n");
    if (header_end == std::string_view::npos) {
      return unexpected(ErrorCode::Parse, "HTTP 响应缺少头部终止");
    }
    body_start = header_end + 2;
  } else {
    body_start = header_end + 4;
  }
  const std::string_view head = text.substr(0, header_end);

  const std::size_t status_line_end = head.find('\n');
  const std::string_view status_line =
      status_line_end == std::string_view::npos ? head : head.substr(0, status_line_end);
  const std::size_t first_space = status_line.find(' ');
  if (first_space == std::string_view::npos) {
    return unexpected(ErrorCode::Parse, "HTTP 响应状态行非法");
  }
  std::string_view status_text = status_line.substr(first_space + 1);
  if (const std::size_t space = status_text.find(' '); space != std::string_view::npos) {
    status_text = status_text.substr(0, space);
  }
  const auto status = st::parse_u64(status_text);
  if (!status || *status > 999) return unexpected(ErrorCode::Parse, "HTTP 状态码非法");
  *status_out = static_cast<int>(*status);

  std::string transfer_encoding;
  std::int64_t content_length = -1;
  std::size_t offset = status_line_end == std::string_view::npos ? head.size() : status_line_end + 1;
  while (offset < head.size()) {
    const std::size_t line_end = head.find('\n', offset);
    const std::size_t stop = line_end == std::string_view::npos ? head.size() : line_end;
    std::string_view line = head.substr(offset, stop - offset);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    offset = stop + 1;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string name = header_value_lower(st::trim(line.substr(0, colon)));
    const std::string_view value = st::trim(line.substr(colon + 1));
    if (name == "transfer-encoding") {
      transfer_encoding = header_value_lower(value);
    } else if (name == "content-length") {
      const auto length = st::parse_u64(value);
      if (length) content_length = static_cast<std::int64_t>(*length);
    } else if (name == "location") {
      location_out->assign(value);
    }
  }

  const std::span<const std::uint8_t> body_bytes = payload.subspan(body_start);
  std::vector<std::uint8_t> body;
  if (transfer_encoding.find("chunked") != std::string::npos) {
    const auto decoded = decode_chunked(body_bytes);
    if (!decoded) return forward_error(decoded.error());
    body = *decoded;
  } else if (content_length >= 0) {
    const std::size_t wanted = static_cast<std::size_t>(content_length);
    const std::size_t available = std::min(wanted, body_bytes.size());
    body.assign(body_bytes.begin(), body_bytes.begin() + static_cast<std::ptrdiff_t>(available));
  } else {
    body.assign(body_bytes.begin(), body_bytes.end());
  }
  if (static_cast<std::uint64_t>(body.size()) > max_body_bytes) {
    return unexpected(ErrorCode::Overflow,
                      std::format("响应体超过上限（{} 字节）", max_body_bytes));
  }
  return body;
}

[[nodiscard]] auto is_redirect(int status) noexcept -> bool {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

[[nodiscard]] auto resolve_redirect(const Url& base, std::string_view location) -> Result<Url> {
  if (has_prefix(location, "http://") || has_prefix(location, "https://")) {
    return parse_http_url(location);
  }
  Url target = base;
  if (!location.empty() && location.front() == '/') {
    target.path = std::string(location);
    return target;
  }
  const std::size_t slash = base.path.rfind('/');
  const std::string directory =
      slash == std::string::npos ? std::string("/") : base.path.substr(0, slash + 1);
  target.path = directory + std::string(location);
  if (invalid_path(target.path)) return unexpected(ErrorCode::Invalid, "重定向地址非法");
  return target;
}

/// 单次请求（连接 → 发送 → 读满 → 解析）。
[[nodiscard]] auto perform_request(const Url& url, const HttpOptions& options)
    -> Result<RawResponse> {
  auto connected = connect_socket(url, options);
  if (!connected) return forward_error(connected.error());
  SocketHandle handle = std::move(*connected);
  const std::string request = build_request(url);
  const std::span<const std::uint8_t> payload(
      reinterpret_cast<const std::uint8_t*>(request.data()), request.size());
  if (const Status sent = send_all(handle.get(), payload); !sent) {
    return forward_error(sent.error());
  }
  const auto raw = read_all(handle.get(), options.max_body_bytes + header_slack_bytes);
  if (!raw) return forward_error(raw.error());

  RawResponse response;
  const auto body = parse_response(*raw, options.max_body_bytes, &response.status, &response.location);
  if (!body) return forward_error(body.error());
  response.body = *body;
  return response;
}

[[nodiscard]] auto request_once(const Url& url, const HttpOptions& options) -> Result<HttpResponse> {
  Url target = url;
  const int attempts = options.max_redirects < 0 ? 0 : options.max_redirects;
  for (int hop = 0; hop <= attempts; ++hop) {
    const auto raw = perform_request(target, options);
    if (!raw) return forward_error(raw.error());
    if (!is_redirect(raw->status)) {
      HttpResponse response;
      response.status = raw->status;
      response.body = raw->body;
      return response;
    }
    if (raw->location.empty()) {
      return unexpected(ErrorCode::Io, "重定向响应缺少 Location 头");
    }
    if (hop == attempts) break;
    const auto next = resolve_redirect(target, raw->location);
    if (!next) return forward_error(next.error());
    target = *next;
  }
  return unexpected(ErrorCode::Io, "重定向次数过多（超过 max_redirects）");
}

}  // namespace

auto http_get(std::string_view url, const HttpOptions& options) -> Result<HttpResponse> {
#if defined(_WIN32)
  const WinsockGuard guard;
  if (!guard.started()) {
    return unexpected(ErrorCode::Io, "Winsock 初始化失败（WSAStartup）");
  }
#endif
  const auto parsed = parse_http_url(url);
  if (!parsed) return forward_error(parsed.error());
  return request_once(*parsed, options);
}

}  // namespace st::pkg::detail
