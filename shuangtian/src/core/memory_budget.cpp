/// 内存预算档位（`ST_MEM_BUDGET`）的解析与单点常量——见 `memory_budget.hpp` 的说明。
///
/// 为何单独一个 TU：`budget()` 有一处进程级缓存（静态局部），而 L8 关心的正是
/// "随手写的可变全局"。放在这里聚成一处、附理由，比散在三个模块里各读一次环境变量好。

#include "st/core/memory_budget.hpp"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>

namespace st::memory {

auto budget() noexcept -> Budget {
  // 静态局部（C++11 起保证线程安全的一次性初始化）。它只是**环境的快照**，
  // 不含业务状态；缓存它让"读一次"这个语义确定（见头文件说明）。
  // lint-allow: L8 环境快照（进程生命周期内不变；缓存它以避免每个缓存构造都查 getenv）
  static const Budget snapshot = [] {
    const auto value = st::fs::read_env("ST_MEM_BUDGET");
    if (!value.has_value()) return Budget::Normal;
    // 大小写不敏感地认 `low`（环境变量的大小写习惯太杂，为一个档位名惩罚用户不值得）
    std::string lowered = *value;
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered == "low" ? Budget::Low : Budget::Normal;
  }();
  return snapshot;
}

auto scale_cache_limit(std::size_t normal_bytes) noexcept -> std::size_t {
  if (budget() == Budget::Normal) return normal_bytes;
  constexpr std::size_t kFloor = 1U * 1024U * 1024U;   // 地板：见头文件的说明
  const std::size_t scaled = normal_bytes / 4U;
  return scaled < kFloor ? kFloor : scaled;
}

auto budget_name() noexcept -> const char* {
  return budget() == Budget::Low ? "low" : "normal";
}

}  // namespace st::memory
