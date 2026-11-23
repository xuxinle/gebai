// ============================================================================
// 包级依赖图（`st::pkg::PackageGraph`）——回归用例
//
// ## 钉住什么
//
// 按需编译的正确性完全压在这张图上：算错一条边 ⇒ 裁掉一个实际被需要的包 ⇒
// 链接期 undefined reference；算多一条边 ⇒ 少裁几个，只是收益变小。
// 所以这里逐条钉住"图该怎么算"，而不是只测一个端到端路径。
//
// 四条是用**实测反例**钉的（都不是假想）：
//   ① 单元归属必须"同名头优先"——`src/geom/gltf.cpp` 实现的是 `st/raster/gltf.hpp`，
//      按目录判会归到 `st/geom`，于是 gallery 会裁掉它其实必需的单元；
//   ② 循环依赖必须按强连通分量收缩，而不是要求零环——
//      `st/ui` ↔ `st/ui/components` 是真实存在的环；
//   ③ 闭包必须含 roots 自身（否则"根包自己"会被判成不需要）；
//   ④ 分量内的包必须同生共死（要么全留要么全裁）。
// ============================================================================

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "st/pkg/graph.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::package_of_header;
using st::pkg::PackageGraph;

/// 造一个"读 include"的假实现：按文件路径查表，未登记的文件视为无 include。
auto make_reader(const std::map<std::string, std::set<std::string>>& table)
    -> PackageGraph::IncludeReader {
  return [table](const std::string& file) -> std::set<std::string> {
    if (const auto found = table.find(file); found != table.end()) return found->second;
    return {};
  };
}

}  // namespace

ST_TEST(pkg_graph_package_of_header_uses_directory_layer) {
  // 包 = 目录层，不含文件名。三层与两层都要正确。
  ST_CHECK_EQ(package_of_header("st/ui/components/button.hpp"), std::string("st/ui/components"));
  ST_CHECK_EQ(package_of_header("st/ui/ui_root.hpp"), std::string("st/ui"));
  ST_CHECK_EQ(package_of_header("st/core/error.hpp"), std::string("st/core"));
  // 顶层单文件（`st/xxx.hpp`）：没有目录层，返回空（调用方跳过）。
  ST_CHECK_EQ(package_of_header("st/pch.hpp"), std::string("st"));
}

ST_TEST(pkg_graph_unit_owner_prefers_same_name_header) {
  // ① 实测反例：`src/geom/gltf.cpp` 实现的是 `st/raster/gltf.hpp`，
  // 按目录判会归 `st/geom`，而那会把 gallery 必需的单元判成"可裁"。
  const auto lookup = [](std::string_view stem) -> std::optional<std::vector<std::string>> {
    if (stem == "gltf") return std::vector<std::string>{"st/raster/gltf.hpp"};
    // `panel` 有**两个**分居不同目录的同名头，且都不在 `st/app`：无法樱定，应回退。
    if (stem == "panel") {
      return std::vector<std::string>{"st/core/panel.hpp", "st/ui/panel.hpp"};
    }
    if (stem == "button") {
      return std::vector<std::string>{"st/ui/button.hpp", "st/raster/button.hpp"};
    }
    return std::nullopt;
  };
  ST_CHECK_EQ(st::pkg::package_of_unit("src/geom/gltf.cpp", lookup), std::string("st/raster"));
  // ② 没有同名头 → 回退到目录同名（`platform_*` 那类不公开头的单元）。
  const auto nothing = [](std::string_view) -> std::optional<std::vector<std::string>> {
    return std::nullopt;
  };
  ST_CHECK_EQ(st::pkg::package_of_unit("src/core/platform_time.cpp", nothing),
              std::string("st/core"));
  // ③ 同目录也有同名头时，优先同目录（不要跳到别处）。
  ST_CHECK_EQ(st::pkg::package_of_unit("src/ui/button.cpp", lookup), std::string("st/ui"));
  // ④ 多个同名头但都不在同目录 → 无法判定，回退目录同名（宁可多编不可错裁）。
  ST_CHECK_EQ(st::pkg::package_of_unit("src/app/panel.cpp", lookup), std::string("st/app"));
}

ST_TEST(pkg_graph_builds_basic_edges) {
  const std::vector<std::string> headers{
      "include/st/a/a.hpp",
      "include/st/b/b.hpp",
  };
  const std::vector<std::string> units{
      "src/a/a.cpp",
      "src/b/b.cpp",
  };
  // a 的头引 b → 包 a 依赖包 b
  const auto reader = make_reader({
      {"include/st/a/a.hpp", {"st/b/b.hpp"}},
  });
  const auto graph = PackageGraph::build(headers, units, reader);
  ST_CHECK_EQ(graph.packages().size(), std::size_t{2});
  ST_CHECK(graph.dependencies().at("st/a").contains("st/b"));
  ST_CHECK(!graph.dependencies().at("st/b").contains("st/a"));
  // 纯 DAG：没有多包分量
  ST_CHECK(graph.cycles().empty());
}

