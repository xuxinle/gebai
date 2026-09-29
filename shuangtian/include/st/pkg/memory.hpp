#pragma once

/// 并行编译的内存预算：把"并发多少路"从"有多少核"改成"有多少内存"。
///
/// ## 为什么需要
///
/// 编译是**内存密集型**的：实测本框架（`-O1 -g`）单个翻译单元峰值 60–490 MB，
/// sanitizer 档 630–700 MB；`nproc` 为 28 的机器上满并发就是 10–20 GB 的瞬时占用。
/// 在 cgroup 限 8 GiB 的容器里，结果是 `cc1plus` 被 OOM killer 杀掉——而**表现极具误导性**：
/// 被杀单元留下半个 `.o`，链接期报一堆 `.Lubsan_data` 未定义，看起来像编译器 bug 或
/// 链接脚本问题，实际根因是"并发路数 × 单单元内存"撞了内存上限。
///
/// 更麻烦的是**容量与核数无关**：容器限 8 GiB 而宿主机 114 GiB 时，`free` 看起来毫无压力，
/// 只有读 cgroup 才知道真实上限。因此这里：
///
/// 1. 探测**可用内存上限**（cgroup v2 `memory.max` → v1 `memory.limit_in_bytes` → 系统内存）；
/// 2. 按"档位单单元内存估算"推导默认并发：`jobs = (预算 - 预留) / 单单元估算`；
/// 3. 超大翻译单元（源文件 ≥ `kLargeUnitBytes`）再走**独立窄闸门**（默认 1），
///    避免它们彼此叠加——即便它们的峰值与体积不成正比，串行化也把最坏情形钉住了。
///
/// 显式 `--jobs` 仍然完全接管（不参与推导），便于 CI 固定行为。

#include <cstdint>
#include <string>

namespace st::pkg {

/// 系统/容器内存上限（MiB）。`source` 说明数字从哪来（诊断用）。
struct MemoryLimit {
  std::uint64_t limit_mb{0};  ///< 0 = 探测失败（调用方应退回"按核数"）
  std::string source{};       ///< 如 `cgroup v2 /sys/fs/cgroup/memory.max`
};

/// 探测可用内存上限：cgroup v2 → cgroup v1 → `/proc/meminfo` → `sysconf`。
/// 环境变量 `ST_MEMORY_MB` 可显式覆盖（0/非法值忽略）。
[[nodiscard]] auto detect_memory_limit() -> MemoryLimit;

/// 单翻译单元内存估算（MiB），按档位区分：sanitizer 档显著更高（实测 630–700 MB）。
[[nodiscard]] auto unit_memory_estimate_mb(std::string_view profile) -> std::uint64_t;

/// 超大单元判定阈值（字节）：超过它的源文件走窄闸门。
///
/// 取值依据：本框架 60 余个源文件里，除 `third_party/quickjs/quickjs.c`（2.1 MB / 6.5 万行）
/// 外都在 120 KB 以内——阈值取 512 KB 只命中真正的大块头，不会把普通单元误判。
inline constexpr std::uint64_t kLargeUnitBytes = 512ULL * 1024ULL;

/// 判定某源文件是否属于超大翻译单元（读不到大小则按"否"处理——不因此拒绝编译）。
[[nodiscard]] auto is_large_unit(std::string_view path) -> bool;

/// 并发决策结果（诊断/日志用，说明"为什么是这个数"）。
struct ConcurrencyPlan {
  std::size_t jobs{1};        ///< 并行编译单元数
  std::size_t jobs_large{1};  ///< 超大单元的并发上限
  std::uint64_t budget_mb{0}; ///< 采用的预算（0=未知）
  std::string reason{};       ///< 如 `内存预算 8192MiB / 单单元 512MiB（san）→ 13` 或 `--jobs 显式指定`
};

/// 推导并发方案。
///
/// - `requested_jobs != 0`：显式指定，直接采用（`reason` 说明），仍推导 `jobs_large`；
/// - 否则按内存预算推导；预算未知时退回 `hardware_concurrency`（并说明是退回）。
[[nodiscard]] auto plan_concurrency(std::size_t requested_jobs, std::size_t requested_jobs_large,
                                    std::uint64_t requested_budget_mb, std::string_view profile,
                                    std::size_t hardware) -> ConcurrencyPlan;

}  // namespace st::pkg
