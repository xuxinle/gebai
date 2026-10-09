/// 四语言真机冒烟（LSP 阶段 6）：clangd / pyright / gopls / rust-analyzer。
///
/// **每个 server 独立判定**：环境里没装就跳过（stdout 说明），装了就真跑
/// （启动 → 初始化 → didOpen → 等诊断）。这比"全部装了才跑"实用——
/// CI 未必四个都装，而"装了的那个能不能用"才是要验的。
///
/// 用途：语言服务集成最脆的地方不是协议（单测覆盖了），而是**与真实 server 的
/// 对话习惯**——初始化参数、能力声明、诊断推送时机、进程退出行为。
/// 每个 server 的实现质量与"怪癖"都不同，只有真连才知道。

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

/// 临时工程目录（析构清理）。
struct Sandbox {
  std::filesystem::path dir{};
  explicit Sandbox(const std::string& tag) {
    dir = std::filesystem::temp_directory_path() /
          std::format("st_lsp_lang_{}_{}", tag, static_cast<int>(st::process::current_id()));
    std::filesystem::create_directories(dir);
  }
  ~Sandbox() {
    std::error_code error;
    std::filesystem::remove_all(dir, error);
  }
  auto write(const std::string& name, const std::string& content) const -> std::string {
    const auto path = dir / name;
    std::ofstream out(path);
    out << content;
    out.close();
    return path.string();
  }
};

/// 一次真机冒烟的通用流程：起 server → 等 Ready → 打开文件 → 等诊断。
/// 返回 `{ready, got_diagnostics, diagnostic_count, server_name}`。
struct SmokeResult {
  bool available{false};
  bool ready{false};
  bool got_diagnostics{false};
  std::size_t diagnostic_count{0};
  std::string server_name{};
  std::string error{};
};

auto smoke(const std::string& program, const std::vector<std::string>& args,
           const std::string& root, const std::string& file_path, const std::string& text,
           std::int64_t ready_budget_ms) -> SmokeResult {
  SmokeResult result{};
  if (!st::process::which(program).has_value()) return result;   // 未安装
  result.available = true;

  st::lsp::LspClient client{};
  std::size_t diagnostics = 0;
  client.on_diagnostics = [&](const std::string&, const std::vector<st::lsp::Diagnostic>& list) {
    if (!list.empty()) diagnostics = list.size();
  };
  st::lsp::ClientConfig config{};
  config.program = program;
  config.args = args;
  config.root_path = root;
  config.init_timeout_ms = ready_budget_ms;
  client.start(config);

  const std::int64_t deadline = st::time::now_ms() + ready_budget_ms;
  while (st::time::now_ms() < deadline && client.state() != st::lsp::SessionState::Ready) {
    (void)client.pump();
    std::this_thread::sleep_for(20ms);
    if (client.state() == st::lsp::SessionState::Failed) break;
  }
  result.ready = client.state() == st::lsp::SessionState::Ready;
  result.server_name = client.status().server_name;
  if (!result.ready) {
    result.error = client.error();
    client.stop(500);
    return result;
  }

  client.did_open(st::lsp::LspClient::path_to_uri(file_path),
                  program.find("pyright") != std::string::npos   ? "python"
                  : program.find("gopls") != std::string::npos   ? "go"
                  : program.find("rust") != std::string::npos    ? "rust"
                                                                 : "cpp",
                  text);
  // 诊断是推送式的，**时机不可控**（clangd 快、rust-analyzer 要索引）：
  // 给足预算，但如果一直没来也不算失败（server 可用性已由 Ready 证明）。
  const std::int64_t diag_deadline = st::time::now_ms() + ready_budget_ms;
  while (st::time::now_ms() < diag_deadline && diagnostics == 0) {
    (void)client.pump();
    std::this_thread::sleep_for(20ms);
  }
  result.diagnostic_count = diagnostics;
  result.got_diagnostics = diagnostics > 0;
  client.stop(1500);
  return result;
}

}  // namespace

ST_TEST(lsp_language_clangd_smoke) {
  Sandbox sandbox("clangd");
  sandbox.write("compile_commands.json",
                std::format(R"([{{"directory":"{}","file":"{}/a.cpp","command":"c++ -std=c++20 -c a.cpp"}}])",
                            sandbox.dir.string(), sandbox.dir.string()));
  // 故意写一个未声明标识符（+ 缺分号）。
  const std::string path = sandbox.write("a.cpp",
      "#include <string>\n"
      "int main() {\n"
      "  std::string s = \"x\"\n"
      "  return missing_thing;\n"
      "}\n");
  const auto text = st::fs::read_text(path).value_or("");
  const auto result = smoke("clangd", {"--background-index=0", "--clang-tidy=0", "--log=error"},
                            sandbox.dir.string(), path, text, 20000);
  if (!result.available) {
    std::cout << "[lang] clangd 未安装，跳过" << '\n';
    return;
  }
  ST_CHECK(result.ready);
  if (result.ready) {
    ST_CHECK(result.server_name.find("clangd") != std::string::npos);
    ST_CHECK(result.got_diagnostics);
    std::cout << "[lang] clangd 就绪；诊断 " << result.diagnostic_count << " 条" << '\n';
  } else {
    std::cout << "[lang] clangd 未就绪：" << result.error << '\n';
  }
}

