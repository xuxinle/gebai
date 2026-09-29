#pragma once

/// 组件库 A · 文本输入（`DESIGN.md` §4.5）：`Input`（单行）与 `TextArea`（多行）。
///
/// 依赖纪律（`CONVENTIONS.md`）：
/// - 文本度量与绘制一律经 `context.text`（`const TextPort*`，为空退化为 `NullTextPort`），
///   ui 层不 include text 层头文件；
/// - 外观只取 `context.theme` 的 token（背景/边框/圆角/控件高度/字号/动效），无硬编码颜色；
/// - 组件在 `measure()` 中把主题 token 同步到 `style_`（背景/圆角/边框），
///   以便 `visual` 树如实上报外观；显式设置过 `style().radius`/`background` 的实例不被覆盖。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 单行文本输入框。
///
/// 交互：点击定位光标、`Home`/`End`/方向键移动、`TextInput` 插入、`Backspace`/`Delete` 删除、
/// `Enter` 触发 `on_submit`；内容变化触发 `on_change`。光标闪烁由 `context.time_seconds` 驱动（1s 周期）。
/// 语义：`text` 暴露占位符（无障碍名称），`value` 暴露当前文本（密码态为掩码 `•`）。
class Input : public Element {
 public:
  Input();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Input"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::TextInput; }

  /// 设置文本（光标移到末尾）。
  void set_text(std::string text);
  /// 占位文本（内容为空时以 `text_faint` 绘制）。
  void set_placeholder(std::string text);
  /// 密码模式：显示掩码 `•`（`value()` 仍返回明文）。
  void set_password(bool value) noexcept;
  /// 前缀图标名（图标集由后续模块提供；当前仅预留占位宽度，不绘制图形）。
  void set_icon_prefix(std::string_view icon_name);
  /// 设置光标字节位置（自动夹取到合法 UTF-8 码点边界）。
  void set_cursor_index(std::size_t index);

  [[nodiscard]] auto value() const noexcept -> const std::string& { return text_; }
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  [[nodiscard]] auto password() const noexcept -> bool { return password_; }
  [[nodiscard]] auto icon_prefix() const noexcept -> const std::string& { return icon_prefix_; }
  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  /// 内容变化回调（实参为最新文本，指向组件内部存储）。
  std::function<void(std::string_view)> on_change{};
  /// `Enter` 提交回调。
  std::function<void(std::string_view)> on_submit{};

 private:
  [[nodiscard]] auto inner_box(const RenderContext& context) const -> math::Rect;
  [[nodiscard]] auto display_text() const -> std::string;
  [[nodiscard]] auto cursor_x(const RenderContext& context) const -> float;
  [[nodiscard]] auto index_at_x(const RenderContext& context, float x) const -> std::size_t;
  auto handle_key(const Event& event) -> bool;
  void insert_text(std::string_view inserted);
  void notify_change();

  std::string text_{};
  std::string placeholder_{};
  std::string icon_prefix_{};
  std::size_t cursor_{0};
  bool password_{false};
};

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

  [[nodiscard]] auto value() const noexcept -> const std::string& { return text_; }
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }
  [[nodiscard]] auto scroll_offset() const noexcept -> float { return scroll_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Canvas& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

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
};

}  // namespace st::ui
