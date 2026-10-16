#pragma once

/// 子进程环境合并的**纯函数**（平台无关语义）——POSIX 分支 env 覆盖语义与
/// Windows `build_environment` 对齐的单一事实源（BACKLOG 315）。
///
/// 为什么放 `src/` 而不是 `include/st/`：这是 `platform_process.cpp` 的内部实现
/// 细节，不属于公共 API（公共面 `st::process::run` 的 `Options::env` 契约不变）；
/// 测试经 `../src/core/process_env.hpp` 直取——先例：`tests/ui_input_selection_test.cpp`
/// 之与 `src/ui/components/components_internal.hpp`。
///
/// 为什么提炼：POSIX 分支曾把 overrides **追加**在 `environ` 之后——`getenv` 取
/// **首个**同名条目 ⇒ 覆盖静默失效（Windows 侧一直是对的）。语义藏两处就必然
/// 走偏（这次就是），提炼后两平台的键匹配规则、剔除/拼接次序都在这 一个函数里
/// 钉死，`tests/core_process_env_test.cpp` 两平台本机都能跑。

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace st::process {

/// 环境条目 `"KEY=value"` 的键部分（首个 `=` 之前；无 `=` 时整条都是键——
/// POSIX 允许这种畸形条目，键比较按整条进行，不会误伤其他条目）。
[[nodiscard]] auto env_entry_key(std::string_view entry) -> std::string_view;

/// 合并子进程环境：**overrides 优先**。
///
/// 语义（与 Windows `build_environment` 一致，两侧都以此为准）：
/// 1. `base` 中键被 `overrides` 命中的条目**不进结果**——同名键只留 overrides 的值
///    （追加两份时生效的是哪一份由各平台查找顺序决定，POSIX `getenv` 取首个，
///    恰好绕开注入的新值，表现为"覆盖静默失效"）；
/// 2. overrides 条目在前、未命中的 `base` 条目按原序在后（顺序确定性只为可测；
///    剔除后同名键至多一份，先后不影响取值）；
/// 3. 键匹配的大小写敏感性由 `case_insensitive` 决定：Windows 传 `true`
///    （`Path` 与 `PATH` 是同一个键，一并剔除）；POSIX 传 `false`
///    （两个不同的键，`PATH` 被覆盖时 `Path` 原样保留）。
[[nodiscard]] auto merge_environment(const std::vector<std::string>& base,
                                     const std::map<std::string, std::string>& overrides,
                                     bool case_insensitive) -> std::vector<std::string>;

}  // namespace st::process
