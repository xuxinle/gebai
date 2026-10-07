/// 测试分片与 JUnit 合并的回归用例。
///
/// 两条被测契约（都属于"错了会静默给出错误结论"的那一类）：
///
/// ① **分片的覆盖性**：N 片的并集必须**恰好**是全集，且两两不相交。
///    漏掉一片 → 一部分用例从此不再被跑（而汇总行只有那句 "passed, 0 failed"，
///    少跑的用例不会有任何痕迹）；重叠 → 同一用例跑两遍（有毒副作用的用例会翻车）。
///    这两种错都不会让任何既有用例变红，只能靠专门断言钉住。
///
/// ② **JUnit 合并**：并行分片下每片写自己的报告，父进程合并。若合并把某片丢了，
///    CI 看到的是"更少的用例、全绿"——比红更糟。这里连"缺文件/空片"一起覆盖。

#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/junit.hpp"

namespace {

/// 分片是**进程级**状态：改完必须恢复原值。
///
/// 不恢复的后果比"这条用例脏"严重得多——`run_all` 用 `in_shard()` 决定跑不跑，
/// 留下一个 `count=3/index=2` 会让**注册顺序在该用例之后的所有用例全部被跳过**，
/// 而汇总行照样打印 "N passed, 0 failed"（只是 N 变小了）。
/// 这类"测试偷走后续用例"的故障只能靠 RAII 恢复 + 断言总数来防。
struct ShardGuard {
  std::pair<std::size_t, std::size_t> saved{st::test::shard()};
  ShardGuard(std::size_t index, std::size_t count) { st::test::set_shard(index, count); }
  ~ShardGuard() { st::test::set_shard(saved.first, saved.second); }
  ShardGuard(const ShardGuard&) = delete;
  auto operator=(const ShardGuard&) -> ShardGuard& = delete;
};

/// 全集快照：**先关掉分片**再取用例名。
///
/// 这一步是必须的：这些用例自己也可能跑在某个分片里（`st test --test-jobs N`），
/// 而 `case_names()` 会按**本进程的分片**过滤——直接拿它当“全集”去比，
/// 会得到“分片的并集不等于全集”这种假失败（实测踩到：840 != 60）。
[[nodiscard]] auto all_case_names() -> std::vector<std::string> {
  const ShardGuard guard(0, 0);
  return st::test::case_names("");
}

/// 全集 ∩ filter（同样先关分片）。
[[nodiscard]] auto all_case_names_filtered(std::string_view needle) -> std::vector<std::string> {
  const ShardGuard guard(0, 0);
  return st::test::case_names(needle);
}

/// 写一份最小 JUnit 文档（与 `st::test::write_junit` 的形状一致）。
void write_report(const std::string& path, const std::vector<std::pair<std::string, bool>>& cases) {
  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n";
  std::size_t failed = 0;
  for (const auto& [name, ok] : cases) {
    if (!ok) ++failed;
  }
  xml += std::format("  <testsuite name=\"st\" tests=\"{}\" failures=\"{}\">\n", cases.size(), failed);
  for (const auto& [name, ok] : cases) {
    if (ok) {
      xml += std::format("    <testcase name=\"{}\" time=\"0.010\"/>\n", name);
    } else {
      xml += std::format("    <testcase name=\"{}\" time=\"0.010\">\n"
                         "      <failure><![CDATA[x:1: 断言失败]]></failure>\n"
                         "    </testcase>\n",
                         name);
    }
  }
  xml += "  </testsuite>\n</testsuites>\n";
  ST_CHECK(st::fs::write_text(path, xml).has_value());
}

/// 数一份 JUnit 文档里的用例数（合并结果的核对口径）。
[[nodiscard]] auto count_cases(std::string_view path) -> std::size_t {
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) return 0;
  std::size_t count = 0;
  std::size_t cursor = 0;
  while ((cursor = text->find("<testcase ", cursor)) != std::string::npos) {
    ++count;
    cursor += 10;
  }
  return count;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 分片
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(shard_parses_index_over_count) {
  const auto [index, count] = st::test::parse_shard("3/8");
  ST_CHECK_EQ(index, std::size_t{3});
  ST_CHECK_EQ(count, std::size_t{8});
}

ST_TEST(shard_rejects_malformed_text) {
  // 没有斜杠 / 空串 / 非数字 / 0 下标 / 下标大于总数——全部回 {0,0}（调用方据此报错）
  ST_CHECK_EQ(st::test::parse_shard("8").first, std::size_t{0});
  ST_CHECK_EQ(st::test::parse_shard("").first, std::size_t{0});
  ST_CHECK_EQ(st::test::parse_shard("a/b").first, std::size_t{0});
  ST_CHECK_EQ(st::test::parse_shard("0/8").first, std::size_t{0});
  ST_CHECK_EQ(st::test::parse_shard("9/8").first, std::size_t{0});
  // 单片（n == 1）不是错误，而是"不分片"的表达
  ST_CHECK_EQ(st::test::parse_shard("1/1").first, std::size_t{0});
  // 尾随的额外内容不静默接受（"2/8/9" 的分母块解不出数字 → 拒绝）
  ST_CHECK_EQ(st::test::parse_shard("2/8/9").first, std::size_t{0});
}

ST_TEST(shard_partitions_cover_every_case_exactly_once) {
  const auto full = all_case_names();
  ST_REQUIRE(!full.empty());

  for (const std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}}) {
    std::vector<std::string> union_all;
    for (std::size_t index = 1; index <= count; ++index) {
      const ShardGuard guard(index, count);
      const auto part = st::test::case_names("");
      union_all.insert(union_all.end(), part.begin(), part.end());
    }
    // 并集 == 全集（**大小与集合都对**：只比大小会漏掉"某用例被换成了另一条"）
    ST_CHECK_EQ(union_all.size(), full.size());
    std::ranges::sort(union_all);
    auto deduped = union_all;
    deduped.erase(std::unique(deduped.begin(), deduped.end()), deduped.end());
    ST_CHECK_EQ(deduped.size(), union_all.size());   // 无重复 = 片间不相交

    auto expected = full;
    std::ranges::sort(expected);
    ST_CHECK(expected == deduped);
  }
}

