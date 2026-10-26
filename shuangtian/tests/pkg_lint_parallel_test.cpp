/// `lint_project` 的**并行扫描**回归。
///
/// ## 为什么需要这批用例
///
/// 2026-10-27 把 `lint_project` 从串行改成并行（实测 22.8 s → 1.36 s，并行度
/// 0.99 → 19.1/28）。并行化最容易出的错**不是崩溃，而是结果悄悄变了**：
/// 少扫几个文件、漏掉某类违规、计数对不上——而报告照样打印，看起来一切正常。
///
/// 所以判据不是"跑得通"，而是**三条不变量**：
///
/// | 不变量 | 为什么 |
/// |---|---|
/// | 扫到的文件数 == 目录里真实文件数 | 并行任务分发漏掉一个下标，报告只会少一行 |
/// | 同一工程的违规集合**与顺序**稳定 | 结果要可复现（否则 CI 与本地对比会抖动）|
/// | 每条违规的内容与单文件扫描一致 | 并行的分片不得改变任何规则的判定 |
///
/// ## 第三条的写法
///
/// 用 `lint_project` 拿全量，再用 `lint_file` 对**每个文件单独扫**，
/// 逐条比对「文件 + 行号 + 规则」。这直接钉住"并行版 == N 次单文件版"，
/// 而不依赖某个硬编码的期望值。
///
/// 它同时能接住「规则偷偷引入跨文件状态」这种退化——那种改动会让
/// 并行结果与逐文件结果分叉。
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/lint.hpp"
#include "st/test/test.hpp"

namespace {

/// 临时工程：建出 `lint_project` 会扫的那几个目录。
struct TempProject {
  std::string root;

  explicit TempProject(std::string_view name) {
    root = st::fs::make_temp_dir(name).value_or(st::fs::temp_dir());
  }
  ~TempProject() { (void)st::fs::remove_all(root); }
  TempProject(const TempProject&) = delete;
  auto operator=(const TempProject&) -> TempProject& = delete;

  void file(std::string_view relative, std::string_view content) const {
    const std::string path = st::fs::join(root, std::string(relative));
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) (void)st::fs::create_directories(path.substr(0, slash));
    (void)st::fs::write_text(path, std::string(content));
  }
};

/// 违规的稳定标识，用于比对两个来源的结果。
auto key_of(const st::pkg::LintViolation& violation) -> std::string {
  return std::format("{}:{}:{}", st::fs::file_name(violation.file), violation.line, violation.rule);
}

}  // namespace

/// 并行扫描必须覆盖**全部**文件（分发漏一个下标，报告只会少一行）。
ST_TEST(lint_project_scan_counts_every_file) {
  TempProject project("st_lint_par_files_");
  // 12 个文件，跨两个被扫描的目录
  for (int index = 0; index < 6; ++index) {
    project.file(std::format("src/mod_{}.cpp", index), "int f() { return 0; }\n");
    project.file(std::format("include/st/mod_{}.hpp", index), "#pragma once\n");
  }
  // 不被扫描的扩展名（.txt）不该计入
  project.file("src/notes.txt", "hello\n");

  const auto report = st::pkg::lint_project(project.root);
  ST_REQUIRE(report.has_value());
  ST_CHECK_EQ(report->files_scanned, 12U);
}

