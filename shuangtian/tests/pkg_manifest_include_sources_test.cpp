// `include_sources`（目标级白名单）的回归：字段必须能被解析、并在序列化往返中保留。
//
// ## 为什么必须有
//
// `include_sources` 是**白名单**语义：非空时目标只编这些 glob，`src/**` 其余一概不进。
// 它的价值在于「漏写会链接报错（可发现）」，而不是像 `exclude_sources` 那样
// 漏一条就静默多编几个单元——实测 `st` 目标逐项排了 13 条，仍因
// `src/ui/window_control.cpp` 间接依赖 `math/geometry.hpp` 把 math 拉了回来。
//
// 因此这条用例钉住两件事：
// ① 清单里写了 `include_sources` → 解析后有值（否则白名单静默失效，退化成全编）；
// ② 序列化往返保留它（`Manifest::write` 会把清单再写出去，丢了就回退）。

#include <optional>
#include <string>

#include "st/core/json.hpp"
#include "st/pkg/manifest.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::Manifest;

/// 解析清单文本（`json_parse` 失败时返回 nullopt，供用例断言）。
[[nodiscard]] auto parse_manifest(const std::string& text) -> std::optional<Manifest> {
  const auto json = st::json_parse(text);
  if (!json) return std::nullopt;
  const auto manifest = Manifest::parse_json(*json, "/probe");
  if (!manifest) return std::nullopt;
  return *manifest;
}

/// 最小清单文本；`tool` 目标带 `include_sources`。
[[nodiscard]] auto manifest_text() -> std::string {
  return R"({
  "name": "probe",
  "version": "0.1.0",
  "kind": "executable",
  "sources": ["src/*.cpp"],
  "targets": {
    "tool": {
      "kind": "executable",
      "sources": ["tools/*.cpp"],
      "include_sources": ["src/pkg/*.cpp", "src/core/*.cpp"]
    }
  }
})";
}

/// 一份**没有** `include_sources` 的最小清单。
[[nodiscard]] auto plain_manifest_text() -> std::string {
  return R"({
  "name": "probe",
  "version": "0.1.0",
  "kind": "executable",
  "sources": ["src/*.cpp"],
  "targets": { "app": { "kind": "executable", "sources": ["app/*.cpp"] } }
})";
}

}  // namespace

ST_TEST(pkg_manifest_parses_target_include_sources) {
  const auto parsed = parse_manifest(manifest_text());
  ST_CHECK(parsed.has_value());
  const auto* target = parsed->find_target("tool");
  ST_CHECK(target != nullptr);
  // 白名单必须被读到：空值会让该目标退化成“编全部库源”（静默变胖）
  ST_CHECK(target->include_sources.size() == 2);
  ST_CHECK(target->include_sources[0] == "src/pkg/*.cpp");
  ST_CHECK(target->include_sources[1] == "src/core/*.cpp");
}

ST_TEST(pkg_manifest_target_without_include_sources_stays_empty) {
  // 没写 `include_sources` 的目标必须保持空 —— 空是「走黑名单/全编」的判据。
  // 若这里错误地回填了默认值，所有既有目标都会突变成白名单语义（构建静默变瘦）。
  const auto parsed = parse_manifest(plain_manifest_text());
  ST_CHECK(parsed.has_value());
  const auto* target = parsed->find_target("app");
  ST_CHECK(target != nullptr);
  ST_CHECK(target->include_sources.empty());
}

ST_TEST(pkg_manifest_include_sources_survives_round_trip) {
  // 序列化往返：`Manifest::write` 会把清单再写出去，
  // 丢掉这个字段就等于下次构建静默回退成全编。
  const auto parsed = parse_manifest(manifest_text());
  ST_CHECK(parsed.has_value());
  const auto reparsed = Manifest::parse_json(parsed->to_json(), "/probe");
  ST_CHECK(reparsed.has_value());
  const auto* target = reparsed->find_target("tool");
  ST_CHECK(target != nullptr);
  ST_CHECK(target->include_sources.size() == 2);
}
