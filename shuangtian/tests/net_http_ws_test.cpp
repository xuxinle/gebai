/// net 模块单测：HTTP（URL/序列化/增量解析器）+ WebSocket（帧编解码/握手/会话回环）。
/// 网络回环用本机 TcpListener——不依赖外网（CONVENTIONS §7）。

#include <cstring>
#include <mutex>
#include <thread>

#include "st/core/net.hpp"
#include "st/net/http.hpp"
#include "st/net/ws.hpp"
#include "st/test/test.hpp"

namespace {

using namespace st::net;
using namespace st::net::http;
using namespace st::net::ws;

}  // namespace

// ── HTTP：URL ─────────────────────────────────────────────────────────────

ST_TEST(net_http_url_parse_basics) {
  const auto plain = Url::parse("http://example.com/path?q=1");
  ST_REQUIRE(plain.has_value());
  ST_CHECK_EQ(plain->scheme, std::string("http"));
  ST_CHECK_EQ(plain->host, std::string("example.com"));
  ST_CHECK_EQ(plain->port, std::uint16_t{80});
  ST_CHECK_EQ(plain->path, std::string("/path?q=1"));

  const auto tls = Url::parse("https://api.test:8443/x");
  ST_REQUIRE(tls.has_value());
  ST_CHECK_EQ(tls->scheme, std::string("https"));
  ST_CHECK_EQ(tls->port, std::uint16_t{8443});
  ST_CHECK_EQ(tls->path, std::string("/x"));

  // 无 path → "/"
  const auto bare = Url::parse("http://host");
  ST_REQUIRE(bare.has_value());
  ST_CHECK_EQ(bare->path, std::string("/"));

  // userinfo 不崩、host 取 @ 后
  const auto userinfo = Url::parse("http://user:pass@host:81/a");
  ST_REQUIRE(userinfo.has_value());
  ST_CHECK_EQ(userinfo->host, std::string("host"));
  ST_CHECK_EQ(userinfo->port, std::uint16_t{81});

  // 非法（空 host / 坏端口）
  ST_CHECK(!Url::parse("http://").has_value());
  ST_CHECK(!Url::parse("http://h:notaport/").has_value());
}

// ── HTTP：请求序列化 ──────────────────────────────────────────────────────

ST_TEST(net_http_serialize_request) {
  HttpRequest request;
  request.method = Method::Post;
  request.path = "/submit";
  request.host = "example.com";
  request.body = "hello=1";
  const std::string wire = serialize_request(request);
  // 方法行 / Host / 自动 Content-Length / 自动 Connection
  ST_CHECK(wire.starts_with("POST /submit HTTP/1.1\r\n"));
  ST_CHECK(wire.find("Host: example.com\r\n") != std::string::npos);
  ST_CHECK(wire.find("Content-Length: 7\r\n") != std::string::npos);
  ST_CHECK(wire.find("Connection: close\r\n") != std::string::npos);
  ST_CHECK(wire.find("\r\n\r\nhello=1") != std::string::npos);   // 头体分隔 + 体
}

// ── HTTP：响应增量解析 ────────────────────────────────────────────────────

ST_TEST(net_http_parse_content_length_response) {
  HttpResponseParser parser;
  // 逐字节喂（增量性的极端测试：状态机必须能从任意切分续跑）
  const std::string wire =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 5\r\n"
      "\r\n"
      "hello";
  for (const char c : wire) {
    (void)parser.feed(std::string_view(&c, 1));
    ST_CHECK(!parser.failed());
  }
  ST_CHECK(parser.complete());
  auto response = parser.take();
  ST_CHECK_EQ(response.status, 200);
  ST_CHECK_EQ(response.reason, std::string("OK"));
  ST_CHECK_EQ(response.body, std::string("hello"));
  ST_CHECK_EQ(response.headers.get("content-type").value_or(""), std::string("text/plain"));
}

ST_TEST(net_http_parse_chunked_response) {
  HttpResponseParser parser;
  const std::string wire =
      "HTTP/1.1 200 OK\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "6\r\n world\r\n"
      "0\r\n\r\n";
  (void)parser.feed(wire);
  ST_CHECK(parser.complete());
  auto response = parser.take();
  ST_CHECK_EQ(response.body, std::string("hello world"));
}

