#pragma once

/// HTTP/1.1 服务端（自研；`DESIGN.md` §net）。
///
/// 与客户端对称的三层，**共用同一份消息模型**：
/// - 消息：`HttpRequest`/`HttpResponse`（在 `http.hpp`，不重复造）
/// - 解析：`HttpRequestParser`（增量喂数据；与 `HttpResponseParser` 同构）
/// - 执行：`HttpServer`（监听 + 接受 + 逐连接收发 + 调 handler）
///
/// 刻意**不做**的东西（做了会变成框架，而不是库）：
/// 线程池、路由表、中间件链、keep-alive 连接池、HTTP/2。连接模型是
/// 「一连接一请求一响应，然后关」（`Connection: close`）——单线程可预测，
/// 需要并发就由调用方起多个 server 或多个线程各管一个 listener。
///
/// 用法（最小服务端）：
/// ```cpp
/// auto server = st::net::http::HttpServer::bind("127.0.0.1", 8080);
/// server->set_handler([](const HttpRequest& request) {
///   return HttpResponse{ .status = 200, .body = "hello " + request.path };
/// });
/// while (running) (void)server->poll_once(200);   // 每 200 ms 回一次手
/// ```
///
/// 记录读法：`poll_once` 一次最多服务**一个**连接（不积压就不阻塞），返回值是
/// 「这一轮是否真的处理了请求」——服务端循环里据此决定要不要让出 CPU。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/core/net.hpp"
#include "st/net/http.hpp"

namespace st::net::http {

/// HTTP 请求的增量解析器（服务端方向）。
///
/// 状态机：请求行 → 头 → 体（Content-Length / chunked）。与 `HttpResponseParser`
/// 同构，差别只有两处：① 首行是 `METHOD path HTTP/1.x` 而不是状态行；
/// ② 没有「读到连接关闭定界」——请求体没有长度就是没有体（RFC 7230 §3.3.3）。
class HttpRequestParser {
 public:
  enum class State : std::uint8_t { RequestLine, Headers, BodyLength, BodyChunked, Done, Failed };

  /// 喂一段数据（返回是否已完整）。失败置 `error()` 且后续 feed 忽略。
  auto feed(std::string_view data) -> bool;
  [[nodiscard]] auto complete() const noexcept -> bool { return state_ == State::Done; }
  [[nodiscard]] auto failed() const noexcept -> bool { return state_ == State::Failed; }
  [[nodiscard]] auto error() const -> std::string { return error_; }

  /// 取走解析结果（complete 后有值）。
  [[nodiscard]] auto take() -> HttpRequest { return std::move(request_); }

  /// 头部 + 请求行的累计字节（上限用的量）。
  [[nodiscard]] auto header_bytes() const noexcept -> std::size_t { return header_bytes_; }
  /// 体上限（防炸内存；构造后可按请求类型调整）。
  void set_max_body(std::uint64_t limit) { max_body_ = limit; }
  void reset();

 private:
  auto parse_request_line() -> bool;
  auto parse_headers() -> bool;
  auto parse_body_by_length() -> bool;
  auto parse_body_chunked() -> bool;
  void fail(std::string reason) {
    state_ = State::Failed;
    error_ = std::move(reason);
  }

  State state_{State::RequestLine};
  HttpRequest request_{};
  std::string buffer_{};        // 未消费字节（跨 feed 保留）
  std::uint64_t body_expected_{0};
  std::size_t header_bytes_{0};
  std::uint64_t chunk_size_remaining_{0};
  std::string error_{};
  std::uint64_t max_body_{16ULL * 1024ULL * 1024ULL};
};

/// 把响应序列化成报文（与 `serialize_request` 对称；测试与自定义传输复用）。
///
/// 自动补齐：`Content-Length`（无则按 body 大小）、`Connection: close`、
/// `Date` 不发（避免依赖时钟格式化的坑）、`Server` 可配。
[[nodiscard]] auto serialize_response(const HttpResponse& response) -> std::string;

/// 常见状态码的短语（handler 不填 `reason` 时用它）。
[[nodiscard]] auto reason_phrase(int status) -> std::string_view;

/// HTTP 服务端：监听 + 接受 + 服务一个请求。
///
/// 生命周期：`bind` 成功即开始监听；`poll_once` 非阻塞友好（等 `timeout_ms`）。
/// handler 是可拷贝的 `std::function`（服务端要持有它，且调用方多半是 lambda）。
class HttpServer {
 public:
  /// 请求处理器：拿到请求，返回响应。**不许阻塞**（它在 poll_once 里同步跑）。
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  /// 绑定并监听（`port == 0` 由内核分配，用 `port()` 取回——测试常用）。
  [[nodiscard]] static auto bind(std::string_view host, std::uint16_t port, int backlog = 32)
      -> Result<HttpServer>;

  HttpServer() = default;
  HttpServer(HttpServer&&) noexcept = default;
  auto operator=(HttpServer&&) noexcept -> HttpServer& = default;
  HttpServer(const HttpServer&) = delete;
  auto operator=(const HttpServer&) -> HttpServer& = delete;
  ~HttpServer() = default;

  [[nodiscard]] auto valid() const noexcept -> bool { return listener_.is_valid(); }
  [[nodiscard]] auto port() const noexcept -> std::uint16_t { return listener_.port(); }
  [[nodiscard]] auto host() const -> std::string { return listener_.host(); }

  /// 处理器（不设则一律 500 并说明「未设置处理器」）。
  void set_handler(Handler handler) { handler_ = std::move(handler); }
  /// `Server:` 响应头（空串 = 不发）。
  void set_server_header(std::string name) { server_header_ = std::move(name); }
  /// 请求体上限（默认 16 MiB）。
  void set_max_body(std::uint64_t limit) { max_body_ = limit; }
  /// 单连接读写超时（默认 5000 ms；慢客户端不占死线程）。
  void set_timeout_ms(int timeout_ms) { timeout_ms_ = timeout_ms; }

  /// 等一个连接并服务一个请求。
  /// 返回 true = 真的处理了一个请求；false = 这段时间没有连接进来（**不是错误**）。
  [[nodiscard]] auto poll_once(int timeout_ms) -> Result<bool>;

  /// 循环服务直到 `stop` 返回 true（`stop` 每轮都会被调；缺省永不停止）。
  /// 返回 `Result<Unit>`：只有监听器层面的错误才失败（单连接错误不中断服务）。
  [[nodiscard]] auto run(const std::function<bool()>& stop = {}) -> Status;

  /// 关掉监听器（已在运行中的 `poll_once` 会拿到错误返回）。
  void close() noexcept { listener_.close(); }

 private:
  /// 服务一个已接受的连接：读一个请求、调 handler、写回。
  /// 单连接内部错误**不上抛**（客户端断开是常态，不该弄死服务端）——
  /// 需要观测时看 `last_error()`。
  void serve_connection(TcpStream& stream);
  /// 读满一个请求（含体）。
  [[nodiscard]] auto read_request(TcpStream& stream) -> Result<HttpRequest>;

  TcpListener listener_{};
  Handler handler_{};
  std::string server_header_{"st-net/0.1"};
  std::uint64_t max_body_{16ULL * 1024ULL * 1024ULL};
  int timeout_ms_{5000};
};

}  // namespace st::net::http
