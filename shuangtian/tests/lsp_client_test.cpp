/// LSP 客户端测试（阶段 2）：生命周期状态机、文档同步、diff 计算。
///
/// 分两档：
/// - **假 server**（stdio 上跑的脚本）：确定性验证握手、超时、异常退出的上报；
/// - **真 clangd**（环境里有才跑）：验证端到端确实能起来、能收诊断。
///   没有 clangd 时**跳过而非失败**——CI 未必装 C++ 语言服务器。

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"
#include "st/lsp/client.hpp"
#include "st/test/test.hpp"

namespace {

using namespace std::chrono_literals;

/// 在临时目录里造一个假 LSP server 脚本（`sh` 写）。
struct FakeServer {
  std::filesystem::path dir{};
  std::filesystem::path script{};

  explicit FakeServer(const std::string& body) {
    dir = std::filesystem::temp_directory_path() /
          std::format("st_lsp_fake_{}", static_cast<int>(st::process::current_id()));
    std::filesystem::create_directories(dir);
    script = dir / "server.sh";
    std::ofstream out(script);
    out << "#!/bin/sh\n" << body << "\n";
    out.close();
    std::filesystem::permissions(script,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read);
  }
  ~FakeServer() {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }
  [[nodiscard]] auto path() const -> std::string { return script.string(); }
};

/// 把客户端泵到某个状态（带超时；返回是否到达）。
auto pump_until(st::lsp::LspClient& client, st::lsp::SessionState want,
                std::int64_t timeout_ms = 8000) -> bool {
  const std::int64_t deadline = st::time::now_ms() + timeout_ms;
  while (st::time::now_ms() < deadline) {
    (void)client.pump();
    if (client.state() == want) return true;
    std::this_thread::sleep_for(5ms);
  }
  return client.state() == want;
}

/// 等一个谓词在 pump 中成立（收集消息用）。
template <typename Predicate>
auto pump_until_true(st::lsp::LspClient& client, Predicate predicate,
                     std::int64_t timeout_ms = 8000) -> bool {
  const std::int64_t deadline = st::time::now_ms() + timeout_ms;
  while (st::time::now_ms() < deadline) {
    (void)client.pump();
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

/// clangd 是否可用（不可用则跳过真机用例）。
[[nodiscard]] auto clangd_path() -> std::string {
  const auto found = st::process::which("clangd");
  return found.has_value() ? *found : std::string{};
}

/// 造一个最小 C++ 工程（clangd 需要一个编译单元 + 编译命令）。
struct CppSandbox {
  std::filesystem::path dir{};
  CppSandbox() {
    dir = std::filesystem::temp_directory_path() /
          std::format("st_lsp_cpp_{}", static_cast<int>(st::process::current_id()));
    std::filesystem::create_directories(dir);
    std::ofstream header(dir / "widget.hpp");
    header << "#pragma once\n"
              "struct Widget {\n"
              "  int width;\n"
              "  int height;\n"
              "};\n";
    header.close();
    std::ofstream source(dir / "main.cpp");
    // 故意留一个错误：`missing_symbol` 未声明 → clangd 应报诊断。
    source << "#include \"widget.hpp\"\n"
              "int area() {\n"
              "  Widget w;\n"
              "  w.width = 3\n"          // 缺分号：语法错误
              "  return missing_symbol;\n"  // 未声明标识符：语义错误
              "}\n";
    source.close();
    // `compile_commands.json`：clangd 靠它知道编译参数（没有它也能工作，但慢）。
    std::ofstream commands(dir / "compile_commands.json");
    commands << "[{\"directory\":\"" << dir.string()
             << "\",\"file\":\"" << (dir / "main.cpp").string()
             << "\",\"command\":\"c++ -std=c++20 -c " << (dir / "main.cpp").string() << "\"}]\n";
    commands.close();
  }
  ~CppSandbox() {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }
};

}  // namespace

ST_TEST(lsp_client_uri_round_trip) {
  // POSIX 绝对路径。
  const std::string uri = st::lsp::LspClient::path_to_uri("/tmp/a b/c.cpp");
  // 空格要转义（URI 里裸空格非法）。
  ST_CHECK_EQ(uri, std::string("file:///tmp/a%20b/c.cpp"));
  ST_CHECK_EQ(st::lsp::LspClient::uri_to_path(uri), std::string("/tmp/a b/c.cpp"));
  // 中文路径也往返保真（LSP 里的非 ASCII 靠百分号转义）。
  const std::string chinese = "/tmp/文档/测试.cpp";
  const std::string chinese_uri = st::lsp::LspClient::path_to_uri(chinese);
  ST_CHECK_EQ(st::lsp::LspClient::uri_to_path(chinese_uri), chinese);
  // 非 file:// 前缀如实返回空（不猜）。
  ST_CHECK_EQ(st::lsp::LspClient::uri_to_path("http://x/y"), std::string{});
}

ST_TEST(lsp_client_diff_computes_minimal_single_range) {
  // 单字符插入（最常见的编辑）。
  const auto insert = st::lsp::compute_single_change("abc", "abXc");
  ST_REQUIRE(insert.has_value());
  ST_CHECK(!insert->is_full);
  ST_CHECK_EQ(insert->text, std::string("X"));
  ST_CHECK_EQ(insert->range.start.character, std::size_t{2});
  ST_CHECK_EQ(insert->range.end.character, std::size_t{2});

  // 删除范围。
  const auto erase = st::lsp::compute_single_change("hello world", "hello");
  ST_REQUIRE(erase.has_value());
  ST_CHECK_EQ(erase->text, std::string{});
  ST_CHECK_EQ(erase->range.start.character, std::size_t{5});
  ST_CHECK_EQ(erase->range.end.character, std::size_t{11});

  // 行内替换。
  const auto replace = st::lsp::compute_single_change("int x = 1;\n", "int x = 42;\n");
  ST_REQUIRE(replace.has_value());
  ST_CHECK_EQ(replace->text, std::string("42"));
  ST_CHECK_EQ(replace->range.start.line, std::size_t{0});

  // 无变化：不该发通知。
  ST_CHECK(!st::lsp::compute_single_change("same", "same").has_value());

  // **UTF-8 边界**：中文插入不能切在字符中间（切了 server 会错位）。
  const auto cjk = st::lsp::compute_single_change("ab", "a中b");
  ST_REQUIRE(cjk.has_value());
  ST_CHECK_EQ(cjk->text, std::string("中"));
  // "a" = 1 字节 1 码元 → 列 1。
  ST_CHECK_EQ(cjk->range.start.character, std::size_t{1});
  ST_CHECK_EQ(cjk->range.end.character, std::size_t{1});

  // 多行编辑（在行尾换行后追加）仍是单区间。
  const auto multiline = st::lsp::compute_single_change("a\nb", "a\nX\nb");
  ST_REQUIRE(multiline.has_value());
  ST_CHECK_EQ(multiline->range.start.line, std::size_t{1});
}

ST_TEST(lsp_client_reports_missing_program) {
  st::lsp::LspClient client;
  st::lsp::ClientConfig missing_config{};
  missing_config.program = "/nonexistent/lsp-server-xyz";
  client.start(missing_config);
  ST_CHECK(client.state() == st::lsp::SessionState::Failed);
  ST_CHECK(client.error().find("无法启动") != std::string::npos);
  // 状态回调也要收到（UI 靠它提示用户）。
  bool notified = false;
  client.on_state_change = [&](st::lsp::SessionState state) {
    if (state == st::lsp::SessionState::Failed) notified = true;
  };
  client.start(missing_config);   // 重试：仍失败（回调这次已装上）
  (void)client.pump();
  ST_CHECK(notified);
}

ST_TEST(lsp_client_completes_handshake_with_fake_server) {
  // 假 server：读完 initialize 请求后回一个标准响应，然后回显后续通知。
  FakeServer server(R"(
read_message() {
  # 读 Content-Length 头，再读那么多字节（用 dd 精确读取）
  len=""
  while IFS= read -r line; do
    line=$(printf '%s' "$line" | tr -d '\r')
    [ -z "$line" ] && break
    case "$line" in
      Content-Length:*) len=$(printf '%s' "$line" | cut -d' ' -f2) ;;
    esac
  done
  [ -z "$len" ] && return 1
  dd bs=1 count="$len" 2>/dev/null
}
send() {
  body="$1"
  # 长度先存变量再拼：子命令替换里那个右括号加引号会提前终结
  # C++ raw string（实测症状：脚本被腰斩、编译器报 character constant too long）。
  length=$(printf '%s' "$body" | wc -c | tr -d ' ')
  printf 'Content-Length: %s\r\n\r\n%s' "$length" "$body"
}
msg=$(read_message)
case "$msg" in
  *'"initialize"'*)
    send '{"jsonrpc":"2.0","id":1,"result":{"capabilities":{"hoverProvider":true,"definitionProvider":true,"completionProvider":{"triggerCharacters":["."]}},"serverInfo":{"name":"fake-lsp","version":"9.9"}}}'
    ;;
esac
# 再收几条（initialized / didOpen），最后自己退出
for i in 1 2 3 4 5 6; do
  read_message > /dev/null || break
done
)");

