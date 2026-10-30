#pragma once

/// @file scene_schema.hpp
/// @brief **场景脚本的字段契约**：白名单、类型、范围、常见错误。
///
/// ## 为什么单独一个数据层（而不是散在解析代码里）
///
/// 这份表要同时服务四件事：
///
/// 1. **未知字段检查**（拼错 `raduis` 必须报错并给候选，不能静默用默认值）；
/// 2. **机读能力清单**（`st api --json`，让模型查表而不是猜）；
/// 3. **自检摘要**（`describe_scene` 指出"可能的配置问题"）；
/// 4. **反向序列化**（知道哪些字段该写出来）。
///
/// 如果把它拆成四份（检查一份、文档一份、自检一份、序列化一份），
/// 那四份迟早会漂移——而"文档说有这个字段、代码其实没读"是最难查的一类问题。
/// 所以**只有一份表**，四件事都从它推。
///
/// ## 一条刻意的取舍：只报错，不自动修
///
/// 拼错字段时给出候选（"你是不是想写 radius"），但**绝不自动替换**。
/// 自动修会让调用方失去校正机会：一旦猜错（`radius` 与 `rings` 只差两字符），
/// 就变成一个新的静默错误。而模型拿到"未知字段 raduis，候选 radius"，
/// 一次就能改对——那比任何自动修复都可靠。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json_fwd.hpp"

namespace st::raster {

/// 一个字段的规格。
struct FieldSpec {
  /// 字段名（JSON 里的键）。
  std::string name{};
  /// 类型：`number` / `bool` / `string` / `array` / `object` / `vec3` / `color`
  ///
  /// `vec3` 是"长度为 3 的数字数组"的简短说法（`translate` / `rotate` / `scale`）；
  /// `color` 是 `"#rrggbb"` 字符串或 `[r,g,b]`。
  std::string type{};
  /// 是否必填。
  bool required{false};
  /// 取值范围说明（人读；空 = 不限）。例：`>= 1`、`0..1`、`"linear"/"ease-in"`
  std::string range{};
  /// 一句话说明（进 `st api --json` 给人/模型看）。
  std::string summary{};
  /// 示例值（JSON 片段，进清单）。
  std::string example{};
  /// 常见错误（进清单；这些都是实际踩过的）。
  std::vector<std::string> pitfalls{};
};

/// 一层对象的字段清单。
struct FieldTable {
  /// 这层是什么（`"scene"` / `"objects[]"` / `"animation.tracks[]"` …），用于报错定位。
  std::string scope{};
  std::vector<FieldSpec> fields{};
  /// 是否允许**任意**键（`extensions` 这类开放字典）。
  bool open{false};

  [[nodiscard]] auto find(std::string_view name) const -> const FieldSpec*;
  [[nodiscard]] auto names() const -> std::vector<std::string>;
};

/// 全部层级的字段表（按 `scope` 索引）。
///
/// 顺序固定（顶层 → objects → animation → …），序列化与清单都依赖它。
[[nodiscard]] auto scene_field_tables() -> const std::vector<FieldTable>&;

/// 按 `scope` 取表；不存在返回 `nullptr`。
[[nodiscard]] auto find_field_table(std::string_view scope) -> const FieldTable*;

/// **编辑距离**（Levenshtein，只算到 `limit` 为止——超了就返回 `limit+1`）。
///
/// 用于"你是不是想写 X"的候选。截断的理由：字段名都很短，
/// 距离超过 3 就没有参考价值，而完整计算对长字符串是 O(n·m)。
[[nodiscard]] auto edit_distance(std::string_view a, std::string_view b, int limit = 3) -> int;

/// 从候选里挑最接近 `typed` 的几个（距离 ≤ `max_distance`，按距离升序）。
///
/// ## 为什么返回多个而不是一个
///
/// `radius` 与 `rings` 只差两个字符，单给一个候选有时会指错方向。
/// 给 2~3 个让调用方（尤其模型）自己判断，比给一个错的好。
[[nodiscard]] auto suggest_fields(std::string_view typed,
                                  const std::vector<std::string>& candidates,
                                  int max_distance = 2, std::size_t max_suggestions = 3)
    -> std::vector<std::string>;

/// 校验一个对象的键是否都在白名单里。
///
/// @param value     要检查的 JSON 对象
/// @param table     该层的字段表
/// @param path      该层在文档里的路径（`"objects[0]"`），用于报错定位
/// @return 全部合法时 `ok()`；否则返回**第一个**未知字段的错误，
///         消息里带路径、未知字段名、最接近的候选。
[[nodiscard]] auto check_unknown_fields(const Json& value, const FieldTable& table,
                                        std::string_view path) -> Status;

}  // namespace st::raster
