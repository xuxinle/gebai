#pragma once

/// `st::Json` 的**轻量前向声明**头（结构评审 B5）。
///
/// ## 为什么需要它
///
/// `st/ext/json.hpp` 会 `#include "nlohmann/json.hpp"`（**25,526 行**，预处理展开
/// 到 13.8 万行）。而 `st::Json` 被 8 个公共头用在**纯签名**位置（参数与返回值）上：
/// `pkg/manifest.hpp`、`pkg/registry.hpp`、`ui/actions.hpp`、`ui/dsl.hpp`、
/// `ui/script_host.hpp`、`ui/declarative_host.hpp`、`control/control.hpp`、`ext/script.hpp`。
///
/// 于是**任何**包含其中之一的编译单元都要付这份代价，实测：
///
/// | 包含 | 预处理行数 | 解析耗时 |
/// |---|---|---|
/// | `st/math/geometry.hpp`（不含 Json） | 约 1 千行 | **0.23s** |
/// | `st/pkg/registry.hpp`（经 json.hpp） | 138,435 行 | **1.83s** |
/// | `st/app/app.hpp` | 151,648 行 | **2.30s** |
///
/// 且 PCH **救不了它**：`st/pch.hpp` 刻意只放标准库、不放项目头
/// （见该头注释：把项目头塞进 PCH 实测让单 TU 从 ~3.5s 涨到 ~5.0s），
/// 所以这份代价是**每个 TU 真付**的。
///
/// ## 用法
///
/// - **只需要 `Json` 出现在签名里**（声明、成员、返回类型）→ 包含本头；
/// - **需要构造/访问 Json**（`Json::object()`、`json_get_*`、`obj["k"]`…）→ 包含
///   `st/ext/json.hpp`（它同时提供本头的内容）。
///
/// ## 为什么自己写而不是用 nlohmann 的 `json_fwd.hpp`
///
/// third_party 里 vendor 的 nlohmann 只带了单头 `json.hpp`（其内部的
/// `INCLUDE_NLOHMANN_JSON_FWD_HPP_` 分支存在，但没有独立分发文件）。为了**不改动
/// third_party**（`third_party/SOURCES.md` 有 SHA-256 台账，改上游文件会破坏可校验性），
/// 这里按上游 `json_fwd.hpp` 的同样口径**重复声明**四个名字。
///
/// ⚠ 硬约束：本头的声明必须与 vendor 的 `nlohmann/json.hpp` 逐字一致
/// （`basic_json` 的模板参数表、`ordered_map` 的四参数、两个 `using`）。
/// 一致性由 `tests/ext_json_test.cpp` 的 `json_fwd_is_abi_compatible_with_full_header`
/// 钉住——那条用例同时包含两个头，任何偏差都会变成**重定义/类型不匹配的编译错误**，
/// 而不是默默分叉成两个类型。

// 命名空间与上游一致（nlohmann 3.x 的 ABI 内联命名空间；见 json.hpp 的
// NLOHMANN_JSON_NAMESPACE_BEGIN）。
//
// ⚠ **必须**用上游自己的守卫宏 `INCLUDE_NLOHMANN_JSON_FWD_HPP_`：
// nlohmann 的 `json.hpp` 内部就是用它包住同一段前向声明的
// （见 vendor 副本的 3495–3562 行）。带上它之后，"先含 json_fwd.hpp 再含
// json.hpp"（或反过来）都不会重复声明；不带则默认模板实参会冲突（实测编译错误：
// "redefinition of default argument"）。这不是取巧——**这就是上游 json_fwd.hpp
// 的设计**（vendored 副本只是没有单独分发那个文件）。
#ifndef INCLUDE_NLOHMANN_JSON_FWD_HPP_
#define INCLUDE_NLOHMANN_JSON_FWD_HPP_

// 与上游 json_fwd.hpp 相同的依赖集（它默认模板实参里用到这些名字）。
#include <cstdint>  // int64_t, uint64_t
#include <map>      // map
#include <memory>   // allocator
#include <string>   // string
#include <vector>   // vector

namespace nlohmann {
inline namespace json_abi_v3_12_0 {

/// 序列化器前向（`basic_json` 的默认模板实参用到）。
template <typename T = void, typename SFINAE = void>
struct adl_serializer;

template <template <typename U, typename V, typename... Args> class ObjectType = std::map,
          template <typename U, typename... Args> class ArrayType = std::vector,
          class StringType = std::string, class BooleanType = bool,
          class NumberIntegerType = std::int64_t, class NumberUnsignedType = std::uint64_t,
          class NumberFloatType = double,
          template <typename U> class AllocatorType = std::allocator,
          template <typename T, typename SFINAE = void> class JSONSerializer = adl_serializer,
          class BinaryType = std::vector<std::uint8_t>, class CustomBaseClass = void>
class basic_json;

/// JSON Pointer（`basic_json` 内部要用；**必须一并声明**）。
/// 漏了它则：本头先包含时它定义了守卫，`json.hpp` 自己的前向块被跳过 →
/// 编译到 json.hpp 内部时报 “`json_pointer` is not a member of `nlohmann`”。
template <typename RefStringType>
class json_pointer;

/// 默认特化（与上游同名）。
using json = basic_json<>;

/// 保序的 map-like 容器（`ordered_json` 的对象类型）。
template <class Key, class T, class IgnoredLess, class Allocator>
struct ordered_map;

/// 本框架统一使用的 JSON 类型（保序）。
using ordered_json = basic_json<nlohmann::ordered_map>;

}  // namespace json_abi_v3_12_0
}  // namespace nlohmann

#endif  // INCLUDE_NLOHMANN_JSON_FWD_HPP_

namespace st {

/// 本框架统一使用的 JSON 类型 = nlohmann 的**保序** JSON。
/// 与 `st/ext/json.hpp` 里的定义是同一条 `using`（那个头包含本头，不是重复定义）。
using Json = nlohmann::ordered_json;

/// 空的 JSON 对象（`{}`）——**在只含前向声明的头里取得 `Json` 实参的唯一正道**。
///
/// 为什么需要它：`Json` 在公共头里是前向声明，而 `Json::object()` / `Json` 作为**成员**
/// 都要求完整类型。把构造收进这个在 `.cpp`（有完整类型）里定义的函数，头就只需要签名。
///
/// **返回 `const&` 而非按值——这是关键，不要改回按值。**
/// 按值返回会让**每一个调用它的模板体**都要求 `Json` 完整类型（按值传参要构造临时对象）；
/// 而返回引用不要求（绑定引用不需要完整类型）。实测（GCC 13/14 对照）：
/// `dsl.hpp` 的 `custom<T>` 里传 `empty_json_object()`，
/// 按值返回时**实例化即报 incomplete type**，改返回引用后**零错误**。
/// 后果就是 `custom<T>` 的使用方（如 `examples/gbcode/main.cpp`）
/// 被迫额外包含 `st/ext/json.hpp`——而 `dsl.hpp` 切前向头的收益正是为了免掉这个代价，
/// `88ba665` 就是因此漏补了 examples/ 而编不过。返回引用从根上消除该需求。
///
/// 典型用法：`st::Json extra_fields = empty_json_object();`（拷贝一份成员，语义不变）。
[[nodiscard]] auto empty_json_object() -> const Json&;

}  // namespace st
