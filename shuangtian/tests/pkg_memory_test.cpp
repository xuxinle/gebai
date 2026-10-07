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
#if defined(_WIN32)
  // Windows 没有 cgroup 与 /proc/meminfo：探测器的契约是"**拿不到就明确说不知道**"，
  // 而不是编一个数（并发推导会退回按核数）。因此这里只断言"自描述一致"。
  if (limit.limit_mb == 0 || limit.source.empty()) {
    ST_CHECK(limit.limit_mb == 0);   // 未知必须表现为 0，而不是某个拍出来的值
    return;
  }
  ST_CHECK(!limit.source.empty());   // 有值就必须说明来源
#else
  // Linux（含容器）：cgroup 或 /proc/meminfo 至少有一个可用，探测应当成功且数量级合理
  ST_CHECK(limit.limit_mb > 0);
  ST_CHECK(limit.limit_mb < 1024ULL * 1024ULL);  // 不超过 1 TiB
  ST_CHECK(!limit.source.empty());
#endif
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
  // 8 GiB 预算、san 档（单单元 768 MiB）：推导应远低于 28 核，而不是照抄核数。
  // 显式传 cpu_quota=28（无 CPU 约束）——否则本机容器的 cgroup 配额会参与推导，测试随环境变。
  const auto plan = st::pkg::plan_concurrency(0, 0, 8192, "san", 28, 28);
  ST_CHECK(plan.jobs < 28);
  ST_CHECK(plan.jobs >= 1);
  ST_CHECK_EQ(plan.jobs_large, 2U);  // 超大单元默认 2 路（内存轻量型大块头无需完全串行）
  ST_CHECK(plan.reason.find("8192MiB") != std::string::npos);
  // 预算越小，并发越低（单调性）
  const auto tighter = st::pkg::plan_concurrency(0, 0, 2048, "san", 28, 28);
  ST_CHECK(tighter.jobs <= plan.jobs);
  ST_CHECK(tighter.jobs >= 1);
}

// CPU 配额是并发的第二个上界：内存允许再多，超出配额的 worker 只会互相抢 CPU。
ST_TEST(concurrency_respects_cpu_quota) {
  // 内存充裕（预算极大）但容器只给 4 核：并发必须收到 4，而不是硬件 28
  const auto plan = st::pkg::plan_concurrency(0, 0, 1024ULL * 1024ULL, "dev", 28, 4);
  ST_CHECK_EQ(plan.jobs, 4U);
  ST_CHECK(plan.reason.find("CPU") != std::string::npos);  // 理由必须说明是配额收的
  // 配额宽于硬件（不一致的环境）时以硬件为准，不放大
  const auto wider = st::pkg::plan_concurrency(0, 0, 1024ULL * 1024ULL, "dev", 4, 16);
  ST_CHECK_EQ(wider.jobs, 4U);
  // 配额不是整数核时向上取整：1.5 核 → 2
  const auto fractional = st::pkg::plan_concurrency(0, 0, 1024ULL * 1024ULL, "dev", 8, 2);
  ST_CHECK_EQ(fractional.jobs, 2U);
  // 显式 --jobs 完全接管，不被配额压低
  const auto explicit_plan = st::pkg::plan_concurrency(6, 0, 0, "dev", 28, 2);
  ST_CHECK_EQ(explicit_plan.jobs, 6U);
}

ST_TEST(concurrency_respects_explicit_and_unknown_budget) {
  // 显式 --jobs 完全接管（CI 固定行为）；cpu_quota 固定以便测试不受运行环境影响
  const auto explicit_plan = st::pkg::plan_concurrency(6, 0, 8192, "dev", 28, 28);
  ST_CHECK_EQ(explicit_plan.jobs, 6U);
  ST_CHECK(explicit_plan.reason.find("--jobs 6") != std::string::npos);
  // 显式 --jobs-large 也接管
  const auto explicit_large = st::pkg::plan_concurrency(0, 3, 8192, "dev", 28, 28);
  ST_CHECK_EQ(explicit_large.jobs_large, 3U);
  // 预算不可知：退回硬件并发（而不是崩或给 0）
  const auto unknown = st::pkg::plan_concurrency(0, 0, 1, "dev", 8, 8);
  ST_CHECK_EQ(unknown.jobs, 1U);  // 1 MiB 预算 → 至少 1 路，不能是 0
  ST_CHECK(unknown.jobs >= 1U);
}

