#pragma once

/// **内存预算单点**（2026-10-14 内存评估的落地）。
///
/// ## 为什么需要它
///
/// 框架里有一组**自设上限的缓存**，它们分散在各自模块里写死常量：
/// 字形位图 24 MiB + 整形结果 16 MiB（每个 `TextRenderer`）、SVG 图标 16 MiB（全局）。
/// 问题不在数字本身（都是合理的），而在于：
/// 1. **看不全**：想知道"这个进程最多可能为缓存吃多少内存"得翻三个文件；
/// 2. **改不动**：CI / 低配设备 / 内存敏感场景想声明式收缩，只能改代码重编。
///
/// 于是把上限收到一处，并给一个**声明式档位**（`ST_MEM_BUDGET=low|normal`）：
/// 低档把所有缓存压到 1/4，让"内存敏感部署"可以一条环境变量表达意图，
/// 而不是逐个模块打补丁。
///
/// ## 口径
///
/// - **默认 `normal`**：与改造前逐位一致（不改变任何现有行为）。
/// - **`low`**：缓存上限 ×0.25（字形 6 MiB / 整形 4 MiB / SVG 4 MiB）。
///   代价是字多的大界面会多命中淘汰（CPU 换内存）——**这笔账该由部署者来算**，
///   而不是由框架替他决定。
/// - 未知值按 `normal` 处理（并**不报错**：环境变量拼错不该让应用起不来；
///   与 `ST_SCALE` 等既有口径一致）。
///
/// ## 与"目标 20MiB"的关系
///
/// 报告实测最小应用 176MB（其中字体整文件驻留 ~86MB + GPU 驱动常驻 ~53MB），
/// 而缓存只有几十 MiB——**所以 `low` 档不是内存问题的主药**（主药是字体映射 +
/// 无头不建 GPU）。它的价值在 CI 与低配：那些场景下几十 MiB 也是可观的，
/// 而且"能声明式说清上限"本身就是可运维性。

#include <cstddef>

#include "st/core/fs.hpp"

namespace st::memory {

/// 预算档位。
enum class Budget {
  Normal,  ///< 默认：与历史行为一致
  Low,     ///< 内存敏感（CI / 低配）：所有缓存上限 ×0.25
};

/// 当前档位（读 `ST_MEM_BUDGET`；进程内**取一次后缓存**）。
///
/// 为何缓存而不是每次读环境：这些值在对象构造期被读（`TextRenderer` 的缓存构造、
/// SVG 缓存首次使用），而环境变量在进程生命周期内不变——每次 `getenv` 是白付。
/// 同时缓存让"改环境变量后行为不变"这件事是**确定的**（不会出现"构造早的实例
/// 用旧值、构造晚的用新值"）。
[[nodiscard]] auto budget() noexcept -> Budget;

/// 按档位缩放一个缓存上限（`normal` 原样返回；`low` 取 1/4，**至少 1 MiB**）。
///
/// 为何设地板：把 4 MiB 压到 1 MiB 以下会让正常界面频繁抖动（每帧重新光栅化），
/// 那时"省内存"换来的是"CPU 打满"——比多占几 MiB 更糟。地板保证低档仍然**可用**。
[[nodiscard]] auto scale_cache_limit(std::size_t normal_bytes) noexcept -> std::size_t;

/// 档位名（`normal` / `low`；用于日志与控制通道如实上报）。
[[nodiscard]] auto budget_name() noexcept -> const char*;

// —— 各缓存的**具名上限**（唯一权威：模块不该再写自己的魔数） ——

/// 字形位图缓存（每 `TextRenderer`）。
[[nodiscard]] inline auto glyph_cache_bytes() noexcept -> std::size_t {
  return scale_cache_limit(24U * 1024U * 1024U);
}

/// 整形结果缓存（每 `TextRenderer`）。
[[nodiscard]] inline auto shaped_cache_bytes() noexcept -> std::size_t {
  return scale_cache_limit(16U * 1024U * 1024U);
}

/// SVG 图标位图缓存（进程内全局，每个 `IconSet`）。
[[nodiscard]] inline auto svg_cache_bytes() noexcept -> std::size_t {
  return scale_cache_limit(16U * 1024U * 1024U);
}

/// 条目数硬上限也按同一比例压（否则低档只是"字节上限变小、条目数不变"，
/// 小位图海量时 map 开销照样在）。
///
/// **至少 1**：0 会让缓存完全失效（每帧重新光栅化）——那是缺陷不是省内存。
[[nodiscard]] inline auto scale_entry_limit(std::size_t normal_entries) noexcept -> std::size_t {
  const std::size_t ceiling = normal_entries == 0U ? 1U : normal_entries;
  if (budget() == Budget::Normal) return ceiling;
  const std::size_t scaled = ceiling / 4U;
  return scaled < 1U ? 1U : scaled;
}

}  // namespace st::memory
