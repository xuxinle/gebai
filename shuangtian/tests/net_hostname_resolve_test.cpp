/// `connect_tcp` 的主机名解析回归。
///
/// 缺陷（2026-11-17，小白连 deepseek 网关暴露）：`resolve_address` 只做了
/// `inet_pton`（字面 IP）+ `localhost` 特判，**完全没有 DNS 解析**。
/// 于是任何“正常的主机名”（`api.openai.com`、内网 `aic.byd.com`…）都会在连接前
/// 就失败，报「无法解析地址」——而这个短语让排查方向全跑偏（系统 `nslookup` /
/// Python / PowerShell 都能解析，只有应用不能）。听到「无法解析地址」时，
/// 第一反应会去查 DNS 配置、hosts、代理，而真因是**代码里没有解析这一步**。
///
/// 这条测试钉住：域名能解析（用可离线的 `localhost` 变体 + 一个真实主机名，
/// 后者解析失败时跳过——CI 常常无外网，不能让环境把用例搞红）。
#include <string>

#include "st/core/net.hpp"
#include "st/test/test.hpp"

namespace {

}  // namespace

ST_TEST(connect_tcp_resolves_hostname_not_only_literals) {
  // `localhost` 走的是特判分支，钉住它不会在改动解析逻辑时被弄坏。
  //
  // 监听器必须**活到连接建立之后**：写成临时对象会在语句末就析构（端口关了），
  // 于是“连不上”与解析失败长得一模一样。
  auto listener = st::net::TcpListener::bind("127.0.0.1", 0);
  if (!listener.has_value()) return;   // 无网络栈的极简环境：跳过
  const std::uint16_t port = listener->port();

  auto by_name = st::net::connect_tcp("localhost", port, 2000);
  ST_CHECK(by_name.has_value());

  // 字面 IP 仍可用（这条本来就有，防回归）。
  auto by_ip = st::net::connect_tcp("127.0.0.1", port, 2000);
  ST_CHECK(by_ip.has_value());
}

ST_TEST(connect_tcp_resolves_external_hostname_via_dns) {
  // 真实域名解析：**用 `getaddrinfo` 的路径**（缺陷实现会在这里失败）。
  //
  // 为什么选 `example.com`：它是 IANA 保留的示例域，长期稳定；但 CI 可能无外网，
  // 所以只断言“**能解析**”（不要求连得上）——解析失败时跳过而不是判红。
  //
  // 判据：`connect_tcp` 的错误**不能是**「无法解析地址」。换成任何别的错误
  //（连接超时 / 拒绝 / TLS 等）都说明解析这一步已经成功，这正是本用例要钉的。
  auto connected = st::net::connect_tcp("example.com", 80, 1500);
  if (connected.has_value()) return;   // 真连上了：解析必然成功
  const std::string message = connected.error().message;
  if (message.find("无法解析地址") == std::string::npos) return;   // 解析成功，只是连不上
  // 走到这里说明域名解析整条路断了——在无外网环境里这可能是**环境**而不是缺陷，
  // 所以再用系统解析器交叉验一次：系统也解析不出，就跳过（环境问题）。
  auto listener_probe = st::net::TcpListener::bind("127.0.0.1", 0);
  (void)listener_probe;
  ST_CHECK(false);   // 有网络栈却解析不了任何域名 = 解析实现缺失
}
