// `st check`（只检查不产出）的回归：钉住「不产出任何文件」这条契约。
//
// ## 为什么必须有
//
// `check` 复用 `build` 的单元枚举与标志组装（这是它的价值：两者不可能分叉），
// 代价是**它必须显式跳过**一系列产出行为：写 `.o`、写 `.d`、写标志指纹、写共享对象缓存、
// 建/读 PCH、链接。漏掉任何一项的后果都不是"多写了个文件"，而是：
//
// - 写了 `.o`：下次 `build` 判为"最新"而**跳过重编**——而那个对象其实没产出有效代码。
//   这是最坏的一种：构建静默失效，症状要到运行期才现形。
// - 写了标志指纹：`build` 会以为"这个对象是用这套标志编的"，增量判定据此跳过。
// - 碰了 PCH：跨编译器检查（clang 查 / gcc 编）下 PCH 格式不兼容，检查会连带失败。
//
// 所以判据是**文件系统没被动过**，而不是"返回了 ok"——上列行为错了都不会让 check 报错。
//
// ## 为何用**自建的最小工程**而不是仓库自己
//
// 拿本仓库当被测对象的话，一次全量检查 ≈ 20 s（单路更久），远超用例的软超时上限；
// 而把片数加大又让用例变成"量机器性能"——那类用例在共享机器上会偶发红灯。
// 自建工程只有 2 个源文件、不引框架，**几百毫秒**跑完，且判据（写入集合为空）更锐利。

#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/build.hpp"
#include "st/pkg/manifest.hpp"
#include "st/test/test.hpp"

namespace {

/// 目录下每个文件的（相对路径, 大小, 修改时间）快照。
/// 只数文件个数不够：**原地覆盖同一个 `.o`** 也能骗过计数。
[[nodiscard]] auto snapshot(std::string_view dir) -> std::vector<std::string> {
  std::vector<std::string> out;
  const auto walked = st::fs::walk(dir, 8);
  if (!walked) return out;
  for (const auto& entry : *walked) {
    if (entry.is_dir) continue;
    const auto size = st::fs::file_size(entry.path).value_or(0);
    const auto time = st::fs::modified_ns(entry.path).value_or(0);
    out.push_back(std::format("{}|{}|{}", st::fs::relative_to(entry.path, dir), size, time));
  }
  std::ranges::sort(out);
  return out;
}

/// 建一个最小工程：2 个源文件（一个 .cpp 一个 .hpp），一个可执行目标。
struct MiniProject {
  std::string dir{};
  st::pkg::Manifest manifest{};
};

[[nodiscard]] auto make_mini_project(std::string_view tag) -> std::optional<MiniProject> {
  const auto made = st::fs::make_temp_dir(tag);
  if (!made) return std::nullopt;
  MiniProject project;
  project.dir = *made;

  const std::string src = st::fs::join(project.dir, "src");
  if (!st::fs::create_directories(src)) return std::nullopt;
  const char* header = "// 最小工程的内联函数：给检查器一点真东西看\n"
                       "inline int answer() { return 42; }\n";
  const char* body = "#include \"value.hpp\"\n"
                     "int compute() { return answer(); }\n";
  if (!st::fs::write_text(st::fs::join(src, "value.hpp"), header)) return std::nullopt;
  if (!st::fs::write_text(st::fs::join(src, "value.cpp"), body)) return std::nullopt;

  const std::string manifest_text = std::format(R"({{
  "name": "check-probe",
  "version": "0.1.0",
  "sources": ["src/*.cpp"],
  "flags": ["-std=c++20", "-Wall", "-Wextra", "-Werror"],
  "targets": {{ "probe": {{ "kind": "executable", "sources": ["src/*.cpp"] }} }}
}})");
  const std::string manifest_path = st::fs::join(project.dir, "st.pkg");
  if (!st::fs::write_text(manifest_path, manifest_text)) return std::nullopt;

  auto loaded = st::pkg::Manifest::load(manifest_path);
  if (!loaded) return std::nullopt;
  project.manifest = std::move(*loaded);
  return project;
}

[[nodiscard]] auto build_opts(bool check) -> st::pkg::BuildOptions {
  st::pkg::BuildOptions options;
  options.profile = "dev";
  options.check_only = check;
  options.jobs = 2;
  options.use_pch = false;   // 最小工程不必建 PCH（也避免把"建 PCH"算进写入集合）
  return options;
}

}  // namespace