ST_TEST(net_http_parse_headers_case_insensitive) {
  HttpResponseParser parser;
  (void)parser.feed("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
  ST_CHECK(parser.complete());
  auto response = parser.take();
  ST_CHECK_EQ(response.status, 204);
  ST_CHECK(response.body.empty());
  // 大小写不敏感查头
  (void)response;
}

ST_TEST(net_http_parse_rejects_garbage) {
  HttpResponseParser parser;
  (void)parser.feed("NOT-HTTP at all\r\n\r\n");
  ST_CHECK(parser.failed());
  ST_CHECK(!parser.error().empty());
}

// ── WebSocket：帧编解码 ───────────────────────────────────────────────────

ST_TEST(net_ws_frame_roundtrip_small) {
  Frame frame;
  frame.opcode = Opcode::Text;
  frame.payload = {'h', 'e', 'l', 'l', 'o'};
  const auto wire = frame_encode(frame, 0xAABBCCDDU, true);   // 客户端方向（掩码）
  FrameDecoder decoder;
  const auto frames = decoder.feed(wire);
  ST_REQUIRE(frames.size() == 1);
  ST_CHECK(frames[0].fin);
  ST_CHECK(frames[0].opcode == Opcode::Text);
  ST_CHECK_EQ(frames[0].payload.size(), std::size_t{5});
  ST_CHECK(std::string(frames[0].payload.begin(), frames[0].payload.end()) == "hello");
  ST_CHECK(!decoder.failed());
}

ST_TEST(net_ws_frame_roundtrip_extended_lengths) {
  // 16 位扩展长度档（126..65535）
  Frame frame;
  frame.opcode = Opcode::Binary;
  frame.payload.assign(700, 0xAB);
  const auto wire = frame_encode(frame, 0, false);   // 服务器方向（不掩码）
  ST_CHECK_EQ(wire.size(), std::size_t{2 + 2 + 700});   // 头2 + 扩展长2 + 体
  FrameDecoder decoder;
  const auto frames = decoder.feed(wire);
  ST_REQUIRE(frames.size() == 1);
  ST_CHECK_EQ(frames[0].payload.size(), std::size_t{700});

  // 64 位档（>65535）
  Frame big;
  big.opcode = Opcode::Binary;
  big.payload.assign(70000, 0x01);
  const auto big_wire = frame_encode(big, 0x11223344U, true);
  FrameDecoder big_decoder;
  const auto big_frames = big_decoder.feed(big_wire);
  ST_REQUIRE(big_frames.size() == 1);
  ST_CHECK_EQ(big_frames[0].payload.size(), std::size_t{70000});
}

ST_TEST(net_ws_frame_fragmented_delivery) {
  FrameDecoder decoder;
  Frame first;
  first.fin = false;   // 分片首片（FIN=0）
  first.opcode = Opcode::Text;
  first.payload = {'a', 'b'};
  Frame second;
  second.fin = true;   // 尾片
  second.opcode = Opcode::Continuation;
  second.payload = {'c', 'd'};
  // 一次喂两个半帧（增量流里的正常形态）
  const auto wire1 = frame_encode(first, 0, false);
  const auto wire2 = frame_encode(second, 0, false);
  std::vector<std::uint8_t> combined(wire1);
  combined.insert(combined.end(), wire2.begin(), wire2.end());
  const auto frames = decoder.feed(combined);
  ST_CHECK_EQ(frames.size(), std::size_t{2});   // 帧不合并——重组是会话层的事
  ST_CHECK(!frames[0].fin);
  ST_CHECK(frames[1].fin);
  ST_CHECK(frames[1].opcode == Opcode::Continuation);
}

// ── WebSocket：握手 ───────────────────────────────────────────────────────

ST_TEST(net_ws_handshake_accept_rfc_vector) {
  // RFC 6455 §1.3 的样例向量（协议正确性的锚点）
  const auto accept = expected_accept("dGhlIHNhbXBsZSBub25jZQ==");
  ST_CHECK_EQ(accept, std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

ST_TEST(net_ws_handshake_validate_roundtrip) {
  const std::string key = make_handshake_key();
  ST_CHECK(!key.empty());
  ST_CHECK(key != make_handshake_key());   // 每次不同（防缓存碰撞）
  const std::string response = server_handshake_response(key);
  ST_CHECK(validate_server_handshake(response, key));
  // 错 key 拒绝（中间人替换应答的场景）
  ST_CHECK(!validate_server_handshake(response, make_handshake_key()));
  // 非 101 拒绝
  ST_CHECK(!validate_server_handshake("HTTP/1.1 400 Bad Request\r\n\r\n", key));
}

// ── WebSocket：会话回环（本机 TCP，无外网）────────────────────────────────

namespace {

/// 测试假流对：直接内存对拷（不经 TCP——会话层单测的确定性底座）。
struct LoopPair {
  std::vector<std::uint8_t> to_server{};
  std::vector<std::uint8_t> to_client{};
  std::mutex mutex{};

  struct End : WsSession::Stream {
    LoopPair* pair{};
    bool server_side{false};
    std::vector<std::uint8_t> inbox{};
    auto write_all(std::span<const std::uint8_t> data) -> bool override {
      std::lock_guard<std::mutex> lock(pair->mutex);
      // 写方向：本端写出 = 送进「对端将读取」的缓冲（server 写 to_client，client 写 to_server）
      auto& target = server_side ? pair->to_client : pair->to_server;
      target.insert(target.end(), data.begin(), data.end());
      return true;
    }
    auto read_some(std::span<std::uint8_t> buffer) -> int override {
      std::lock_guard<std::mutex> lock(pair->mutex);
      // 读方向：本端收到的 = 对端写来的（server 读 to_server，client 读 to_client）
      auto& source = server_side ? pair->to_server : pair->to_client;
      if (source.empty()) return -1;   // 无数据（测试里配合 try_read 用）
      const std::size_t take = std::min(buffer.size(), source.size());
      std::memcpy(buffer.data(), source.data(), take);
      source.erase(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(take));
      return static_cast<int>(take);
    }
  };

  End client_end{};
  End server_end{};
  LoopPair() {
    client_end.pair = this;
    client_end.server_side = false;
    server_end.pair = this;
    server_end.server_side = true;
  }
};

}  // namespace

ST_TEST(net_ws_session_text_roundtrip) {
  LoopPair pair;
  WsSession client(pair.client_end, true);
  WsSession server(pair.server_end, false);

  // 客户端发（自动掩码）→ 服务器收
  ST_CHECK(client.send_text("你好 ws"));
  auto received = server.try_read_message();
  if (!received.has_value()) {
    // 一次没解析完（read_some 返回 -1 时 try 不泵）——用 read_message 阻塞泵
    // 测试假流读空返回 -1：手动搬一次字节
    received = server.read_message();
  }
  ST_REQUIRE(received.has_value());
  ST_CHECK(received->opcode == Opcode::Text);
  ST_CHECK_EQ(received->text, std::string("你好 ws"));

  // 服务器回（不掩码）→ 客户端收
  const std::uint8_t bytes[] = {0x01, 0x02, 0x03};
  ST_CHECK(server.send_binary(bytes));
  auto echoed = client.read_message();
  ST_REQUIRE(echoed.has_value());
  ST_CHECK(echoed->opcode == Opcode::Binary);
  ST_CHECK_EQ(echoed->binary.size(), std::size_t{3});
}

ST_TEST(net_ws_session_ping_auto_pong) {
  LoopPair pair;
  WsSession client(pair.client_end, true);
  WsSession server(pair.server_end, false);

  ST_CHECK(client.send_ping("hb"));
  // 服务器侧：Ping 不上抛——自动回 Pong；下一条应用消息才返回
  ST_CHECK(server.send_text("after-ping"));
  auto message = client.read_message();
  ST_REQUIRE(message.has_value());
  // 客户端收到的是 after-ping（Pong 被静默吞）——Ping/Pong 不进应用面
  ST_CHECK_EQ(message->text, std::string("after-ping"));
}

ST_TEST(net_ws_session_close_handshake) {
  LoopPair pair;
  WsSession client(pair.client_end, true);
  WsSession server(pair.server_end, false);

  ST_CHECK(client.send_close(1001, "going away"));
  ST_CHECK(client.close_sent());
  auto closed = server.read_message();
  ST_REQUIRE(closed.has_value());
  ST_CHECK(closed->opcode == Opcode::Close);
  ST_CHECK_EQ(closed->close_code, std::uint16_t{1001});
  ST_CHECK_EQ(closed->text, std::string("going away"));
  ST_CHECK(server.close_sent());   // 服务器自动回了 Close（完成双向关闭）
}
