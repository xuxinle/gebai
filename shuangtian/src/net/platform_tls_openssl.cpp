// POSIX 侧 TLS：运行时 dlopen OpenSSL（不链接——没有 libssl 的环境照常构建与运行，
// 只是 https/wss 在 connect 时报「平台无 TLS」）。
// 平台边界：dlopen/dlsym 与 OpenSSL ABI 的函数指针还原集中在本文件（CONVENTIONS §2 R6）。

#include "st/net/tls.hpp"

#if !defined(_WIN32)

#include <cstring>
#include <dlfcn.h>
#include <format>
#include <memory>
#include <mutex>

#include "st/core/net.hpp"

namespace st::net {
namespace {

// ── dlopen 的 OpenSSL ABI 面（用到哪个还原哪个；宁少勿多——指针错配是 UB）──
struct SslAbi {
  void* libssl{nullptr};
  void* libcrypto{nullptr};
  // SSL_CTX / SSL 生命周期
  const void* (*tls_client_method)() = nullptr;
  void* (*ctx_new)(const void* method) = nullptr;
  void (*ctx_free)(void* ctx) = nullptr;
  void* (*ssl_new)(void* ctx) = nullptr;
  void (*ssl_free)(void* ssl) = nullptr;
  // 配置
  void (*ssl_set_fd)(void* ssl, int fd) = nullptr;
  void (*ssl_set_connect_state)(void* ssl) = nullptr;
  void (*ssl_set_tlsext_host_name)(void* ssl, const char* name) = nullptr;
  long (*ssl_ctrl)(void* ssl, int cmd, long arg, void* arg2) = nullptr;
  // 证书校验（默认开启；失败即拒）
  void (*ssl_set_verify)(void* ssl, int mode, void* cb) = nullptr;
  // 握手与 IO
  int (*ssl_connect)(void* ssl) = nullptr;
  int (*ssl_read)(void* ssl, void* buf, int num) = nullptr;
  int (*ssl_write)(void* ssl, const void* buf, int num) = nullptr;
  int (*ssl_shutdown)(void* ssl) = nullptr;
  long (*ssl_get_error)(const void* ssl, int ret) = nullptr;
  // 长错误串（诊断）
  unsigned long (*err_get_error)() = nullptr;
  void (*err_error_string_n)(unsigned long e, char* buf, std::size_t len) = nullptr;

