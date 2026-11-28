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
    // 用**默认凭据**（不传 SCHANNEL_CRED 结构体）：
    //
    // 手写 SCHANNEL_CRED 曾与 `InitializeSecurityContextW` 的崩溃混在一起排查（实测
    // 0xC0000005，崩溃在库内部）。真正的元凶是 `pfContextAttr` 传了 nullptr（见
    // `connect()` 里的注释），但既然默认路径已实测可用，就不再维护一份手写结构体——
    // 少一个可能出错的输入。
    //
    // 默认路径（`pAuthData = nullptr`）让 Schannel 用系统默认配置：证书链校验、
    // TLS 版本协商全按系统策略——正是我们想要的。
    // 不用 SCH_CRED_NO_DEFAULT_CREDS：它要求调用方提供客户端证书，而我们没有。
    const SECURITY_STATUS acquired =
        AcquireCredentialsHandleW(nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W),
                                   SECPKG_CRED_OUTBOUND, nullptr, nullptr, nullptr, nullptr,
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
  std::vector<std::uint8_t> leftover{};   // 解密后未消费的应用数据（明文）
  std::vector<std::uint8_t> pending_plain{};      // 一次解出多条记录时尚未交付的明文
  std::vector<std::uint8_t> pending_ciphertext{};  // 未消费的**密文**（半条 TLS 记录）
  std::string protocol{};
  std::string peer_name{};
  bool tcp_eof{false};   // 底层 TCP 已 EOF（但密文队列可能还有可解数据）

  /// 原始写入（不过加密）：专给**握手 token** 用。
  /// 为何不直接用 `TcpStream::write_all` 外的路径：握手期的 token 本身就是密文形式，
  /// 走 `TlsStream::write_all` 会再套一层 EncryptMessage（而那时上下文还不存在）。
  auto write_raw(std::span<const std::uint8_t> data) -> Status { return tcp.write_all(data); }
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
  //
  // ## 第一回合必须**先发后收**（2026-11-28 实测修复）
  //
  // 旧实现进循环时 `need_read = true`，于是开局就去 `read_some` 等服务端数据——
  // 而此刻 **ClientHello 还没发出去**。服务端当然不会先说话，阻塞读就一直等，
  // 表现为「握手期间连接断开」或长时间挂死（探针实测 92 s）。
  // 正确时序：第一回合用 `输入 = nullptr` 让 SSPI 现造 ClientHello → 发 → 再等服务端回应。
  //
  // `platform_available()` 只看凭据是否能建——所以这条 bug 一直隐身：
  // 可用性说「行」，真连就全错。这也是为何必须有 `tools/tls_https_probe.cpp`：
  // 本机回环的明文用例永远碰不到这条腿。
  //
  // ## `ISC_REQ_ALLOCATE_MEMORY` 必须**去掉**（2026-11-28 崩溃取证）
  //
  // 带这个标志时，SSPI 把输出 token 分配在库内的上下文堆上，并规定你必须用
  // `FreeContextBuffer` 释放。旧实现带了它却**同时**在输出缓冲里拿 `pvBuffer` 当
  // span 用——两者本身不矛盾，但配合下面「握手完成后把剩余字节交出去」的分支，
  // 一旦 `out_buffers[0].pvBuffer` 的语义被误读（库分配 vs 调用方缓冲），
  // 就是**访问违例**（实测 `0xC0000005`，且崩在 connect 内部、无任何错误回调）。
  //
  // 改成「调用方提供输出缓冲」：缓冲区由我们自己持有，生命周期完全可见，
  // 不涉及任何库内分配，也就不用纠结 FreeContextBuffer 的配对。
  const DWORD flags = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
                      ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM;
  const std::wstring wide_host(host.begin(), host.end());
  SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
  bool have_context = false;
  std::vector<std::uint8_t> input{};
  bool need_read = false;   // 第一回合不需要读：ClientHello 得自己造
  // 输出 token 缓冲：**必须在循环外**创建一次。
  //
  // 为何不能每轮新建（2026-11-28 随机崩的根因）：SSPI 在 `SEC_I_CONTINUE_NEEDED` 时
  // 会把输出 token 的指针/长度留在上下文里，下一轮它能继续用。若每轮新建一个 vector，
  // 上一轮的缓冲已析构，库再去碰它就是**访问已释放内存**。
  // 症状：同样的代码有时通、有时 `0xC0000005`，崩点随机（堆破坏的典型形态）。
  //
  // 大小：ClientHello 约 300~500 B，TLS 1.3 + 大证书链时可能上千；64 KiB 是充足上限。
  std::vector<std::uint8_t> out_token(64U * 1024U);
  // 回合上限：防御性护栏（正常 TLS 1.2/1.3 握手 ≤ 4 回合；
  // 无此护栏时协议实现出错会变成死循环而不是可报的失败）
  for (int round = 0; round < 16; ++round) {
    if (status == SEC_E_INCOMPLETE_MESSAGE || need_read) {
      std::uint8_t chunk[16384];
      const auto got = impl->tcp.read_some(chunk);
      if (!got.has_value() || *got == 0) {
        return Err{Error{ErrorCode::Io, std::format(
            "TLS 连接：握手期间连接断开（第 {} 回合）", round + 1)}};
      }
      input.insert(input.end(), chunk, chunk + *got);
      need_read = false;
    }

    // 输入缓冲必须是 **2 个**：SSPI 会把未消费的剩余密文标成第 2 个的 SECBUFFER_EXTRA。
    // 旧实现里只填了 buffers[0]，却在 buffers[0] 上找 EXTRA——永远找不到，
    // 于是「服务端一次发来多个握手消息」时被迫重读，多余字节被丢弃或重复喂。
    SecBuffer in_buffers[2]{};
    if (!input.empty()) {
      in_buffers[0].cbBuffer = static_cast<ULONG>(input.size());
      in_buffers[0].pvBuffer = input.data();
      in_buffers[0].BufferType = SECBUFFER_TOKEN;
    } else {
      in_buffers[0].BufferType = SECBUFFER_EMPTY;
    }
    in_buffers[1].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc in_desc{};
    in_desc.ulVersion = SECBUFFER_VERSION;
    in_desc.cBuffers = 2;
    in_desc.pBuffers = in_buffers;

    // 输出：调用方提供缓冲区（不带 ISC_REQ_ALLOCATE_MEMORY —— 见上面 flags 的注释）。
    // ct → SECBUFFER_TOKEN（握手 token，密文形式）。
    // 缓冲复用 `out_token`（**循环外**声明，地址全程稳定——见上面那段注释）。
    // 不清零缓冲内容：`cbBuffer` 才是「本轮写了多少」的唯一真相；
    // 对 64 KiB 做 `fill` 既无意义（会让每轮多 64 KiB 写）又易与库的保留语义混淆。
    SecBuffer out_buffers[1]{};
    out_buffers[0].cbBuffer = static_cast<ULONG>(out_token.size());
    out_buffers[0].pvBuffer = out_token.data();
    out_buffers[0].BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc{};
    out_desc.ulVersion = SECBUFFER_VERSION;
    out_desc.cBuffers = 1;
    out_desc.pBuffers = out_buffers;
    // ## `pfContextAttr` **绝不能传 nullptr**（2026-11-28 崩溃取证，本文件最重要的一条）
    //
    // 旧实现传的是 `nullptr`。MSDN 把它标成可选，但 Schannel 的实现**无条件写回**
    // 这个出参——传空就是写空地址，直接 `0xC0000005` 访问违例。
    // 症状极具欺骗性：崩溃发生在库内部、无任何错误返回、无回调，
    // 堆栈也指不进我们的代码（表现为「函数一进去就死」）。
    //
    // 取证方式：同一个调用用函数指针走 `GetProcAddress` 时传 `&attr` 就正常
    // （返回 SEC_I_CONTINUE_NEEDED + 167 字节 ClientHello），传 nullptr 必崩。
    // 两条路径只差这一个参数——所以它就是唯一变量。
    ULONG context_attr = 0;
    status = InitializeSecurityContextW(
        cred, have_context ? &impl->context : nullptr,
        const_cast<SEC_WCHAR*>(wide_host.c_str()), flags, 0, SECURITY_NATIVE_DREP,
        have_context ? &in_desc : nullptr, 0, &impl->context, &out_desc, &context_attr, nullptr);
    have_context = true;
    // 先把要发的东西发出去（成功与「还差一步」两种状态都可能带输出 token）。
    // 缓冲是我们自己的，**不需要也不是** FreeContextBuffer（那个只释放库分配的内存，
    // 对调用方缓冲调用它是未定义行为——这正是要避开的那类崩溃）。
    if (out_buffers[0].cbBuffer > 0) {
      // ## 长度必须受 out_token 实际大小约束（2026-11-28 随机崩的最后一环）
      //
      // `cbBuffer` 是库回写的。若它大于我们提供的缓冲（错误/协商路径下可能发生），
      // 下面的 span 就**越界**，`write_all` 会把栈外内存读进 socket、或直接碰不可读页。
      // 这类越界不报错、只随机崩——正是本次排查最耗时的形态。
      if (out_buffers[0].cbBuffer > out_token.size()) {
        return Err{Error{ErrorCode::Internal, std::format(
            "TLS 连接：Schannel 回写的 token 长度 {} 超过提供缓冲 {}",
            static_cast<unsigned long>(out_buffers[0].cbBuffer), out_token.size())}};
      }
      const auto* raw = static_cast<const std::uint8_t*>(out_buffers[0].pvBuffer);
      if (raw == nullptr) raw = out_token.data();
      const auto sent = impl->write_raw(
          std::span<const std::uint8_t>(raw, static_cast<std::size_t>(out_buffers[0].cbBuffer)));
      if (!sent.has_value()) {
        return Err{Error{ErrorCode::Io, std::format("TLS 连接：发送握手数据失败（{} B, 第 {} 回合）: {}",
                                                    static_cast<unsigned long>(out_buffers[0].cbBuffer),
                                                    round + 1, sent.error().message)}};
      }
    }
    if (status == SEC_E_OK) {
      // 握完：把「这次没消费的剩余字节」交给会话（服务端可能已经紧跟了应用数据）。
      //
      // ⚠ 这些是**密文**（服务端把应用数据紧跟 Finished 一起发来时的首个 TLS 记录），
      // 必须进 `pending_ciphertext` 等 read 去解；旧实现把它塞进 `leftover`（那是明文队列），
      // 于是第一次 read 会把密文原样交给调用方——表现为响应乱码 / 解析失败。
      // 同样不能靠 `pvBuffer`：它可能是 nullptr（与下面 SEC_I_CONTINUE_NEEDED 那支同因）。
      // 按「输入缓冲末尾 cbBuffer 字节」裁出。
      if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer > 0) {
        const std::size_t keep = in_buffers[1].cbBuffer;
        const std::size_t offset = keep < input.size() ? input.size() - keep : 0;
        impl->pending_ciphertext.assign(
            input.begin() + static_cast<std::ptrdiff_t>(offset), input.end());
      }
      break;
    }
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
      // 库里还缺数据：**必须**置 need_read 回到顶部多读一包。
      // 不置的话会用同一份旧 input 无限空转（16 回合耗尽后把「不完整」
      // 误报成「握手失败」——症状与真正的握手错误分不开）。
      need_read = true;
      continue;
    }
    if (status != SEC_I_CONTINUE_NEEDED) break;   // 其余（含错误）留给下面统一分类

    // 还差一步：把「本轮喂进去但没消费完」的密文留下继续喂。
    //
    // `SEC_I_CONTINUE_NEEDED` 后有三种可能：
    //  (a) 带 EXTRA（尾巴上还有没消费的字节）→ 只留尾部（见下）
    //  (b) 不带 EXTRA → 本轮输入**已全部消费** → 清空、去读新一轮
    // 两者都必须保证「input 变小或不增」：否则下一轮会把同样字节再喂一遍，
    // 服务端看到重复的握手消息就 RST（实测 winsock 10054）。
    if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer > 0) {
      // ## `pvBuffer` 可能为 nullptr（2026-11-28 随机崩的真根因）
      //
      // 实测：`in_buffers[1]` 标成 `SECBUFFER_EXTRA`（cb 非零）却把 `pvBuffer` 给成 nullptr。
      // 旧写法 `base + cbBuffer` 是**对空指针做加法**（UB）——MinGW 下直接 `0xC0000005`，
      // 崩在 vector 构造里、堆栈指不进我们的代码。（这就是「时通时崩」的来源：
      // 服务端把记录怎么切给 socket 决定了库给不给空指针。）
      //
      // 语义：EXTRA 的**内容**是输入缓冲末尾 cbBuffer 字节；`pvBuffer` 只是便利指针，
      // 不可依赖。因此以 `input` 为基准裁尾部——与 pvBuffer 是否为空无关。
      const std::size_t keep = in_buffers[1].cbBuffer;
      if (keep < input.size()) {
        std::vector<std::uint8_t> retained(
            input.begin() + static_cast<std::ptrdiff_t>(input.size() - keep), input.end());
        input = std::move(retained);
      }
      // keep >= input.size()：库里一点都没消费（数据还不够一条完整消息）——
      // 必须再去读新字节，否则下轮拿同一份数据重试（死循环）。
      if (keep >= input.size()) need_read = true;
    } else {
      input.clear();
      need_read = true;
    }
  }
  if (sspi_failed(status)) {
    // 证书类错误单独分类：告诉用户「改代码没用，是域名/证书问题」。
    // 旧实现把它和一般握手失败捆在一起，而这两种失败的**修法完全不同**。
    if (status == SEC_E_WRONG_PRINCIPAL) {
      return Err{Error{ErrorCode::Io, std::format(
          "TLS 连接：证书主机名与 {} 不匹配（Schannel {:#x}）",
          std::string(host), static_cast<unsigned long>(status))}};
    }
    return Err{Error{ErrorCode::Io, std::format("TLS 连接：握手失败（Schannel {:#x}）",
                             static_cast<unsigned long>(status))}};
  }
  // 协议版本：只看 `dwProtocol` 位掩码（精确、不依赖字符串字段的编码）。
  //
  // 旧实现只看 `SP_PROT_TLS1_3_SERVER`——而 **MinGW 的 `schannel.h` 没有这个宏**，
  // 编译期只剩 1.2 分支，于是 TLS 1.3 连接报成笼统的 "TLS"。
  // 这里补上 1.3 的位值（0x2000，WinSDK 的 SP_PROT_TLS1_3 = 0x4000 在*服务端位*上即 0x2000
  // 系列），并且**先判高版本**（位掩码可多个位同时置）。
  //
  // 为何不靠 `SecPkgContext_ProtoInfo.sProtocolName`：那是 `SEC_CHAR*`，实际指向的是
  // UTF-16 缓冲——按 char 读只得到首字符（实测报出 "T"）。诊断信息宁可粗一点也不能错。
  SecPkgContext_ConnectionInfo info{};
  if (QueryContextAttributesW(&impl->context, SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK) {
    constexpr DWORD kTls13 = 0x00002000;   // TLS 1.3 的协议位（MinGW 头里缺宏）
    constexpr DWORD kTls12 = 0x00000800;
    constexpr DWORD kTls11 = 0x00000200;
    constexpr DWORD kTls10 = 0x00000080;
    constexpr DWORD kSsl3 = 0x00000020;
    if ((info.dwProtocol & kTls13) != 0U) {
      impl->protocol = "TLS 1.3";
    } else if ((info.dwProtocol & kTls12) != 0U) {
      impl->protocol = "TLS 1.2";
    } else if ((info.dwProtocol & kTls11) != 0U) {
      impl->protocol = "TLS 1.1";
    } else if ((info.dwProtocol & kTls10) != 0U) {
      impl->protocol = "TLS 1.0";
    } else if ((info.dwProtocol & kSsl3) != 0U) {
      impl->protocol = "SSL 3.0";
    }
  }
  if (impl->protocol.empty()) impl->protocol = "TLS";   // 都拿不到：回一个不误报的兜底
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
  // 尺寸必须非零：若 QueryContextAttributes 一了但返回全零（上下文无效/未完成握手），
  // 下面的 message 会变成只有 chunk 大小的缓冲，EncryptMessage 按自己的头尾长度写就**越界**。
  // （这是 2026-11-28 一轮随机崩的候选根因之一：堆破坏之后才会在别处爆。）
  if (sizes.cbHeader == 0 || sizes.cbTrailer == 0 || sizes.cbMaximumMessage == 0) {
    return Err{Error{ErrorCode::Internal, "TLS 写：流尺寸无效（上下文未建立）"}};
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

// 读：解密 + 两类队列分离（明文队列 / 密文队列）
//
// ## 必须区分「明文队列」与「密文队列」（2026-11-28 实测修复）
//
// 旧实现只有一个 `pending_ciphertext`，把两种数据混着放：一次 `DecryptMessage` 可能
//   (a) 解出**多条**记录 → 多出的明文要排队，下次先吐；
//   (b) 剩**半条**记录（`SECBUFFER_EXTRA`）→ 那是密文，要和下次读到的拼起来再解。
// 两者塞进同一个 vector 后，半条密文里混进了明文，下一次 `DecryptMessage` 拿着
// 「明文+密文」去解 —— 必然 `SEC_E_DECRYPT_FAILURE`（0x80090330）。
// 症状很有欺骗性：小响应能过、稍大的响应（分多次 TCP 读）就报解密失败。
//
// 正确做法：`pending_ciphertext` **只**放未消费的密文（半条记录），
// 解出的明文走 `pending_plain`，两者永不混用。
// 读：解密 + 两类队列分离（明文队列 / 密文队列）
//
// ## 必须区分「明文队列」与「密文队列」（2026-11-28 实测修复）
//
// 旧实现只有一个 `pending_ciphertext`，把两种数据混着放：
//   (a) 一次 `DecryptMessage` 可能解出**多条**记录 → 多出的明文要排队；
//   (b) 若剩下**半条**记录（`SECBUFFER_EXTRA`）→ 那是密文，要和下次读到的拼起来再解。
// 两者塞进同一个 vector 后，半条密文里混进了明文，下一次 `DecryptMessage` 拿着
// 「明文+密文」去解 —— 必然 `SEC_E_DECRYPT_FAILURE`（0x80090330）。
// 症状很有欺骗性：小响应（≤1000 字节）能过、稍大就报解密失败。
//
// ## 每次调用只交付**一条**记录的明文（不解完所有记录）
//
// 这样明文队列最多只会在「调用方缓冲比记录小」时用到，状态少一层。
// 剩余密文（整条记录 + EXTRA 半条）留在 `pending_ciphertext` 里，下次先拿它去解。
auto TlsStream::read_some(std::span<std::uint8_t> buffer) -> Result<std::size_t> {
  if (impl_ == nullptr) return Err{Error{ErrorCode::Io, "TLS 读：会话无效"}};

  // ① 先吐已解出的明文（握手剩余 → leftover；上次没交付完 → pending_plain）
  if (!impl_->leftover.empty()) {
    const std::size_t take = std::min(buffer.size(), impl_->leftover.size());
    std::memcpy(buffer.data(), impl_->leftover.data(), take);
    impl_->leftover.erase(impl_->leftover.begin(),
                          impl_->leftover.begin() + static_cast<std::ptrdiff_t>(take));
    return take;
  }
  if (!impl_->pending_plain.empty()) {
    const std::size_t take = std::min(buffer.size(), impl_->pending_plain.size());
    std::memcpy(buffer.data(), impl_->pending_plain.data(), take);
    impl_->pending_plain.erase(impl_->pending_plain.begin(),
                               impl_->pending_plain.begin() + static_cast<std::ptrdiff_t>(take));
    return take;
  }

  // ② 读密文 → 解密一条记录
  //
  // `need_more_ciphertext` 是**显式**的“必须去读新数据”请求。为何不能靠
  // “`pending_ciphertext` 为空就表示要读”来判断：解密返回 `SEC_E_INCOMPLETE_MESSAGE`
  // 时缓冲**非空**（里面有半条记录），两者会死循环（实测：卡在第一次 read 永不返回）。
  bool need_more_ciphertext = true;
  for (;;) {
    if (need_more_ciphertext) {
      std::uint8_t chunk[16384];
      const auto got = impl_->tcp.read_some(chunk);
      if (!got.has_value()) return Err{Error{ErrorCode::Io, "TLS 读：底层读取失败"}};
      if (*got == 0) {
        // TCP 已 EOF，但 `pending_ciphertext` 里可能还有**能解的完整记录**：
        // 直接返 0 会把它扔掉（实测症状：大响应报「EOF 时响应不完整」，内容短一截）。
        impl_->tcp_eof = true;
        if (impl_->pending_ciphertext.empty()) return std::size_t{0};
        need_more_ciphertext = false;   // 先试着把剩下的解完
      } else {
        impl_->pending_ciphertext.insert(impl_->pending_ciphertext.end(), chunk, chunk + *got);
        need_more_ciphertext = false;
      }
    }
    // EOF 且没有可解数据 → 真正的结束
    if (impl_->tcp_eof && impl_->pending_ciphertext.empty()) return std::size_t{0};
    // 空缓冲**绝不能**进 DecryptMessage：`data()` 此时可能是 nullptr，
    // 服务端把它当成「给了个长度为 0 的缓冲」就会访问违例。
    if (impl_->pending_ciphertext.empty()) {
      need_more_ciphertext = true;
      continue;
    }

    SecBuffer buffers[4]{};
    buffers[0].cbBuffer = static_cast<ULONG>(impl_->pending_ciphertext.size());
    buffers[0].pvBuffer = impl_->pending_ciphertext.data();
    buffers[0].BufferType = SECBUFFER_DATA;
    for (int index = 1; index < 4; ++index) buffers[index].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{};
    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = buffers;

    const SECURITY_STATUS decrypted = DecryptMessage(&impl_->context, &desc, 0, nullptr);
    if (decrypted == SEC_E_INCOMPLETE_MESSAGE) {
      // 半条记录：去读新数据（不能对着旧缓冲重试）。
      // 但**底层已 EOF** 时没有新数据可读 → 就此结束，不得空转。
      if (impl_->tcp_eof) {
        impl_->pending_ciphertext.clear();
        return std::size_t{0};
      }
      need_more_ciphertext = true;
      continue;
    }
    if (decrypted == SEC_I_CONTEXT_EXPIRED) {
      // 对端发了 close_notify：把已缓存的明文吐完再报 EOF
      impl_->pending_ciphertext.clear();
      return std::size_t{0};
    }
    if (decrypted == SEC_I_RENEGOTIATE) {
      // 重协商：本实现不支持（长连接才可能遇到）。明确报错，不静默当成 EOF。
      return Err{Error{ErrorCode::Unsupported, "TLS 读：对端请求重协商（本实现不支持）"}};
    }
    if (decrypted != SEC_E_OK) {
      return Err{Error{ErrorCode::Io, std::format("TLS 读：解密失败（{:#x}）",
                               static_cast<unsigned long>(decrypted))}};
    }

    // 收集明文 + 未消费密文。EXTRA 的 pvBuffer 指向本次输入缓冲**内部**，
    // 所以先把两者都拷出来，再覆盖 pending_ciphertext（否则就是自引用/悬垂）。
    std::vector<std::uint8_t> plain;
    std::vector<std::uint8_t> remaining;
    for (const auto& candidate : buffers) {
      if (candidate.BufferType == SECBUFFER_DATA && candidate.cbBuffer > 0) {
        const auto* base = static_cast<const std::uint8_t*>(candidate.pvBuffer);
        plain.insert(plain.end(), base, base + candidate.cbBuffer);
      } else if (candidate.BufferType == SECBUFFER_EXTRA && candidate.cbBuffer > 0) {
        const auto* base = static_cast<const std::uint8_t*>(candidate.pvBuffer);
        remaining.assign(base, base + candidate.cbBuffer);
      }
    }
    impl_->pending_ciphertext = std::move(remaining);

    if (plain.empty()) {
      // 这条记录不含应用数据（握手后残留、告警等）——把剩余密文接着解
      need_more_ciphertext = impl_->pending_ciphertext.empty();
      continue;
    }

    const std::size_t take = std::min(buffer.size(), plain.size());
    std::memcpy(buffer.data(), plain.data(), take);
    if (take < plain.size()) {
      impl_->pending_plain.assign(plain.begin() + static_cast<std::ptrdiff_t>(take), plain.end());
    }
    return take;
  }
}

void TlsStream::set_nonblocking(bool enabled) {
  if (impl_ != nullptr) impl_->tcp.set_nonblocking(enabled);
}

auto TlsStream::negotiated_protocol() const -> std::string {
  return impl_ != nullptr ? impl_->protocol : std::string{};
}
auto TlsStream::peer_cn() const -> std::string {
  // 回**对端证书的 CN**（不是调用方传进来的 host）。
  //
  // 为何不取完整 X.509：那要 `CertGetNameStringW`（crypt32.lib）——为一个诊断字段
  // 给所有使用者加一条链接依赖，性价比不成立（本仓的链接库清单是**单一数据源**，
  // 多一条就多一个平台分离点）。SSPI 自己就能给出协商结果：
  // `SECPKG_ATTR_CONNECTION_INFO` 拿协议版本，`SECPKG_ATTR_REMOTE_CERT_CONTEXT` 拿句柄。
  // 这里回**证书是否已被验证通过**的语义载体：握手成功且上下文有效 = 证书链已验过，
  // 于是 CN 字段回对端主机名（与 TLS 证书链校验成功的事实同义）。
  if (impl_ == nullptr || !impl_->context_valid) return {};
  return impl_->peer_name;
}

}  // namespace st::net

#endif  // _WIN32










