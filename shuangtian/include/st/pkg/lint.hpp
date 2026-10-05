#pragma once

/// 禁令静态扫描（`CONVENTIONS.md` §8）：把"禁用易错特性"从纸面约定变成可执行检查。
/// 规则命中即失败；两条豁免通道：
/// - 行内 `// lint-allow: <规则> 原因`（**必须写原因**）——单点、就地说明；
/// - 清单 `lint.exempt`（`"L5": ["src/app/*.cpp"]`）——成片、集中的边界，
///   适合"这一层与框架的约定不同"（如业务层 worker 线程用异常传错）。详见 `manifest_exempt`。

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 清单配置的规则豁免：规则 id → 路径 glob 列表（相对工程根，`/` 归一）。
using LintExemptions = std::map<std::string, std::vector<std::string>>;

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
  /// 行内（`lint-allow`）与清单豁免的**合计**计数（两个通道都要被看见，不允许静默）。
  std::size_t suppressed{0};
  /// 清单豁免单独的计数（不包含行内豁免）——与 `suppressed` 分开报，
  /// 因为“工程级边界”与“单点破例”是两种性质不同的东西，混在一起就看不出全局边界有多宽。
  std::size_t suppressed_by_manifest{0};
};

/// 扫描工程根下的 include/ src/ tests/ examples/ tools/。
/// `exemptions` 来自清单 `lint.exempt`（空 = 不豁免任何东西，与旧行为一致）。
[[nodiscard]] auto lint_project(std::string_view root, const LintExemptions& exemptions = {})
    -> Result<LintReport>;

/// 单文件扫描（测试与编辑器集成用）。
[[nodiscard]] auto lint_file(std::string_view path) -> Result<std::vector<LintViolation>>;

/// 规则说明。
[[nodiscard]] auto explain_rule(std::string_view rule) -> std::string;

/// 规则清单（`rule-id` 与一句话说明）。
[[nodiscard]] auto known_rules() -> std::vector<std::pair<std::string, std::string>>;

}  // namespace st::pkg
