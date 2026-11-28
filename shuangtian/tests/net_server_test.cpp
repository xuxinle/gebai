/// net 服务端单测：HTTP 服务端（请求解析器 / 响应序列化 / 回环服务）+ WS 服务端握手与会话。
/// 全部走本机回环（`TcpListener` + 127.0.0.1），不依赖外网（CONVENTIONS §7）。
///
/// 线程模型：服务端在 `std::jthread` 里跑 `poll_once` 循环，客户端在主线程同步发请求——
/// 这是**最基本的双向验证**（只测其中一侧的用例会漏掉「双方都以为自己是对的」那类缺陷）。

#include <array>
#include <chrono>
#include <format>
#include <string>
#include <thread>

#include "st/core/net.hpp"
#include "st/net/http.hpp"
#include "st/net/http_server.hpp"
#include "st/net/ws.hpp"
#include "st/net/ws_server.hpp"
#include "st/test/test.hpp"

namespace {

using namespace st::net;
using namespace st::net::http;
using namespace st::net::ws;

/// 客户端：打开一条明文 TCP 连接并发一段原始报文，读到对端关闭为止。
/// 用原始报文而不是 `HttpClient`——服务端要能被**任意**字节流驱动，
/// 用自家客户端测自家服务端只会暴露「双方同样错」的那部分。
[[nodiscard]] auto raw_exchange(std::uint16_t port, std::string_view wire) -> std::string {
  auto stream = connect_tcp("127.0.0.1", port, 2000);
  if (!stream.has_value() || !stream->is_valid()) return {};
  (void)stream->write_all(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));   // lint-allow: L6 测试报文字节视图
  std::string received;
  std::array<std::uint8_t, 4096> buffer{};
  for (;;) {
    auto got = stream->read_some(buffer);
    if (!got.has_value() || *got == 0) break;
    received.append(reinterpret_cast<const char*>(buffer.data()), *got);   // lint-allow: L6 同上
    if (received.size() > (1U << 20U)) break;   // 防用例挂死
  }
  return received;
}

/// 服务端后台线程：服务 `count` 个请求后退出（用例能确定地等它结束）。
struct ServerRunner {
  std::jthread thread{};

  ServerRunner(HttpServer& server, int count) {
    thread = std::jthread([&server, count](std::stop_token token) {
      int served = 0;
      while (served < count && !token.stop_requested()) {
        auto tick = server.poll_once(100);
        if (tick.has_value() && *tick) ++served;
      }
    });
  }
  ~ServerRunner() {
    if (thread.joinable()) {
      thread.request_stop();
      thread.join();
    }
  }
};

[[nodiscard]] auto find_header(std::string_view response, std::string_view name) -> std::string {
  const std::string needle = std::format("\r\n{}: ", name);
  if (const auto at = response.find(needle); at != std::string_view::npos) {
    const auto start = at + needle.size();
    const auto end = response.find("\r\n", start);
    return std::string(response.substr(start, end - start));
  }
  // 首行之后紧跟的情况（第一个头）
  const std::string first = std::format("{}: ", name);
  if (response.starts_with("HTTP/1.1")) {
    if (const auto at2 = response.find(first); at2 != std::string_view::npos) {
      const auto start = at2 + first.size();
      const auto end = response.find("\r\n", start);
      return std::string(response.substr(start, end - start));
    }
  }
  return {};
}

}  // namespace

// ── HTTP 请求解析器 ──────────────────────────────────────────────────────

ST_TEST(net_http_request_parser_basics) {
  HttpRequestParser parser;
  ST_CHECK(parser.feed("GET /hello?x=1 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n"));
  ST_REQUIRE(parser.complete());
  const HttpRequest request = parser.take();
  ST_CHECK(request.method == Method::Get);
  ST_CHECK_EQ(request.path, std::string("/hello?x=1"));
  ST_CHECK_EQ(request.host, std::string("localhost"));
  ST_CHECK_EQ(request.port, std::uint16_t{8080});
  ST_CHECK(request.body.empty());
}

ST_TEST(net_http_request_parser_body_and_chunked) {
  // Content-Length 体：分三段喂（增量性的极端测试）
  {
    HttpRequestParser parser;
    const std::string wire = "POST /u HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world";
    ST_CHECK(!parser.feed(wire.substr(0, 30)));
    ST_CHECK(!parser.feed(wire.substr(30, 5)));
    ST_CHECK(parser.feed(wire.substr(35)));
    ST_REQUIRE(parser.complete());
    const HttpRequest request = parser.take();
    ST_CHECK(request.method == Method::Post);
    ST_CHECK_EQ(request.body, std::string("hello world"));
  }
  // chunked 体
  {
    HttpRequestParser parser;
    const std::string wire =
        "PUT /u HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    ST_CHECK(parser.feed(wire));
    ST_REQUIRE(parser.complete());
    ST_CHECK_EQ(parser.take().body, std::string("hello world"));
  }
}

