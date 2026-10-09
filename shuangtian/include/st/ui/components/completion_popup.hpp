#pragma once

/// 补全弹层（LSP 阶段 4）：候选项列表 + 选中项的详情面板。
///
/// ## 为什么自成组件而不塞进 `CodeEditor`
///
/// 编辑器只提供"光标几何"与"插入文本"两件事，补全的**状态**（候选从哪来、请求发没发、
/// 选中第几项、详情是什么）属于应用/桥接层。组件只做三件事：
/// 展示候选（带类型徽标）、键盘导航（↑↓/PageUp/PageDown/Enter/Esc/Tab）、
/// 回报"用户选了哪个"（`on_accept` / `on_dismiss`）。
///
/// 这样它也能给别的输入场景复用（命令参数补全、路径补全），不绑死在 LSP 上。
///
/// ## 形态
///
/// ```
/// ┌──────────────────────────────┬────────────────────────┐
/// │ ● width                  int │  struct Widget 的成员   │
/// │ ○ height                 int │                        │
/// └──────────────────────────────┴────────────────────────┘
/// ```
/// 左列是候选项（图标 + 标签 + 右侧类型/来源），右列是选中项的详情（可选）。
/// 详情面板**按需**：没有详情（server 还没回 `completionItem/resolve`）就不占位。
///
/// ## 定位
///
/// 锚点是"光标矩形"（`set_anchor`），弹层在其**下方**；下方空间不足时翻到上方
/// （与 `MenuPanel` 同一策略——不用调用方算，它不知道弹层多高）。

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 一个候选项。
struct CompletionItemView {
  /// 主文本（如 `width`）——插入到文档里的就是它（或 `insert_text`）。
  std::string label{};
  /// 右侧的类型/来源标签（如 `int`、`method`、`clangd`）。空则不画。
  std::string detail{};
  /// 实际插入文本（空 = 用 `label`）。用于 LSP 的 `insertText` / snippet。
  std::string insert_text{};
  /// 左侧徽标字符（如 `ƒ` 函数、`○` 变量）。空则不画。
  std::string badge{};
  /// 排序/过滤用的额外匹配文本（如 `filterText`）。
  std::string filter_text{};
};

class CompletionPopup : public Element {
 public:
  static constexpr float kRowHeight{22.0f};
  static constexpr float kPadding{6.0f};
  static constexpr float kMinWidth{240.0f};
  static constexpr float kMaxWidth{420.0f};
  static constexpr float kMaxHeight{260.0f};
  static constexpr float kDetailWidth{260.0f};

  CompletionPopup();

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "CompletionPopup";
  }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Menu; }
  /// 浮层：命中只在自己的矩形内（与菜单面板同一契约）。
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return visible(); }

  /// 设置候选（整体替换）。`selected` 重置到 0（新候选集下旧下标无意义）。
  void set_items(std::vector<CompletionItemView> items);
  [[nodiscard]] auto item_count() const noexcept -> std::size_t { return items_.size(); }
  [[nodiscard]] auto item(std::size_t index) const -> const CompletionItemView*;

  /// 锚点 = **光标矩形**（弹层贴它下沿；下方不够翻上方）。
  void set_anchor(math::Rect anchor);
  [[nodiscard]] auto anchor_rect() const noexcept -> math::Rect { return anchor_; }

  /// 选中项下标（-1 = 无）。
  [[nodiscard]] auto selected_index() const noexcept -> std::ptrdiff_t { return selected_; }
  void set_selected_index(std::ptrdiff_t index);

  /// 选中项的详情（右侧面板显示；空 = 不显示详情区）。
  void set_detail(std::string detail);
  [[nodiscard]] auto detail() const noexcept -> const std::string& { return detail_; }

  /// 已过滤可见的候选下标（`set_filter` 之后）。
  [[nodiscard]] auto visible_count() const noexcept -> std::size_t {
    return visible_items_.size();
  }
  /// 按前缀/子串过滤（大小写不敏感；空串 = 全显示）。
  ///
  /// 为什么组件做过滤而不是调用方：**过滤规则要与渲染口径一致**（可见行 = 哪些候选），
  /// 分两处实现必然分叉（一处按 filterText、一处按 label，结果键盘选中的是隐形项）。
  void set_filter(std::string filter);
  [[nodiscard]] auto filter() const noexcept -> const std::string& { return filter_; }

  /// 第 `row` 可见行的矩形（测试与命中共用）。
  [[nodiscard]] auto row_rect(std::size_t row) const noexcept -> math::Rect;
  [[nodiscard]] auto list_rect() const noexcept -> math::Rect { return list_; }
  [[nodiscard]] auto panel_rect() const noexcept -> math::Rect { return bounds_; }
  /// 卡片矩形（浮层自定位；= bounds_，独立方法是为了语义清楚）。
  [[nodiscard]] auto card_rect() const noexcept -> math::Rect { return bounds_; }

  /// 接受候选（Enter/Tab/点击）：回调参数是**插入文本**（空则退回 label）。
  std::function<void(const std::string& insert_text)> on_accept{};
  /// 关闭（Esc/点击别处/接受后）：`accepted` 区分"选了"与"取消"。
  std::function<void(bool accepted)> on_dismiss{};
  /// 选中项变化（用于按需请求 `completionItem/resolve` 拿详情）。
  std::function<void(std::size_t index)> on_selection_changed{};

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 重算可见集合（`filter_` 变化或候选集变化后）。
  void rebuild_visible();
  /// 把选中项夹到可见集合内（空集合 → -1）。
  void clamp_selection();
  /// 移动选中（`delta` 行；跨页用 `delta = ±page`）。
  void move_selection(std::ptrdiff_t delta);
  /// 接受当前选中项。
  void accept();
  /// 当前选中项在 `visible_` 里的行号（-1 = 无）。
  [[nodiscard]] auto selected_row() const -> std::ptrdiff_t;

  std::vector<CompletionItemView> items_{};
  std::vector<std::size_t> visible_items_{};   ///< 过滤后可见的候选（存 items_ 下标）
  std::string filter_{};
  std::string detail_{};
  math::Rect anchor_{};
  math::Rect list_{};
  math::Rect detail_rect_{};
  std::ptrdiff_t selected_{-1};
  float scroll_y_{0.0f};
  int hover_row_{-1};
};

}  // namespace st::ui