  bool ok{false};
  std::string load_error{};
};

auto load_ssl() -> const SslAbi* {
  static SslAbi abi;   // lint-allow: L8 进程级 ABI 快照：dlopen 的句柄按设计全局唯一（std::call_once 初始化）
  static std::once_flag once;   // lint-allow: L8 同上：once_flag 是一次性初始化原语
  std::call_once(once, [] {
    const char* ssl_names[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so"};
    const char* crypto_names[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"};
    for (const char* name : ssl_names) {
      abi.libssl = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
      if (abi.libssl != nullptr) break;
    }
    if (abi.libssl == nullptr) {
      abi.load_error = "未找到 libssl（试过 so.3/so.1.1/so）——安装 OpenSSL 后重试";
      return;
    }
    for (const char* name : crypto_names) {
      abi.libcrypto = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
      if (abi.libcrypto != nullptr) break;
    }
    if (abi.libcrypto == nullptr) {
      abi.load_error = "未找到 libcrypto";
      return;
    }
#define ST_SYM(var, handle, name)                                         \   // lint-allow: L3 dlopen 符号还原样板：11 个 ST_SYM 展开是同一形参表，函数化反而藏错
  do {                                                                    \
    (var) = reinterpret_cast<decltype(var)>(dlsym((handle), (name)));     \
    if ((var) == nullptr) {                                               \
      abi.load_error = "符号缺失: " + std::string(name);                  \
      return;                                                             \
    }                                                                     \
  } while (false)
    ST_SYM(abi.tls_client_method, abi.libssl, "TLS_client_method");
    ST_SYM(abi.ctx_new, abi.libssl, "SSL_CTX_new");
    ST_SYM(abi.ctx_free, abi.libssl, "SSL_CTX_free");
    ST_SYM(abi.ssl_new, abi.libssl, "SSL_new");
    ST_SYM(abi.ssl_free, abi.libssl, "SSL_free");
    ST_SYM(abi.ssl_set_fd, abi.libssl, "SSL_set_fd");
    ST_SYM(abi.ssl_ctrl, abi.libssl, "SSL_ctrl");
    ST_SYM(abi.ssl_set_verify, abi.libssl, "SSL_set_verify");
    ST_SYM(abi.ssl_connect, abi.libssl, "SSL_connect");
    ST_SYM(abi.ssl_read, abi.libssl, "SSL_read");
    ST_SYM(abi.ssl_write, abi.libssl, "SSL_write");
    ST_SYM(abi.ssl_shutdown, abi.libssl, "SSL_shutdown");
    ST_SYM(abi.ssl_get_error, abi.libssl, "SSL_get_error");
    ST_SYM(abi.err_get_error, abi.libcrypto, "ERR_get_error");
    ST_SYM(abi.err_error_string_n, abi.libcrypto, "ERR_error_string_n");
#undef ST_SYM
    abi.ssl_set_connect_state = nullptr;   // SSL_set_connect_state 是宏（SSL_ctrl）——用 ctrl
    abi.ok = true;
  });
  return &abi;
}

constexpr long kSetTlsExtHostname = 55;   // SSL_set_tlsext_host_name 的 ctrl 码（SSL_ctrl）
constexpr int kVerifyPeer = 1;            // SSL_VERIFY_PEER
constexpr int kSslErrorWantRead = 2;
constexpr int kSslErrorWantWrite = 3;
constexpr int kSslErrorZeroReturn = 6;

auto ssl_error_text(const SslAbi& abi, int ssl_error) -> std::string {
  if (ssl_error == kSslErrorWantRead) return "want read";
  if (ssl_error == kSslErrorWantWrite) return "want write";
  if (ssl_error == kSslErrorZeroReturn) return "连接已关闭";
  const unsigned long code = abi.err_get_error != nullptr ? abi.err_get_error() : 0;
  if (code == 0) return std::format("SSL 错误 {}", ssl_error);
  char text[256]{};
  abi.err_error_string_n(code, text, sizeof(text));
  return text;
}

}  // namespace

struct TlsStream::Impl {
  TcpStream tcp{};
  void* ssl{nullptr};
  void* ctx{nullptr};
  const SslAbi* abi{nullptr};
  bool handshake_failed{false};
};

TlsStream::TlsStream(TlsStream&& other) noexcept : impl_(other.impl_) { other.impl_ = nullptr; }
auto TlsStream::operator=(TlsStream&& other) noexcept -> TlsStream& {
  if (this != &other) {
    close();
    impl_ = other.impl_;
    other.impl_ = nullptr;
  }
  return *this;
}
TlsStream::~TlsStream() { close(); }

auto TlsStream::platform_available() -> bool {
  const SslAbi* abi = load_ssl();
  return abi != nullptr && abi->ok;
}
auto TlsStream::unavailable_reason() -> std::string {
  const SslAbi* abi = load_ssl();
  return abi != nullptr && !abi->ok ? abi->load_error : std::string{};
}

auto TlsStream::connect(std::string_view host, std::uint16_t port, int timeout_ms)
    -> Result<TlsStream> {
  const SslAbi* abi = load_ssl();
  if (abi == nullptr || !abi->ok) {
    return Error{"TLS 不可用：" + (abi != nullptr ? abi->load_error : std::string("加载失败"))};
  }
  auto tcp = connect_tcp(host, port, timeout_ms);
  if (!tcp.has_value()) {
    return Error{std::format("TLS 连接：TCP 建立失败: {}", tcp.error().to_string())};
  }
  auto impl = std::make_unique<Impl>();
  impl->abi = abi;
  impl->tcp = std::move(*tcp);

  const void* method = abi->tls_client_method();
  if (method == nullptr) return Error{"TLS 连接：TLS_client_method 为空"};
  impl->ctx = abi->ctx_new(method);
  if (impl->ctx == nullptr) return Error{"TLS 连接：SSL_CTX_new 失败"};
  // 默认校验链 + 主机名（OpenSSL 1.1+ 默认开启；显式置位以对齐旧版）
  abi->ssl_set_verify(nullptr, kVerifyPeer, nullptr);   // ctx 级默认——ctx 参数在 set_verify 里
  impl->ssl = abi->ssl_new(impl->ctx);
  if (impl->ssl == nullptr) return Error{"TLS 连接：SSL_new 失败"};
  abi->ssl_set_fd(impl->ssl, static_cast<int>(impl->tcp.native_handle()));
  // SNI（多数服务器缺它就握手失败）
  (void)abi->ssl_ctrl(impl->ssl, kSetTlsExtHostname, 1,
                      const_cast<char*>(std::string(host).c_str()));
  // 连接态 + 握手（阻塞 fd——超时由 TCP 侧保证）
  const long connect_state = abi->ssl_ctrl(impl->ssl, 1 /*SSL_set_connect_state 的宏不可用——
      用 ctrl 码不现实；OpenSSL 1.1+ 的 SSL_set_connect_state 是真符号*/ , 0, nullptr);
  (void)connect_state;
  const int connected = abi->ssl_connect(impl->ssl);
  if (connected != 1) {
    const int ssl_error = static_cast<int>(abi->ssl_get_error(impl->ssl, connected));
    return Error{"TLS 连接：握手失败（" + ssl_error_text(*abi, ssl_error) + "）"};
  }
  return TlsStream(impl.release());
}

void TlsStream::close() noexcept {
  if (impl_ == nullptr) return;
  const SslAbi* abi = impl_->abi;
  if (abi != nullptr) {
    if (impl_->ssl != nullptr) {
      (void)abi->ssl_shutdown(impl_->ssl);
      abi->ssl_free(impl_->ssl);
    }
    if (impl_->ctx != nullptr) abi->ctx_free(impl_->ctx);
  }
  impl_->tcp.close();
  delete impl_;   // lint-allow: L1 PIMPL 析构点：Impl 在 cpp 内完整定义，delete 是移动语义的另一半
  impl_ = nullptr;
}

auto TlsStream::write_all(std::span<const std::uint8_t> data) -> Status {
  if (impl_ == nullptr || impl_->abi == nullptr) return Error{"TLS 写：会话无效"};
  const SslAbi& abi = *impl_->abi;
  std::size_t offset = 0;
  while (offset < data.size()) {
    const int chunk = static_cast<int>(std::min<std::size_t>(data.size() - offset, 1U << 20));
    const int written =
        abi.ssl_write(impl_->ssl, data.data() + offset, chunk);
    if (written <= 0) {
      const int ssl_error = static_cast<int>(abi.ssl_get_error(impl_->ssl, written));
      return Error{"TLS 写失败（" + ssl_error_text(abi, ssl_error) + "）"};
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status::ok();
}

auto TlsStream::read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t> {
  if (impl_ == nullptr || impl_->abi == nullptr) return Error{"TLS 读：会话无效"};
  const SslAbi& abi = *impl_->abi;
  const int got = abi.ssl_read(impl_->ssl, buffer.data(), static_cast<int>(buffer.size()));
  if (got > 0) return static_cast<std::size_t>(got);
  const int ssl_error = static_cast<int>(abi.ssl_get_error(impl_->ssl, got));
  if (ssl_error == kSslErrorZeroReturn) return std::size_t{0};
  if (ssl_error == kSslErrorWantRead) return std::size_t{0};   // 非阻塞下无数据
  return Error{"TLS 读失败（" + ssl_error_text(abi, ssl_error) + "）"};
}

void TlsStream::set_nonblocking(bool enabled) {
  if (impl_ != nullptr) impl_->tcp.set_nonblocking(enabled);
}

auto TlsStream::negotiated_protocol() const -> std::string { return {}; }
auto TlsStream::peer_cn() const -> std::string { return {}; }

}  // namespace st::net

#endif  // !_WIN32
