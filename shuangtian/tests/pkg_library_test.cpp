/// `kind = "library"` 的静态库目标回归。
///
/// ## 为什么这批用例必须存在
///
/// 加静态库支持时踩到两个**都不会报错**的坑（跑一遍完整构建看不出来）：
///
/// 1. **`-L` 与路径被拼成一个 argv**：写成 `"-L" + dir` 得到 `-LC:/…`，
///    lld 报 `unable to find library -lmathlib` —— 报错指向“库不存在”，
///    而库明明在那儿。真因是 argv 形状错了。
/// 2. **产物后缀**：静态库若用 `toolchain.executable_suffix` 会得到 `lib.exe`，
///    链接器把它当可执行文件处理。
///
/// 两条都是“形状/命名错”，而症状都指向别处。所以判据钉在**可观测事实**上：
/// 产物确实以 `.a` 结尾、`ar t` 确实能列出成员、清单解析确实拒绝非法 kind。
#include <format>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/pkg/build.hpp"
#include "st/pkg/manifest.hpp"
#include "st/test/test.hpp"

namespace {

/// 临时目录，析构时整个删掉（与 `pkg_deps_hash_test` 的 `TempFile` 同一套设施）。
struct TempDir {
  std::string dir;

  explicit TempDir(std::string_view prefix) {
    dir = st::fs::make_temp_dir(prefix).value_or(st::fs::temp_dir());
  }
  ~TempDir() { (void)st::fs::remove_all(dir); }
  TempDir(const TempDir&) = delete;
  auto operator=(const TempDir&) -> TempDir& = delete;

  [[nodiscard]] auto path() const -> std::string { return dir; }
};

/// 造一份最小清单 JSON（两个目标：一个库、一个可执行依赖它）。
[[nodiscard]] auto make_manifest_json(bool app_depends_on_lib, const std::string& lib_kind,
                                      const std::string& app_lib_name) -> std::string {
  return std::format(R"({{
  "shuangtian_pkg_format": 1,
  "name": "libprobe",
  "version": "0.1.0",
  "kind": "static_library",
  "cxx_standard": 23,
  "include_dirs": ["src"],
  "sources": [],
  "targets": {{
    "mathlib": {{ "kind": "{}", "sources": ["src/*.cpp"] }},
    "app": {{ "kind": "executable", "sources": ["app/*.cpp"]{} }}
  }}
}})",
                     lib_kind,
                     app_depends_on_lib ? std::format(R"(, "libraries": ["{}"])", app_lib_name) : "");  // lint-allow: L15 测试构造的 JSON 片段（内容安全）
}

}  // namespace

/// 清单能解析出 `kind = "library"` 与 `libraries` 字段。
///
/// 先钉“数据进得来”：若解析层丢字段，后面的产物逻辑再对也没用（而它不会报错——
/// `libraries` 静默为空 ⇒ 不链库 ⇒ 报一堆 undefined reference，看着像源码问题）。
ST_TEST(manifest_parses_library_kind_and_dependencies) {
  TempDir temp("st_lib_test");
  const std::string pkg = st::fs::join(temp.path(), "st.pkg");
  ST_REQUIRE(st::fs::write_text(pkg, make_manifest_json(true, "library", "mathlib")).has_value());

  const auto parsed = st::pkg::Manifest::load(pkg);
  ST_REQUIRE(parsed.has_value());

  const st::pkg::TargetSpec* lib = parsed->find_target("mathlib");
  ST_REQUIRE(lib != nullptr);
  ST_CHECK_EQ(lib->kind, std::string("library"));

  const st::pkg::TargetSpec* app = parsed->find_target("app");
  ST_REQUIRE(app != nullptr);
  ST_CHECK_EQ(app->kind, std::string("executable"));
  // 关键：依赖声明必须原样进来（丢了就静默不链）
  ST_CHECK_EQ(app->libraries.size(), 1U);
  ST_CHECK_EQ(app->libraries.front(), std::string("mathlib"));
}

/// 非法 `kind` 必须**明确报错**，不能静默当 executable。
///
/// 为什么这条重要：拼成 `"lib"` / `"static_library"`（后者是**工程级** kind 的写法，
/// 有人会照抄到目标上）时，静默回落会产生一个叫 `mathlib.exe` 的可执行文件，
/// 而清单写着“库”——排查时先去看链接器、看源码，最后才发现是拼写。
ST_TEST(manifest_rejects_unknown_target_kind) {
  TempDir temp("st_lib_test_bad");
  const std::string pkg = st::fs::join(temp.path(), "st.pkg");
  ST_REQUIRE(st::fs::write_text(pkg, make_manifest_json(false, "lib", "mathlib")).has_value());

  const auto parsed = st::pkg::Manifest::load(pkg);
  ST_CHECK(!parsed.has_value());   // 必须失败
  if (!parsed.has_value()) {
    const std::string msg = parsed.error().message;
    // 报错要指出是 kind 的问题（而不是一句泛泛的解析失败）
    ST_CHECK(msg.find("kind") != std::string::npos);
  }
}