ST_TEST(net_http_request_parser_rejects_garbage) {
  // 非 HTTP/1.x
  {
    HttpRequestParser parser;
    ST_CHECK(!parser.feed("GET / HTTP/2.0\r\n\r\n"));
    ST_CHECK(parser.failed());
    ST_CHECK(!parser.error().empty());
  }
  // 不支持的方法（501 的入口）
  {
    HttpRequestParser parser;
    (void)parser.feed("BREW /pot HTTP/1.1\r\n\r\n");
    ST_CHECK(parser.failed());
    ST_CHECK(parser.error().find("方法") != std::string::npos);
  }
  // 无 CL 无 chunked = 无体（**不能**读关闭定界，否则连接挂到超时）
  {
    HttpRequestParser parser;
    ST_CHECK(parser.feed("GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    ST_REQUIRE(parser.complete());
    ST_CHECK(parser.take().body.empty());
  }
  // 体超上限
  {
    HttpRequestParser parser;
    parser.set_max_body(4);
    ST_CHECK(!parser.feed("POST / HTTP/1.1\r\nContent-Length: 99\r\n\r\n"));
    ST_CHECK(parser.failed());
  }
}

// ── HTTP 响应序列化 ──────────────────────────────────────────────────────

ST_TEST(net_http_response_serialize) {
  HttpResponse response;
  response.status = 200;
  response.body = "hello";
  const std::string wire = serialize_response(response);
  ST_CHECK(wire.starts_with("HTTP/1.1 200 OK\r\n"));
  ST_CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
  ST_CHECK(wire.find("Connection: close\r\n") != std::string::npos);
  ST_CHECK(wire.ends_with("\r\n\r\nhello"));

  // 204 无体：不带 Content-Length（RFC 要求）
  HttpResponse empty;
  empty.status = 204;
  const std::string wire204 = serialize_response(empty);
  ST_CHECK(wire204.find("Content-Length") == std::string::npos);
  ST_CHECK(wire204.ends_with("\r\n\r\n"));
}

// ── HTTP 服务端回环（真 TCP）─────────────────────────────────────────────

ST_TEST(net_http_server_serves_get) {
  auto server = HttpServer::bind("127.0.0.1", 0);
  ST_REQUIRE(server.has_value());
  ST_CHECK(server->valid());
  ST_CHECK(server->port() != 0);
  server->set_handler([](const HttpRequest& request) {
    HttpResponse response;
    response.status = 200;
    response.body = "path=" + request.path;
    return response;
  });
  ServerRunner runner(*server, 1);

  const std::string got = raw_exchange(server->port(),
                                       "GET /echo/42 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
  ST_CHECK(got.starts_with("HTTP/1.1 200 OK\r\n"));
  ST_CHECK(got.ends_with("path=/echo/42"));
  ST_CHECK_EQ(find_header(got, "Content-Length"), std::string("13"));   // "path=/echo/42"
}

ST_TEST(net_http_server_serves_post_body) {
  auto server = HttpServer::bind("127.0.0.1", 0);
  ST_REQUIRE(server.has_value());
  server->set_handler([](const HttpRequest& request) {
    HttpResponse response;
    response.status = 201;
    response.body = "got:" + request.body;
    return response;
  });
  ServerRunner runner(*server, 1);

  const std::string got = raw_exchange(
      server->port(), std::string("POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello"));
  ST_CHECK(got.starts_with("HTTP/1.1 201 Created\r\n"));
  ST_CHECK(got.ends_with("got:hello"));
}

ST_TEST(net_http_server_404_and_chunked_request) {
  auto server = HttpServer::bind("127.0.0.1", 0);
  ST_REQUIRE(server.has_value());
  // 按路径分流：/chunk 回 200 回显体，其它 404
  server->set_handler([](const HttpRequest& request) {
    HttpResponse response;
    if (request.path == "/chunk") {
      response.status = 200;
      response.body = request.body;
    } else {
      response.status = 404;
      response.body = "nope";
    }
    return response;
  });
  ServerRunner runner(*server, 2);

  const std::string not_found =
      raw_exchange(server->port(), "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  ST_CHECK(not_found.starts_with("HTTP/1.1 404 Not Found\r\n"));

  const std::string chunked = raw_exchange(
      server->port(),
      "POST /chunk HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nabc\r\n0\r\n\r\n");
  ST_CHECK(chunked.starts_with("HTTP/1.1 200 OK\r\n"));
  ST_CHECK(chunked.ends_with("abc"));
}

ST_TEST(net_http_server_timeout_returns_false) {
  auto server = HttpServer::bind("127.0.0.1", 0);
  ST_REQUIRE(server.has_value());
  // 没有连接进来：poll_once 到期返回 false（不是错误）——服务端循环靠这条不空转
  const auto start = std::chrono::steady_clock::now();
  const auto tick = server->poll_once(60);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ST_REQUIRE(tick.has_value());
  ST_CHECK(!*tick);
  ST_CHECK(elapsed >= std::chrono::milliseconds(40));
}

ST_TEST(net_http_server_accept_for_returns_empty_on_timeout) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ST_REQUIRE(listener.has_value());
  const auto start = std::chrono::steady_clock::now();
  const auto accepted = listener->accept_for(50);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ST_REQUIRE(accepted.has_value());
  ST_CHECK(!accepted->has_value());   // 超时 = 空，不报错
  ST_CHECK(elapsed >= std::chrono::milliseconds(30));
}

// ── WebSocket 服务端握手 ────────────────────────────────────────────────

ST_TEST(net_ws_server_handshake_and_echo) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ST_REQUIRE(listener.has_value());
  const std::uint16_t port = listener->port();

  // 服务端线程：握手 → 收一条 → 回一条 → 会话结束
  std::string echoed{};
  std::jthread server_thread([&]() {
    auto accepted = listener->accept_for(2000);
    if (!accepted.has_value() || !accepted->has_value()) return;
    auto stream = std::make_unique<TcpStream>(std::move(**accepted));
    auto session = accept_handshake(std::move(stream), 2000);
    if (!session.has_value()) return;
    auto message = (*session)->read_message();
    if (!message.has_value()) return;
    echoed = message->text;
    (void)(*session)->send_text("echo:" + message->text);
    (void)(*session)->send_close(1000, "bye");
  });

  // 客户端：明文连接 + 手写握手 + 发一帧文本（复用协议层生成帧）
  auto client = connect_tcp("127.0.0.1", port, 2000);
  ST_REQUIRE(client.has_value());
  const std::string key = make_handshake_key();
  const std::string request = client_handshake_request("127.0.0.1", port, "/chat", key);
  ST_REQUIRE(client->write_all(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(request.data()), request.size())));   // lint-allow: L6 测试报文字节视图

  // 读 101 应答
  std::string reply;
  std::array<std::uint8_t, 1024> buffer{};
  while (reply.find("\r\n\r\n") == std::string::npos) {
    const auto got = client->read_some(buffer);
    ST_REQUIRE(got.has_value());
    if (*got == 0) break;
    reply.append(reinterpret_cast<const char*>(buffer.data()), *got);   // lint-allow: L6 测试字节视图
  }
  ST_CHECK(validate_server_handshake(reply, key));

  // 发一帧文本（客户端方向必须掩码——frame_encode 用 masked=true）
  const auto frame = frame_encode(
      Frame{.fin = true, .opcode = Opcode::Text, .payload = {'h', 'i'}}, 0x01020304U, true);
  ST_REQUIRE(client->write_all(frame));

  // 收 echo（服务器帧不掩码）+ close
  std::string frames;
  for (;;) {
    const auto got = client->read_some(buffer);
    ST_REQUIRE(got.has_value());
    if (*got == 0) break;
    frames.append(reinterpret_cast<const char*>(buffer.data()), *got);   // lint-allow: L6 同上
    if (frames.size() > 4096) break;
  }
  server_thread.request_stop();
  server_thread.join();

  ST_CHECK_EQ(echoed, std::string("hi"));
  FrameDecoder decoder;
  const auto decoded = decoder.feed(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size()));   // lint-allow: L6 测试字节视图
  ST_REQUIRE(decoded.size() >= 1);
  ST_CHECK(decoded[0].opcode == Opcode::Text);
  ST_CHECK_EQ(std::string(decoded[0].payload.begin(), decoded[0].payload.end()),
              std::string("echo:hi"));
}

ST_TEST(net_ws_server_rejects_plain_http) {
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ST_REQUIRE(listener.has_value());
  const std::uint16_t port = listener->port();

  std::string failure{};
  std::jthread server_thread([&]() {
    auto accepted = listener->accept_for(2000);
    if (!accepted.has_value() || !accepted->has_value()) return;
    auto stream = std::make_unique<TcpStream>(std::move(**accepted));
    auto session = accept_handshake(std::move(stream), 1000);
    if (!session.has_value()) failure = session.error().message;
  });

  const std::string got = raw_exchange(port, "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
  server_thread.request_stop();
  server_thread.join();

  ST_CHECK(!failure.empty());
  ST_CHECK(got.starts_with("HTTP/1.1 400"));
}
