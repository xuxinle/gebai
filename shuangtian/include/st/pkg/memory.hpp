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
#include <string_view>

namespace st::pkg {

/// 系统/容器内存上限（MiB）。`source` 说明数字从哪来（诊断用）。
struct MemoryLimit {
  std::uint64_t limit_mb{0};  ///< 0 = 探测失败（调用方应退回"按核数"）
  std::string source{};       ///< 如 `cgroup v2 /sys/fs/cgroup/memory.max`
};

/// 探测可用内存上限：cgroup v2 → cgroup v1 → Windows `GlobalMemoryStatusEx`（platform_memory.cpp）
/// → `/proc/meminfo`。环境变量 `ST_MEMORY_MB` 可显式覆盖（0/非法值忽略）。
[[nodiscard]] auto detect_memory_limit() -> MemoryLimit;

/// Windows 平台内存上限（MiB；非 Windows 返回 0）——总量与可用量取小。
/// 实现在 `src/pkg/platform_memory.cpp`（`<windows.h>` 只允许出现在 platform_*）。
[[nodiscard]] auto platform_memory_limit_mb() -> std::uint64_t;

/// 容器/系统的 **CPU 配额**（可用核数；0 = 不可知，调用方退回硬件并发）。
///
/// 与内存上限同理：`std::thread::hardware_concurrency()` 报的是**宿主机**核数，
/// 容器里真正能用的是 cgroup 配额。按宿主机核数开并发落在小配额的容器上就是纯亏损——
/// 每个 `cc1plus` 都被 CFS 限流，时间花在上下文切换上。实测本框架（4 核配额容器）：
/// 默认 10 路 109 s、6 路 101 s、4 路 **96.7 s**（越少越快）。
///
/// 探测顺序：cgroup v2 `cpu.max`（`quota period` 或 `max`）→ cgroup v1 `cpu.cfs_quota_us`/
/// `cpu.cfs_period_us` → 0（不可知）。取整向上（零头也值得占一个 worker）。
[[nodiscard]] auto detect_cpu_quota() -> std::size_t;

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
  std::size_t cpu_quota{0};   ///< 采用的 CPU 配额核数（0=不可知）
  std::string reason{};       ///< 如 `内存预算 8192MiB / 单单元 512MiB（san）→ 13` 或 `--jobs 显式指定`
};

/// 推导并发方案。
///
/// - `requested_jobs != 0`：显式指定，直接采用（`reason` 说明），仍推导 `jobs_large`；
/// - 否则按内存预算推导；预算未知时退回 `hardware_concurrency`（并说明是退回）。
/// `cpu_quota` 是容器 CPU 配额（0 = 不可知）；最终 `jobs` 取「内存推导」与「CPU 配额」的
/// **较小值**——内存不够会 OOM，CPU 不够只是变慢，两个上界都必须尊重。
[[nodiscard]] auto plan_concurrency(std::size_t requested_jobs, std::size_t requested_jobs_large,
                                    std::uint64_t requested_budget_mb, std::string_view profile,
                                    std::size_t hardware, std::size_t cpu_quota = 0)
    -> ConcurrencyPlan;

/// 测试分片方案（与编译并发**同一套资源探测**，但约束不同）。
struct ShardPlan {
  std::size_t shards{1};      ///< 分片数（1 = 单进程，与分片前逐位等价）
  std::size_t test_cores{1};  ///< 套件实际能并行的核数（= min(硬件, CPU 配额)；探测不到则 = 硬件）
  std::string reason{};       ///< 如 `测试核数 28 → 14 片`
};

/// 单测试进程的内存估算（MiB）。
///
/// 取值依据：实测本框架全套件单进程峰值工作集 **112 MB**（分片进程只更小——
/// 片内用例数只有 1/N）。取 128 而不是 112：向上取整留一点余量，
/// 估算偏保守只是少开两片，偏乐观就是 OOM。
inline constexpr std::uint64_t kTestProcessMemoryMb = 128;

/// 推导测试分片数（**按机器实际资源**，与编译并发同源）。
///
/// ## 为何不能靠"核数 / 2"
///
/// 并行编译的上界是**内存**（每个 `cc1plus` 上百 MB），而并行的测试进程上界是
/// **CPU 与内存两者取小**：测试进程实测峰值工作集仅 112 MB（内存很宽松），
/// 但它**占满一个核**（CPU/墙钟 ≈ 0.98），所以并行度受核数硬约束。
/// 于是同一台机器上两类并行的容量**相差一个量级**，不能共用一条公式。
///
/// ## 实测到的两个上界
///
/// - **核数**：18 片时实测并行度 **11.0**（CPU/墙钟），19 片时可并发核数降到 **9**——
///   这就是"超过某个点后墙钟不再改善甚至变差"的机制（CPU 密集型单元在超订时退化）；
/// - **内存**：`预算 × 7/8 ÷ 128MiB`。本机 19572 MiB → 106 片，不构成约束；
///   但内存紧张的容器里它会先于核数生效（否则 OOM 杀掉整个分片）。
///
/// 取两者的**较小值**，再夹到 `[1, test_cores]`。
///
/// 注意：**不做"核数 / 2"**。该除数是针对编译并发的经验值（编译单元内存敏感、
/// 且与链接期争内存），对测试进程没有依据；片数多少只会影响**机时效率**
/// （超订时 CPU 密集型用例互相抢核而变慢），不改变结果正确性。
[[nodiscard]] auto plan_test_shards(std::size_t requested_shards, std::size_t hardware,
                                    std::size_t cpu_quota = 0, std::uint64_t budget_mb = 0,
                                    std::size_t cpu_cap = 0) -> ShardPlan;

}  // namespace st::pkg