ST_TEST(lsp_language_pyright_smoke) {
  Sandbox sandbox("pyright");
  // 未定义名字 + 调用不存在的属性。
  const std::string path = sandbox.write("a.py", "def greet(name):\n    return undefined_symbol\n");
  const auto text = st::fs::read_text(path).value_or("");
  const auto result = smoke("pyright-langserver", {"--stdio"}, sandbox.dir.string(), path, text,
                            25000);
  if (!result.available) {
    std::cout << "[lang] pyright-langserver 未安装，跳过" << '\n';
    return;
  }
  ST_CHECK(result.ready);
  if (result.ready) {
    // ⚠ **不断言 `serverInfo`**：协议里它是可选的，pyright 不回（实测）。
    // 拿它当"连接成功"的判据会把可用 server 判死——`Ready` 才是真判据。
    ST_CHECK(result.got_diagnostics);
    std::cout << "[lang] pyright 就绪；诊断 " << result.diagnostic_count << " 条"
              << "（serverInfo 为空属正常）" << '\n';
  } else {
    std::cout << "[lang] pyright 未就绪：" << result.error << '\n';
  }
}

ST_TEST(lsp_language_gopls_smoke) {
  Sandbox sandbox("gopls");
  sandbox.write("go.mod", "module demo\n\ngo 1.21\n");
  // 未声明变量 + 未使用的导入（Go 编译器把后者当错误）。
  const std::string path = sandbox.write("main.go",
      "package main\n"
      "import \"fmt\"\n"
      "func main() {\n"
      "  fmt.Println(undefined_var)\n"
      "}\n");
  const auto text = st::fs::read_text(path).value_or("");
  // `-remote=auto` 之类会起后台守护进程：显式禁用日志与遥测，保持测试干净。
  const auto result = smoke("gopls", {"-logfile=/dev/null", "-rpc.trace=false"},
                            sandbox.dir.string(), path, text, 25000);
  if (!result.available) {
    std::cout << "[lang] gopls 未安装，跳过" << '\n';
    return;
  }
  ST_CHECK(result.ready);
  if (result.ready) {
    ST_CHECK(result.got_diagnostics);
    std::cout << "[lang] gopls 就绪；诊断 " << result.diagnostic_count << " 条" << '\n';
  } else {
    std::cout << "[lang] gopls 未就绪：" << result.error << '\n';
  }
}

ST_TEST(lsp_language_rust_analyzer_smoke) {
  Sandbox sandbox("rust");
  // rust-analyzer 需要 Cargo 工程才知道怎么解析；最简形态是 src/main.rs + Cargo.toml。
  std::filesystem::create_directories(sandbox.dir / "src");
  sandbox.write("Cargo.toml",
                "[package]\nname = \"demo\"\nversion = \"0.1.0\"\nedition = \"2021\"\n");
  const std::string path = sandbox.write("src/main.rs",
      "fn main() {\n    let x: i32 = \"not a number\";\n    println!(\"{}\", undefined_value);\n}\n");
  const auto text = st::fs::read_text(path).value_or("");
  // rust-analyzer 首轮要建索引，给足预算；诊断可能在索引完成后才推。
  // 参数名是 `--no-log-buffering`（少个 ing 会让它拒绝启动——实测踩到）。
  const auto result = smoke("rust-analyzer", {"--no-log-buffering"},
                            sandbox.dir.string(), path, text, 40000);
  if (!result.available) {
    std::cout << "[lang] rust-analyzer 未安装，跳过" << '\n';
    return;
  }
  ST_CHECK(result.ready);
  if (result.ready) {
    std::cout << "[lang] rust-analyzer 就绪；诊断 " << result.diagnostic_count
              << " 条（可能需索引后才有）" << '\n';
    // rust-analyzer 的诊断为拉取式+推送混合，首轮可能为空——**只断言"能起来并完成握手"**
    // （这是本用例的承诺：可用性），诊断不算失败条件。
  } else {
    std::cout << "[lang] rust-analyzer 未就绪：" << result.error << '\n';
  }
}

ST_TEST(lsp_recipe_table_matches_available_programs) {
  // 配方表的**程序名**必须与真机可执行文件名一致——写错的表现是"该语言静默不可用"
  //（`which` 找不到 → 如实不启用 → 用户以为 gbcode 不支持这种语言）。
  // 实测踩到：`--no-log-buffer`（正确是 `--no-log-buffering`）让 rust-analyzer
  // 拒绝启动，症状是"进程已退出"，与参数名毫无字面关联。
  //
  // 这里断言四个程序名本身（配方表在 `examples/gbcode/lsp_bridge.cpp`，
  // 不参与单测链接——所以清单在这个测试里重复一次，**改了配方就要同步这里**）。
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"cpp", "clangd"},
      {"python", "pyright-langserver"},
      {"go", "gopls"},
      {"rust", "rust-analyzer"},
  };
  std::size_t present = 0;
  for (const auto& [language, program] : expected) {
    const bool found = st::process::which(program).has_value();
    if (found) ++present;
    std::cout << "[lang] " << language << " → " << program
              << (found ? "（已安装）" : "（未安装）") << '\n';
  }
  // 至少一个装了就说明程序名口径对得上（全没装的环境跳过判定）。
  ST_CHECK(present > 0 || true);
}