  st::lsp::LspClient client;
  st::lsp::ClientConfig fake_config{};
  fake_config.program = server.path();
  fake_config.init_timeout_ms = 5000;
  client.start(fake_config);
  ST_CHECK(client.state() == st::lsp::SessionState::Starting);

  const bool fake_ready = pump_until(client, st::lsp::SessionState::Ready);
  // 用假 server 的具体路径兜底诊断（失败时给出原因，而不是只说"没到 Ready"）。
  ST_CHECK(fake_ready);
  if (!fake_ready) {
    std::cout << "[lsp] 未到 Ready：state=" << static_cast<int>(client.state())
              << " error=" << client.error() << '\n';
  }
  const auto fake_status = client.status();
  ST_CHECK_EQ(fake_status.server_name, std::string("fake-lsp"));
  ST_CHECK_EQ(fake_status.server_version, std::string("9.9"));
  ST_CHECK(fake_status.supports_hover);
  ST_CHECK(fake_status.supports_definition);
  ST_CHECK(fake_status.supports_completion);
  ST_CHECK(!fake_status.supports_references);   // 假 server 没声明 → 不该假装支持
  ST_CHECK_EQ(client.capabilities().completion_trigger_characters.size(), std::size_t{1});
  ST_CHECK_EQ(client.capabilities().completion_trigger_characters[0], std::string("."));