ST_TEST(shard_indices_are_one_based_and_stable) {
  const auto full = all_case_names();
  ST_REQUIRE(full.size() >= 6);

  // 第 1 片的第 0/1 个用例 = 全集下标 0/8（按注册顺序取模）
  {
    const ShardGuard guard(1, 8);
    const auto part = st::test::case_names("");
    ST_REQUIRE(part.size() >= 2);
    ST_CHECK_EQ(part[0], full[0]);
    ST_CHECK_EQ(part[1], full[8]);
  }
  // 第 2 片的首个 = 全集下标 1
  {
    const ShardGuard guard(2, 8);
    const auto part = st::test::case_names("");
    ST_REQUIRE(!part.empty());
    ST_CHECK_EQ(part[0], full[1]);
  }
}

ST_TEST(shard_suffix_is_empty_without_partitioning) {
  // 单进程跑的产物路径必须与分片前**逐字符一致**（否则产物目录名会漂移）。
  // 求值先于恢复：`ST_CHECK_EQ` 的左侧在分片仍生效时求值（`suffix_of` 内部先设后读）。
  const auto suffix_of = [](std::size_t index, std::size_t count) -> std::string {
    const ShardGuard guard(index, count);
    return st::test::shard_suffix();
  };
  ST_CHECK_EQ(suffix_of(0, 0), std::string{});
  ST_CHECK_EQ(suffix_of(1, 1), std::string{});
  ST_CHECK_EQ(suffix_of(2, 8), std::string("-shard2of8"));
  ST_CHECK_EQ(suffix_of(8, 8), std::string("-shard8of8"));
}

ST_TEST(shard_filter_still_applies_within_partition) {
  const auto full = all_case_names();
  ST_REQUIRE(full.size() >= 10);
  // 取一个必定匹配多条的前缀，构造"分片 ∩ filter"。不能拿 `full[4]` 的**前 3 个字符**
  // 当针——用例名是散开的，那个前缀可能只命中它自己（甚至一个依赖顺序的巧合）。
  // 这里直接找**公共前缀足够长**的一对：让针来自实际存在的一组用例名。
  std::string needle;
  for (const auto& name : full) {
    const std::string candidate = name.substr(0, std::min<std::size_t>(4, name.size()));
    std::size_t hits = 0;
    for (const auto& other : full) {
      if (other.starts_with(candidate)) ++hits;
    }
    if (hits >= 2) {
      needle = candidate;
      break;
    }
  }
  ST_REQUIRE(!needle.empty());

  const auto filtered = all_case_names_filtered(needle);
  ST_REQUIRE(filtered.size() >= 2);
  for (const auto& name : filtered) ST_CHECK(name.find(needle) != std::string::npos);

  // 分片后的候选集必须是**全集∩filter** 的子集（且下标口径仍按全集——最容易写错的地方）
  const ShardGuard guard(1, 4);
  const auto part = st::test::case_names(needle);
  for (const auto& name : part) {
    ST_CHECK(std::ranges::find(filtered, name) != filtered.end());
  }
  // 并集口径：四片的 filter 结果合起来应恰好等于 filtered
  std::vector<std::string> union_all;
  for (std::size_t index = 1; index <= 4; ++index) {
    st::test::set_shard(index, 4);
    const auto piece = st::test::case_names(needle);
    union_all.insert(union_all.end(), piece.begin(), piece.end());
  }
  ST_CHECK_EQ(union_all.size(), filtered.size());
}

