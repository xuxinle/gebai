#pragma once

/// 禁令静态扫描（`CONVENTIONS.md` §8）：把"禁用易错特性"从纸面约定变成可执行检查。
/// 规则命中即失败；行内 `// lint-allow: <规则> 原因` 可豁免（必须写原因）。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

struct LintViolation {
  std::string file{};
  int line{0};
  std::string rule{};
  std::string text{};
  bool advisory{false};  ///< 提示级（不导致失败）
};

struct LintReport {
  std::vector<LintViolation> violations{};
  std::size_t files_scanned{0};
  std::size_t suppressed{0};
};

/// 扫描工程根下的 include/ src/ tests/ examples/ tools/。
[[nodiscard]] auto lint_project(std::string_view root) -> Result<LintReport>;

/// 单文件扫描（测试与编辑器集成用）。
[[nodiscard]] auto lint_file(std::string_view path) -> Result<std::vector<LintViolation>>;

/// 规则说明。
[[nodiscard]] auto explain_rule(std::string_view rule) -> std::string;

/// 规则清单（`rule-id` 与一句话说明）。
[[nodiscard]] auto known_rules() -> std::vector<std::pair<std::string, std::string>>;

}  // namespace st::pkg
