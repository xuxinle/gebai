#pragma once

/// 自研 JSON：**解析 / 序列化**（`st/core/json.hpp` 的实现核心，勿直接包含）。
///
/// ## 为什么解析器是手写的而不是复用第三方
///
/// 见 `json_value.hpp` 文件头的替换动因。除去接口语义问题，手写解析器还换来了三件实际能力：
///
/// 1. **位置化诊断（行 + 列 + 字节）**：旧库只给 `byte` 偏移，而人和模型都按"第 3 行第 8 列"
///    定位——`PITFALLS.md` 里"AI 改错 JSON 后在元素间反复猜"的记录就是这种成本。
/// 2. **深度预检与解析共用一次扫描**：旧实现为了不让递归下降爆栈，在解析前做一遍线性预检，
///    再解析一遍（两次遍历）。这里在递归中直接计数，一次遍历即拦下（超限返回 `Parse` 错误，
///    不是把进程打崩）。
/// 3. **字符串构建零冗余**：无转义时直接切片（一次 `assign`），有转义才走解码路径——
///    UTF-8 中文占绝大多数字节，这条快路径是真实的吞吐收益。
///
/// ## 错误姿态
///
/// 解析失败返回 `Result<Json>`（`ErrorCode::Parse`），消息里带**行/列/字节 + 附近片段**。
/// 从不用异常：`CONVENTIONS.md` §3.1 的错误契约是 `Result`，且框架没有通用异常边界。

#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/core/json_value.hpp"

namespace st {
namespace json_detail {

/// 解析选项。
struct ParseOptions {
  /// 嵌套深度上限（防递归下降爆栈：极深嵌套会把进程打崩，那是 DoS 面）。
  std::uint32_t max_depth = 128;
  /// 是否把"超出 `Number.MAX_SAFE_INTEGER` 风格的整数"按原样保真。
  /// 本实现始终保真（整数走 int64/uint64），此字段保留给未来"遇超限整数即报错"的严格档。
  bool strict_numbers = false;
};

/// 解析 JSON 文本（**不抛异常**；失败返回 `Parse` 错误）。
[[nodiscard]] auto parse(std::string_view text, const ParseOptions& options) -> Result<Json>;

/// 序列化：紧凑（`indent == 0`）或缩进美化。
///
/// `ensure_ascii=false`（默认）：UTF-8 原样输出（中文可读、体积小）；
/// `ensure_ascii=true`：全部非 ASCII 转成 `\uXXXX`（跨语言/跨工具的保守形态）。
[[nodiscard]] auto dump(const Json& value, std::uint32_t indent, bool ensure_ascii) -> std::string;

/// 追加序列化到已有字符串（避免中间拷贝）。
auto dump_to(std::string& out, const Json& value, std::uint32_t indent, bool ensure_ascii) -> void;

}  // namespace json_detail
}  // namespace st
