/// 文档示例的可编译 + 可运行验证：`docs/NETWORK.md` 里的最小示例照抄能不能跑。
///
/// 为何要有这个：文档里的代码不经过编译器就是**承诺**，而承诺会过期
/// （签名改了、头文件挪了、参数顺序变了）。本文件把示例原样搬进来，
/// 由构建强制它保持可编译；再由用例强制它**真的能跑**。
///
/// 口径与 `docs/AS_LIBRARY.md` 一致：只用 `st_core` 里的 net 头，不碰 UI/raster。

#include <array>
#include <cstdint>
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

/// 文档 §4.2 的 WS echo 服务端：服务**一个**连接后返回（示例里是死循环，这里可测）。
void ws_echo_once(TcpListener& listener) {
  auto conn = listener.accept_for(2000);
  if (!conn || !conn->has_value()) return;
  auto session = ws::accept_handshake(std::make_unique<TcpStream>(std::move(**conn)), 3000);
  if (!session) return;
  while (auto message = (*session)->read_message()) {
    if (message->opcode == ws::Opcode::Text) (void)(*session)->send_text(message->text);
    if (message->opcode == ws::Opcode::Close) break;
  }
}

}  // namespace

ST_TEST(net_doc_http_server_example_runs) {
  // ── 文档 §3 的最小服务端（逐行照抄）──
  auto server = http::HttpServer::bind("127.0.0.1", 0);   // 0 = 内核分配端口
  ST_REQUIRE(server.has_value());
  server->set_handler([](const http::HttpRequest& request) {
    http::HttpResponse response;
    response.status = request.path == "/health" ? 200 : 404;
    response.body = response.status == 200 ? "ok" : "not found";
    response.headers.set("Content-Type", "text/plain; charset=utf-8");
    return response;
  });
  const std::uint16_t port = server->port();
  ST_CHECK(port != 0);

  std::jthread serving([&server](std::stop_token token) {
    while (!token.stop_requested()) {
      if (auto tick = server->poll_once(100); !tick.has_value()) return;
    }
  });

  // 客户端侧：文档 §2 的 HttpClient
  http::HttpClient client;
  client.timeout_ms = 3000;
  auto ok_response = client.get(std::format("http://127.0.0.1:{}/health", port));
  ST_REQUIRE(ok_response.has_value());
  ST_CHECK_EQ(ok_response->status, 200);
  ST_CHECK_EQ(ok_response->body, std::string("ok"));

  auto missing = client.get(std::format("http://127.0.0.1:{}/nope", port));
  ST_REQUIRE(missing.has_value());
  ST_CHECK_EQ(missing->status, 404);

  serving.request_stop();
  serving.join();
}

ST_TEST(net_doc_ws_echo_example_runs) {
  // ── 文档 §4.2 的 WS echo 服务端 ──
  auto listener = TcpListener::bind("127.0.0.1", 0);
  ST_REQUIRE(listener.has_value());
  std::jthread echo([&listener] { ws_echo_once(*listener); });

  // 客户端侧：连 TCP → 握手 → 会话
  auto tcp = connect_tcp("127.0.0.1", listener->port(), 2000);
  ST_REQUIRE(tcp.has_value());
  ws::TcpStreamAdapter adapter(*tcp);
  const std::string key = ws::make_handshake_key();
  const std::string upgrade =
      ws::client_handshake_request("127.0.0.1", listener->port(), "/echo", key);
  ST_REQUIRE(tcp->write_text(upgrade).has_value());

  std::string reply;
  std::size_t header_end = std::string::npos;
  std::array<std::uint8_t, 1024> buffer{};
  while (header_end == std::string::npos) {
    const auto got = tcp->read_some(buffer);
    ST_REQUIRE(got.has_value());
    ST_REQUIRE(*got != 0);
    reply.append(reinterpret_cast<const char*>(buffer.data()), *got);   // lint-allow: L6 字节视图
    header_end = reply.find("\r\n\r\n");
    ST_REQUIRE(reply.size() <= 8192);
  }
  ST_CHECK(ws::validate_server_handshake(reply, key));
  // 握手多读到的字节（首帧）必须交给会话，否则帧流错位
  if (header_end + 4 < reply.size()) adapter.preload(reply.substr(header_end + 4));

  ws::WsSession session(adapter, /*client_side=*/true);
  ST_REQUIRE(session.send_text("ping-doc"));
  const auto echoed = session.read_message();
  ST_REQUIRE(echoed.has_value());
  ST_CHECK(echoed->opcode == ws::Opcode::Text);
  ST_CHECK_EQ(echoed->text, std::string("ping-doc"));

  (void)session.send_close(1000, "done");
  echo.request_stop();
  echo.join();
}
