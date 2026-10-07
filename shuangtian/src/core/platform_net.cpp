// 平台边界：本文件集中封装系统 socket API（CONVENTIONS §2 R6 的受控例外——sockaddr 布局转换）。
#include "st/core/net.hpp"

#include <cstring>
#include <format>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace st::net {
namespace {

#if defined(_WIN32)
using NativeHandle = SOCKET;
inline constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;

[[nodiscard]] auto startup() -> bool {
  static const bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

void close_handle(NativeHandle handle) {
  if (handle != kInvalidHandle) closesocket(handle);
}

[[nodiscard]] auto last_error_text() -> std::string {
  return std::format("winsock error {}", WSAGetLastError());
}
[[nodiscard]] auto would_block(int error) -> bool {
  return error == WSAEWOULDBLOCK;
}
/// winsock 不设 errno——socket 错误必须读 WSAGetLastError（审视报告 §4.1：
/// 非阻塞 socket 的 would-block 若误读 errno 会被错分类成 Io/Timeout，
/// 表现为连接被静默通忘或无潔等待，是停摆竞态的头号嫌疑）。
[[nodiscard]] auto get_socket_error() -> int { return WSAGetLastError(); }
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;

[[nodiscard]] auto startup() -> bool { return true; }

void close_handle(NativeHandle handle) {
  if (handle != kInvalidHandle) ::close(handle);
}

[[nodiscard]] auto last_error_text() -> std::string {
  return std::string(std::strerror(errno));
}
[[nodiscard]] auto would_block(int error) -> bool {
  return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
}
[[nodiscard]] auto get_socket_error() -> int { return errno; }
#endif

[[nodiscard]] auto to_handle(std::intptr_t value) -> NativeHandle {
  return static_cast<NativeHandle>(value);
}

[[nodiscard]] auto from_handle(NativeHandle value) -> std::intptr_t {
  return static_cast<std::intptr_t>(value);
}

[[nodiscard]] auto is_valid_handle(NativeHandle value) -> bool { return value != kInvalidHandle; }

/// 等待文件描述符可读/可写。
[[nodiscard]] auto wait_handle(NativeHandle handle, bool readable, int timeout_ms) -> Result<bool> {
#if defined(_WIN32)
  WSAPOLLFD entry{};
  entry.fd = handle;
  entry.events = readable ? POLLRDNORM : POLLWRNORM;
  const int ready = WSAPoll(&entry, 1, timeout_ms);
  if (ready < 0) return unexpected(ErrorCode::Io, last_error_text());
  return ready > 0;
#else
  pollfd entry{};
  entry.fd = handle;
  entry.events = readable ? POLLIN : POLLOUT;
  int ready = 0;
  do {
    ready = ::poll(&entry, 1, timeout_ms);
  } while (ready < 0 && errno == EINTR);
  if (ready < 0) return unexpected(ErrorCode::Io, last_error_text());
  return ready > 0;
#endif
}

void set_blocking(NativeHandle handle, bool enabled) {
#if defined(_WIN32)
  u_long mode = enabled ? 0UL : 1UL;
  // `FIONBIO` 展开为 `_IOW(...)`（winsock2.h），后者在系统头里做 size_t→long 转换——
  // clang 的 `-Wsign-conversion` 会为此报错（GCC 不报）。显式先取到 long，
  // 让转换发生在我们的代码里而不是系统头内部。
  const long request = static_cast<long>(FIONBIO);
  ioctlsocket(handle, request, &mode);
#else
  const int flags = fcntl(handle, F_GETFL, 0);
  if (flags < 0) return;
  const int updated = enabled ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  fcntl(handle, F_SETFL, updated);
#endif
}

[[nodiscard]] auto resolve_address(std::string_view host, std::uint16_t port, sockaddr_in& out)
    -> bool {
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  const std::string text(host);
  if (text.empty() || text == "localhost") {
    out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return true;
  }
  if (::inet_pton(AF_INET, text.c_str(), &out.sin_addr) == 1) return true;
  return false;
}

}  // namespace

// —— TcpStream ——

TcpStream::~TcpStream() { close(); }

TcpStream::TcpStream(TcpStream&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

auto TcpStream::operator=(TcpStream&& other) noexcept -> TcpStream& {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

void TcpStream::close() noexcept {
  if (handle_ < 0) return;
  close_handle(to_handle(handle_));
  handle_ = -1;
}

auto TcpStream::write_all(std::span<const std::uint8_t> data) -> Status {
  if (handle_ < 0) return unexpected(ErrorCode::Invalid, "连接已关闭");
  std::size_t written = 0;
  while (written < data.size()) {
#if defined(_WIN32)
    const int sent = ::send(to_handle(handle_), reinterpret_cast<const char*>(data.data() + written),
                            static_cast<int>(data.size() - written), 0);
#else
    const auto sent = ::send(to_handle(handle_), data.data() + written, data.size() - written,
                             MSG_NOSIGNAL);
#endif
    if (sent <= 0) {
      if (would_block(get_socket_error())) {
        auto ready = wait_handle(to_handle(handle_), false, 5000);
        if (!ready) return forward_error(ready.error());
        if (!*ready) return unexpected(ErrorCode::Timeout, "写超时");
        continue;
      }
      return unexpected(ErrorCode::Io, std::string("发送失败: ").append(last_error_text()));
    }
    written += static_cast<std::size_t>(sent);
  }
  return ok();
}

auto TcpStream::write_text(std::string_view text) -> Status {
  return write_all(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

auto TcpStream::read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t> {
  if (handle_ < 0) return unexpected(ErrorCode::Invalid, "连接已关闭");
  if (buffer.empty()) return std::size_t{0};
  while (true) {
#if defined(_WIN32)
    const int received = ::recv(to_handle(handle_), reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0);
#else
    const auto received = ::recv(to_handle(handle_), buffer.data(), buffer.size(), 0);
#endif
    if (received == 0) return std::size_t{0};
    if (received < 0) {
      if (would_block(get_socket_error())) {
        auto ready = wait_handle(to_handle(handle_), true, 5000);
        if (!ready) return forward_error(ready.error());
        if (!*ready) return unexpected(ErrorCode::Timeout, "读超时");
        continue;
      }
      return unexpected(ErrorCode::Io, std::string("接收失败: ").append(last_error_text()));
    }
    return static_cast<std::size_t>(received);
  }
}

auto TcpStream::read_exact(std::span<std::uint8_t> buffer) -> Status {
  std::size_t filled = 0;
  while (filled < buffer.size()) {
    auto chunk = read_some(buffer.subspan(filled));
    if (!chunk) return forward_error(chunk.error());
    if (*chunk == 0) return unexpected(ErrorCode::Io, "对端提前关闭连接");
    filled += *chunk;
  }
  return ok();
}

auto TcpStream::wait_readable(int timeout_ms) -> Result<bool> {
  if (handle_ < 0) return unexpected(ErrorCode::Invalid, "连接已关闭");
  return wait_handle(to_handle(handle_), true, timeout_ms);
}

void TcpStream::set_nonblocking(bool enabled) {
  if (handle_ >= 0) set_blocking(to_handle(handle_), !enabled);
}

void TcpStream::set_no_delay(bool enabled) {
  if (handle_ < 0) return;
  const int value = enabled ? 1 : 0;
  ::setsockopt(to_handle(handle_), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&value), static_cast<socklen_t>(sizeof(value)));
}

auto TcpStream::peer_address() const -> std::string {
  if (handle_ < 0) return {};
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  if (::getpeername(to_handle(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return {};
  }
  char text[INET_ADDRSTRLEN] = {};
  ::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
  return std::format("{}:{}", text, ntohs(address.sin_port));
}

// —— TcpListener ——

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_), host_(std::move(other.host_)) {
  other.handle_ = -1;
}

auto TcpListener::operator=(TcpListener&& other) noexcept -> TcpListener& {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    host_ = std::move(other.host_);
    other.handle_ = -1;
  }
  return *this;
}

void TcpListener::close() noexcept {
  if (handle_ < 0) return;
  close_handle(to_handle(handle_));
  handle_ = -1;
}

auto TcpListener::bind(std::string_view host, std::uint16_t port, int backlog) -> Result<TcpListener> {
  if (!startup()) return unexpected(ErrorCode::Io, "网络初始化失败");
  sockaddr_in address{};
  if (!resolve_address(host, port, address)) {
    return unexpected(ErrorCode::Invalid, std::string("无法解析地址: ").append(host));
  }
  const NativeHandle handle = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!is_valid_handle(handle)) return unexpected(ErrorCode::Io, last_error_text());

  const int reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               static_cast<socklen_t>(sizeof(reuse)));
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_handle(handle);
    return unexpected(ErrorCode::Busy, std::string("绑定失败: ").append(last_error_text()));
  }
  if (::listen(handle, backlog) != 0) {
    close_handle(handle);
    return unexpected(ErrorCode::Io, std::string("监听失败: ").append(last_error_text()));
  }

  TcpListener listener;
  listener.handle_ = from_handle(handle);
  listener.host_ = std::string(host);
  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
    listener.port_ = ntohs(bound.sin_port);
  } else {
    listener.port_ = port;
  }
  return listener;
}

