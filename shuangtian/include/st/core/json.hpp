#pragma once

/// JSON 能力：**自研实现**（`json_value.hpp` + `json_parse.hpp` + `json.cpp`）。
///
/// ## 为什么不再用第三方库
///
/// 本框架曾用 nlohmann/json 3.12.0。它不是"不好用"，而是**接口语义与本框架的错误契约冲突**，
/// 且冲突方式是"编译不报错、运行期炸"（`CONVENTIONS.md` §3.8、`docs/PITFALLS.md` 都有实测记录）：
///
/// | 旧库行为 | 本仓实测后果 |
/// |---|---|
/// | `at()` 键缺失抛 `out_of_range` | 框架无通用异常边界 → `std::terminate`（鼠标事件修饰键缺席把整个应用干掉） |
/// | const `operator[]` 键缺失是 UB | 取可选字段时崩溃/读随机内存（控制通道回包构造失败） |
/// | 花括号造数组 | `Json x{Json::object()}` → `[{}]`，之后按对象用即抛 `type_error.305` |
/// | `get<T>()` 类型不符抛 `type_error` | 用例里未捕获异常带走整个测试进程 |
/// | 只给字节偏移的解析错误 | 人和模型都按行列定位，排障时靠猜 |
///
/// 自研实现的四条承诺（**可测**，见 `tests/core_json_test.cpp`）：
///
/// 1. **零抛出**：除内存耗尽外没有任何成员会抛。缺键/越界/类型不符一律退化为空值——
///    `operator[]` 缺键返回共享 null 节点（**不是 UB**），`as_i64()` 对字符串返回 0。
/// 2. **无歧义构造**：`Json{}` = null、`Json::object()` = `{}`、`Json::array()` = `[]`；
///    没有 `initializer_list` 构造，所以花括号永远不会变成数组。
/// 3. **保序**：对象按插入顺序存储在 `vector` 里（清单/lock 要被人读、被 git diff）。
/// 4. **数字保真**：整数/无符号/浮点分开存。`2^53+1`、雪花 ID、`uint64` 最大值逐位往返；
///    `1.0` 不写成 `1`、`-0.0` 保持负零（"是浮点"本身是语义）。
///
/// ## 用法约定
///
/// - **构造**：`Json::object()` / `Json::array()` / `Json::array({...})`，或直接赋标量。
/// - **读**：`json_get_string/i64/double/bool/string_array`、`json_find`、`json_at`、`json_path`
///   ——都不抛，缺键给默认值或 null 节点。
/// - **写**：`node["k"] = v`（对象）、`arr.push_back(v)` / `arr[i] = v`（数组）。
/// - **遍历**：`for (const auto& [key, value] : node.items())`（对象）、
///   `for (const auto& item : arr)`（数组）。
///
/// ⚠ 与旧库的三处**有意不同**（迁移时唯一要注意的）：
/// - `Json x{...}` 现在是**编译错误**（没有花括号构造）——想要数组写 `Json::array({...})`。
///   这是刻意的：宁可编译期报错，也不要运行期得到 `[{}]`。
/// - `.get<T>()` 不存在：用 `as_i64()` / `as_double()` / `as_string()` / `as_bool()`，
///   或带默认值的 `json_get_*`（前者"缺了给 0"，后者"缺了给你指定的值"）。
/// - `.value(key, fallback)` 不存在：用 `json_get_* (node, key, fallback)`。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json_fwd.hpp"   // 前向声明（本头同时提供，保留是为了老代码的包含不断）
#include "st/core/json_parse.hpp" // 解析/序列化实现（含 json_value.hpp）
#include "st/core/json_value.hpp"

