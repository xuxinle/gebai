// `include_third_party_sources`（目标级第三方源白名单）的回归：字段必须能被解析、
// 并在序列化往返中保留。
//
// ## 为什么必须有（工程级全量的实测代价）
//
// `third_party_sources` 是工程级的：一家进清单，所有目标都编它。对走按需链接
// （归档 + gc-sections）的目标，多余的第三方只是白编几个单元；但对**直铺 .o**
// 的链接路径（自举脚本 bootstrap.ps1/sh），没人引用的第三方成员会被强制全链——
// 而它们引用的移植层符号在（被 include_sources 正确滤掉的）框架源里，链接期报
// `undefined reference`（实测：micropython 进清单后自举立即可复现，
// `gc_collect`/`mp_hal_stdout_tx_strn_cooked`/`nlr_jump_fail`）。
//
// 因此这条用例钉住两件事：
// ① 清单里写了 `include_third_party_sources` → 解析后有值（否则白名单静默失效，
//    自举链接继续报错，且报错指向源码、方向被带偏）；
// ② 序列化往返保留它（Manifest::write 会把清单再写出去，丢了就回退）。

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

/// 最小清单文本；`tool` 目标带 `include_third_party_sources`。
[[nodiscard]] auto manifest_text() -> std::string {
  return R"({
  "name": "probe",
  "version": "0.1.0",
  "kind": "executable",
  "sources": ["src/*.cpp"],
  "third_party_sources": ["third_party/quickjs/*.c", "third_party/sqlite/*.c",
                          "third_party/micropython/py/*.c"],
  "targets": {
    "tool": {
      "kind": "executable",
      "sources": ["tools/*.cpp"],
      "include_third_party_sources": ["third_party/quickjs/*", "third_party/sqlite/*"]
    }
  }
})";
}

/// 一份**没有** `include_third_party_sources` 的最小清单。
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

ST_TEST(pkg_manifest_parses_target_include_third_party_sources) {
  const auto parsed = parse_manifest(manifest_text());
  ST_CHECK(parsed.has_value());
  const auto* target = parsed->find_target("tool");
  ST_CHECK(target != nullptr);
  // 白名单必须被读到：空值会让该目标退化成“编全部第三方源”（自举链接失败的方向）
  ST_CHECK(target->include_third_party_sources.size() == 2);
  ST_CHECK(target->include_third_party_sources[0] == "third_party/quickjs/*");
  ST_CHECK(target->include_third_party_sources[1] == "third_party/sqlite/*");
}

ST_TEST(pkg_manifest_target_without_include_third_party_stays_empty) {
  // 没写 `include_third_party_sources` 的目标必须保持空 ——
  // 空是「工程级全量」的判据。若这里错误地回填了默认值，所有既有目标都会
  // 突变成白名单语义（第三方被静默裁掉、链接报错才可发现）。
  const auto parsed = parse_manifest(plain_manifest_text());
  ST_CHECK(parsed.has_value());
  const auto* target = parsed->find_target("app");
  ST_CHECK(target != nullptr);
  ST_CHECK(target->include_third_party_sources.empty());
}

ST_TEST(pkg_manifest_include_third_party_sources_survives_round_trip) {
  // 序列化往返：`Manifest::write` 会把清单再写出去，
  // 丢掉这个字段就等于下次构建静默回退成工程级全量（自举链接再次失败）。
  const auto parsed = parse_manifest(manifest_text());
  ST_CHECK(parsed.has_value());
  const auto reparsed = Manifest::parse_json(parsed->to_json(), "/probe");
  ST_CHECK(reparsed.has_value());
  const auto* target = reparsed->find_target("tool");
  ST_CHECK(target != nullptr);
  ST_CHECK(target->include_third_party_sources.size() == 2);
  ST_CHECK(target->include_third_party_sources[0] == "third_party/quickjs/*");
}