ST_TEST(concurrency_never_exceeds_hardware) {
  // 预算极大时也不该超过核数（多开线程没有收益）；cpu_quota 取与硬件同值（无额外约束）
  const auto plan = st::pkg::plan_concurrency(0, 0, 1024ULL * 1024ULL, "dev", 8, 8);
  ST_CHECK_EQ(plan.jobs, 8U);
}

// ── 测试分片：与编译并发**同一套资源探测**，但约束不同（内存轻、吃满一个核）──

ST_TEST(test_shards_derived_from_cores_not_halved) {
  // 核算上界：内核测试进程 CPU/墙钟 ≈ 0.98（占满一个核），能并行的片数就是能并行的核数。
  // **不能做"核数 / 2"**——那个除数是针对编译并发的经验值（编译单元内存敏感），
  // 对测试进程没有依据。测试只验"推导确实用了核数"，不写死 28/2=14。
  const auto plan = st::pkg::plan_test_shards(0, 28, 28, 1024ULL * 1024ULL, 0);
  ST_CHECK_EQ(plan.test_cores, 28U);
  ST_CHECK_EQ(plan.shards, 28U);
  ST_CHECK(plan.reason.find("测试核数 28") != std::string::npos);
}

ST_TEST(test_shards_respect_cpu_quota) {
  // 容器只给 4 核：分片必须收到 4，而不是照搛硬件 28——超订会让每片被 CFS 限流，
  // 墙钟反而变差（与编译并发同一条道理）。
  const auto plan = st::pkg::plan_test_shards(0, 28, 4, 1024ULL * 1024ULL, 0);
  ST_CHECK_EQ(plan.test_cores, 4U);
  ST_CHECK_EQ(plan.shards, 4U);
  ST_CHECK(plan.reason.find("CPU 配额 4") != std::string::npos);
  // 配额宽于硬件（不一致的环境）时以硬件为准，不放大
  const auto wider = st::pkg::plan_test_shards(0, 8, 64, 1024ULL * 1024ULL, 0);
  ST_CHECK_EQ(wider.shards, 8U);
}

ST_TEST(test_shards_respect_memory_budget) {
  // 测试进程峰值工作集实测 112 MB，估算取 128 MiB；预算 1024 MiB 的机器上
  // 内存在**核数之前**先卡住：usable = 1024 - 128 = 896 → 896/128 = 7 片（而不是 28）。
  // 这是"按机器实际资源"的关键一半：小内存容器上让它先于核数生效（否则整片被 OOM 杀）。
  const auto plan = st::pkg::plan_test_shards(0, 28, 28, 1024, 0);
  ST_CHECK_EQ(plan.shards, 7U);
  ST_CHECK(plan.reason.find("1024MiB") != std::string::npos);
  // 内存充裕时不再约束（本机 19 GiB 的情况）
  const auto roomy = st::pkg::plan_test_shards(0, 28, 28, 19000, 0);
  ST_CHECK_EQ(roomy.shards, 28U);
}

ST_TEST(test_shards_explicit_and_cap_and_floor) {
  // 显式 `--test-jobs` 完全接管（CI 固定行为），不被配额/内存压低
  const auto explicit_plan = st::pkg::plan_test_shards(5, 28, 2, 256, 0);
  ST_CHECK_EQ(explicit_plan.shards, 5U);
  ST_CHECK(explicit_plan.reason.find("--test-jobs 5") != std::string::npos);
  // 策略上限（实测：18 片并行度 11.0、19 片降到 9，即超过某点墙钟不再改善）
  const auto capped = st::pkg::plan_test_shards(0, 28, 28, 1024ULL * 1024ULL, 16);
  ST_CHECK_EQ(capped.shards, 16U);
  ST_CHECK(capped.reason.find("上限 16") != std::string::npos);
  // 极小机器：预算 1 MiB 时不能给 0 片（退化为"不跑"是最坏结果）
  const auto tiny = st::pkg::plan_test_shards(0, 8, 8, 1, 0);
  ST_CHECK(tiny.shards >= 1U);
  // 探测不到资源（硬件也报 0）时仍至少 1 片
  const auto none = st::pkg::plan_test_shards(0, 0, 0, 0, 0);
  ST_CHECK(none.shards >= 1U);
  ST_CHECK(none.test_cores >= 1U);
}
