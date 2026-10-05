/// 独立工程（引用 framework）的**集成级**回归：真建一个最小工程，真构建、真跑起来。
///
/// ## 为什么必须有这一层（框架单测两层都漏掉了同一个缺陷）
///
/// 框架单测与 `st test` 覆盖不到这条路径——它们验的是**框架自己**怎么构建，
/// 而缺陷恰恰只在「**引用方工程**怎么构建框架」时才显形。实测踩到的两个都在本用例覆盖内：
///
/// 1. **框架 `c_flags` 被调用方工程的顶掉**：`make_framework_flags` 曾取调用方的
///    `manifest.c_flags`（独立工程一般是**空**的）而不是框架的，于是框架单元里那个 C 源
///    （`third_party/sqlite/sqlite3.c`）丢了 `-include st_sqlite3_config.h` ⇒
///    `SQLITE_ENABLE_COLUMN_METADATA` 未定义 ⇒ `sqlite3_column_table_name` 在**链接期**
///    undefined reference（`src/ext/database.cpp` 老老实实调了它）。
///    关键：**框架自己构建时两者恰好相等**（同一个清单），所以只有独立工程能暴露。
/// 2. **CLI 不认 `--shots`**：驱动方启动应用的固定契约含 `--shots <dir>`，
///    共享 CLI 不认 → `Invalid` 退出 → 应用根本起不来
///    （`tests/app_cli_test.cpp` 钉解析那一半，本用例钉「模板工程真能跑」这一半）。
///
/// 所以判据是**产物能不能真跑起来**，不是「函数返回了 ok」：第 1 个缺陷在编译期**零症状**
/// （头文件里根本不声明那两个符号），任何不走到「链接 + 执行」的检查都会漏掉它。
///
/// ## 成本控制
///
/// 真构建 ≈ 10 s（83 单元，多数命中共享对象缓存；框架规模决定，本仓库实测）。
/// `ST_INTEGRATION_BUILD=0` 可关——但关掉时用例**明确跳过并打印原因**，不制造假绿信号。

#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/pkg/build.hpp"
#include "st/pkg/manifest.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::BuildOptions;

/// 框架根：`st test` 注入 `ST_TEST_FRAMEWORK_ROOT`；拿不到就**跳过而不是猜仓库布局**
/// （猜错比跳过更糟：会构造出一个假的失败）。
[[nodiscard]] auto framework_root() -> std::string {
  if (const char* from_env = std::getenv("ST_TEST_FRAMEWORK_ROOT");
      from_env != nullptr && *from_env != '\0') {
    return std::string(from_env);
  }
  return {};
}

/// 这个源文件同时是拼装用的**片段**（不是最终格式串）：所有 `{}` 都是字面量。
constexpr std::string_view kManifestTemplate = R"JSON({
  "name": "st_integration",
  "version": "0.1.0",
  "kind": "executable",
  "framework": { "path": "@FRAMEWORK@" },
  "sources": [],
  "targets": { "st_integration": { "kind": "executable", "sources": ["src/*.cpp"] } },
  "dependencies": { "modules": [], "source": [], "system": [] }
}
)JSON";

/// 最小应用：只做两件"必须能过"的事——
/// ① 声明 `st::ext::Database` 并用到它的元信息接口（**链接** sqlite3.c 的那条路径）；
/// ② 解析通用命令行（触到 `--shots` 那条路径）；成功打印标记并以 0 退出。
constexpr std::string_view kMainSource = R"CPP(// 由 tests/pkg_integration_test.cpp 生成的最小独立工程。
#include <cstdio>

#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/ext/database.hpp"

auto run_app(int argc, char** argv) -> int {
  st::ext::Database database;   // 链接 sqlite3.c：缺 COLUMN_METADATA 时在链接期炸
  st::app::CommonOptions options;
  if (auto parsed = st::app::parse_common_options(argc, argv, options); !parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
    return 1;
  }
  std::printf("integration-ok\n");
  return 0;
}

ST_MAIN(run_app)
)CPP";

/// 执行产物并返回退出码（-1 = 没能启动）。**参数用数组传**，不经 shell——
/// 免得给路径加引号这件事把失败原因搅浑。
[[nodiscard]] auto run_artifact(const std::string& path, const std::vector<std::string>& arguments)
    -> int {
  const auto result = st::process::run(path, arguments);
  if (!result) {
    st::print("[独立工程] 无法执行产物: {}\n", result.error().to_string());
    return -1;
  }
  if (!result->stdout_text.empty()) {
    st::print("[独立工程] 产物输出: {}\n", result->stdout_text);
  }
  if (!result->stderr_text.empty()) {
    st::print("[独立工程] 产物 stderr: {}\n", result->stderr_text);
  }
  return result->exit_code;
}

}  // namespace

ST_TEST(independent_project_builds_links_and_runs) {
  if (const char* disabled = std::getenv("ST_INTEGRATION_BUILD");
      disabled != nullptr && std::string_view(disabled) == "0") {
    st::print("[独立工程] ST_INTEGRATION_BUILD=0 → 跳过（**非通过**：真构建被显式关闭）\n");
    return;
  }
  const std::string framework = framework_root();
  if (framework.empty()) {
    st::print("[独立工程] 未提供 ST_TEST_FRAMEWORK_ROOT → 跳过\n");
    return;
  }
  if (!st::fs::is_regular_file(st::fs::join(framework, "st.pkg"))) {
    st::print("[独立工程] {} 下没有 st.pkg → 跳过\n", framework);
    return;
  }

  // 临时工程目录：每次跑都从零开始（残留的 build/ 会让"到底编了什么"不可知；
  // 共享对象缓存另有 ST_HOME 管，不影响正确性）
  const std::string root = st::fs::join(
      st::fs::temp_dir(), std::format("st-integration-{}", st::process::current_id()));
  st::print("[独立工程] 生成并构建 {}（framework={}）\n", root, framework);
  (void)st::fs::remove_all(root);
  ST_REQUIRE(st::fs::create_directories(st::fs::join(root, "src")).has_value());

  const std::string manifest_text =
      st::replace_all(kManifestTemplate, "@FRAMEWORK@", framework);
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "st.pkg"), manifest_text).has_value());
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "src/main.cpp"), std::string(kMainSource))
                 .has_value());

  auto manifest = st::pkg::Manifest::load(st::fs::join(root, "st.pkg"));
  ST_REQUIRE(manifest.has_value());

  BuildOptions options;
  options.root = root;
  options.target = "st_integration";
  options.profile = "dev";
  const auto stats = st::pkg::build(*manifest, options);
  if (!stats) {
    // 构建失败就是本用例要抓的东西，不能静默跳过
    st::print("[独立工程] 构建失败: {}\n", stats.error().to_string());
    ST_CHECK(false);
    return;
  }
  ST_CHECK(stats->units_total > 0);
  ST_CHECK(stats->linked);
  ST_REQUIRE(!stats->artifact.empty());
  ST_CHECK(st::fs::is_regular_file(stats->artifact));

  // ① 产物真能跑（链接期缺陷到这里才显形）
  ST_CHECK_EQ(run_artifact(stats->artifact, {}), 0);
  // ② 驱动方的完整契约参数也能跑（`--shots` 是其中最容易漏的一个）。
  //    注意分工：「`--shots` 被吸收且落到了 `screenshot_dir`」由 `app_cli_test.cpp` 钉
  //    （那需要读到 `CommonOptions`）；这里钉的是「带上它整条启动链路不报错退出」。
  ST_CHECK_EQ(
      run_artifact(stats->artifact,
                   {"--headless", "--control-port", "0", "--shots", root + "/shots"}),
      0);
}
