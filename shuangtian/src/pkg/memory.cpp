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
                      std::size_t hardware) -> ConcurrencyPlan {
  ConcurrencyPlan plan;
  const std::size_t hardware_jobs = std::max<std::size_t>(1, hardware);

  // 预算：显式 > 探测
  if (requested_budget_mb > 0) {
    plan.budget_mb = requested_budget_mb;
  } else {
    const MemoryLimit detected = detect_memory_limit();
    plan.budget_mb = detected.limit_mb;
  }

  if (requested_jobs != 0) {
    plan.jobs = requested_jobs;
    plan.reason = std::format("--jobs {} 显式指定", requested_jobs);
  } else if (plan.budget_mb == 0) {
    plan.jobs = hardware_jobs;
    plan.reason = std::format("内存上限不可知 → 退回硬件并发 {}", hardware_jobs);
  } else {
    const std::uint64_t unit_mb = unit_memory_estimate_mb(profile);
    // 预留 1/8 给链接期、系统与其它负载：预算全给编译会让最坏情形仍然贴上限
    const std::uint64_t usable_mb = plan.budget_mb - plan.budget_mb / 8;
    const std::uint64_t derived = usable_mb / std::max<std::uint64_t>(1, unit_mb);
    plan.jobs = static_cast<std::size_t>(std::clamp<std::uint64_t>(derived, 1, hardware_jobs));
    plan.reason = std::format("内存预算 {}MiB / 单单元 {}MiB（{}）→ {}（上限硬件并发 {}）",
                              plan.budget_mb, unit_mb, profile, plan.jobs, hardware_jobs);
  }

  // 超大单元闸门：默认 1（串行）。即使并行度很高，超大单元也不会互相叠加。
  plan.jobs_large = requested_jobs_large != 0 ? requested_jobs_large
                                              : std::min<std::size_t>(1, plan.jobs);
  return plan;
}

}  // namespace st::pkg