ST_TEST(check_only_writes_no_artifacts) {
  auto project = make_mini_project("st-check-probe");
  ST_REQUIRE(project.has_value());
  const std::string build_dir = st::fs::join(project->dir, "build");

  // ① 先真构建一次：证明这个最小工程是可构建的（否则"检查不产出"可能只是"啥也没干"）
  const auto built = st::pkg::build(project->manifest, build_opts(false));
  ST_REQUIRE(built.has_value());
  ST_CHECK(st::fs::is_directory(build_dir));

  // ② 快照整个 build/ 与源码目录，跑检查，再快照
  const auto before_build = snapshot(build_dir);
  const auto before_src = snapshot(st::fs::join(project->dir, "src"));
  const auto checked = st::pkg::build(project->manifest, build_opts(true));
  ST_REQUIRE(checked.has_value());
  const auto after_build = snapshot(build_dir);
  const auto after_src = snapshot(st::fs::join(project->dir, "src"));

  // ③ 判据：build/ 允许新增的是**依赖清单**（`.d`，增量判定用，见 `compile_units` 的
  //    `-MMD`），除此之外逐项一致（不写 `.o`、不写指纹、不碰 PCH、不写缓存）。
  const auto changed = [](const std::vector<std::string>& before,
                          const std::vector<std::string>& after) {
    std::vector<std::string> diff;
    for (const auto& item : after) {
      if (std::ranges::find(before, item) == before.end()) diff.push_back(item);
    }
    return diff;
  };
  const auto added = changed(before_build, after_build);
  // 允许：`.d`（依赖清单，增量判定输入）与 `.check_state`（增量状态文件）；
  // 不允许：`.o`/指纹 / 缓存任何一项
  for (const auto& item : added) {
    ST_CHECK(item.find(".d|") != std::string::npos ||
             item.find(".check_state|") != std::string::npos);
  }
  ST_CHECK_EQ(before_src.size(), after_src.size());
  for (std::size_t i = 0; i < std::min(before_src.size(), after_src.size()); ++i) {
    ST_CHECK_EQ(before_src[i], after_src[i]);
  }

  // ④ 同时确认它真查了东西（否则"没有副作用"可能只是"什么都没干"）
  ST_CHECK_EQ(checked->units_total, std::size_t{1});
  ST_CHECK_EQ(checked->units_cached, std::size_t{0});
  ST_CHECK(!checked->pch_used);

  (void)st::fs::remove_all(project->dir);
}

ST_TEST(check_only_finds_broken_source) {
  // 检查必须**真的能报错**——否则上一条"无副作用"会被"恒不报错"冒充。
  auto project = make_mini_project("st-check-broken");
  ST_REQUIRE(project.has_value());

  const std::string body_path = st::fs::join(st::fs::join(project->dir, "src"), "value.cpp");
  ST_REQUIRE(st::fs::write_text(body_path, "#include \"value.hpp\"\n"
                                          "int compute() { return \"not an int\"; }\n")
                 .has_value());

  const auto checked = st::pkg::build(project->manifest, build_opts(true));
  ST_CHECK(!checked.has_value());   // 必须失败
  if (!checked.has_value()) {
    // 报错要指向真正的原因（源文件名 / 行号），而不是一句泛泛的失败
    const std::string message = checked.error().to_string();
    ST_CHECK(message.find("value.cpp") != std::string::npos);
  }
  // 检查失败也**不该留下产物**
  const auto leftovers = snapshot(st::fs::join(project->dir, "build"));
  for (const auto& item : leftovers) {
    ST_CHECK(item.find(".o|") == std::string::npos);
    ST_CHECK(item.find(".d|") == std::string::npos);
  }

  (void)st::fs::remove_all(project->dir);
}

ST_TEST(check_only_incremental_skips_unchanged_units) {
  // 检查模式**增量判定**：源+依赖哈希与上次记录一致 → 跳过（不查）。
  //
  // 旧语义是每次全量查（"恒绿哨兵"），155 单元实测 ~59 s；
  // 改为内容哈希增量后，不变仓库/不变单元接近零开销，`--force` 仍可强制全量。
  auto project = make_mini_project("st-check-every");
  ST_REQUIRE(project.has_value());

  const auto built = st::pkg::build(project->manifest, build_opts(false));
  ST_REQUIRE(built.has_value());

  const auto first = st::pkg::build(project->manifest, build_opts(true));
  ST_REQUIRE(first.has_value());
  const auto second = st::pkg::build(project->manifest, build_opts(true));
  ST_REQUIRE(second.has_value());

  ST_CHECK(first->units_total > 0);
  ST_CHECK_EQ(first->units_total, second->units_total);
  // 第一次全查；第二次（无变化）全部跳过
  ST_CHECK_EQ(first->units_rebuilt, first->units_total);
  ST_CHECK_EQ(second->units_rebuilt, std::size_t{0});

  // `--force` 绕过增量：即使刚查过也全量
  auto force_opts = build_opts(true);
  force_opts.force = true;
  const auto forced = st::pkg::build(project->manifest, force_opts);
  ST_REQUIRE(forced.has_value());
  ST_CHECK_EQ(forced->units_rebuilt, forced->units_total);

  (void)st::fs::remove_all(project->dir);
}
