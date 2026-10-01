/// `st lint` 规则测试（`src/pkg/lint.cpp`）。
///
/// 重点覆盖 **L13**（组件遮蔽 `Element` 保护成员 / `semantics_flags` 覆写重建标志）：
/// 这条规则守的是一类**编译零警告、且能被错误的单测掩盖**的缺陷——焦点写基类、
/// 读遮蔽副本时，直接调 `set_focused` 的组件级测试读写落在同一侧，只有真实应用
/// （`UiRoot::set_focus`）才暴露（`CodeEditor` 光标永不绘制即此因）。
/// 规则本身若失灵，这类缺陷会再次静默进来，因此两个方向都要锁住：
/// **命中真实缺陷**（否则规则形同虚设）与**不误报反例**（否则 CI 天天假失败）。
///
/// 其余规则走 `lint_project` 的全量扫描（工程自身 0 违反即是它们的持续验证）。

#include "st/test/test.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/lint.hpp"

namespace {

/// 把源码写进临时文件并扫描（规则是按行文本判定，不需要真的编译）。
[[nodiscard]] auto lint_source(std::string_view name, std::string_view source)
    -> std::vector<st::pkg::LintViolation> {
  const std::string path = st::fs::join(st::fs::temp_dir(), std::string(name));
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << source;
  }
  auto result = st::pkg::lint_file(path);
  (void)st::fs::remove_file(path);
  if (!result) return {};
  return *result;
}

[[nodiscard]] auto has_rule(const std::vector<st::pkg::LintViolation>& violations,
                            std::string_view rule) -> bool {
  for (const auto& violation : violations) {
    if (violation.rule == rule) return true;
  }
  return false;
}

[[nodiscard]] auto count_rule(const std::vector<st::pkg::LintViolation>& violations,
                              std::string_view rule) -> std::size_t {
  std::size_t count = 0;
  for (const auto& violation : violations) {
    if (violation.rule == rule) ++count;
  }
  return count;
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// L13：组件遮蔽 Element 保护成员
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_l13_flags_element_subclass_member_shadowing) {
  // 真实缺陷形态（CodeEditor 曾自带 bool focused_{false}）：
  // Element 子类里声明与基类同名的成员 → 必须命中
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class ProbeWidget : public Element {
 public:
  ProbeWidget() { set_focusable(true); }

 private:
  bool focused_{false};
  std::string key_{};
};
}  // namespace st::ui
)";
  const auto violations = lint_source("l13_shadow_probe.hpp", source);
  ST_CHECK(has_rule(violations, "L13"));
  ST_CHECK_EQ(count_rule(violations, "L13"), std::size_t{2});  // focused_ 与 key_ 各一处
}

ST_TEST(lint_l13_ignores_non_element_classes_and_plain_method_bodies) {
  // 反面一：不继承 Element 的类声明同名成员是合法的（UiRoot 就有 focused_/hovered_）
  // 反面二：`class Element` 自己不该被当成「Element 子类」（类名里含 Element 而已）
  // 反面三：成员函数体里的 `return children_;` 不是成员声明
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class NotAnElement {
 private:
  bool focused_{false};
  int bounds_{0};
};
class Element {
 public:
  auto children_count() const -> std::size_t { return children_; }

 private:
  bool focused_{false};
};
}  // namespace st::ui
)";
  const auto violations = lint_source("l13_shadow_negative.hpp", source);
  ST_CHECK(!has_rule(violations, "L13"));
}

ST_TEST(lint_l13_accepts_element_subclass_without_shadowing) {
  // 正确写法：子类只声明自己的成员（名字不与基类重合）
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class FineWidget : public Element {
 public:
  FineWidget() { set_focusable(true); }

 private:
  std::string label_{};
  int tab_width_{4};
};
}  // namespace st::ui
)";
  const auto violations = lint_source("l13_shadow_fine.hpp", source);
  ST_CHECK(!has_rule(violations, "L13"));
}

// ————————————————————————————————————————————————————————————————————————————
// L13：semantics_flags 覆写重建标志
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_l13_flags_semantics_flags_rebuild_in_both_definition_forms) {
  // 缺陷形态：覆写体内以 `SemanticsFlags flags{}` 起手 → 丢掉基类的
  // visible/enabled/focused/hovered/pressed。类内 inline 定义与类外定义两种都要认出。
  const std::string inline_form = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class ProbeWidget : public Element {
 public:
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override {
    SemanticsFlags flags{};
    flags.editable = true;
    return flags;
  }
};
}  // namespace st::ui
)";
  ST_CHECK(has_rule(lint_source("l13_flags_inline.hpp", inline_form), "L13"));

  const std::string out_of_line = R"(
#include "st/ui/components/basic.hpp"
namespace st::ui {
auto ProbeWidget::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags{};
  flags.editable = false;
  return flags;
}
}  // namespace st::ui
)";
  ST_CHECK(has_rule(lint_source("l13_flags_outline.cpp", out_of_line), "L13"));
}

ST_TEST(lint_l13_accepts_base_result_first_and_unrelated_fresh_flags) {
  // 正确写法：以基类结果起手再覆写特有字段
  const std::string correct = R"(
#include "st/ui/element.hpp"
namespace st::ui {
auto ProbeWidget::semantics_flags() const -> SemanticsFlags {
  SemanticsFlags flags = Element::semantics_flags();
  flags.editable = true;
  return flags;
}
}  // namespace st::ui
)";
  ST_CHECK(!has_rule(lint_source("l13_flags_correct.cpp", correct), "L13"));

  // 反面：覆写体**之外**构造一份全新标志是合法的（如根节点的默认标志）
  const std::string unrelated = R"(
#include "st/ui/element.hpp"
namespace st::ui {
auto make_root_flags() -> SemanticsFlags {
  SemanticsFlags flags{};
  flags.visible = true;
  return flags;
}
}  // namespace st::ui
)";
  ST_CHECK(!has_rule(lint_source("l13_flags_unrelated.cpp", unrelated), "L13"));
}

// ————————————————————————————————————————————————————————————————————————————
// 规则表与豁免
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(lint_rule_catalogue_exposes_every_rule_with_help) {
  const auto rules = st::pkg::known_rules();
  ST_CHECK(!rules.empty());
  bool has_l13 = false;
  for (const auto& [id, description] : rules) {
    ST_CHECK(!id.empty());
    ST_CHECK(!description.empty());
    if (id == "L13") has_l13 = true;
  }
  ST_CHECK(has_l13);
  // 说明文本对「专用检查实现的规则」（pattern 为空）也要给出可读信息，而不是空正则
  const std::string help = st::pkg::explain_rule("L13");
  ST_CHECK(help.find("L13") != std::string::npos);
  ST_CHECK(help.find("专用检查") != std::string::npos);
}

ST_TEST(lint_allow_comment_suppresses_violation) {
  // 行内 `// lint-allow: <规则> 原因` 可豁免（规则表约定）：豁免必须逐条写明原因
  const std::string source = R"(
#include "st/ui/element.hpp"
namespace st::ui {
class ProbeWidget : public Element {
 private:
  bool focused_{false};  // lint-allow: L13 探针：验证豁免通道有效
};
}  // namespace st::ui
)";
  ST_CHECK(!has_rule(lint_source("l13_allow.hpp", source), "L13"));
}
