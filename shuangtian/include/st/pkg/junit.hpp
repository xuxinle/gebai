#pragma once

/// JUnit XML 报告合并（多进程分片测试用；**父进程**——构建驱动——的职责）。
///
/// ## 为什么需要
///
/// `st test --test-jobs N` 把用例分成 N 片、每片一个独立测试进程。若让每片直接写
/// 同一个 `ST_JUNIT_XML` 路径，**后跑的片会覆盖先跑的**——CI 只看到最后一片，
/// 而"少了一半用例"这件事在报告层面看不出来（`testsuites` 结构是合法的，只是很小）。
///
/// 所以每片写各自的临时文件，父进程在这里合并成一份。
///
/// ## 合并口径
///
/// 输入是各片写出的 `<testsuites><testsuite name="st">…` 文档。合并只做**结构级拼接**：
/// 收集全部 `<testcase>` 片段，重新包一层 `<testsuite>`。
///
/// 不做 XML 树解析：格式由本仓库的 `test::write_junit` 唯一产生（形状固定），
/// 合并还必须容忍"某片写了空文档"（0 用例的片）——字符串级扫描就够，
/// 引一套 XML 解析器只为这个不值得，还多一处"解析器行为差异"的失败面。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 把 `reports` 里的全部用例合并写成 `output`。
///
/// - 缺文件/空文件/没有 `<testcase>` 都**不算错**（0 用例的片是合法的）；
/// - 全部片都没有用例时**不写** `output`（与单进程 `write_junit` 语义一致：
///   没有结果就产出一份误导性的空报告还不如不产出），`failed` 回填 0；
/// - 用例名**排序**后写出：片间是并行的、完成顺序不稳定，排序让同一套用例连续两次跑
///   得到完全相同的报告字节（否则 CI 的 diff 会随调度抖动，无法用作“这次与上次有没有变化”的判据）。
[[nodiscard]] auto merge_junit_files(const std::vector<std::string>& reports, std::string_view output,
                                     std::size_t& failed) -> Status;

}  // namespace st::pkg
