/// `st build` 顺带刷新过期测试二进制的**判定逻辑**回归。
///
/// ## 为什么这些用例值得写
///
/// 这条判定判错的两种方向都有代价，而代价都落在「AI 拿到错的结果」上：
///
/// | 判错方向 | 后果 |
/// |---|---|
/// | 该重建却没重建（漏报） | 跑了旧二进制、看到旧结果 → 去查被测代码，白烧几轮（实测踩过，代价最贵） |
/// | 不该重建却重建（误报） | 每次 `st build` 多编几百个测试单元 → 构建时间翻倍，人开始习惯性加 `--no-tests` |
///
/// 两个方向都不是"看起来差不多"能接受的，所以四条边界逐条钉住。
///
/// ## 为什么用真实文件、真实时间顺序
///
/// 判定函数只做一件事：比两个文件的 mtime。mock 掉文件系统会把**被测的那件事
/// 本身**抹掉（桩回一个恒等值 → 错实现也判绿，本仓库踩过这种假绿）。
///
/// 没有"设置 mtime"的公共 API，所以用**真实写入顺序 + 真实等待**造时间差：
/// 先写旧的，`sleep` 一小段，再写新的。mtime 精度在 Windows/NTFS 上是 100ns，
/// `sleep_for(20ms)` 足够把它区分开（用例总耗时 < 100ms）。
#include <chrono>
#include <thread>

#include "st/core/fs.hpp"
#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

/// mtime 精度兜底：Windows 上 FAT 是 2 秒、NTFS 是 100ns，Linux 通常 ns 级。
/// 取 20ms 足以区分，又不至于让四条用例明显变慢。
constexpr std::chrono::milliseconds kTick{20};

/// 写一个文件（内容带序号，避免"内容完全相同 → mtime 不更新"的实现差异）。
void write_marker(const std::string& path, int seq) {
  (void)st::fs::write_text(path, std::format("x{}", seq));
  std::this_thread::sleep_for(kTick);
}

}  // namespace

ST_TEST(test_binary_staleness_ignores_missing_binary) {
  // 从没跑过测试 = **正常状态**，不是"过期"。
  //
  // 判成过期的后果：第一次 `st build` 就要编 200+ 个测试单元——
  // 而多数人只是想把应用跑起来。这条边界最容易被"顺手写上 true"破坏。
  const auto dir = st::fs::make_temp_dir("st_stale_missing");
  ST_CHECK(dir.has_value());
  if (!dir) return;
  const std::string test_src = st::fs::join(*dir, "a_test.cpp");
  write_marker(test_src, 1);
  const std::string binary = st::fs::join(*dir, "st_tests.exe");   // 不存在

  ST_CHECK(!st::pkg::test_binary_is_stale({test_src}, binary));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_detects_newer_test_source) {
  // 核心场景：改了测试断言、还没跑 st test → 二进制旧了。
  const auto dir = st::fs::make_temp_dir("st_stale_newer");
  ST_CHECK(dir.has_value());
  if (!dir) return;
  // 先写二进制（旧），后写测试源（新）
  const std::string binary = st::fs::join(*dir, "st_tests.exe");
  write_marker(binary, 1);
  const std::string stale_src = st::fs::join(*dir, "a_test.cpp");
  write_marker(stale_src, 2);

  ST_CHECK(st::pkg::test_binary_is_stale({stale_src}, binary));
  ST_CHECK(st::pkg::test_binary_is_stale({stale_src, binary}, binary));   // 混入不重要项也成立
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_is_quiet_when_fresh) {
  // 刚跑过 st test：二进制比所有测试源都新 → 不重建（否则每次构建都白编一遍）。
  const auto dir = st::fs::make_temp_dir("st_stale_fresh");
  ST_CHECK(dir.has_value());
  if (!dir) return;
  const std::string src_a = st::fs::join(*dir, "a_test.cpp");
  write_marker(src_a, 1);
  const std::string src_b = st::fs::join(*dir, "b_test.cpp");
  write_marker(src_b, 2);
  // 最后写二进制 → 它比两个源都新
  const std::string binary = st::fs::join(*dir, "st_tests.exe");
  write_marker(binary, 3);

  ST_CHECK(!st::pkg::test_binary_is_stale({src_a, src_b}, binary));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_survives_missing_test_file) {
  // 声明的测试文件被删了：不能因此判成"过期"（那只会在构建时再失败一次），
  // 也不能崩。真正的错误由 `st test` 在编译阶段报。
  const auto dir = st::fs::make_temp_dir("st_stale_gone");
  ST_CHECK(dir.has_value());
  if (!dir) return;
  const std::string binary = st::fs::join(*dir, "st_tests.exe");
  write_marker(binary, 1);
  const std::string gone = st::fs::join(*dir, "deleted_test.cpp");

  ST_CHECK(!st::pkg::test_binary_is_stale({gone}, binary));
  ST_CHECK(!st::pkg::test_binary_is_stale({}, binary));   // 空列表同理
  (void)st::fs::remove_all(*dir);
}


ST_TEST(modified_ns_has_sub_second_precision) {
  // ## 这条用例钉的是一个**框架级**缺陷（本轮实测发现）
  //
  // `modified_ns` 的 Windows 实现原先是 `std::filesystem::last_write_time`，
  // 而 MinGW 把它**截断到整秒**：同一秒内先后写两个文件，两边返回值 `% 1e9`
  // 都是 0，连"谁先写的"都分不出来。
  //
  // 代价不在这一个函数上——**所有按 mtime 做的增量判断粒度都退到秒**：
  // 同一秒内改源码再构建会被判成"没变"，于是拿旧产物当新结果。
  // 这正是本仓库反复踩的"改了没生效"的一支。
  //
  // 判据用"两次写入之间有明显间隔，时间戳必须不同"——不依赖具体精度值，
  // 只要不是整秒截断就必然成立。
  const auto dir = st::fs::make_temp_dir("st_mtime_precision");
  ST_CHECK(dir.has_value());
  if (!dir) return;

  const std::string a = st::fs::join(*dir, "a.txt");
  const std::string b = st::fs::join(*dir, "b.txt");
  (void)st::fs::write_text(a, "1");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  (void)st::fs::write_text(b, "2");

  const auto ta = st::fs::modified_ns(a);
  const auto tb = st::fs::modified_ns(b);
  ST_CHECK(ta.has_value());
  ST_CHECK(tb.has_value());
  if (ta && tb) {
    // 50ms 的间隔在秒级截断下必然丢失（两者相等），在真实精度下必然可分
    ST_CHECK(*tb > *ta);
    // 再加一道：纳秒值的低位不该恒为 0（整秒截断的铁证）
    ST_CHECK((*ta % 1000000) != 0 || (*tb % 1000000) != 0);
  }
  (void)st::fs::remove_all(*dir);
}
