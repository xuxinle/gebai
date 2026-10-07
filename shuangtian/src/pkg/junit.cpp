#include "st/pkg/junit.hpp"

#include <algorithm>
#include <format>

#include "st/core/fs.hpp"

namespace st::pkg {
namespace {

/// 从一个 JUnit 文档里抽出全部 `<testcase …>…</testcase>` 片段（自闭合的也要）。
///
/// 只做**标签级**扫描而不是 XML 解析：格式由本仓库的 `write_junit` 唯一产生
/// （缩进、属性顺序、CDATA 转义都固定），而合并要容忍空文档。引一套 XML 解析器
/// 只为这个不值得，且多一处"解析器行为差异"的失败面。
[[nodiscard]] auto extract_cases(std::string_view xml) -> std::vector<std::string> {
  std::vector<std::string> cases;
  std::size_t cursor = 0;
  constexpr std::string_view kOpen = "<testcase ";
  while (true) {
    const std::size_t begin = xml.find(kOpen, cursor);
    if (begin == std::string_view::npos) break;
    const std::size_t self_close = xml.find("/>", begin);
    const std::size_t close = xml.find("</testcase>", begin);
    if (close == std::string_view::npos) {
      if (self_close == std::string_view::npos) break;   // 结构损坏：到此为止
      cases.emplace_back(xml.substr(begin, self_close + 2 - begin));
      cursor = self_close + 2;
      continue;
    }
    if (self_close != std::string_view::npos && self_close < close) {
      cases.emplace_back(xml.substr(begin, self_close + 2 - begin));
      cursor = self_close + 2;
      continue;
    }
    cases.emplace_back(xml.substr(begin, close + std::string_view("</testcase>").size() - begin));
    cursor = close + std::string_view("</testcase>").size();
  }
  return cases;
}

[[nodiscard]] auto is_failure_case(std::string_view fragment) -> bool {
  return fragment.find("<failure>") != std::string_view::npos;
}

}  // namespace

auto merge_junit_files(const std::vector<std::string>& reports, std::string_view output,
                       std::size_t& failed) -> Status {
  failed = 0;
  std::vector<std::string> cases;
  for (const auto& path : reports) {
    const auto text = fs::read_text(path);
    if (!text.has_value()) continue;   // 0 用例的片没写文件：合法
    for (auto& item : extract_cases(*text)) cases.push_back(std::move(item));
  }
  if (cases.empty()) return ok();   // 没有结果就不产出空报告（与单进程语义一致）

  // **按用例名排序**：片间并行、完成顺序不稳定，排序让同一套用例的报告逐字节可复现
  // （否则 CI 的 diff 会随调度抖动，无法用作"这次与上次有没有变化"的判据）。
  std::ranges::sort(cases);
  for (const auto& item : cases) {
    if (is_failure_case(item)) ++failed;
  }

  std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites>\n";
  xml += std::format("  <testsuite name=\"st\" tests=\"{}\" failures=\"{}\">\n", cases.size(), failed);
  for (const auto& item : cases) {
    xml.append("    ").append(item).append("\n");
  }
  xml += "  </testsuite>\n</testsuites>\n";
  return fs::write_text(output, xml);
}

}  // namespace st::pkg