auto TcpListener::accept() -> Result<TcpStream> {
  if (handle_ < 0) return unexpected(ErrorCode::Invalid, "监听器已关闭");
  while (true) {
    const NativeHandle client = ::accept(to_handle(handle_), nullptr, nullptr);
    if (is_valid_handle(client)) {
      TcpStream stream(from_handle(client));
      stream.set_no_delay(true);
      return stream;
    }
    if (would_block(get_socket_error())) {
      auto ready = wait_handle(to_handle(handle_), true, 500);
      if (!ready) return forward_error(ready.error());
      continue;
    }
    return unexpected(ErrorCode::Io, std::string("accept 失败: ").append(last_error_text()));
  }
}

auto TcpListener::wait_readable(int timeout_ms) -> Result<bool> {
  if (handle_ < 0) return unexpected(ErrorCode::Invalid, "监听器已关闭");
  return wait_handle(to_handle(handle_), true, timeout_ms);
}

void TcpListener::set_nonblocking(bool enabled) {
  if (handle_ >= 0) set_blocking(to_handle(handle_), !enabled);
}

auto connect_tcp(std::string_view host, std::uint16_t port, int timeout_ms) -> Result<TcpStream> {
  if (!startup()) return unexpected(ErrorCode::Io, "网络初始化失败");
  sockaddr_in address{};
  if (!resolve_address(host, port, address)) {
    return unexpected(ErrorCode::Invalid, std::string("无法解析地址: ").append(host));
  }
  const NativeHandle handle = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!is_valid_handle(handle)) return unexpected(ErrorCode::Io, last_error_text());
  TcpStream stream(from_handle(handle));
  stream.set_nonblocking(true);
  const int result = ::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result != 0) {
    const int error = get_socket_error();
    // POSIX 非阻塞 connect 返回 EINPROGRESS = 连接进行中而非失败
    // （Win32 对应 WSAEWOULDBLOCK，已在 would_block 内）；漏掉会让本机回环连接
    // 被误判成 Io 错误直接放弃，控制协议测试全部连不上。
#if defined(_WIN32)
    const bool pending = would_block(error);
#else
    const bool pending = would_block(error) || error == EINPROGRESS;
#endif
    if (!pending) {
      return unexpected(ErrorCode::Io, std::string("连接失败: ").append(last_error_text()));
    }
    auto ready = wait_handle(handle, false, timeout_ms);
    if (!ready) return forward_error(ready.error());
    if (!*ready) return unexpected(ErrorCode::Timeout, "连接超时");
  }
  stream.set_nonblocking(false);
  stream.set_no_delay(true);
  return stream;
}

}  // namespace st::net
