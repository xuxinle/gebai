// 平台边界：本文件集中封装 Schannel（CONVENTIONS §2 R6 受控例外——系统安全 API 的
// 句柄与缓冲区布局转换必须在本文件完成）。
//
// TLS over TcpStream：先用 st::net::connect_tcp 建 TCP，再在其上跑 Schannel 客户端握手。
// 证书校验：SCH_CRED_AUTO_CRED_VALIDATION（系统证书库链验证）；SNI 用 host 参数。

#include "st/net/tls.hpp"

#if defined(_WIN32)

// 包含顺序（MinGW 的严格契约）：winsock2 先于 windows（其 #warning 强制）；
// Schannel 类型在 schannel.h（SCHANNEL_CRED）；SSPI 在 sspi.h；UNISP_NAME 在 security.h。
#define SECURITY_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <security.h>
#include <sspi.h>
#include <schannel.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <vector>

#include "st/core/net.hpp"

namespace st::net {
namespace {

auto client_credentials() -> CredHandle* {
  static CredHandle handle{};   // lint-allow: L8 进程级 Schannel 凭据：跨连接复用是 SSPI 的设计用法（与 log sink 同类）
  static TimeStamp expiry{};   // lint-allow: L8 同上：AcquireCredentialsHandle 的过期时间出参
  static bool ready = false;   // lint-allow: L8 同上：懒初始化完成标记
  if (!ready) {
    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    const SECURITY_STATUS acquired =
        AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W),
                                   SECPKG_CRED_OUTBOUND, nullptr, &cred, nullptr, nullptr,
                                   &handle, &expiry);
    ready = acquired == SEC_E_OK;
  }
  return ready ? &handle : nullptr;
}

auto sspi_failed(SECURITY_STATUS status) -> bool {
  // SEC_I_* 是Informational（非错误）；FAILED() 宏等价于 status < 0
  return static_cast<long>(status) < 0;
}

}  // namespace

