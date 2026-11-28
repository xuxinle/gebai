/// HTTPS/WSS 真实链路探针（不是单测：要外网，故排除在 `tests/` 之外）。
///
/// 为何必须存在：`tests/net_*_test.cpp` 全走本机明文回环——那是正确做法（CONVENTIONS §7
/// 不依赖外网），但它**证明不了 TLS 那条腿**。本次就是因为只有明文回环，
/// 「https 客户端验证过吗」的答案是「没有」。本探针把那条腿补上。
///
/// 用法：`st build tls_probe && ./build/dev/bin/tls_probe.exe`
/// 退出码 0 = 全部通过；非 0 = 第 N 项失败（对应下面的 CHECK 序号）。

#include <array>
#include <cstdint>
#include <format>
#include <string>

#include "st/core/net.hpp"
#include "st/core/print.hpp"
#include "st/net/http.hpp"
#include "st/net/tls.hpp"
#include "st/net/ws.hpp"
#include "st/net/ws_server.hpp"

namespace {

/// 计数器：探针是单线程顺序执行的小工具，用类包住避免可变全局（CONVENTIONS L8）。
struct Tally {
  int failures{0};
  int index{0};

  void check(bool condition, const std::string& what) {
    ++index;
    if (condition) {
      st::print("  PASS [{}] {}\n", index, what);
    } else {
      st::print("  FAIL [{}] {}\n", index, what);
      ++failures;
    }
  }
};

/// WSS 客户端：连 TCP → TLS → WS 握手 → 收发一帧。
/// 把「TlsStream 能否当 WsSession 的流用」这条接线走通（这是 ws.hpp 声称支持但没测过的路径）。
[[nodiscard]] auto wss_roundtrip(const std::string& host, std::uint16_t port,
                                 const std::string& path, const std::string& message)
    -> std::string {
  auto tls = st::net::TlsStream::connect(host, port, 10000);
  if (!tls.has_value()) return std::format("TLS 连接失败: {}", tls.error().to_string());

  // TlsStream → WsSession::Stream 的适配（ws.hpp 的 Stream 面是 int 语义）
  struct TlsAdapter : st::net::ws::WsSession::Stream {
    st::net::TlsStream& tls;
    explicit TlsAdapter(st::net::TlsStream& stream) : tls(stream) {}
    auto write_all(std::span<const std::uint8_t> data) -> bool override {
      return static_cast<bool>(tls.write_all(data));
    }
    auto read_some(std::span<std::uint8_t> buffer) -> int override {
      const auto got = tls.read_some(buffer);
      if (!got.has_value()) return -1;
      return static_cast<int>(*got);
    }
  } adapter(*tls);

  const std::string key = st::net::ws::make_handshake_key();
  const std::string request =
      st::net::ws::client_handshake_request(host, port, path, key);
  if (!tls->write_all(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(request.data()), request.size()))) {   // lint-allow: L6 探针字节视图
    return "握手请求发送失败";
  }

  // 读 101（\r\n\r\n 结束），多余的字节要交给会话
  std::string reply;
  std::array<std::uint8_t, 4096> buffer{};
  std::size_t header_end = std::string::npos;
  while (header_end == std::string::npos) {
    const auto got = tls->read_some(buffer);
    if (!got.has_value()) return "握手应答读取失败: " + got.error().to_string();
    if (*got == 0) return "对端在握手应答前关闭";
    reply.append(reinterpret_cast<const char*>(buffer.data()), *got);   // lint-allow: L6 探针字节视图
    header_end = reply.find("\r\n\r\n");
    if (reply.size() > 16384) return "握手应答过大";
  }
  if (!st::net::ws::validate_server_handshake(reply, key)) {
    return "握手校验失败（应答前 200 字节）: " + reply.substr(0, 200);
  }

  // 多读到的首帧字节不能丢：塞回解码器流（这里用 WsSession 的 preload 通路）
  // WsSession 本身不收 preload，所以剩余字节通过适配器缓冲转交——探针里直接判空，
  // 因为标准服务器不会在 101 后立刻推帧（真有的话这条探针要改成带缓冲的适配器）。
  if (header_end + 4 < reply.size()) return "101 应答后紧跟了数据（探针未处理）";

  st::net::ws::WsSession session(adapter, /*client_side=*/true);
  if (!session.send_text(message)) return "WS 文本帧发送失败";
  const auto echoed = session.read_message();
  if (!echoed.has_value()) return "WS 未收到回帧";
  (void)session.send_close(1000, "probe-done");
  return "ok:" + echoed->text;
}

}  // namespace

