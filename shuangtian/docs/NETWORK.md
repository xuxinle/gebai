# 霜天网络层速查（TCP / HTTP / WS / TLS）

> 面向「要立刻写出能跑的网络代码」的人和大模型。设计理由见 `DESIGN.md` §4.10；
> 本页只讲**怎么用**、**坑在哪**。
>
> 全部零第三方依赖。所有失败经 `st::Result`（`std::expected`）返回，**不抛异常**。

## 0. 三十秒选型

| 你要做的事 | 用 |
|---|---|
| 连 TCP 服务端 | `st::net::connect_tcp(host, port)` |
| 开 TCP 服务端 | `st::net::TcpListener::bind(host, port)` + `accept_for(ms)` |
| 发 HTTP 请求 | `st::net::http::HttpClient` |
| 写 HTTP 服务端 | `st::net::http::HttpServer` |
| 连 WebSocket | `st::net::connect_tcp` + `WsSession(client=true)`（或 `xiangqi` 里的握手法） |
| 写 WS 服务端 | `TcpListener` + `st::net::ws::accept_handshake` |
| 加 TLS | URL 用 `https://` / `wss://` —— `TlsStream` 自动接管 |

## 1. TCP

```cpp
#include "st/core/net.hpp"
using namespace st::net;

// 客户端
auto stream = connect_tcp("127.0.0.1", 8080, /*timeout_ms=*/2000);
if (!stream) { /* stream.error().to_string() */ }
auto wrote = stream->write_text("hello");
std::array<std::uint8_t, 4096> buf{};
auto got = stream->read_some(buf);     // *got == 0 表示对端关闭
```

```cpp
// 服务端主循环（唯一推荐的写法）
auto listener = TcpListener::bind("127.0.0.1", 8080);
while (!stop) {
  auto conn = listener->accept_for(200);        // 200 ms 没连接 → 空（不是错误）
  if (!conn) break;                             // 监听器坏了
  if (!conn->has_value()) continue;             // 超时：回去看 stop 标志
  serve(**conn);
}
```

**坑**：

1. `accept()` 是死阻塞的（内部 would-block 时 500 ms 一轮死循环）。服务端一律用
   `accept_for(timeout_ms)`——它是「服务端循环能看见停止标志」的唯一保证。
2. `read_some` 返回 0 = 对端关闭，**不是**错误；返回 `unexpected` 才是错误。两者都要处理。
3. 主循环里别用阻塞 `read_some` 直接读会慢的对端——先 `wait_readable(ms)`。

## 2. HTTP 客户端

```cpp
#include "st/net/http.hpp"
using namespace st::net::http;

HttpClient client;
client.timeout_ms = 5000;
client.max_redirects = 5;                      // 0 = 不跟

auto response = client.get("https://example.com/path");     // https 自动走 TLS
if (!response) { /* response.error().message */ }
response->status;                              // 200
response->headers.get("content-type");
response->body;
response->hop_chain;                           // 跳转经过的 URL（审计用）

// 其它方法
client.post(url, "application/json", R"({"a":1})");
client.request(url, Method::Put, headers, body);
```

**能力边界**：不做 cookie 罐、不做连接池（每次请求新建连接，`Connection: close`）、
不做 HTTP/2、不做 multipart 构造（要传文件自己拼体）。

## 3. HTTP 服务端

```cpp
#include "st/net/http_server.hpp"
using namespace st::net::http;

auto server = HttpServer::bind("127.0.0.1", 0);   // 0 = 内核分配端口
if (!server) { /* server.error() */ }
server->set_handler([](const HttpRequest& request) {
  HttpResponse response;
  response.status = request.path == "/health" ? 200 : 404;
  response.body = response.status == 200 ? "ok" : "not found";
  response.headers.set("Content-Type", "text/plain; charset=utf-8");
  return response;
});
std::print("监听 {}\n", server->port());

// 方式 A：自己控制节奏
while (running) {
  auto tick = server->poll_once(200);
  // true = 服务了一个请求；false = 这段时间没连接
}

// 方式 B：让它跑（stop 每轮被调，返回 true 收工）
server->run([&] { return !running; });
```

`HttpRequest` 里有什么：`method` / `path`（含查询串）/ `host` / `port` / `headers` / `body`。

