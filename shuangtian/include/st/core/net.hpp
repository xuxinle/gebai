#pragma once

/// TCP 网络（控制通道与依赖下载共用）：监听器 + 连接流。
/// 平台实现集中在 `src/core/platform_net.cpp`（POSIX socket；Windows 走 winsock 分支）。
/// 约定：所有句柄 RAII 管理，错误经 `Result` 返回；`read_some` 返回 0 表示对端关闭。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "st/core/error.hpp"

namespace st::net {

class TcpListener;

class TcpStream {
 public:
  TcpStream() noexcept = default;
  ~TcpStream();
  TcpStream(const TcpStream&) = delete;
  auto operator=(const TcpStream&) -> TcpStream& = delete;
  TcpStream(TcpStream&& other) noexcept;
  auto operator=(TcpStream&& other) noexcept -> TcpStream&;

  [[nodiscard]] auto is_valid() const noexcept -> bool { return handle_ >= 0; }
  void close() noexcept;

  [[nodiscard]] auto write_all(std::span<const std::uint8_t> data) -> Status;
  [[nodiscard]] auto write_text(std::string_view text) -> Status;
  /// 读一次（返回实际字节数；0 表示对端已关闭）。
  [[nodiscard]] auto read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t>;
  /// 读满 `buffer.size()`（不足即 EOF 报错）。
  [[nodiscard]] auto read_exact(std::span<std::uint8_t> buffer) -> Status;

  /// 等待可读（毫秒；`timeout_ms < 0` 表示无限等待）。
  [[nodiscard]] auto wait_readable(int timeout_ms) -> Result<bool>;
  void set_nonblocking(bool enabled);
  /// 关闭 Nagle（控制通道小包低延迟）。
  void set_no_delay(bool enabled);

  [[nodiscard]] auto peer_address() const -> std::string;

 private:
  friend class TcpListener;
  friend auto connect_tcp(std::string_view host, std::uint16_t port, int timeout_ms)
      -> Result<TcpStream>;
  explicit TcpStream(std::intptr_t handle) noexcept : handle_(handle) {}

  std::intptr_t handle_{-1};
};

class TcpListener {
 public:
  TcpListener() noexcept = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  auto operator=(const TcpListener&) -> TcpListener& = delete;
  TcpListener(TcpListener&& other) noexcept;
  auto operator=(TcpListener&& other) noexcept -> TcpListener&;

  /// 绑定并开始监听（`port == 0` 时由内核分配，用 `port()` 取回）。
  [[nodiscard]] static auto bind(std::string_view host, std::uint16_t port, int backlog = 32)
      -> Result<TcpListener>;

  [[nodiscard]] auto is_valid() const noexcept -> bool { return handle_ >= 0; }
  void close() noexcept;
  [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }
  [[nodiscard]] auto host() const -> std::string { return host_; }

  [[nodiscard]] auto accept() -> Result<TcpStream>;
  [[nodiscard]] auto wait_readable(int timeout_ms) -> Result<bool>;
  void set_nonblocking(bool enabled);

 private:
  std::intptr_t handle_{-1};
  std::uint16_t port_{0};
  std::string host_{};
};

/// 主动连接（客户端）。
[[nodiscard]] auto connect_tcp(std::string_view host, std::uint16_t port,
                               int timeout_ms = 2000) -> Result<TcpStream>;

}  // namespace st::net
