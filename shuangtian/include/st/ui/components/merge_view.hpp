#pragma once

/// **三方合并视图**（`MergeView`）：base / ours / theirs 三栏并列，逐段取舍。
///
/// ## 为什么是独立组件而不是 `DiffView` 的第三种 `DiffLayout`
///
/// `DiffView` 的比较对象是**两段文本**，它的导航单位是"差异块（hunk）"、
/// 语义是"看哪里变了"。合并视图的比较对象是**三段文本 + 一个决策**，
/// 导航单位是"区段（segment）"、语义是"这一段要谁"——它需要一个 `DiffView`
/// 没有的东西：**每段的选择状态**。塞进 `DiffView` 会让那个组件的每个分支
/// 都多一个"是不是合并模式"的判断，而两个语义混住之后 `disabled_lines`、
/// `read_only` 这些字段的含义也会分叉（合并视图里"只读"是指"不能改选择"）。
///
/// ## 数据来源
///
/// 区段划分用 `st::text::merge_three_way`（与 Git 无关的纯文本能力）：
/// 五态区段中只有 `Conflict` 需要人来选，其余（未变 / 只我方改 / 只他方改 /
/// 两侧同改）自动可解。**这一条决定了界面长什么样**：
/// 只有冲突段需要出现"取我方 / 取他方"的按钮，若把 `BothSame` 也当冲突
/// （那是常见错法），用户会在两份一模一样的内容里做无意义的选择。
///
/// ## 与写回的关系（本组件只到"选择"为止）
///
/// 它给出 `choices()`（每段取哪侧）与 `result()`（合成文本），
/// **不**自己写文件：写盘要经撤销栈、编码探测、行尾保持那一整套（见 `CodeEditor`）。
/// 组件负责"决定内容是什么"，宿主负责"怎么落地"——这样两件事各自可测。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/text/merge.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class MergeView : public Element {
 public:
  MergeView();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "MergeView"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  // —— 装载 ——
  /// 装载三份文本（**整体替换**，选择状态重置）。
  ///
  /// 三份都为空 = 清空（显示占位）。划分走 `text::merge_three_way`，
  /// 所以调用方不必自己算区段。
  void set_sides(std::string_view base_text, std::string_view ours_text,
                 std::string_view theirs_text);
  /// 只给两侧（无共同祖先；等同 `base` 为空——「两侧都在同一位置新增」的形态）。
  void set_texts(std::string_view ours_text, std::string_view theirs_text);
  void clear();

  [[nodiscard]] auto plan() const noexcept -> const text::MergePlan& { return plan_; }
  [[nodiscard]] auto base_text() const noexcept -> const std::string& { return base_text_; }
  [[nodiscard]] auto ours_text() const noexcept -> const std::string& { return ours_text_; }
  [[nodiscard]] auto theirs_text() const noexcept -> const std::string& { return theirs_text_; }

  // —— 逐段取舍 ——
  /// 把第 `index` 段的选择设为 `take_theirs`（`false` = 取我方）。
  ///
  /// 非冲突段**如实拒绝**（返回 `false`）：它们只有一种正确结果，
  /// "允许改"会让界面出现一个改了不生效的按钮（比禁用更糟）。
  auto set_take_theirs(std::size_t index, bool take_theirs) -> bool;
  /// 第 `index` 段的选择（非冲突段返回它**实际**会取的那一侧）。
  [[nodiscard]] auto takes_theirs(std::size_t index) const -> bool;
  /// 全部冲突段一次选定（"全部取我方/取他方"按钮）。
  void set_all_conflicts(bool take_theirs);
  /// 每段的选择（`true` = 取他方）；非冲突段填它实际会取的一侧——这样直接把它
  /// 喂回 `merge_apply` 时结果与 `result()` 一致（不必再过滤一遍）。
  [[nodiscard]] auto choices() const -> std::vector<int>;

  [[nodiscard]] auto conflict_count() const noexcept -> std::size_t { return plan_.conflicts; }
  [[nodiscard]] auto resolved_count() const noexcept -> std::size_t {
    return plan_.segments.size() - conflict_count();
  }
  /// 是否还有未决定的冲突。宿主据此决定"能不能提交合并结果"。
  [[nodiscard]] auto has_pending_conflicts() const noexcept -> bool {
    for (std::size_t index = 0; index < plan_.segments.size(); ++index) {
      if (plan_.segments[index].kind == text::MergeKind::Conflict && !decided_[index]) return true;
    }
    return false;
  }
  /// 合成结果（按当前选择）。
  [[nodiscard]] auto result() const -> std::string;
  /// 结果变化回调（选择变了就触发；宿主可用它更新"预览"）。
  std::function<void()> on_choice_changed{};

  // —— 导航 ——
  /// 跳到下一个/上一个**未决定**的冲突段（返回其下标；`nullopt` = 没有了）。
  ///
  /// 为何是"未决定的"而不是"下一个冲突"：合并的实际操作是"把剩下的一个个处理掉"，
  /// 跳到已经决定过的那个等于让用户重看一遍（而他刚看完）。
  [[nodiscard]] auto next_pending_conflict() -> std::optional<std::size_t>;
  [[nodiscard]] auto previous_pending_conflict() -> std::optional<std::size_t>;
  /// 当前聚焦的段（绘制高亮用）。
  void set_current_segment(std::size_t index);
  [[nodiscard]] auto current_segment() const noexcept -> std::size_t {
    return current_segment_;
  }

  // —— 显示 ——
  void set_show_base(bool value);
  [[nodiscard]] auto show_base() const noexcept -> bool { return show_base_; }
  void set_font_size(float size);

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action,
                                   std::string_view argument) -> bool override;
  [[nodiscard]] auto semantics_text() const -> std::string override;

  // —— 几何（命中与绘制共用；避免"测试复刻布局公式"的假绿）——
  /// 三栏的横向范围（`base` 栏在 `show_base` 为假时宽度为 0）。
  [[nodiscard]] auto column_rect(int column) const -> math::Rect;
  [[nodiscard]] auto column_count() const noexcept -> int { return show_base_ ? 3 : 2; }
  /// 第 `index` 段所在的行带（三栏同高——它们按区段行数逐段对齐）。
  [[nodiscard]] auto segment_rect(std::size_t index) const -> math::Rect;
  /// 第 `index` 段的按钮区（取我方 / 取他方；只在冲突段非空）。
  [[nodiscard]] auto take_ours_rect(std::size_t index) const -> math::Rect;
  [[nodiscard]] auto take_theirs_rect(std::size_t index) const -> math::Rect;
  /// 内容总高（滚动用）。
  [[nodiscard]] auto content_height() const -> float;

 private:
  /// 每个区段的行高（`行数 * line_height + 上下留白`）。
  [[nodiscard]] auto segment_height(std::size_t index) const -> float;
  /// 内容坐标 → 屏幕 y（含滚动）。
  [[nodiscard]] auto segment_top(std::size_t index) const -> float;
  [[nodiscard]] auto line_height() const -> float;

  text::MergePlan plan_{};
  std::string base_text_{};
  std::string ours_text_{};
  std::string theirs_text_{};
  /// 每段是否"取他方"（长度与 `plan_.segments` 同）。
  std::vector<bool> take_theirs_{};
  /// 每段是否**已被用户明确决定**（只有冲突段需要）。
  ///
  /// 为何与 `take_theirs_` 分开：两态的默认值是 `false`（取我方），
  /// 而"没决定"与"决定取我方"在界面上必须不同——前者要提示、后者不用。
  std::vector<bool> decided_{};

  std::size_t current_segment_{0};
  float scroll_{0.0f};
  bool show_base_{true};
  float font_size_{13.0f};
  float line_height_{0.0f};

  math::Color surface_{};
  math::Color surface_alt_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color ours_tint_{};
  math::Color theirs_tint_{};
  math::Color conflict_tint_{};
  math::Color focus_ring_{};
};

}  // namespace st::ui