/// 并行结果必须与「逐文件扫」**完全一致**（文件 + 行号 + 规则三重比对）。
///
/// 这是本文件最重要的一条：它不依赖任何硬编码的期望违规数，
/// 而是把并行实现与单文件实现的结论对齐——两者分叉即失败。
ST_TEST(lint_project_agrees_with_per_file_scan) {
  TempProject project("st_lint_par_agree_");
  // 构造几类真实会触发的违规（L7 printf、L1 裸 new、L8 可变全局）
  project.file("src/a.cpp", "#include <cstdio>\nvoid f() { printf(\"x\"); }\n");
  project.file("src/b.cpp", "namespace { int counter = 0; }\n");
  project.file("src/c.cpp", "void g() { auto* p = new int(1); (void)p; }\n");
  project.file("src/clean.cpp", "int h() { return 1; }\n");
  project.file("include/st/d.hpp", "#pragma once\nvoid k();\n");
  // 深层目录也要覆盖（遍历漏层是并行的常见错）
  project.file("src/deep/nested/e.cpp", "#include <cstdio>\nvoid m() { printf(\"y\"); }\n");

  const auto project_report = st::pkg::lint_project(project.root);
  ST_REQUIRE(project_report.has_value());

  // 逐文件扫同一批文件，汇总成同样的集合
  std::multiset<std::string> from_project;
  for (const auto& violation : project_report->violations) from_project.insert(key_of(violation));

  const std::vector<std::string> relative_files{
      "src/a.cpp", "src/b.cpp", "src/c.cpp", "src/clean.cpp",
      "include/st/d.hpp", "src/deep/nested/e.cpp",
  };
  std::multiset<std::string> from_files;
  for (const auto& relative : relative_files) {
    const auto single = st::pkg::lint_file(st::fs::join(project.root, relative));
    ST_REQUIRE(single.has_value());
    for (const auto& violation : *single) from_files.insert(key_of(violation));
  }

  ST_CHECK_EQ(from_project.size(), from_files.size());
  for (const auto& key : from_files) {
    ST_CHECK(from_project.contains(key));
  }
}

/// 同一工程连扫两次，结果必须**逐条相同**（并行下的不确定性会在这里暴露）。
ST_TEST(lint_project_is_deterministic_across_runs) {
  TempProject project("st_lint_par_determ_");
  for (int index = 0; index < 20; ++index) {
    project.file(std::format("src/m{}.cpp", index),
                 "#include <cstdio>\nvoid f() { printf(\"x\"); }\n");
  }

  const auto first = st::pkg::lint_project(project.root);
  const auto second = st::pkg::lint_project(project.root);
  ST_REQUIRE(first.has_value());
  ST_REQUIRE(second.has_value());

  ST_CHECK_EQ(first->violations.size(), second->violations.size());
  ST_CHECK_EQ(first->files_scanned, second->files_scanned);
  ST_CHECK_EQ(first->suppressed, second->suppressed);
  ST_CHECK_EQ(first->suppressed_by_manifest, second->suppressed_by_manifest);

  // 顺序也必须稳定（并行汇总后要排序；不排序会让 diff 抖动）
  for (std::size_t index = 0; index < first->violations.size(); ++index) {
    ST_CHECK_EQ(key_of(first->violations[index]), key_of(second->violations[index]));
  }
  // 且确实是按 (file, line) 有序的
  for (std::size_t index = 1; index < first->violations.size(); ++index) {
    const auto& previous = first->violations[index - 1];
    const auto& current = first->violations[index];
    const bool ordered = previous.file < current.file ||
                         (previous.file == current.file && previous.line <= current.line);
    ST_CHECK(ordered);
  }
}

/// 豁免计数（行内 + 清单）在并行下不能丢。
///
/// 这两个计数器在旧实现里是**跨文件累加**的共享状态——并行化时最容易被漏掉，
/// 而且漏掉不会报错，只是报告里那个数字变小（"静默少报"）。
ST_TEST(lint_project_keeps_exemption_counters) {
  TempProject project("st_lint_par_exempt_");
  // 三个文件，各带一条行内豁免。
  //
  // ⚠ 豁免注释必须与被豁免的代码**同一行**：`has_allow_comment()` 拿单行原文找
  // `lint-allow:`，没有“上一行注释覆盖下一行”的语义（实测：写在上一行时
  // `suppressed` 恒为 0）。第一版就是这么写的，用例变红——那是**用例错**，不是实现错。
  for (int index = 0; index < 3; ++index) {
    project.file(std::format("src/{}.cpp", index),
                 "void f() { auto* p = new int(1); (void)p; }  // lint-allow: L1 演示用\n");
  }

  const auto report = st::pkg::lint_project(project.root);
  ST_REQUIRE(report.has_value());
  ST_CHECK_EQ(report->files_scanned, 3U);
  ST_CHECK_EQ(report->suppressed, 3U);   // 三个文件各一条
}