  // 文档同步：打开 → 版本 1；变更 → 版本 2（即使 server 不响应也不该崩）。
  client.did_open("file:///tmp/x.cpp", "cpp", "int main(){}\n");
  ST_CHECK(client.is_open("file:///tmp/x.cpp"));
  ST_CHECK_EQ(client.document_version("file:///tmp/x.cpp"), std::int64_t{1});
  const auto change =
      st::lsp::compute_single_change("int main(){}\n", "int main(){ }\n");
  ST_REQUIRE(change.has_value());
  client.did_change("file:///tmp/x.cpp", {*change}, "int main(){ }\n");
  ST_CHECK_EQ(client.document_version("file:///tmp/x.cpp"), std::int64_t{2});
  ST_CHECK_EQ(client.document_text("file:///tmp/x.cpp"), std::string("int main(){ }\n"));
  client.did_close("file:///tmp/x.cpp");
  ST_CHECK(!client.is_open("file:///tmp/x.cpp"));
  // 未打开的文档变更：静默忽略（不是错误，是用途错）。
  client.did_change("file:///tmp/never.cpp", {*change}, "x");
  ST_CHECK_EQ(client.document_version("file:///tmp/never.cpp"), std::int64_t{0});

  client.stop(1500);
  ST_CHECK(client.state() == st::lsp::SessionState::Stopped);
}

ST_TEST(lsp_client_reports_handshake_timeout) {
  // 假 server：收下 initialize 但**永不回应**（模拟卡死的 server）。
  FakeServer server(R"(
read len=""
while IFS= read -r line; do
  line=$(printf '%s' "$line" | tr -d '\r')
  [ -z "$line" ] && break
  case "$line" in Content-Length:*) len=$(printf '%s' "$line" | cut -d' ' -f2) ;; esac
done
[ -n "$len" ] && dd bs=1 count="$len" 2>/dev/null > /dev/null
sleep 30
)");
  st::lsp::LspClient client;
  st::lsp::ClientConfig timeout_config{};
  timeout_config.program = server.path();
  timeout_config.init_timeout_ms = 300;   // 短超时，测试快
  client.start(timeout_config);
  const bool failed = pump_until(client, st::lsp::SessionState::Failed, 3000);
  ST_CHECK(failed);
  ST_CHECK(client.error().find("超时") != std::string::npos);
  ST_CHECK(client.error().find("300") != std::string::npos);   // 原因里带超时值
  client.stop(300);
}

ST_TEST(lsp_client_reports_unexpected_exit) {
  // 假 server：接受启动后立刻退出（模拟崩溃）。
  FakeServer server("exit 7\n");
  st::lsp::LspClient client;
  st::lsp::ClientConfig exit_config{};
  exit_config.program = server.path();
  exit_config.init_timeout_ms = 3000;
  client.start(exit_config);
  const bool failed = pump_until(client, st::lsp::SessionState::Failed, 4000);
  ST_CHECK(failed);
  ST_CHECK(client.error().find("退出") != std::string::npos);
  client.stop(300);
}

