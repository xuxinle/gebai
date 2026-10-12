#pragma once

/// TLS 传输层（`DESIGN.md` §net）：系统 TLS 的薄封装，接口对齐 `TcpStream`。
///
/// 平台实现（受控例外，集中在 platform_*）：
/// - Windows：**Schannel**（`platform_tls_schannel.cpp`）——系统自带，零依赖
/// - POSIX：**运行时 dlopen OpenSSL**（`platform_tls_openssl.cpp`）——不链接、
///   运行时找 libssl.so；找不到时 `connect` 返回明确错误（不强加依赖）
///
/// 与 HTTP/WS 的组合：`HttpClient`/`WsSession` 走 `Stream` 抽象（ws.hpp），
/// TlsStream 满足同一读写面——https/wss 直接叠加。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "st/core/error.hpp"

namespace st::net {

/// TLS 会话（客户端形态——本框架的网络需求是「出去连」，不做入站 TLS）。
class TlsStream {
 public:
  TlsStream() noexcept = default;
  ~TlsStream();
  TlsStream(const TlsStream&) = delete;
  auto operator=(const TlsStream&) -> TlsStream& = delete;
  TlsStream(TlsStream&& other) noexcept;
  auto operator=(TlsStream&& other) noexcept -> TlsStream&;

  /// 与 `connect_tcp` 同参（host/port/超时）+ SNI 用 host。
  /// 失败经 Result（证书校验失败/握手超时/平台无 TLS——分开报）。
  [[nodiscard]] static auto connect(std::string_view host, std::uint16_t port,
                                    int timeout_ms = 10000) -> Result<TlsStream>;

  [[nodiscard]] auto is_valid() const noexcept -> bool { return impl_ != nullptr; }
  void close() noexcept;

  [[nodiscard]] auto write_all(std::span<const std::uint8_t> data) -> Status;
  /// 读一次（0 = 对端关闭）。
  [[nodiscard]] auto read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t>;
  void set_nonblocking(bool enabled);

  /// 本会话协商出的协议/加密套件（诊断上报；拿不到为空串）。
  [[nodiscard]] auto negotiated_protocol() const -> std::string;
  /// 对端证书的主题 CN（诊断；无证书为空）。
  [[nodiscard]] auto peer_cn() const -> std::string;

  /// 平台 TLS 可用性（Schannel 恒真；POSIX 看 dlopen 是否找到 OpenSSL）。
  [[nodiscard]] static auto platform_available() -> bool;
  /// 不可用时的原因（给用户看的提示，如「安装 libssl3」）。
  [[nodiscard]] static auto unavailable_reason() -> std::string;

 private:
  struct Impl;
  explicit TlsStream(Impl* impl) noexcept : impl_(impl) {}
  Impl* impl_{nullptr};
};

}  // namespace st::net