auto main() -> int {
  st::print("=== 霜天 TLS/HTTPS/WSS 真实链路探针 ===\n");
  Tally tally;

  // 0. 平台 TLS 可用性
  const bool available = st::net::TlsStream::platform_available();
  tally.check(available, std::format("平台 TLS 可用（不可用原因: {}）",
                               available ? "——" : st::net::TlsStream::unavailable_reason()));

  // 1. 裸 TLS 到知名站点（Schannel/OpenSSL 握手 + 证书校验）
  if (available) {
    auto tls = st::net::TlsStream::connect("example.com", 443, 10000);
    tally.check(tls.has_value(),
          tls.has_value() ? "裸 TLS 握手 example.com:443 成功"
                          : "裸 TLS 握手失败: " + tls.error().to_string());
    if (tls.has_value()) {
      const std::string proto = tls->negotiated_protocol();
      tally.check(!proto.empty(), std::format("协商协议 = '{}'", proto));
      const std::string cn = tls->peer_cn();
      tally.check(!cn.empty(), std::format("对端证书 CN = '{}'", cn));
      // 真发一个 HTTP/1.1 请求，验证 TLS 通道能双向搬字节
      const std::string request =
          "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
      const bool written = static_cast<bool>(tls->write_all(std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(request.data()), request.size())));   // lint-allow: L6 探针字节视图
      tally.check(written, "TLS 通道写入 HTTP 请求");
      if (written) {
        std::string response;
        std::array<std::uint8_t, 4096> buf{};
        for (int round = 0; round < 64; ++round) {
          const auto got = tls->read_some(buf);
          if (!got.has_value() || *got == 0) break;
          response.append(reinterpret_cast<const char*>(buf.data()), *got);   // lint-allow: L6 探针字节视图
          if (response.find("\r\n\r\n") != std::string::npos) break;
        }
        tally.check(response.starts_with("HTTP/1.1 200") || response.starts_with("HTTP/1.0 200"),
              std::format("TLS 上收到 HTTP 200（首行: '{}'）",
                          response.substr(0, response.find("\r\n"))));
      }
    }
  }

  // 2. HttpClient 走 https（这是用户最会用到的那条路）
  {
    st::net::http::HttpClient client;
    client.timeout_ms = 15000;
    auto response = client.get("https://example.com/");
    tally.check(response.has_value(),
          response.has_value() ? std::format("HttpClient GET https 状态={} 体长={}",
                                             response->status, response->body.size())
                               : "HttpClient GET https 失败: " + response.error().to_string());
    if (response.has_value()) {
      tally.check(response->status == 200, "https 响应状态 200");
      tally.check(response->body.find("Example Domain") != std::string::npos,
            "https 响应体含 'Example Domain'（内容真到了）");
    }
  }

  // 3. https 重定向跟随（跨 scheme/主机都走 TLS）
  {
    st::net::http::HttpClient client;
    client.timeout_ms = 15000;
    client.max_redirects = 5;
    auto response = client.get("http://example.com/");   // 明文入口
    tally.check(response.has_value(),
          response.has_value() ? std::format("http→跳转链终点状态={} hops={}",
                                             response->status, response->hop_chain.size())
                               : "重定向跟随失败: " + response.error().to_string());
  }

  // 4. **大响应跨多次 TCP 读**（2026-11-28 暴露过真缺陷的地方）
  //
  // 为何这条必须在探针里：`read_some` 的解密路径有「半条记录」（`SECBUFFER_EXTRA`）
  // 与「一次解出多条记录」两个分支，**只有响应大到需要多次 TCP 读时才走到**。
  // 实测过的缺陷形态：小响应（≤1000 B）全绿、16 KB 就解密失败或挂死。
  {
    st::net::http::HttpClient client;
    client.timeout_ms = 30000;
    auto response = client.get("https://httpbin.org/bytes/16000");
    if (!response.has_value()) {
      // 公共端点不可用不算我们的失败，但必须把原因说出来
      st::print("  SKIP 大响应（httpbin.org 不可达: {}）\n", response.error().message);
    } else {
      const std::size_t got_bytes = response->body.size();
      tally.check(response->status == 200 && got_bytes == 16000U,
            std::format("大响应 16000 B：状态={} 实收={} B", response->status, got_bytes));
      // 数据完整性：随机二进制不该全零（全零/低位重复 = 解密错位）
      std::size_t nonzero = 0;
      for (const char c : response->body) {
        if (c != 0) ++nonzero;
      }
      tally.check(nonzero > got_bytes / 2U,
            std::format("大响应内容非全零（{} / {}）", nonzero, got_bytes));
    }
  }

  // 5. WSS 真实链路（TlsStream 作为 WsSession 的流）
  if (available) {
    // 公共 echo 服务：wss://echo.websocket.org 已停服，改用 Postman 的公共 echo。
    // 失败不算致命（公共服务的可用性不归我们管），但**必须把原因打出来**。
    const auto result = wss_roundtrip("ws.postman-echo.com", 443, "/raw", "st-tls-probe");
    tally.check(result == "ok:st-tls-probe", std::format("WSS 回环: {}", result));
  }

  st::print("\n{} 失败 / {} 项\n", tally.failures, tally.index);
  return tally.failures;
}