**体定界的正确姿势**（这也是自己写解析器时最容易错的一条）：

| 请求头 | 体 |
|---|---|
| 有 `Content-Length: N` | 读 N 字节 |
| `Transfer-Encoding: chunked` | 按块读，`0\r\n\r\n` 结束 |
| 两者都没有 | **没有体**（不是「读到关闭」——请求方向读到关闭会把连接挂到超时）|

**不做什么**（有意）：线程池、路由表、中间件、keep-alive 连接池、HTTP/2。
要并发就自己起线程，每线程一个 listener；要路由就在 handler 里按 `path` 分。

## 4. WebSocket

协议层双向通用（`st/net/ws.hpp`）：帧编解码、握手串生成与校验、`WsSession`。

### 4.1 客户端

```cpp
#include "st/net/ws.hpp"
#include "st/net/ws_server.hpp"    // TcpStreamAdapter
using namespace st::net;
using namespace st::net::ws;

auto tcp = connect_tcp("127.0.0.1", 9000);
TcpStreamAdapter adapter(*tcp);
const std::string key = make_handshake_key();
(void)tcp->write_text(client_handshake_request("127.0.0.1", 9000, "/chat", key));
// …读应答直到 "\r\n\r\n"，把多余的字节 adapter.preload(剩余)…
// validate_server_handshake(reply, key) 校验
WsSession session(adapter, /*client_side=*/true);
session.send_text("你好");
auto message = session.read_message();      // 自动回 Pong、静默吞 Pong
```

### 4.2 服务端（十行）

```cpp
#include "st/net/ws_server.hpp"
using namespace st::net;
using namespace st::net::ws;

auto listener = TcpListener::bind("127.0.0.1", 9000);
for (;;) {
  auto conn = listener->accept_for(200);
  if (!conn || !conn->has_value()) continue;
  auto session = accept_handshake(std::make_unique<TcpStream>(std::move(**conn)), 3000);
  if (!session) continue;                       // 非 WS 请求已被回了 400
  while (auto message = (*session)->read_message()) {
    if (message->opcode == Opcode::Text) (*session)->send_text(message->text);
    if (message->opcode == Opcode::Close) break;
  }
}
```

`accept_handshake` 校验四项：`GET` + `Upgrade: websocket` + `Connection: Upgrade` +
`Sec-WebSocket-Key`（版本只支持 13，其它回 426）。失败尽量回 400，错误分类：

| 返回 | 含义 |
|---|---|
| `Timeout` | 对端连上但不发握手 |
| `Io` | 握手完成前对端关闭 |
| `Invalid` | 不是 WS 握手（已回 400/405）|
| `Unsupported` | `Sec-WebSocket-Version` 不是 13（已回 426）|

**`ServerSession` 不可移动/拷贝**（适配器持 `TcpStream&`），要带走就用
`std::unique_ptr<ServerSession>`——`accept_handshake` 返回的就是它。

## 5. TLS（**已实测，含六条真实缺陷的代价**）

- 客户端：URL 用 `https://` / `wss://` 即可，`TlsStream` 自动接管（SNI 用 host）。
- `TlsStream::platform_available()`：Windows 恒真（Schannel）；POSIX 看能否
  `dlopen` 到 OpenSSL——**不链接**，找不到时给明确原因（`unavailable_reason()`）。
- 只做出站 TLS。**入站（TLS 服务端）未实现**——需要时按同一 `platform_*` 模式补。

### 5.1 怎么验证它真能用（**单测验证不了这条腿**）

`tests/net_*_test.cpp` 全走本机明文回环（CONVENTIONS §7 不许依赖外网）——
那是正确做法，但它**证明不了 TLS**。于是有独立的探针：

```
st build tls_probe --profile dev
./build/dev/bin/tls_probe.exe
```

13 项：裸 TLS 握手 / 协商协议 / 对端证书 / TLS 上搬 HTTP / `HttpClient` 走 https /
**16000 字节大响应**（逼出多记录与半记录两条解密路径）/ WSS 回环。

> 为何大响应那一项必须留：小响应（≤1000 B）所有路径都绿，16 KB 才走到
> 「cross-read 解密」「明文/密文队列」这些分支。删了它，回归会静默回来。

