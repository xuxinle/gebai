#pragma once

/// `st::Json` 的**轻量前向声明**头（结构评审 B5）。
///
/// ## 为什么需要它
///
/// `st/core/json.hpp` 带着完整的解析/序列化实现（本仓自研，不再依赖第三方单头）。
/// 而 `st::Json` 被一批公共头用在**纯签名**位置（参数与返回值）上：
/// `pkg/manifest.hpp`、`pkg/registry.hpp`、`ui/actions.hpp`、`ui/dsl.hpp`、`ui/script_host.hpp`、
/// `ui/declarative_host.hpp`、`control/control.hpp`、`ext/script.hpp`、`lsp/*`。
///
/// 那些 TU 不需要知道 `Json` 的布局，只需要"有这么个类型"。把它们切到本头后，
/// 编译单元不必解析完整实现（实测单 TU 预处理行数从 13.8 万降到千余行）。
///
/// ## 用法
///
/// - **只需要 `Json` 出现在签名里**（声明、成员指针、返回类型）→ 包含本头；
/// - **需要构造/访问 `Json`**（`Json::object()`、`json_get_*`、`obj["k"]`…）→ 包含
///   `st/core/json.hpp`（它同时提供本头的内容）。
///
/// ## `empty_json_object()` 为什么返回引用
///
/// **返回 `const&` 而非按值——这是关键，不要改回按值。**
/// 按值返回会让**每一个调用它的模板体**都要求 `Json` 完整类型（按值传参要构造临时对象）；
/// 返回引用不要求（绑定引用不需要完整类型）。实测（GCC 13/14 对照）：
/// `dsl.hpp` 的 `custom<T>` 里传 `empty_json_object()`，按值返回时**实例化即报 incomplete type**，
/// 改返回引用后零错误。典型用法：`st::Json extra_fields = empty_json_object();`

namespace st {

/// JSON 值（完整定义在 `st/core/json_value.hpp`，由 `st/core/json.hpp` 提供）。
///
/// **这里是真正的前向声明**（不再重复声明第三方模板）：自研实现只有一个具体类型，
/// 因此不存在"两处声明必须逐字一致"的脆弱约束。
class Json;

/// 空的 JSON 对象（`{}`）——**在只含前向声明的头里取得 `Json` 实参的唯一正道**。
///
/// 定义在 `src/core/json.cpp`（那里有完整类型）；返回引用，避免强制调用方要完整类型
/// （理由见文件头）。
[[nodiscard]] auto empty_json_object() -> const Json&;

}  // namespace st
