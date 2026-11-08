#pragma once

/// @file input_box.hpp
/// @brief `Input`：单行文本输入框。。
///
/// 从 `input.hpp` 拆出（**一组件一头**）。拆分规则与理由见 `st/ui/components/README.md`：
/// 一个头一个组件 → 依赖关系写在组件自己身上 → include 谁就只有谁进产物。
/// `input.hpp` 仍保留**聚合**（只 include 各子头），既有引用不必一次改完。

#include <functional>
#include <string>
#include <string_view>
#include "st/ui/component_id.hpp"
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
  /// 选区（字节偏移，**已归一化**且已夹到码点边界）；无选择时 `begin == end`。
  ///
  /// 单行框的选区不需要“行号”——用**字节区间**表达就够。
  /// 与 `CodeEditor` / `MarkdownView` / `Terminal` 的 `selection` 属性同一语义。
  [[nodiscard]] auto selection() const -> std::pair<std::size_t, std::size_t>;
  /// 设置选区（反向传参会自动归一；两端都夹到码点边界与文本长度内）。
  void set_selection(std::size_t anchor, std::size_t focus);
  /// 清空选区（不动光标）。
  void clear_selection() noexcept { select_anchor_ = kNoSelection; }
  /// 是否有非空选区。
  [[nodiscard]] auto has_selection() const -> bool;
  /// 选中文本（**密码框恒为空**——不给“绕过掩码取明文”的口子）。
  [[nodiscard]] auto selected_text() const -> std::string;

  [[nodiscard]] auto value() const noexcept -> const std::string& { return text_; }
  [[nodiscard]] auto placeholder() const noexcept -> const std::string& { return placeholder_; }
  [[nodiscard]] auto password() const noexcept -> bool { return password_; }
  [[nodiscard]] auto icon_prefix() const noexcept -> const std::string& { return icon_prefix_; }
  [[nodiscard]] auto cursor_index() const noexcept -> std::size_t { return cursor_; }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;

  // 属性面：协议 `set` / 脚本 `$('#x').set()` 的**唯一入口**。
// 不实现它，`{"value": "x"}` 会被静默忽略——实测踩过：对输入框 set value 一直无效且不报错。
[[nodiscard]] auto get_property(std::string_view name) const
-> std::optional<std::string> override;
auto set_property(std::string_view name, std::string_view value) -> bool override;
[[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 只读属性（`selection`）：`get` 能读、`set` 不生效。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
/// 动作面（与 `TextArea` 对齐；此前单行 Input 缺这一面——DESIGN §8.1.1 反推的
/// 「API 存在但动作面未实现」同族缺口）：`submit`/`activate`（触发 `on_submit`）、
/// `clear`（清空）。
[[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
-> bool override;

  /// 主题变化 → 重落底色/描边（见 `.cpp` 处的注释：它们原本只在 measure 里
  /// 做一次性“未设则填”，切主题时 style 会永久停在旧值）。
  void apply_theme(const Theme& theme) override;

  /// 内容变化回调（实参为最新文本，指向组件内部存储）。
  std::function<void(std::string_view)> on_change{};
  /// `Enter` 提交回调。
  std::function<void(std::string_view)> on_submit{};

  private:
 /// 我方上一次写进 `style_.background` / `border_color` 的值（全透明 = 从未写过）。
 /// 用于区分“这个色是主题落的”与“这个色是调用方显式设的”——前者该随主题更新，
 /// 后者不能动。仅看 `a == 0` 不够：首轮填充后它就再也分不清两者了。
 math::Color applied_theme_bg_{};
 math::Color applied_theme_border_{};
 /// 底色/描边是否已由主题接管（首轮无条件接管一次，之后才看“有没有被调用方改过”）。
 bool theme_style_owned_{false};
 [[nodiscard]] auto inner_box(const RenderContext& context) const -> math::Rect;
  [[nodiscard]] auto display_text() const -> std::string;
  [[nodiscard]] auto cursor_x(const RenderContext& context) const -> float;
  /// 给定**字节索引** → 屏幕 x（与 `cursor_x` 同算法；选区高亮两端用它）。
  [[nodiscard]] auto index_to_x(const RenderContext& context, std::size_t text_index) const -> float;
  [[nodiscard]] auto index_at_x(const RenderContext& context, float x) const -> std::size_t;
  auto handle_key(const Event& event) -> bool;
  void insert_text(std::string_view inserted);
  void notify_change();

  std::string text_{};
  std::string placeholder_{};
  std::string icon_prefix_{};
  std::size_t cursor_{0};
  /// 输入法预编辑串（显示态；**不进 `text_`**）。
  std::string preedit_{};
  /// 上一帧绘制的插入点矩形（`caret_rect()` 用；它拿不到 `RenderContext`）。
  mutable math::Rect last_caret_rect_{};
  bool password_{false};
  /// 上一次绘制时光标的**闪烁可见相位**；只在真翻转的那帧才上报损坏区
  ///（每帧报会与帧间损坏判据互相触发成每帧续帧循环，见 Terminal 同名成员）。
  /// `-1` = 从未画过。mutable：paint 是 const 路径。
  mutable int last_blink_visible_{-1};

  // —— 选区（2026-10-14）——
  //
  // 单行框的选区只需**一个字节偏移**：锚点与光标（`cursor_`）就是两端。
  // （不用 `Caret{line, offset}`：这里根本没有“行”这个维度，多一层反而易错。）
  // `kNoSelection` = 无选区（与“锚点 == 光标”区分开：单击不应当成“有选择”）。
  static constexpr std::size_t kNoSelection = std::numeric_limits<std::size_t>::max();
  std::size_t select_anchor_{kNoSelection};   ///< 拖选起点（按下那一端）
  bool selecting_{false};                     ///< 鼠标拖选中
  /// 双击/三击判定：上次按下的时间与位置（时间取自 `RenderContext`）。
  std::int64_t last_click_ms_{-1};
  float last_click_x_{0.0f};
  int click_streak_{0};   ///< 连击次数（1=单击、2=双击选词、3=三击整行）

  /// 选区两端（已归一 + 夹取）；无选区时 `{cursor_, cursor_}`。
  [[nodiscard]] auto ordered_selection() const -> std::pair<std::size_t, std::size_t>;
  /// 删除当前选区（用于替换输入）；返回是否有东西被删。
  auto erase_selection() -> bool;
  /// 在光标处插入并清空选区。**这是所有文本插入的唯一入口**——
  /// 否则“有选区时插入”的行为会在多处各写一遍（必然漏一处）。
  void replace_selection_with(std::string_view inserted);
};

}  // namespace st::ui

ST_UI_COMPONENT(::st::ui::Input, "Input")
