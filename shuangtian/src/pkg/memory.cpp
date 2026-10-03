#include "st/pkg/memory.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

namespace st::pkg {
namespace {

/// `memory.max` 的否定回答（cgroup v2 用 `max` 表示"无限制"）。
[[nodiscard]] auto parse_limit_text(std::string_view text) -> std::uint64_t {
  const std::string_view trimmed = st::trim(text);
  if (trimmed.empty() || trimmed == "max") return 0;
  const auto parsed = parse_i64(trimmed);
  if (!parsed.has_value() || *parsed <= 0) return 0;
  return static_cast<std::uint64_t>(*parsed) / (1024ULL * 1024ULL);
}

/// 读一个文件里的字节数（cgroup 的两个数字格式）。
[[nodiscard]] auto read_limit_file(std::string_view path) -> std::uint64_t {
  if (!fs::is_regular_file(path)) return 0;
  const auto text = fs::read_text(path);
  return text.has_value() ? parse_limit_text(*text) : 0;
}

/// cgroup v2 `cpu.max` 解析：`"<quota> <period>"`（quota 为 `max` = 无限制）。
///
/// 配额不是整数核时**向上取整**：1.5 核给 1 个 worker 会白丢半核，给 2 个最多轻微超额。
[[nodiscard]] auto parse_cpu_max(std::string_view text) -> std::size_t {
  const auto parts = st::split(st::trim(text), ' ');
  if (parts.size() < 2) return 0;
  const auto quota = parse_i64(parts[0]);
  const auto period = parse_i64(parts[1]);
  if (!quota.has_value() || !period.has_value() || *quota <= 0 || *period <= 0) return 0;
  const std::int64_t whole = (*quota + *period - 1) / *period;  // 向上取整
  return whole > 0 ? static_cast<std::size_t>(whole) : 0;
}

/// cgroup v1 的配额是**两个文件**（`cpu.cfs_quota_us` / `cpu.cfs_period_us`），
/// quota 为 `-1` 表示无限制。
[[nodiscard]] auto parse_cpu_cfs_quota(std::string_view quota_text, std::string_view period_text)
    -> std::size_t {
  const auto quota = parse_i64(st::trim(quota_text));
  const auto period = parse_i64(st::trim(period_text));
  if (!quota.has_value() || !period.has_value() || *quota <= 0 || *period <= 0) return 0;
  const std::int64_t whole = (*quota + *period - 1) / *period;
  return whole > 0 ? static_cast<std::size_t>(whole) : 0;
}

}  // namespace

auto detect_memory_limit() -> MemoryLimit {
  MemoryLimit limit;
  // ① 显式覆盖（CI/容器里数字不一定可信，允许钉住）
  if (const auto env = fs::read_env("ST_MEMORY_MB"); env.has_value() && !env->empty()) {
    const auto parsed = parse_i64(*env);
    if (parsed.has_value() && *parsed > 0) {
      limit.limit_mb = static_cast<std::uint64_t>(*parsed);
      limit.source = "环境变量 ST_MEMORY_MB";
      return limit;
    }
  }
  // ② cgroup v2：容器里**唯一可信**的上限（宿主机内存可能大得多，free 看不出压力）
  if (const std::uint64_t value = read_limit_file("/sys/fs/cgroup/memory.max"); value > 0) {
    limit.limit_mb = value;
    limit.source = "cgroup v2 /sys/fs/cgroup/memory.max";
    return limit;
  }
  // ③ cgroup v1
  if (const std::uint64_t value = read_limit_file("/sys/fs/cgroup/memory/memory.limit_in_bytes");
      value > 0) {
    limit.limit_mb = value;
    limit.source = "cgroup v1 memory.limit_in_bytes";
    return limit;
  }
#if defined(_WIN32)
  // ③.5 Windows：GlobalMemoryStatusEx（封装在 platform_memory.cpp）。原先只探测
  // cgroup/meminfo，Windows 上恒"不可知" → 退回满核并发——恰好违背本机制
  // "按内存防 OOM"的初衷（8GiB 机器满并发 dev 档 28 路就是瞬时 14GiB）。
  if (const std::uint64_t value = platform_memory_limit_mb(); value > 0) {
    limit.limit_mb = value;
    limit.source = "GlobalMemoryStatusEx（物理总量与可用量取小）";
    return limit;
  }
#endif
  // ④ 退回系统内存（Linux/POSIX 通用；无则调用方按核数兜底）
  if (const auto text = fs::read_text("/proc/meminfo"); text.has_value()) {
    for (const auto& line : st::split(*text, '\n')) {
      if (!line.starts_with("MemTotal:")) continue;
      const auto parts = st::split(line, ' ');
      for (const auto& part : parts) {
        const auto parsed = parse_i64(part);
        if (!parsed.has_value() || *parsed <= 0) continue;
        limit.limit_mb = static_cast<std::uint64_t>(*parsed) / 1024ULL;  // kB → MiB
        limit.source = "/proc/meminfo MemTotal";
        return limit;
      }
    }
  }
  return limit;  // 探测失败：调用方退回"按核数"
}

auto is_large_unit(std::string_view path) -> bool {
  const auto size = fs::file_size(path);
  return size.has_value() && *size >= kLargeUnitBytes;
}

auto detect_cpu_quota() -> std::size_t {
  // ① 显式覆盖（CI/容器里数字不一定可信，允许钉住）
  if (const auto env = fs::read_env("ST_CPU_LIMIT"); env.has_value() && !env->empty()) {
    const auto parsed = parse_i64(*env);
    if (parsed.has_value() && *parsed > 0) return static_cast<std::size_t>(*parsed);
  }
  // ② cgroup v2：`cpu.max` 形如 `"400000 100000"`（= 4 核）或 `"max 100000"`（无限）
  if (const auto text = fs::read_text("/sys/fs/cgroup/cpu.max"); text.has_value()) {
    if (const std::size_t cores = parse_cpu_max(*text); cores > 0) return cores;
  }
  // ③ cgroup v1：quota 与 period 分成两个文件
  if (const auto quota = fs::read_text("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
      quota.has_value()) {
    const auto period = fs::read_text("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (period.has_value()) {
      if (const std::size_t cores = parse_cpu_cfs_quota(*quota, *period); cores > 0) return cores;
    }
  }
  return 0;  // 不可知：调用方退回硬件并发
}

auto unit_memory_estimate_mb(std::string_view profile) -> std::uint64_t {
  // 数字来自实测（本框架 60 余单元 + QuickJS）：
  //   `-O1 -g` 典型 60–490 MB、`-O0` 更低；sanitizer 档 630–700 MB。
  // 取实测上限而非均值——估算偏保守只是慢一点，偏乐观就是 OOM。
  if (profile == "san") return 768;
  if (profile == "debug" || profile == "quick") return 384;
  return 512;  // dev / release
}

auto plan_concurrency(std::size_t requested_jobs, std::size_t requested_jobs_large,
                      std::uint64_t requested_budget_mb, std::string_view profile,
                      std::size_t hardware, std::size_t cpu_quota) -> ConcurrencyPlan {
  ConcurrencyPlan plan;
  const std::size_t hardware_jobs = std::max<std::size_t>(1, hardware);
  plan.cpu_quota = cpu_quota != 0 ? cpu_quota : detect_cpu_quota();
  // CPU 上界：配额可知且小于硬件核数时以配额为准（超额 worker 只会互相抢 CPU）
  const std::size_t cpu_jobs =
      plan.cpu_quota != 0 ? std::min(hardware_jobs, plan.cpu_quota) : hardware_jobs;
  const std::string cpu_note = plan.cpu_quota != 0 && plan.cpu_quota < hardware_jobs
                                   ? std::format("，CPU 配额 {} 核", plan.cpu_quota)
                                   : std::string{};

  // 预算：显式 > 探测
  if (requested_budget_mb > 0) {
    plan.budget_mb = requested_budget_mb;
  } else {
    const MemoryLimit detected = detect_memory_limit();
    plan.budget_mb = detected.limit_mb;
  }

  if (requested_jobs != 0) {
    // 显式 `--jobs` 完全接管（CI 固定行为），不参与推导（也不被配额压）
    plan.jobs = requested_jobs;
    plan.reason = std::format("--jobs {} 显式指定", requested_jobs);
  } else if (plan.budget_mb == 0) {
    plan.jobs = cpu_jobs;
    plan.reason = std::format("内存上限不可知 → {}（硬件并发 {}{}）", plan.jobs, hardware_jobs,
                              cpu_note);
  } else {
    const std::uint64_t unit_mb = unit_memory_estimate_mb(profile);
    // 预留 1/8 给链接期、系统与其它负载：预算全给编译会让最坏情形仍然贴上限
    const std::uint64_t usable_mb = plan.budget_mb - plan.budget_mb / 8;
    const std::uint64_t derived = usable_mb / std::max<std::uint64_t>(1, unit_mb);
    const auto memory_jobs = static_cast<std::size_t>(std::clamp<std::uint64_t>(derived, 1, cpu_jobs));
    plan.jobs = memory_jobs;
    const std::string memory_note =
        std::format("内存预算 {}MiB / 单单元 {}MiB（{}）→ {}", plan.budget_mb, unit_mb, profile,
                    memory_jobs);
    if (cpu_jobs < memory_jobs) {
      // 内存允许更多、CPU 不够：以 CPU 配额收尾并说明（不然构建比按配额还慢）
      plan.reason = std::format("{}；CPU 配额 {} 核（硬件 {}）更紧 → {}", memory_note, cpu_jobs,
                                hardware_jobs, cpu_jobs);
    } else {
      plan.reason = std::format("{}（上限硬件并发 {}{}）", memory_note, hardware_jobs, cpu_note);
    }
  }

  // 超大单元闸门：默认 2（与普通并发共享 CPU）——它们是**内存轻量型**大块头
  // （实测 dsl.cpp 10 s / 637 MB、build.cpp 11 s / 553 MB），串行化只制造长尾。
  // 真需要独占（如 quickjs.c）时 `--jobs-large 1` 可钉住。
  plan.jobs_large = requested_jobs_large != 0 ? requested_jobs_large
                                              : std::min<std::size_t>(2, plan.jobs);
  return plan;
}

}  // namespace st::pkg