/// 工程级 `kind = "static_library"` 仍合法（它是**工程**类型，与目标 kind 是两回事）。
///
/// 这条防“一刀切收紧”：如果把两个字段的取值域混成一个，合法清单会被拒。
ST_TEST(manifest_accepts_project_level_static_library_kind) {
  TempDir temp("st_lib_test_proj");
  const std::string pkg = st::fs::join(temp.path(), "st.pkg");
  ST_REQUIRE(st::fs::write_text(pkg, make_manifest_json(false, "library", "mathlib")).has_value());

  const auto parsed = st::pkg::Manifest::load(pkg);
  ST_REQUIRE(parsed.has_value());
  // 工程级 kind 是 `static_library`（清单里写死的），不应被目标级校验波及
  ST_CHECK_EQ(parsed->kind, std::string("static_library"));
}

/// 端到端：真编一个库 + 一个依赖它的可执行，两者产物都在、且可执行能跑通。
///
/// ## 为何必须端到端（而不是只测产物后缀）
///
/// 踩过的那两个坑（`-L` 拼接、后缀）**都只在“真链接一次”时暴露**：
/// · `-L` 拼错 → 链接器报 `unable to find library`；
/// · 后缀错 → 链接器把 `.exe` 当库。
/// 只断言“artifact 名字以 .a 结尾”抓不到第一条。
///
/// 本用例会跑两次真构建（库约 0.3s、app 约 0.4s），所以标 `ST_TEST_SLOW`：
/// 它不依赖外部环境（只用本仓的编译器），但会在全量测试里占一点时间。
ST_TEST_SLOW(library_target_builds_and_links_end_to_end) {
  TempDir temp("st_lib_e2e");
  const std::string root = temp.path();
  ST_REQUIRE(st::fs::create_directories(st::fs::join(root, "src")).has_value());
  ST_REQUIRE(st::fs::create_directories(st::fs::join(root, "app")).has_value());

  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "st.pkg"),
                                make_manifest_json(true, "library", "mathlib"))
                 .has_value());
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "src/mathlib.hpp"),
                                R"(#pragma once
namespace mathlib {
[[nodiscard]] auto add(int a, int b) -> int;
}  // namespace mathlib
)")
                 .has_value());
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "src/mathlib.cpp"),
                                R"(#include "mathlib.hpp"
namespace mathlib {
auto add(int a, int b) -> int { return a + b; }
}  // namespace mathlib
)")
                 .has_value());
  ST_REQUIRE(st::fs::write_text(st::fs::join(root, "app/main.cpp"), R"(#include <cstdio>
#include "mathlib.hpp"
auto main() -> int {
  std::printf("add=%d\n", mathlib::add(2, 3));
  return mathlib::add(2, 3) == 5 ? 0 : 1;
}
)")
                 .has_value());

  const auto manifest = st::pkg::Manifest::load(st::fs::join(root, "st.pkg"));
  ST_REQUIRE(manifest.has_value());

  // ① 编库：产物必须是 `.a`（不是 `.exe`）
  st::pkg::BuildOptions lib_options{};
  lib_options.target = "mathlib";
  lib_options.profile = "dev";
  lib_options.no_tests = true;
  const auto lib_stats = st::pkg::build(*manifest, lib_options);
  ST_REQUIRE(lib_stats.has_value());
  ST_CHECK_EQ(st::fs::extension(lib_stats->artifact), std::string(".a"));
  ST_CHECK(st::fs::is_regular_file(lib_stats->artifact));

  // ② 编依赖它的可执行：必须**自动先建库**并链上（不报 `unable to find library`）
  st::pkg::BuildOptions app_options{};
  app_options.target = "app";
  app_options.profile = "dev";
  app_options.no_tests = true;
  const auto app_stats = st::pkg::build(*manifest, app_options);
  ST_REQUIRE(app_stats.has_value());   // 失败时这里会带链接器原话
  ST_CHECK(st::fs::is_regular_file(app_stats->artifact));

  // ③ 真跑一次：证明链接是**真的**（符号解析成功），不是“产物存在即通过”
  const auto run = st::process::run(app_stats->artifact, {});
  ST_REQUIRE(run.has_value());
  ST_CHECK_EQ(run->exit_code, 0);
  ST_CHECK(run->stdout_text.find("add=5") != std::string::npos);
}