ST_TEST(pkg_graph_handles_cycles_via_components) {
  // ② 实测反例的形状：ui ↔ ui/components 双向引用。
  const std::vector<std::string> headers{
      "include/st/ui/ui_root.hpp",
      "include/st/ui/components/panel.hpp",
  };
  const std::vector<std::string> units{};
  const auto reader = make_reader({
      {"include/st/ui/ui_root.hpp", {"st/ui/components/panel.hpp"}},
      {"include/st/ui/components/panel.hpp", {"st/ui/ui_root.hpp"}},
  });
  const auto graph = PackageGraph::build(headers, units, reader);
  // 环被识别成一个多包分量（而不是报错/死循环）
  const auto cycles = graph.cycles();
  ST_CHECK_EQ(cycles.size(), std::size_t{1});
  ST_CHECK_EQ(cycles.front().size(), std::size_t{2});
  // 两个包同属一个分量
  ST_CHECK_EQ(*graph.component_of("st/ui"), *graph.component_of("st/ui/components"));
  // ④ 分量内同生共死：任取其一，闭包都含两者
  const auto from_ui = graph.closure({"st/ui"});
  ST_CHECK(from_ui.contains("st/ui"));
  ST_CHECK(from_ui.contains("st/ui/components"));
  const auto from_component = graph.closure({"st/ui/components"});
  ST_CHECK(from_component.contains("st/ui"));
}

ST_TEST(pkg_graph_closure_includes_roots_and_transitive) {
  const std::vector<std::string> headers{
      "include/st/a/a.hpp",
      "include/st/b/b.hpp",
      "include/st/c/c.hpp",
      "include/st/d/d.hpp",
  };
  const auto reader = make_reader({
      {"include/st/a/a.hpp", {"st/b/b.hpp"}},
      {"include/st/b/b.hpp", {"st/c/c.hpp"}},
      // d 孤立
  });
  const auto graph = PackageGraph::build(headers, {}, reader);
  const auto reachable = graph.closure({"st/a"});
  // ③ 闭包含 roots 自身
  ST_CHECK(reachable.contains("st/a"));
  // 传递可达
  ST_CHECK(reachable.contains("st/b"));
  ST_CHECK(reachable.contains("st/c"));
  // 不可达的不进来
  ST_CHECK(!reachable.contains("st/d"));
}

ST_TEST(pkg_graph_closure_survives_diamond) {
  // 菱形依赖：a→b, a→c, b→d, c→d。闭包不应重复计数，也不该漏 d。
  const std::vector<std::string> headers{
      "include/st/a/a.hpp", "include/st/b/b.hpp", "include/st/c/c.hpp", "include/st/d/d.hpp"};
  const auto reader = make_reader({
      {"include/st/a/a.hpp", {"st/b/b.hpp", "st/c/c.hpp"}},
      {"include/st/b/b.hpp", {"st/d/d.hpp"}},
      {"include/st/c/c.hpp", {"st/d/d.hpp"}},
  });
  const auto graph = PackageGraph::build(headers, {}, reader);
  const auto reachable = graph.closure({"st/a"});
  ST_CHECK_EQ(reachable.size(), std::size_t{4});
}

ST_TEST(pkg_graph_units_for_filters_by_package) {
  const std::vector<std::string> headers{
      "include/st/a/a.hpp",
      "include/st/b/b.hpp",
  };
  const std::vector<std::string> units{
      "src/a/a.cpp",
      "src/b/b.cpp",
  };
  const auto graph = PackageGraph::build(headers, units, make_reader({}));
  const auto only_a = graph.units_for({"st/a"});
  ST_CHECK_EQ(only_a.size(), std::size_t{1});
  ST_CHECK_EQ(only_a.front(), std::string("src/a/a.cpp"));
  const auto both = graph.units_for({"st/a", "st/b"});
  ST_CHECK_EQ(both.size(), std::size_t{2});
}

ST_TEST(pkg_graph_unit_owner_uses_self_include_when_ambiguous) {
  // 实测反例（jianmu 链接失败的真实原因）：
  // `src/geom/animation.cpp` 实现 `st/raster/animation.hpp`，但仓里**同时存在**
  // `st/ui/animation.hpp` —— “唯一同名头”与“同目录同名头”两条规则双双失效。
  // 决定性的证据在那个 cpp 自己的 `#include`：它引的是 `st/raster/animation.hpp`。
  const auto lookup = [](std::string_view stem) -> std::optional<std::vector<std::string>> {
    if (stem == "animation") {
      return std::vector<std::string>{"st/raster/animation.hpp", "st/ui/animation.hpp"};
    }
    return std::nullopt;
  };
  const auto self_includes = [](const std::string& file) -> std::set<std::string> {
    if (file == "src/geom/animation.cpp") return {"st/raster/animation.hpp"};
    return {};
  };
  const st::pkg::IncludeReader reader = self_includes;
  // ① 有 self_include 时按它定（而不是回退到 `st/geom`）
  ST_CHECK_EQ(st::pkg::package_of_unit("src/geom/animation.cpp", lookup, &reader),
              std::string("st/raster"));
  // ② 不传 self_includes 时仍回退目录同名（旧行为保留，便于对比）
  ST_CHECK_EQ(st::pkg::package_of_unit("src/geom/animation.cpp", lookup), std::string("st/geom"));
  // ③ self_include 指向的头不在同名候选里 → 不采信（防拼错路径）
  const st::pkg::IncludeReader lying = [](const std::string&) -> std::set<std::string> {
    return {"st/nope/x.hpp"};
  };
  ST_CHECK_EQ(st::pkg::package_of_unit("src/geom/animation.cpp", lookup, &lying),
              std::string("st/geom"));
}

ST_TEST(pkg_graph_unknown_root_is_ignored) {
  // 起点包不在图里（拼错/已删）不应崩，也不该影响其余闭包。
  const std::vector<std::string> headers{"include/st/a/a.hpp"};
  const auto graph = PackageGraph::build(headers, {}, make_reader({}));
  const auto reachable = graph.closure({"st/nonexistent", "st/a"});
  ST_CHECK(reachable.contains("st/a"));
  ST_CHECK(!reachable.contains("st/nonexistent"));
}