struct TlsStream::Impl {
  TcpStream tcp{};
  CtxtHandle context{};
  bool context_valid{false};
  std::vector<std::uint8_t> leftover{};          // 解密后未消费的应用数据（明文）
  std::vector<std::uint8_t> pending_ciphertext{}; // 半条 TLS 记录（跨 read 保留）
  std::string protocol{};
  std::string peer_name{};
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

auto TlsStream::platform_available() -> bool { return client_credentials() != nullptr; }
auto TlsStream::unavailable_reason() -> std::string {
  return client_credentials() == nullptr ? "Schannel 凭据创建失败" : std::string{};
}

auto TlsStream::connect(std::string_view host, std::uint16_t port, int timeout_ms)
    -> Result<TlsStream> {
  auto tcp = connect_tcp(host, port, timeout_ms);
  if (!tcp.has_value()) {
    return Err{Error{ErrorCode::Io, std::format("TLS 连接：TCP 建立失败: {}",
                                                tcp.error().to_string())}};
  }
  CredHandle* cred = client_credentials();
  if (cred == nullptr) return Err{Error{ErrorCode::Io, "TLS 连接：Schannel 凭据不可用"}};

  auto impl = std::make_unique<Impl>();
  impl->tcp = std::move(*tcp);
  impl->peer_name = std::string(host);

  // ── 握手：InitializeSecurityContext 逐回合（token 来回送到 TCP）──
  const DWORD flags = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                      ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
                      ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM;
  const std::wstring wide_host(host.begin(), host.end());
  SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
  bool have_context = false;
  std::vector<std::uint8_t> input{};
  bool need_read = true;
  while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE) {
    if (status == SEC_E_INCOMPLETE_MESSAGE || need_read) {
      std::uint8_t chunk[16384];
      const auto got = impl->tcp.read_some(chunk);
      if (!got.has_value() || *got == 0) {
        return Err{Error{ErrorCode::Io, "TLS 连接：握手期间连接断开"}};
      }
      input.insert(input.end(), chunk, chunk + *got);
      need_read = false;
    }
    SecBuffer in_buffers[2]{};
    in_buffers[0].cbBuffer = static_cast<ULONG>(input.size());
    in_buffers[0].pvBuffer = input.data();
    in_buffers[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc in_desc{};
    in_desc.ulVersion = SECBUFFER_VERSION;
    in_desc.cBuffers = 2;
    in_desc.pBuffers = in_buffers;

    SecBuffer out_buffers[1]{};
    SecBufferDesc out_desc{};
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers = 1;
    out_desc.pBuffers = out_buffers;

    status = InitializeSecurityContextW(
        cred, have_context ? &impl->context : nullptr,
        const_cast<SEC_WCHAR*>(wide_host.c_str()), flags, 0, SECURITY_NATIVE_DREP,
        have_context ? &in_desc : nullptr, 0, &impl->context, &out_desc, nullptr, nullptr);
    have_context = true;

    if (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK) {
      if (out_buffers[0].cbBuffer > 0 && out_buffers[0].pvBuffer != nullptr) {
        const auto sent =
            impl->tcp.write_all(std::span<const std::uint8_t>(
                static_cast<const std::uint8_t*>(out_buffers[0].pvBuffer),
                out_buffers[0].cbBuffer));
        FreeContextBuffer(out_buffers[0].pvBuffer);
        if (!sent.has_value()) return Err{Error{ErrorCode::Io, "TLS 连接：发送握手数据失败"}};
      }
      // 输入未消费完（EXTRA）→ 下回合继续喂
      ULONG extra = 0;
      if (in_buffers[0].BufferType == SECBUFFER_EXTRA) extra = in_buffers[0].cbBuffer;
      if (extra > 0 && extra <= input.size()) {
        input.erase(input.begin(), input.end() - static_cast<std::ptrdiff_t>(extra));
      } else if (status == SEC_I_CONTINUE_NEEDED) {
        input.clear();
        need_read = true;
      }
      if (status == SEC_E_OK) {
        if (!input.empty()) impl->leftover = std::move(input);
        break;
      }
      continue;
    }
    if (status == SEC_E_INCOMPLETE_MESSAGE) continue;
    if (status == SEC_E_WRONG_PRINCIPAL) {
      return Err{Error{ErrorCode::Io, "TLS 连接：证书主机名不匹配"}};
    }
    break;
  }
  if (sspi_failed(status)) {
    return Err{Error{ErrorCode::Io, std::format("TLS 连接：握手失败（Schannel {:#x}）",
                             static_cast<unsigned long>(status))}};
  }
  SecPkgContext_ConnectionInfo info{};
  if (QueryContextAttributesW(&impl->context, SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK) {
    impl->protocol = info.dwProtocol & SP_PROT_TLS1_3_SERVER
                         ? "TLS 1.3"
                         : (info.dwProtocol & SP_PROT_TLS1_2_SERVER ? "TLS 1.2" : "TLS");
  }
  impl->context_valid = true;
  return TlsStream(impl.release());
}

void TlsStream::close() noexcept {
  if (impl_ == nullptr) return;
  if (impl_->context_valid) (void)DeleteSecurityContext(&impl_->context);
  impl_->tcp.close();
  delete impl_;   // lint-allow: L1 PIMPL 析构点
  impl_ = nullptr;
}

auto TlsStream::write_all(std::span<const std::uint8_t> data) -> Status {
  if (impl_ == nullptr) return Err{Error{ErrorCode::Io, "TLS 写：会话无效"}};
  SecPkgContext_StreamSizes sizes{};
  if (QueryContextAttributesW(&impl_->context, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
    return Err{Error{ErrorCode::Io, "TLS 写：取流尺寸失败"}};
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t chunk =
        std::min<std::size_t>(data.size() - offset, sizes.cbMaximumMessage);
    std::vector<std::uint8_t> message(sizes.cbHeader + chunk + sizes.cbTrailer);
    std::memcpy(message.data() + sizes.cbHeader, data.data() + offset, chunk);
    SecBuffer buffers[4]{};
    buffers[0].cbBuffer = sizes.cbHeader;
    buffers[0].pvBuffer = message.data();
    buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    buffers[1].cbBuffer = static_cast<ULONG>(chunk);
    buffers[1].pvBuffer = message.data() + sizes.cbHeader;
    buffers[1].BufferType = SECBUFFER_DATA;
    buffers[2].cbBuffer = sizes.cbTrailer;
    buffers[2].pvBuffer = message.data() + sizes.cbHeader + chunk;
    buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    buffers[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{};
    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = buffers;
    const SECURITY_STATUS encrypted = EncryptMessage(&impl_->context, 0, &desc, 0);
    if (encrypted != SEC_E_OK) {
      return Err{Error{ErrorCode::Io, std::format("TLS 写：加密失败（{:#x}）",
                               static_cast<unsigned long>(encrypted))}};
    }
    const std::size_t total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
    const auto sent = impl_->tcp.write_all(std::span<const std::uint8_t>(message.data(), total));
    if (!sent.has_value()) return sent;
    offset += chunk;
  }
  return ok();
}

auto TlsStream::read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t> {
  if (impl_ == nullptr) return Err{Error{ErrorCode::Io, "TLS 读：会话无效"}};
  if (!impl_->leftover.empty()) {
    const std::size_t take = std::min(buffer.size(), impl_->leftover.size());
    std::memcpy(buffer.data(), impl_->leftover.data(), take);
    impl_->leftover.erase(impl_->leftover.begin(),
                          impl_->leftover.begin() + static_cast<std::ptrdiff_t>(take));
    return take;
  }
  for (;;) {
    std::uint8_t chunk[16384];
    const auto got = impl_->tcp.read_some(chunk);
    if (!got.has_value()) return Err{Error{ErrorCode::Io, "TLS 读：底层失败"}};
    if (*got == 0) return std::size_t{0};
    std::vector<std::uint8_t> input(chunk, chunk + *got);
    for (;;) {
      SecBuffer buffers[4]{};
      buffers[0].cbBuffer = static_cast<ULONG>(input.size());
      buffers[0].pvBuffer = input.data();
      buffers[0].BufferType = SECBUFFER_DATA;
      for (int index = 1; index < 4; ++index) buffers[index].BufferType = SECBUFFER_EMPTY;
      SecBufferDesc desc{};
      desc.ulVersion = SECBUFFER_VERSION;
      desc.cBuffers = 4;
      desc.pBuffers = buffers;
      const SECURITY_STATUS decrypted = DecryptMessage(&impl_->context, &desc, 0, nullptr);
      if (decrypted == SEC_E_OK) {
        const std::uint8_t* data_ptr = nullptr;
        ULONG data_size = 0;
        const std::uint8_t* extra_ptr = nullptr;
        ULONG extra_size = 0;
        for (const auto& candidate : buffers) {
          if (candidate.BufferType == SECBUFFER_DATA && data_ptr == nullptr) {
            data_ptr = static_cast<const std::uint8_t*>(candidate.pvBuffer);
            data_size = candidate.cbBuffer;
          }
          if (candidate.BufferType == SECBUFFER_EXTRA) {
            extra_ptr = static_cast<const std::uint8_t*>(candidate.pvBuffer);
            extra_size = candidate.cbBuffer;
          }
        }
        if (data_ptr == nullptr || data_size == 0) {
          if (extra_ptr != nullptr && extra_size > 0) {
            input.assign(extra_ptr, extra_ptr + extra_size);
            continue;
          }
          break;
        }
        const std::size_t take = std::min<std::size_t>(buffer.size(), data_size);
        std::memcpy(buffer.data(), data_ptr, take);
        if (take < data_size) {
          impl_->leftover.assign(data_ptr + take, data_ptr + data_size);
        }
        if (extra_ptr != nullptr && extra_size > 0) {
          // 未消费密文放回 input（下一次 read_some 的解密素材）——
          // 与 leftover（明文）分开：先吐明文，密文进 leftover 处理完后仍要解。
          // 简化处理：追加到 leftover 会把密文当明文——正确做法是单独密文队列。
          // 本实现：只处理明文部分；密文留在 input，通过 recv_pending 挂起。
          impl_->pending_ciphertext.assign(extra_ptr, extra_ptr + extra_size);
        }
        return take;
      }
      if (decrypted == SEC_E_INCOMPLETE_MESSAGE) {
        if (!impl_->pending_ciphertext.empty()) {
          input.insert(input.begin(), impl_->pending_ciphertext.begin(),
                       impl_->pending_ciphertext.end());
          impl_->pending_ciphertext.clear();
        }
        break;   // 攒更多密文再试
      }
      if (decrypted == SEC_I_CONTEXT_EXPIRED) return std::size_t{0};
      return Err{Error{ErrorCode::Io, std::format("TLS 读：解密失败（{:#x}）",
                               static_cast<unsigned long>(decrypted))}};
    }
    // 密文不足：先挂起旧的，继续读 TCP
    if (!input.empty()) {
      impl_->pending_ciphertext.insert(impl_->pending_ciphertext.end(), input.begin(),
                                       input.end());
      input.clear();
    }
    if (!impl_->pending_ciphertext.empty()) {
      // 下一轮外层循环把挂起密文与新读到的合并——读更多
      std::uint8_t more[16384];
      const auto got2 = impl_->tcp.read_some(more);
      if (!got2.has_value() || *got2 == 0) {
        return Err{Error{ErrorCode::Io, "TLS 读：等待记录期间连接断开"}};
      }
      impl_->pending_ciphertext.insert(impl_->pending_ciphertext.end(), more, more + *got2);
      input = std::move(impl_->pending_ciphertext);
      impl_->pending_ciphertext.clear();
    }
  }
}

void TlsStream::set_nonblocking(bool enabled) {
  if (impl_ != nullptr) impl_->tcp.set_nonblocking(enabled);
}

auto TlsStream::negotiated_protocol() const -> std::string {
  return impl_ != nullptr ? impl_->protocol : std::string{};
}
auto TlsStream::peer_cn() const -> std::string {
  return impl_ != nullptr ? impl_->peer_name : std::string{};
}

}  // namespace st::net

#endif  // _WIN32