// ————————————————————————————————————————————————————————————————————————————
// JUnit 合并
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(junit_merge_concatenates_all_shards) {
  // 独立临时目录：`temp_dir()` 是**进程间共用的根**，固定文件名在多片并行时会互相踩
  const auto made = st::fs::make_temp_dir("st-junit-cat");
  ST_REQUIRE(made.has_value());
  const std::string dir = *made;
  const std::string a = st::fs::join(dir, "a.xml");
  const std::string b = st::fs::join(dir, "b.xml");
  const std::string c = st::fs::join(dir, "c.xml");
  const std::string out = st::fs::join(dir, "out.xml");

  write_report(a, {{"case_alpha", true}, {"case_beta", false}});
  write_report(b, {{"case_gamma", true}});
  write_report(c, {});   // 空片：0 用例

  std::size_t failed = 0;
  const auto status = st::pkg::merge_junit_files({a, b, c}, out, failed);
  ST_CHECK(status.has_value());
  ST_CHECK_EQ(failed, std::size_t{1});
  ST_CHECK_EQ(count_cases(out), std::size_t{3});

  // 用例名**排序**输出：片间并行、完成顺序不稳定，排序让报告逐字节可复现
  const auto text = st::fs::read_text(out);
  ST_REQUIRE(text.has_value());
  const auto alpha = text->find("case_alpha");
  const auto beta = text->find("case_beta");
  const auto gamma = text->find("case_gamma");
  ST_CHECK(alpha != std::string::npos && beta != std::string::npos && gamma != std::string::npos);
  ST_CHECK(alpha < beta);
  ST_CHECK(beta < gamma);

  (void)st::fs::remove_all(dir);
}

ST_TEST(junit_merge_tolerates_missing_and_empty_inputs) {
  const auto made = st::fs::make_temp_dir("st-junit-tol");
  ST_REQUIRE(made.has_value());
  const std::string dir = *made;
  const std::string missing = st::fs::join(dir, "not-there.xml");
  const std::string out = st::fs::join(dir, "out.xml");

  // 全缺 → 不产出误导性的空报告（与单进程 `write_junit` 的语义一致），且不算错
  std::size_t failed = 0;
  ST_CHECK(st::pkg::merge_junit_files({missing, missing}, out, failed).has_value());
  ST_CHECK(!st::fs::exists(out));
  ST_CHECK_EQ(failed, std::size_t{0});

  // 一份有、一份缺 → 合出那一份（而不是因为"有一片没写"就整体失败）
  write_report(missing, {{"case_solo", true}});
  ST_CHECK(st::pkg::merge_junit_files({out, missing}, out, failed).has_value());
  ST_CHECK_EQ(count_cases(out), std::size_t{1});

  (void)st::fs::remove_all(dir);
}

ST_TEST(junit_merge_counts_failures_from_every_shard) {
  const auto made = st::fs::make_temp_dir("st-junit-fail");
  ST_REQUIRE(made.has_value());
  const std::string dir = *made;
  const std::string a = st::fs::join(dir, "f1.xml");
  const std::string b = st::fs::join(dir, "f2.xml");
  const std::string out = st::fs::join(dir, "fout.xml");
  write_report(a, {{"case_one", false}, {"case_two", true}});
  write_report(b, {{"case_three", false}});

  std::size_t failed = 0;
  ST_CHECK(st::pkg::merge_junit_files({a, b}, out, failed).has_value());
  ST_CHECK_EQ(failed, std::size_t{2});
  ST_CHECK_EQ(count_cases(out), std::size_t{3});
  // 失败详情必须原样保留（合并丢弃 CDATA 会让"哪条挂了"无从查起）
  const auto text = st::fs::read_text(out);
  ST_REQUIRE(text.has_value());
  ST_CHECK(text->find("<failure>") != std::string::npos);

  (void)st::fs::remove_all(dir);
}
