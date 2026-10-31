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
/// 判据是**内容哈希**（不是 mtime）：用真实文件、真实写入，
/// 并显式构造“只动时间戳”与“等长改写”两种情形。
#include <chrono>
#include <thread>

#include "st/core/fs.hpp"
#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

/// 写一个文件（内容带序号，便于构造“内容变了/没变”两种情形）。
void write_marker(const std::string& path, int seq) {
  (void)st::fs::write_text(path, std::format("x{}", seq));
}

}  // namespace

ST_TEST(test_binary_staleness_ignores_missing_binary) {
  // 从没跑过测试 = **正常状态**，不是"过期"。
  //
  // 判成过期的后果：第一次 `st build` 就要编 200+ 个测试单元——
  // 而多数人只是想把应用跑起来。这条边界最容易被"顺手写上 true"破坏。
  const auto dir = st::fs::make_temp_dir("st_stale_missing");
  ST_REQUIRE(dir.has_value());
  const std::string test_src = st::fs::join(*dir, "a_test.cpp");
  write_marker(test_src, 1);
  const std::string binary = st::fs::join(*dir, "st_tests");   // 不存在

  ST_CHECK(!st::pkg::test_binary_is_stale({test_src}, binary));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_detects_changed_test_source) {
  // 核心场景：改了测试断言、还没跑 st test → 二进制旧了。
  const auto dir = st::fs::make_temp_dir("st_stale_changed");
  ST_REQUIRE(dir.has_value());
  const std::string test_src = st::fs::join(*dir, "a_test.cpp");
  write_marker(test_src, 1);
  const std::string binary = st::fs::join(*dir, "st_tests");
  write_marker(binary, 1);
  (void)st::fs::write_text(st::pkg::test_binary_stamp_path(binary),
                           st::pkg::test_source_stamp({test_src}));
  ST_CHECK(!st::pkg::test_binary_is_stale({test_src}, binary));

  // **等长**改写：mtime+size 判据在这里会漏，内容哈希必然抓住
  write_marker(test_src, 2);
  ST_CHECK(st::pkg::test_binary_is_stale({test_src}, binary));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_is_quiet_when_fresh) {
  // 刚跑过 st test 且之后没改过内容 → 不重建（否则每次构建都白编一遍）。
  const auto dir = st::fs::make_temp_dir("st_stale_fresh");
  ST_REQUIRE(dir.has_value());
  const std::string src_a = st::fs::join(*dir, "a_test.cpp");
  const std::string src_b = st::fs::join(*dir, "b_test.cpp");
  write_marker(src_a, 1);
  write_marker(src_b, 2);
  const std::string binary = st::fs::join(*dir, "st_tests");
  write_marker(binary, 3);
  (void)st::fs::write_text(st::pkg::test_binary_stamp_path(binary),
                           st::pkg::test_source_stamp({src_a, src_b}));

  ST_CHECK(!st::pkg::test_binary_is_stale({src_a, src_b}, binary));

  // 再写一遍同样的内容（模拟 touch / `git checkout`：时间戳全刷新，内容一字未改）
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  write_marker(src_a, 1);
  ST_CHECK(!st::pkg::test_binary_is_stale({src_a, src_b}, binary));
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_survives_missing_test_file) {
  // 声明的测试文件被删了：不能因“读不到”就判成过期（那只会在构建时再失败一次），
  // 也不能崩。真正的错误由 `st test` 在编译阶段报。
  const auto dir = st::fs::make_temp_dir("st_stale_gone");
  ST_REQUIRE(dir.has_value());
  const std::string binary = st::fs::join(*dir, "st_tests");
  write_marker(binary, 1);
  (void)st::fs::write_text(st::pkg::test_binary_stamp_path(binary),
                           st::pkg::test_source_stamp({}));
  const std::string gone = st::fs::join(*dir, "deleted_test.cpp");

  ST_CHECK(!st::pkg::test_binary_is_stale({gone}, binary));
  ST_CHECK(!st::pkg::test_binary_is_stale({}, binary));   // 空列表同理
  (void)st::fs::remove_all(*dir);
}

ST_TEST(test_binary_staleness_without_stamp_is_conservative) {
  // 没有出生快照（旧产物，或刚被人拷进来）→ 无从校验，保守判过期。
  // 反过来（判不过期）会在“别人塞进来的旧二进制”上永久静默。
  const auto dir = st::fs::make_temp_dir("st_stale_nostamp");
  ST_REQUIRE(dir.has_value());
  const std::string test_src = st::fs::join(*dir, "a_test.cpp");
  write_marker(test_src, 1);
  const std::string binary = st::fs::join(*dir, "st_tests");
  write_marker(binary, 1);

  ST_CHECK(st::pkg::test_binary_is_stale({test_src}, binary));
  (void)st::fs::remove_all(*dir);
}
