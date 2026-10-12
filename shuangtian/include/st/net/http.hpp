#pragma once

/// HTTP/1.1 客户端（自研；`DESIGN.md` §net）：请求/响应模型 + 解析器 + 传输执行。
///
/// 分层（与 WebSocket/TLS 共用的底座）：
/// - 消息模型：`HttpRequest`/`HttpResponse`（头/体/状态；体上限可配）
/// - 解析器：`HttpResponseParser`（增量喂数据；Content-Length 与 chunked 两种定界）
/// - 传输执行：`HttpClient`（明文走 `TcpStream`；HTTPS 见 `tls.hpp` 的 `TlsStream` 叠加）
///
/// 错误语义：一切失败经 `Result`（网络错误/协议错误/超时分开报，不静默吞）。
/// 来源说明：解析逻辑收编自 pkg 层私有实现（`platform_http.cpp`，572 行）并下沉
/// 到 net 模块——pkg 改为薄调用方，全方法支持取代原 GET-only。

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/net.hpp"

namespace st::net::http {

/// 请求方法（REST 常用集）。
enum class Method : std::uint8_t { Get, Post, Put, Delete, Head, Patch };

[[nodiscard]] constexpr auto method_name(Method method) -> std::string_view {
  switch (method) {
    case Method::Get: return "GET";
    case Method::Post: return "POST";
    case Method::Put: return "PUT";
    case Method::Delete: return "DELETE";
    case Method::Head: return "HEAD";
    case Method::Patch: return "PATCH";
  }
  return "GET";
}

/// 大小写不敏感的头集合（HTTP 语义要求；查找用小写键）。
class Headers {
 public:
  void set(std::string_view name, std::string_view value);
  /// 首个匹配（大小写不敏感；不存在 = nullopt）。
  [[nodiscard]] auto get(std::string_view name) const -> std::optional<std::string>;
  [[nodiscard]] auto has(std::string_view name) const -> bool { return get(name).has_value(); }
  /// 全部（保留插入序；序列化用）。
  [[nodiscard]] auto all() const -> const std::vector<std::pair<std::string, std::string>>& {
    return entries_;
  }
  void clear() { entries_.clear(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_{};
};

struct HttpRequest {
  Method method{Method::Get};
  std::string path{"/"};          ///< 含查询串（"?" 起）；不含 scheme/host
  std::string host{};             ///< Host 头（也从这构造）
  std::uint16_t port{80};
  Headers headers{};
  std::string body{};             ///< Post/Put 的负载（空 = 无）
  /// 头部读取上限（防恶意/损坏响应把内存吃爆；默认 64 KiB）。
  std::size_t max_header_bytes{64U * 1024U};
  /// 响应体上限（默认 512 MiB，与 pkg 旧实现同口径）。
  std::uint64_t max_body_bytes{512ULL * 1024ULL * 1024ULL};
};

struct HttpResponse {
  int status{0};                  ///< 200/404/…
  std::string reason{};           ///< 状态行短语（"OK"）
  Headers headers{};
  std::string body{};
  /// 是否被重定向跟随过（最终响应 = 最后一次）。
  bool redirected{false};
  /// 每次跳转经过的 URL（诊断/审计用；含起点）。
  std::vector<std::string> hop_chain{};

  [[nodiscard]] auto ok() const noexcept -> bool { return status >= 200 && status < 300; }
};

/// 增量响应解析器：`feed` 喂数据直到 `complete()`；错误分类返回（不抛）。
///
/// 状态机：状态行 → 头 → 体（Content-Length 或 chunked 或读到连接关闭）。
/// 一次解析一个响应（连接复用由 `HttpClient` 的调用方管理）。
class HttpResponseParser {
 public:
  enum class State : std::uint8_t { StatusLine, Headers, BodyLength, BodyChunked, Done, Failed };

  /// 喂入一段数据（返回是否已完整）。失败置 `error()` 且后续 feed 忽略。
  auto feed(std::string_view data) -> bool;
  [[nodiscard]] auto complete() const noexcept -> bool { return state_ == State::Done; }
  [[nodiscard]] auto failed() const noexcept -> bool { return state_ == State::Failed; }
  [[nodiscard]] auto error() const -> std::string { return error_; }

  /// 取走解析结果（complete 后有效）。
  [[nodiscard]] auto take() -> HttpResponse { return std::move(response_); }
  /// 头部累计字节（上限 enforcement 用）。
  [[nodiscard]] auto header_bytes() const noexcept -> std::size_t { return header_bytes_; }
  /// 还需要多少体字节（诊断；chunked/读关闭模式返回 nullopt）。
  [[nodiscard]] auto body_remaining() const -> std::optional<std::uint64_t>;
  /// 无 Content-Length 且非 chunked：读到连接关闭定界（EOF 时调用方调这个收尾）。
  auto finish_by_eof() -> bool;
  /// HEAD 请求（响应无体）。
  void set_head_only(bool value) { head_only_ = value; }
  /// 体上限（防炸内存；构造后可调）。
  void set_max_body(std::uint64_t limit) { max_body_ = limit; }

  void reset();

 private:
  [[nodiscard]] static constexpr auto max_header_default_hint() -> std::size_t {
    return 16U * 1024U;
  }
  [[nodiscard]] auto max_body_cap() const -> std::uint64_t { return max_body_; }
  auto parse_status_line() -> bool;
  auto parse_headers() -> bool;
  auto parse_body_by_length(std::string_view data) -> bool;
  auto parse_body_chunked(std::string_view data) -> bool;

  State state_{State::StatusLine};
  HttpResponse response_{};
  std::string buffer_{};          // 未消费的字节（跨 feed 保留）
  std::uint64_t body_expected_{0};
  std::uint64_t body_received_{0};
  std::size_t header_bytes_{0};
  std::string error_{};
  bool head_only_{false};         // HEAD 响应：头后即完
  bool eof_delimited_{false};     // 无长度无 chunked：读到关闭
  std::uint64_t chunk_size_remaining_{0};
  std::uint64_t max_body_{512ULL * 1024ULL * 1024ULL};
};

/// URL 解析结果（够用子集：scheme/host/port/path）。
struct Url {
  std::string scheme{"http"};
  std::string host{};
  std::uint16_t port{80};
  std::string path{"/"};
  /// 解析失败返回 nullopt（不是抛异常——URL 来自配置/外部输入）。
  [[nodiscard]] static auto parse(std::string_view url) -> std::optional<Url>;
  [[nodiscard]] auto to_string() const -> std::string;
};

/// 把请求序列化成报文（不含传输；测试与自定义传输复用同一份字节级真相）。
[[nodiscard]] auto serialize_request(const HttpRequest& request) -> std::string;

/// 极简 cookie/重定向等高级语义不进本层（调用方按 `status`/`Location` 自理；
/// `follow_redirects` 在客户端里给默认行为）。


/// HTTP 客户端（传输执行：连接 + 收发 + 重定向；明文与 TLS 一体）。
///
/// 用法：
/// ```cpp
/// HttpClient client;
/// auto response = client.get("https://example.com/path", 8000);
/// ```
/// https/wss 自动走 `TlsStream`（平台 TLS）；http 走 TcpStream。
class HttpClient {
 public:
  /// 总超时（连接+读，毫秒）；重定向每跳各计。
  int timeout_ms{8000};
  /// 重定向上限（3xx 的 Location 跟随次数；0 = 不跟）。
  int max_redirects{5};
  /// User-Agent（空 = 不发该头）。
  std::string user_agent{"st-net/0.1"};

  [[nodiscard]] auto get(const std::string& url) -> Result<HttpResponse>;
  [[nodiscard]] auto get(const std::string& url, const Headers& headers) -> Result<HttpResponse>;
  [[nodiscard]] auto post(const std::string& url, std::string_view content_type,
                          std::string_view body) -> Result<HttpResponse>;
  [[nodiscard]] auto request(const std::string& url, Method method, const Headers& headers,
                             std::string body) -> Result<HttpResponse>;

  /// 拆开的两个半步（高级调用方复用连接管理/自定义传输）：
  /// ① 执行一次请求（不跟重定向）。
  [[nodiscard]] auto fetch_once(const std::string& url, const HttpRequest& request)
      -> Result<HttpResponse>;
};

}  // namespace st::net::http
