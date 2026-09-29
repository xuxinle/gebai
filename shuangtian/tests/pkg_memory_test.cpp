/// 并行编译的内存预算与单元分档测试。
///
/// 背景：并发路数按 `nproc` 定的时代，在 cgroup 限 8 GiB 的容器里 28 路并行编译
/// （单单元实测 60–490 MB、san 档 630–700 MB）必被 OOM killer 杀掉，而**表现极具误导性**：
/// 被杀单元留下半个 `.o`，链接期报一堆 `.Lubsan_data` 未定义。这套推导就是让"并发"由内存决定。
///
/// 另外覆盖 `fs::read_text` 的一个真实缺陷：它原先按 `seekg/tellg` 得到的大小定长读，
/// 而 `/proc`、`/sys`、cgroup 的虚拟文件 size 报 0（内容却非空）→ 读到空串，
/// 使 cgroup 内存探测永远失败（按内存推导并发随之失效）。

#include "st/test/test.hpp"

#include <cstdint>
#include <fstream>
#include <string>

#include "st/core/fs.hpp"
#include "st/pkg/memory.hpp"

namespace {

[[nodiscard]] auto write_probe_file(const std::string& name, std::size_t bytes)
    -> std::string {
  const std::string path = st::fs::join(st::fs::temp_dir(), name);
  std::string payload(bytes, 'x');
  payload.append("\n");
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << payload;
  stream.close();
  return path;
}

}  // namespace

ST_TEST(memory_limit_parsing_is_sane) {
  const auto limit = st::pkg::detect_memory_limit();
  // 本机（容器）必然有 cgroup 或 /proc/meminfo：探测应当成功且数量级合理
  ST_CHECK(limit.limit_mb > 0);
  ST_CHECK(limit.limit_mb < 1024ULL * 1024ULL);  // 不超过 1 TiB
  ST_CHECK(!limit.source.empty());
}

ST_TEST(read_text_handles_virtual_size_zero_files) {
  // `/proc/meminfo` 是典型的"size 报 0、内容非空"的虚拟文件（Linux）
  const std::string virtual_path = "/proc/meminfo";
  if (!st::fs::is_regular_file(virtual_path)) {
    return;  // 非 Linux 环境：跳过（该缺陷只在虚拟文件系统上出现）
  }
  const auto text = st::fs::read_text(virtual_path);
  ST_CHECK(text.has_value());
  ST_CHECK(!text->empty());
  ST_CHECK(text->find("MemTotal") != std::string::npos);
}

ST_TEST(large_unit_threshold_discriminates_by_size) {
  const std::string small = write_probe_file("st_mem_small.cpp", 4 * 1024);
  const std::string large = write_probe_file("st_mem_large.c", 900 * 1024);
  ST_CHECK(!st::pkg::is_large_unit(small));
  ST_CHECK(st::pkg::is_large_unit(large));
  // 读不到的文件按"否"处理：不因为 stat 失败就拒绝编译
  ST_CHECK(!st::pkg::is_large_unit("/nonexistent/path/x.cpp"));
  (void)st::fs::remove_file(small);
  (void)st::fs::remove_file(large);
}

ST_TEST(concurrency_derives_from_memory_not_cores) {
  // 8 GiB 预算、san 档（单单元 768 MiB）：推导应远低于 28 核，而不是照抄核数
  const auto plan = st::pkg::plan_concurrency(0, 0, 8192, "san", 28);
  ST_CHECK(plan.jobs < 28);
  ST_CHECK(plan.jobs >= 1);
  ST_CHECK_EQ(plan.jobs_large, 1U);  // 超大单元默认串行
  ST_CHECK(plan.reason.find("8192MiB") != std::string::npos);
  // 预算越小，并发越低（单调性）
  const auto tighter = st::pkg::plan_concurrency(0, 0, 2048, "san", 28);
  ST_CHECK(tighter.jobs <= plan.jobs);
  ST_CHECK(tighter.jobs >= 1);
}

ST_TEST(concurrency_respects_explicit_and_unknown_budget) {
  // 显式 --jobs 完全接管（CI 固定行为）
  const auto explicit_plan = st::pkg::plan_concurrency(6, 0, 8192, "dev", 28);
  ST_CHECK_EQ(explicit_plan.jobs, 6U);
  ST_CHECK(explicit_plan.reason.find("--jobs 6") != std::string::npos);
  // 显式 --jobs-large 也接管
  const auto explicit_large = st::pkg::plan_concurrency(0, 3, 8192, "dev", 28);
  ST_CHECK_EQ(explicit_large.jobs_large, 3U);
  // 预算不可知：退回硬件并发（而不是崩或给 0）
  const auto unknown = st::pkg::plan_concurrency(0, 0, 1, "dev", 8);
  ST_CHECK_EQ(unknown.jobs, 1U);  // 1 MiB 预算 → 至少 1 路，不能是 0
  ST_CHECK(unknown.jobs >= 1U);
}

ST_TEST(concurrency_never_exceeds_hardware) {
  // 预算极大时也不该超过核数（多开线程没有收益）
  const auto plan = st::pkg::plan_concurrency(0, 0, 1024ULL * 1024ULL, "dev", 8);
  ST_CHECK_EQ(plan.jobs, 8U);
}
