#pragma once

/// @file text_area.hpp
/// @brief `TextArea`：多行文本域：自动换行（经 `TextPort::wrap`）+ 内部竖向滚动（光标始终可见）。。
///
/// 从 `input.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `input.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include "st/ui/component_registry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 多行文本域：自动换行（经 `TextPort::wrap`）+ 内部竖向滚动（光标始终可见）。
///
/// 交互：点击定位光标（行列）、`Enter` 换行、方向键/`Home`/`End` 移动、滚轮滚动、`PageUp`/`PageDown` 翻页。
/// 行高取 `metrics.line_height_body × font_size`。
class TextArea : public Element {
 public:
  TextArea();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "TextArea"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextArea; }

  void set_text(std::string text);
  void set_placeholder(std::string text);
  /// 设置光标字节位置（自动夹取到合法 UTF-8 码点边界）。
  void set_cursor_index(std::size_t index);
  /// 内部滚动偏移（像素），自动夹取到 `[0, 内容高 - 可视高]`。
  void set_scroll_offset(float offset);

  /// 选区（字节偏移，**已归一化**且已夹到码点边界）；无选择时 `begin == end`。
  ///
  /// 与 `Input` / `CodeEditor` / `MarkdownView` / `Terminal` 同一套语义。
  /// **多行也用字节偏移**（而不是 `{行, 列}`）：
  /// `text_` 本身就是一个字符串，字节偏移天然跨行唯一，行/列反而是要多维护一层。
  [[nodiscard]] auto selection() const -> std::pair<std::size_t, std::size_t>;
  void set_selection(std::size_t anchor, std::size_t focus);
  void clear_selection() noexcept { select_anchor_ = kNoSelection; }
  [[nodiscard]] auto has_selection() const -> bool;
  /// 选中文本（只读模式也可拷——只读是“不能改”，不是“不能选”）。
  [[nodiscard]] auto selected_text() const -> std::string;
  /// **只读**：编辑禁入（文本插入/删除/换行一律忽略），但**光标仍可定位**、
  /// 仍可滚动、仍可获得焦点。
  ///
  /// 存在的理由（2026-10-05，来自实战）：JSON 格式化输出想做成只读展示，
  /// 当时只能 `set_enabled(false)` 凑合——而 disabled 的语义是"这个控件不可用"：
  /// 视觉上变灰（读起来像出错/未就绪），且不可交互（没法选中看、没法滚动）。
  /// "只读"与"禁用"是两件不同的事，不该靠后者将就。
  ///
  /// 与 `Input` 的动作面对齐（`clear` 已有先例）：属性面 `read_only` 读写同一状态。
  void set_read_only(bool value) noexcept;
  [[nodiscard]] auto read_only() const noexcept -> bool { return read_only_; }

  [[nodiscard]] auto value() const noexcept -> const std::string& { return text_; }
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }
  [[nodiscard]] auto scroll_offset() const noexcept -> float { return scroll_; }
  /// 输入法插入点（`Element` 契约；由上一帧绘制时缓存）。
  [[nodiscard]] auto caret_rect() const -> math::Rect override { return last_caret_rect_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  // 属性面（协议 set / 脚本 $('#x').set() 的唯一入口；v0.1.5 补齐——此前 TextArea
  // 未实现它，`set value` 被静默忽略，与 Input 踩过的坑同类）。
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 只读属性（`selection`）：`get` 能读、`set` 不生效。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 内容变化回调（实参为最新文本）。
  std::function<void(std::string_view)> on_change{};

 private:
  /// 折行结果：可见行的字节区间（`[begin, end)`，不含换行符本身）。
  struct Line {
    std::size_t begin{0};
    std::size_t end{0};
  };

  [[nodiscard]] auto inner_box(const RenderContext& context) const -> math::Rect;
  [[nodiscard]] auto layout_lines(const RenderContext& context, float width) const -> std::vector<Line>;
  [[nodiscard]] auto line_height(const RenderContext& context) const -> float;
  [[nodiscard]] auto row_of(const std::vector<Line>& lines, std::size_t index) const -> std::size_t;
  [[nodiscard]] auto index_at_point(const RenderContext& context, math::Point point) const
      -> std::size_t;
  /// 给定**字节索引** → 屏幕坐标（选区高亮两端用它）。
  [[nodiscard]] auto index_to_point(const RenderContext& context, std::size_t index) const
      -> math::Point;
  /// 选区两端（已归一 + 夹取）；无选区时 `{cursor_, cursor_}`。
  [[nodiscard]] auto ordered_selection() const -> std::pair<std::size_t, std::size_t>;
  /// 删除当前选区（用于替换输入）。返回是否有东西被删。
  auto erase_selection() -> bool;
  /// 在光标处插入并清空选区。**所有文本插入的唯一入口**。
  void replace_selection_with(std::string_view inserted);
  void sync_scroll(const RenderContext& context);
  void insert_text(std::string_view inserted);
  void erase_forward();
  void erase_backward();
  /// 上下移动一行（尽量保持水平位置）。
  void move_vertical(const RenderContext& context, int direction);
  /// 移到当前行首/行尾。
  void move_to_line_edge(const RenderContext& context, bool to_end);
  void notify_change();

  std::string text_{};
  std::string placeholder_{};
  std::size_t cursor_{0};
  float scroll_{0.0f};
  /// 输入法预编辑串（显示态；**不进 `text_`**）。
  std::string preedit_{};
  /// 上一帧绘制的插入点矩形（`caret_rect()` 用）。
  mutable math::Rect last_caret_rect_{};
  /// 上一次绘制时光标的**闪烁可见相位**；只在真翻转的那帧才上报损坏区
  ///（见 Input 同名成员的说明）。
  mutable int last_blink_visible_{-1};
  /// 只读（编辑禁入，但可聚焦/定位光标/滚动）。与 `enabled_` **无关**：
  /// 后者是“控件不可用”（变灰、不响应），两者语义不同，不要用一个代替另一个。
  bool read_only_{false};

  // —— 选区（2026-10-14，与 `Input` 同一套字节偏移模型）——
  static constexpr std::size_t kNoSelection = std::numeric_limits<std::size_t>::max();
  std::size_t select_anchor_{kNoSelection};
  bool selecting_{false};
  std::int64_t last_click_ms_{-1};
  float last_click_x_{0.0f};
  int click_streak_{0};
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::TextArea, "TextArea")
