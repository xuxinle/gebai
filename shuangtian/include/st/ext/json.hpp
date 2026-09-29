#pragma once

/// JSON 能力：**基于 nlohmann/json**（`vendor/nlohmann/json.hpp`，v3.12.0，MIT）。
///
/// 取代原先的自研 `st::json`。替换动因（实测数据见 `CONVENTIONS.md` §3.7）：
/// 旧实现把所有数字统一存 `double`，`9007199254740993`(2^53+1) 会被写成 `9007199254740992`、
/// 雪花 ID `1234567890123456789` 变 `1.2345678901234568e+18` 且 `as_i64()` 差 21；
/// nlohmann 分整型/浮点存储，往返保真。
///
/// ## 为什么还要这一层薄封装
///
/// 不是重新实现 JSON，而是**把 nlohmann 的抛异常接口收敛成 `Result` 风格**：
/// nlohmann 的 `at()` / `value()` / `get<T>()` 在「键缺失或类型不匹配」时**抛异常**，
/// 而本框架的错误处理契约是 `Result`（`CONVENTIONS.md` §3.1：不用异常做业务错误）。
/// 若直接在业务代码里四处 `try/catch`，既冗长又容易漏。因此：
///
/// - **解析/写盘**：`json_parse` / `json_parse_file` / `json_write_file` → `Result` / `Status`；
/// - **读取**：`json_get_*` / `json_find` / `json_at` —— 永不抛异常，类型不符即退化为默认值
///   （对应"配置写错了不该让进程崩"的失败安全姿态）；
/// - **构造**：直接用 nlohmann 原生接口（`json::object()`、`obj["k"] = v`、`push_back`），不再包一层。
///
/// 注意：`nlohmann::json` 的 **const `operator[]` 在键不存在时是未定义行为**，
/// 因此读取一律走 `json_find` / `json_at` / `json_get_*`，不要写 `const_json["k"]`。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "st/core/error.hpp"

namespace st {

/// 本框架统一使用的 JSON 类型。
///
/// 用 **`ordered_json`**（保留键的插入顺序）而非默认 `json`（按字典序排序）：
/// 清单与 lock 文件是要**被人读、被 git diff** 的产物，键序稳定比排序更重要——
/// 排序会让一次无关的读取-回写产生满屏 diff。
using Json = nlohmann::ordered_json;

/// 解析嵌套深度上限（防栈溢出：递归下降解析器遇到极深嵌套会爆栈）。
/// 与旧自研实现保持一致（128），并由 `json_parse` 在解析前线性预检。
inline constexpr std::uint32_t kJsonMaxDepth = 128;

/// 解析 JSON 文本。错误：`Parse`（含 nlohmann 的"第 N 字节"诊断）。
[[nodiscard]] auto json_parse(std::string_view text) -> Result<Json>;

/// 读取并解析 JSON 文件。错误：`NotFound` / `Io` / `Parse`。
[[nodiscard]] auto json_parse_file(std::string_view path) -> Result<Json>;

/// 写出 JSON 文件（`pretty` 为真时缩进 2 空格）。错误：`Io`。
auto json_write_file(std::string_view path, const Json& value, bool pretty = true) -> Status;

/// 紧凑/美化序列化（**绝不抛**：非法 UTF-8 以 U+FFFD 替换而非抛 type_error）。
[[nodiscard]] auto json_dump(const Json& value, bool pretty = false, std::uint32_t indent = 2)
    -> std::string;

/// 追加序列化到已有字符串（避免中间拷贝）。
auto json_dump_to(std::string& out, const Json& value, bool pretty, std::uint32_t indent = 2) -> void;

// ————————————————————————————————————————————————————————————————————————————
// 安全读取（类型不符 = 取默认值，不抛异常）
// ————————————————————————————————————————————————————————————————————————————

/// 查子节点（不存在或非对象返回 `nullptr`；返回指针指向 `value` 内部，生命周期同它）。
[[nodiscard]] auto json_find(const Json& value, std::string_view key) noexcept -> const Json*;

/// 取子节点引用；不存在时返回静态 null 节点（可安全读取、可 `is_null()` 判断）。
[[nodiscard]] auto json_at(const Json& value, std::string_view key) noexcept -> const Json&;

[[nodiscard]] auto json_get_string(const Json& value, std::string_view key,
                                   std::string_view fallback = {}) -> std::string;
[[nodiscard]] auto json_get_i64(const Json& value, std::string_view key, std::int64_t fallback = 0)
    -> std::int64_t;
[[nodiscard]] auto json_get_double(const Json& value, std::string_view key, double fallback = 0.0)
    -> double;
[[nodiscard]] auto json_get_bool(const Json& value, std::string_view key, bool fallback = false)
    -> bool;
/// 取字符串数组（非字符串元素被跳过；非数组返回空）。
[[nodiscard]] auto json_get_string_array(const Json& value, std::string_view key)
    -> std::vector<std::string>;

/// 路径式取值：`json_path(doc, "spec.source.kind")`——任一层缺失即返回静态 null 节点。
[[nodiscard]] auto json_path(const Json& value, std::string_view dotted_path) noexcept -> const Json&;

/// 子节点为字符串时按字符串取值，否则给默认值（YAML 风格的宽松整数/布尔字面量也能读）。
[[nodiscard]] auto json_as_string(const Json& value, std::string_view fallback = {}) -> std::string;
/// 数字 → `int64`（整型直读；浮点判断无小数部分才转换，避免静默截断）。
[[nodiscard]] auto json_as_i64(const Json& value, std::int64_t fallback = 0) -> std::int64_t;
[[nodiscard]] auto json_as_double(const Json& value, double fallback = 0.0) -> double;
[[nodiscard]] auto json_as_bool(const Json& value, bool fallback = false) -> bool;

}  // namespace st