ST_TEST(lsp_client_request_before_ready_is_honest) {
  // 未就绪时发请求：**如实返回 0**（不假装发出去了）——自动化最怕"调了没报错但没生效"。
  st::lsp::LspClient client;
  ST_CHECK_EQ(client.request("textDocument/hover", st::Json::object()), std::int64_t{0});
  ST_CHECK(client.status().pending_requests == 0);
}

ST_TEST(lsp_client_real_clangd_smoke) {
  const std::string server = clangd_path();
  if (server.empty()) {
    std::cout << "[lsp] 未安装 clangd，跳过真机冒烟" << '\n';
    return;
  }
  CppSandbox sandbox;
  const std::string source_path = (sandbox.dir / "main.cpp").string();

  st::lsp::LspClient client;
  st::lsp::ClientConfig clangd_config{};
  clangd_config.program = server;
  clangd_config.root_path = sandbox.dir.string();
  clangd_config.args = {"--log=error", "--background-index=0", "--clang-tidy=0"};
  client.start(clangd_config);

  const bool clangd_ready = pump_until(client, st::lsp::SessionState::Ready, 20000);
  ST_CHECK(clangd_ready);
  if (!clangd_ready) {
    std::cout << "[lsp] clangd 未就绪：state=" << static_cast<int>(client.state())
              << " error=" << client.error() << '\n';
    client.stop(1000);
    return;
  }
  // clangd 应报出 serverInfo（名字里含 clangd）。
  ST_CHECK(client.status().server_name.find("clangd") != std::string::npos);
  ST_CHECK(client.status().supports_hover);
  ST_CHECK(client.status().supports_completion);
  ST_CHECK(client.status().supports_definition);

  // 打开源码：clangd 应推 publishDiagnostics（我们文件里有语法错误 + 未声明标识符）。
  const auto text = st::fs::read_text(source_path);
  ST_REQUIRE(text.has_value());
  std::vector<st::lsp::Diagnostic> diagnostics;
  std::string diagnostics_uri;
  client.on_diagnostics = [&](const std::string& uri, const std::vector<st::lsp::Diagnostic>& list) {
    if (!list.empty()) {
      diagnostics_uri = uri;
      diagnostics = list;
    }
  };
  client.did_open(st::lsp::LspClient::path_to_uri(source_path), "cpp", *text);

  const bool got = pump_until_true(client, [&] { return !diagnostics.empty(); }, 20000);
  ST_CHECK(got);
  if (!got) {
    std::cout << "[lsp] clangd 未推诊断（超时）；state=" << static_cast<int>(client.state())
              << '\n';
  } else {
    ST_CHECK(diagnostics_uri.find("main.cpp") != std::string::npos);
    // 至少一条错误诊断（我们故意写了语法错误）。
    bool has_error = false;
    for (const auto& diagnostic : diagnostics) {
      if (diagnostic.severity == 1) has_error = true;
    }
    ST_CHECK(has_error);
    // 诊断消息非空（不然 UI 上没法显示）。
    ST_CHECK(!diagnostics.front().message.empty());
    std::cout << "[lsp] clangd 诊断 " << diagnostics.size() << " 条；首条："
              << diagnostics.front().message.substr(0, 60) << '\n';

    // hover 请求（真往返：请求 → 响应）。
    // ⚠ `position` 与 `textDocument` 是**兄弟**字段——别塞进 textDocument 里
    //（塞错时 clangd 回 `InvalidParams` 错误而不是忽略，本测试就是这么抓到自己的
    //  字段拼错：`error=1 body=null`）。
    st::Json params = st::Json::object();
    st::Json item = st::Json::object();
    item["uri"] = st::lsp::LspClient::path_to_uri(source_path);
    params["textDocument"] = std::move(item);
    st::Json position = st::Json::object();
    position["line"] = 2;      // `Widget w;` 那行
    position["character"] = 3; // `Widget`
    params["position"] = std::move(position);
    bool hover_answered = false;
    std::string hover_content;
    client.on_response = [&](std::int64_t id, const std::string& method, const st::Json& result,
                             bool is_error) {
      if (method == "textDocument/hover") {
        hover_answered = !is_error;
        hover_content = st::json_dump(result).substr(0, 200);
        (void)id;
      }
    };
    const std::int64_t request_id = client.request("textDocument/hover", std::move(params));
    ST_CHECK(request_id > 0);
    const bool answered = pump_until_true(client, [&] { return hover_answered; }, 30000);
    ST_CHECK(answered);
    if (answered) {
      std::cout << "[lsp] clangd hover 响应：" << hover_content.substr(0, 80) << '\n';
    }
  }

  client.stop(2000);
  ST_CHECK(client.state() == st::lsp::SessionState::Stopped);
}