namespace st {

/// 解析嵌套深度上限（防栈溢出：递归下降解析器遇到极深嵌套会爆栈）。
inline constexpr std::uint32_t kJsonMaxDepth = 128;

/// 解析 JSON 文本。错误：`Parse`（消息含**行/列/字节 + 附近内容**）。
[[nodiscard]] auto json_parse(std::string_view text) -> Result<Json>;

/// 读取并解析 JSON 文件。错误：`NotFound` / `Io` / `Parse`。
[[nodiscard]] auto json_parse_file(std::string_view path) -> Result<Json>;

/// 写出 JSON 文件（`pretty` 为真时缩进 2 空格）。错误：`Io`。
auto json_write_file(std::string_view path, const Json& value, bool pretty = true) -> Status;

/// 紧凑/美化序列化（**绝不抛**：非法 UTF-8 被替换为 U+FFFD，NaN/Infinity 写成 null）。
[[nodiscard]] auto json_dump(const Json& value, bool pretty = false, std::uint32_t indent = 2)
    -> std::string;

/// 追加序列化到已有字符串（避免中间拷贝）。
auto json_dump_to(std::string& out, const Json& value, bool pretty, std::uint32_t indent = 2) -> void;

// ————————————————————————————————————————————————————————————————————————————
// 安全读取（类型不符 = 取默认值，不抛异常）
// ————————————————————————————————————————————————————————————————————————————

/// 子节点为字符串时按字符串取值，否则给默认值（YAML 风格的宽松整数/布尔字面量也能读）。
///
/// 声明在前：下面 `json_get_*` 是 inline 的，需要先看到这些签名。
[[nodiscard]] auto json_as_string(const Json& value, std::string_view fallback = {}) -> std::string;
/// 数字 → `int64`（整型直读；浮点判断无小数部分才转换，避免静默截断）。
[[nodiscard]] auto json_as_i64(const Json& value, std::int64_t fallback = 0) -> std::int64_t;
[[nodiscard]] auto json_as_double(const Json& value, double fallback = 0.0) -> double;
[[nodiscard]] auto json_as_bool(const Json& value, bool fallback = false) -> bool;

/// 查子节点（不存在或非对象返回 `nullptr`；返回指针指向 `value` 内部，生命周期同它）。
///
/// 不知道用哪个时用这个：`nullptr` 与“有值但值为 null”是两回事，它都答得了。
///
/// ## ⚠ 指针有效期：**任何结构变更后都可能失效**
///
/// 对象/数组底层是 `std::vector`（保序的代价），所以：
///
/// | 操作 | 旧指针 |
/// |---|---|
/// | 只读（`find` / `json_at` / `contains` / `dump` / 遍历） | 有效 |
/// | 插入新键、`push_back`（触发扩容时） | **失效**（元素被搬到新内存） |
/// | `erase` / `erase_index` | **失效**（后面的元素前移） |
///
/// 实测：插入 200 个键后，原 `find("a")` 的地址已变。这与 `std::vector` 的
/// 迭代器规则一致——**存的不是索引，而是地址**。
///
/// 使用纪律：跨越任何写入操作时，重新 `find` 一次；或先用 `size()` 预留好容量。
///
/// ## 为何是 inline 定义而非 .cpp 导出
/// 这是全部读取路径的**唯一入口**（`json_at` / `json_get_*` / `json_path` 全经它）。
/// 放 `.cpp` 里跨 TU 不能内联，而内联的真正收益是**字面量键的比较特化**：
/// 同一实现、同一数据，`.cpp` 版实测 8.2 ns/次；内联后 5.0 ns/次（与 header-only 的旧库同级）。
[[nodiscard]] inline auto json_find(const Json& value, std::string_view key) noexcept -> const Json* {
  return value.find(key);
}

/// 取子节点引用；不存在时返回静态 null 节点（可安全读取、可 `is_null()` 判断）。
///
/// ⚠ **不要用 `json_at(x,k).is_null()` 判断“键在不在”**：键不存在与键显式为 `null`
/// 在这个调用上长得一模一样（都返回 null 节点）。要分辨就改用 `json_find`。
/// `json_at` 的定位是“我确定要链式读，缺了也无所谓”。
[[nodiscard]] inline auto json_at(const Json& value, std::string_view key) noexcept -> const Json& {
  const Json* found = value.find(key);
  return found != nullptr ? *found : Json::null_node();
}

[[nodiscard]] inline auto json_get_string(const Json& value, std::string_view key,
                                          std::string_view fallback = {}) -> std::string {
  return json_as_string(json_at(value, key), fallback);
}
[[nodiscard]] inline auto json_get_i64(const Json& value, std::string_view key,
                                       std::int64_t fallback = 0) -> std::int64_t {
  return json_as_i64(json_at(value, key), fallback);
}
[[nodiscard]] inline auto json_get_double(const Json& value, std::string_view key,
                                          double fallback = 0.0) -> double {
  return json_as_double(json_at(value, key), fallback);
}
[[nodiscard]] inline auto json_get_bool(const Json& value, std::string_view key,
                                        bool fallback = false) -> bool {
  return json_as_bool(json_at(value, key), fallback);
}
/// 取字符串数组（非字符串元素被跳过；非数组返回空）。
[[nodiscard]] auto json_get_string_array(const Json& value, std::string_view key)
    -> std::vector<std::string>;

/// 路径式取值：`json_path(doc, "spec.source.kind")`——任一层缺失即返回静态 null 节点。
[[nodiscard]] auto json_path(const Json& value, std::string_view dotted_path) noexcept -> const Json&;

// ————————————————————————————————————————————————————————————————————————————
// “键到底在不存” —— 三种不可互相替代的问法
// ————————————————————————————————————————————————————————————————————————————
//
// 为什么值得单独开一节：这是 JSON 读写里最容易写错、且**错了不报错**的地方。
// 三种状态在不少 API 上长得一样，但业务含义完全不同：
//
//   {"k": null}   ← 作者**写了**这个键，明确表达“没有”
//   {}            ← 作者**没提**这个键，应该用默认值
//   {"k": 0}      ← 有值，就是这个值（不是没值）
//
// | 问法 | 缺失 | 显式 null | 有值 |
// |---|---|---|---|
// | `json_has(x,"k")` | false | **true** | true |
// | `json_find(x,"k")` | nullptr | 指针 | 指针 |
// | `json_at(x,"k").is_null()` | true | true | false |  ← 前两种分不开

/// 键**存在与否**（显式 `null` 也算存在）。
///
/// 与 `contains` 等价，但名字说清了“存在”而不是“有值”——
/// 读代码的人不会把它误看成“非空”。
[[nodiscard]] inline auto json_has(const Json& value, std::string_view key) noexcept -> bool {
  return value.find(key) != nullptr;
}

/// 键存在**且值非 null**（即“真的给了个东西”）。
///
/// 这是“取可选字段”时通常真正想问的：`{"detail": null}` 与 `{}` 在业务上都算“没给”。
/// 需要区分它们时再自己用 `json_find`。
[[nodiscard]] inline auto json_has_value(const Json& value, std::string_view key) noexcept -> bool {
  const Json* found = value.find(key);
  return found != nullptr && !found->is_null();
}

// 刻意**没有** `json_missing()`：
// 写成 `!json_missing(x,"k")` 的地方，读代码时要多转一次弯。`!json_has(...)` 直接可读。

}  // namespace st