### 5.2 实现者必须知道的六条（都是 2026-11-28 实测踩出来的）

| 坑 | 现象 | 正确做法 |
|---|---|---|
| 握手开局先读 | 挂死 92 s 或“握手期间连接断开” | 第一回合**先发** ClientHello（输入传 `nullptr`），再等服务端回应 |
| `pfContextAttr` 传 `nullptr` | 一进库就 `0xC0000005` | **必须**传有效 `ULONG*`——MSDN 说可选，Schannel 实现无条件写回 |
| `SECBUFFER_EXTRA.pvBuffer` 当非空用 | 随机崩，堆栈指向库内部 | 它**可能是 nullptr**；按「`input` 末尾 cbBuffer 字节」裁剪，不做空指针算术 |
| `SEC_I_CONTINUE_NEEDED` 且 `keep >= input.size()` | 服务端 RST（winsock 10054） | 库里一点未消费 ⇒ **必须去读新数据**，否则下轮重发同一份握手字节 |
| `ISC_REQ_ALLOCATE_MEMORY` + 自备缓冲 | 崩溃或泄漏 | 用调用方缓冲就别带这个标志；带了就必须 `FreeContextBuffer` |
| 明文/密文塞同一个队列 | 小响应绿、16 KB 解密失败 | 明文队列（多条记录）与密文队列（半条记录）**分开** |

另有一个「诊断失真」：`negotiated_protocol()` 曾只看 `SP_PROT_TLS1_3_SERVER`，
而 MinGW 的 `schannel.h` 无此宏 → TLS 1.3 报成笼统 `"TLS"`。现在按位掩码判并补 1.3 位值。

### 5.3 一个读路径的不可见约束

`read_some` 的 `SEC_E_INCOMPLETE_MESSAGE` 是**正常状态**（半条 TLS 记录），不是错误：
必须去读更多密文；若底层已 EOF 而缓冲里还有**完整记录**，得先解完再报 EOF——
否则调用方看到的就是「响应体莫名短一截」（实测：报 `EOF 时响应不完整`）。

## 6. 六条通用纪律

1. **错误经 `Result` 传播，不抛**。网络错误 / 协议错误 / 超时是**分开**的 `ErrorCode`——
   别把它们压成一个 bool。
2. **超时无处不在**。连接、重定向每跳、服务端每轮，都要有上限。
3. **上限无处不在**。头 64 KiB、体按需设（客户端默认 512 MiB、服务端默认 16 MiB）。
   没有上限的解析器是一条远程 OOM 通道。
4. **字节视图用 `std::span<const std::uint8_t>`**；`char` ↔ `uint8_t` 的转换只发生在
   协议编解码点，并带 `// lint-allow: L6` 说明。
5. **测试一律本机回环**（`TcpListener::bind("127.0.0.1", 0)`），不碰外网。
   端口用 0 让内核分配，别写死。见 `tests/net_server_test.cpp`。
6. **服务端测试要双向验证**：服务端线程 + 客户端主线程序列跑。只用自家客户端测自家服务端，
   会漏掉「双方同样错」那一类缺陷——`raw_exchange()` 就是为此存在的。
7. **声称支持 TLS 就必须有外网探针**。本机回环测不出 TLS 的任何真实缺陷（见 §5.1），
   而 `platform_available()` 只看凭据能不能建——它能建成功的情况下，握手代码再错也报“可用”。

## 7. 用例与探针索引

| 文件 | 覆盖 | 是否需要外网 |
|---|---|---|
| `tests/net_http_ws_test.cpp` | URL 解析、请求序列化、响应增量解析、WS 帧/握手/会话回环 | 否 |
| `tests/net_server_test.cpp` | HTTP 请求解析器、响应序列化、HTTP 服务端回环、`accept_for` 超时、WS 服务端握手与 echo | 否 |
| `tests/net_doc_examples_test.cpp` | **把 `docs/NETWORK.md` 的示例原样编译并跑**（文档即承诺，不编就过期） | 否 |
| `tests/net_hostname_resolve_test.cpp` | 域名解析 | 否 |
| `tools/tls_https_probe.cpp` | 裸 TLS / 协议版本 / 证书 / HTTPS GET / **16 KB 大响应** / WSS 回环 | **是** |

