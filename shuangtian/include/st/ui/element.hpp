#pragma once

/// 组件树核心：Element 基类（布局/绘制/事件/语义）+ 语义树与视觉树快照（控制通道 tree/visual 用）。
/// 设计原则：组件是**自绘**的——没有系统控件，全部像素由 renderer 产生，外观跨平台一致。

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/style.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

using ElementId = std::string;

/// 语义角色（无障碍与自动化对齐；控制通道 `tree`/`find` 的选择维度）。
enum class Role : std::uint8_t {
  None,
  Panel,
  Text,
  Heading,
  Button,
  Link,
  Icon,
  TextInput,
  TextArea,
  Checkbox,
  Radio,
  Switch,
  Slider,
  Select,
  List,
  ListItem,
  Table,
  Cell,
  Tab,
  ProgressBar,
  Dialog,
  Tooltip,
  ScrollBar,
  Separator,
  Image,
  Markdown,
  Code,
};

[[nodiscard]] constexpr auto to_string(Role role) noexcept -> std::string_view {
  switch (role) {
    case Role::None: return "none";
    case Role::Panel: return "panel";
    case Role::Text: return "text";
    case Role::Heading: return "heading";
    case Role::Button: return "button";
    case Role::Link: return "link";
    case Role::Icon: return "icon";
    case Role::TextInput: return "text_input";
    case Role::TextArea: return "text_area";
    case Role::Checkbox: return "checkbox";
    case Role::Radio: return "radio";
    case Role::Switch: return "switch";
    case Role::Slider: return "slider";
    case Role::Select: return "select";
    case Role::List: return "list";
    case Role::ListItem: return "list_item";
    case Role::Table: return "table";
    case Role::Cell: return "cell";
    case Role::Tab: return "tab";
    case Role::ProgressBar: return "progress_bar";
    case Role::Dialog: return "dialog";
    case Role::Tooltip: return "tooltip";
    case Role::ScrollBar: return "scroll_bar";
    case Role::Separator: return "separator";
    case Role::Image: return "image";
    case Role::Markdown: return "markdown";
    case Role::Code: return "code";
  }
  return "none";
}

inline constexpr std::string_view kFlagTrue = "true";

struct SemanticsFlags {
  bool visible{true};
  bool enabled{true};
  bool focused{false};
  bool hovered{false};
  bool pressed{false};
  bool selected{false};
  bool checked{false};
  bool scrollable{false};
  bool editable{false};
};

/// 语义树节点（`tree` 协议输出；不含绘制信息）。
struct SemanticsNode {
  ElementId id{};
  std::string type{};
  Role role{Role::None};
  math::Rect bounds{};
  std::string text{};
  std::string value{};
  SemanticsFlags flags{};
  std::vector<SemanticsNode> children{};
};

/// 视觉树节点（`visual` 协议输出：实际画了什么、命中区在哪）。
struct VisualNode {
  ElementId id{};
  std::string type{};
  math::Rect bounds{};
  bool visible{true};
  float opacity{1.0f};
  std::string fill{};      ///< 主填充（css 字符串，如 `#2563eb`）
  float radius{0.0f};
  std::string text{};      ///< 该层绘制的文本（若有）
  bool hit_target{false};  ///< 是否参与命中测试
  std::uint32_t depth{0};
  std::vector<VisualNode> children{};
};

/// 布局约束。
struct Constraints {
  float max_width{kUnbounded};
  float max_height{kUnbounded};
  float available_width{kUnbounded};
  float available_height{kUnbounded};
};

enum class EventKind : std::uint8_t {
  MouseMove,
  MouseDown,
  MouseUp,
  Click,
  DoubleClick,
  TripleClick,
  Wheel,
  KeyDown,
  KeyUp,
  TextInput,
  FocusIn,
  FocusOut,
  HoverIn,
  HoverOut,
};

struct Event {
  EventKind kind{EventKind::MouseMove};
  math::Point position{};
  int button{0};
  int click_count{1};
  float wheel_delta{0.0f};
  std::string key{};
  std::string code{};
  std::string text{};
  bool ctrl{false};
  bool shift{false};
  bool alt{false};
  bool meta{false};
  bool handled{false};
};

/// 命中测试与绘制所需的渲染环境。
struct RenderContext {
  const Theme& theme;
  const TextPort* text{nullptr};  ///< 空时退化为 `NullTextPort`
  double time_seconds{0.0};
};

class Element {
 public:
  Element();
  virtual ~Element();
  Element(const Element&) = delete;
  auto operator=(const Element&) -> Element& = delete;

  // —— 标识与类型 ——
  [[nodiscard]] auto id() const noexcept -> const ElementId& { return id_; }
  void set_id(ElementId id) { id_ = std::move(id); }

  /// 稳定逻辑身份（可选）：同一逻辑元素在**重建/重排后仍拿到同一个 id**。
  ///
  /// 自动 id 是路径式的（`Type[index]`），索引会随插入/删除/筛选整体位移，于是
  /// 「列表刷新后原来看中的那一项变成别的东西」。显式 `set_id` 能解决，但要求调用方
  /// 同时维护「元素到 id」的映射、重建时把 id 重新贴回去，容易漏。
  ///
  /// `key` 与之互补：只描述「这个元素在兄弟之间是谁」（业务身份，如任务 id），
  /// 路径拼装、去重、选择器安全性交给框架。带 key 的自动 id 形如 `tasks/ListItem@task-42`。
  [[nodiscard]] auto key() const noexcept -> const std::string& { return key_; }
  void set_key(std::string key) { key_ = std::move(key); }
  [[nodiscard]] virtual auto type() const noexcept -> std::string_view { return "Element"; }
  [[nodiscard]] virtual auto role() const noexcept -> Role { return Role::None; }

  // —— 树结构 ——
  [[nodiscard]] auto parent() const noexcept -> Element* { return parent_; }
  /// 路径式自动 id（未显式设置时由 UiRoot 在挂载时生成）。
  [[nodiscard]] auto derived_id() const -> ElementId;
  [[nodiscard]] auto children() const noexcept -> std::span<const std::unique_ptr<Element>> {
    return children_;
  }
  [[nodiscard]] auto children() noexcept -> std::span<std::unique_ptr<Element>> { return children_; }
  auto add_child(std::unique_ptr<Element> child) -> Element*;
  auto insert_child(std::size_t index, std::unique_ptr<Element> child) -> Element*;
  auto remove_child(Element* child) -> std::unique_ptr<Element>;
  void clear_children();
  [[nodiscard]] auto child_count() const noexcept -> std::size_t { return children_.size(); }
  [[nodiscard]] auto child_at(std::size_t index) const noexcept -> Element*;

  // —— 样式 ——
  [[nodiscard]] auto style() noexcept -> Style& { return style_; }
  [[nodiscard]] auto style() const noexcept -> const Style& { return style_; }
  void set_style(Style style) { style_ = std::move(style); }
  [[nodiscard]] auto visible() const noexcept -> bool { return visible_; }
  void set_visible(bool visible) noexcept { visible_ = visible; }
  [[nodiscard]] auto enabled() const noexcept -> bool { return enabled_; }
  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
  [[nodiscard]] auto focusable() const noexcept -> bool { return focusable_; }
  void set_focusable(bool value) noexcept { focusable_ = value; }
  [[nodiscard]] virtual auto hit_test(math::Point) const noexcept -> bool;

  // —— 交互状态 ——
  [[nodiscard]] auto hovered() const noexcept -> bool { return hovered_; }
  [[nodiscard]] auto pressed() const noexcept -> bool { return pressed_; }
  [[nodiscard]] auto focused() const noexcept -> bool { return focused_; }
  void set_hovered(bool value) noexcept { hovered_ = value; }
  void set_pressed(bool value) noexcept { pressed_ = value; }
  virtual void set_focused(bool value) noexcept { focused_ = value; }

  // —— 布局与绘制 ——
  [[nodiscard]] auto bounds() const noexcept -> math::Rect { return bounds_; }
  [[nodiscard]] auto measured_size() const noexcept -> math::Size { return measured_; }
  /// 从主题刷新 `style_`（UiRoot 在布局前按脏标记调用）：组件据此把 token 落到具体样式。
  virtual void apply_theme(const Theme& theme) { (void)theme; }
  /// 计算自身尺寸（写入 `measured_`）；容器组件需递归测量子节点。
  virtual void measure(const RenderContext& context, const Constraints& constraints);
  /// 应用最终矩形并布局子节点。
  virtual void arrange(const RenderContext& context, math::Rect rect);
  /// 绘制自身与子节点（坐标已由 arrange 定好，直接画到画布绝对坐标）。
  virtual void paint(const RenderContext& context, raster::Canvas& canvas) const;
  /// 子类绘制自身内容（在 paint_box 之后、子节点之前）。
  virtual void paint_content(const RenderContext& context, raster::Canvas& canvas) const {
    (void)context;
    (void)canvas;
  }

  // —— 事件 ——
  /// 返回 true 表示事件已处理（不再冒泡）。
  virtual auto on_event(const RenderContext& context, Event& event) -> bool;
  /// 键盘激活（Enter/Space）。
  virtual void activate() {}

  // —— 语义与视觉快照 ——
  [[nodiscard]] virtual auto semantics_text() const -> std::string { return {}; }
  [[nodiscard]] virtual auto semantics_value() const -> std::string { return {}; }
  [[nodiscard]] virtual auto semantics_flags() const -> SemanticsFlags;
  void collect_semantics(SemanticsNode& node) const;
  void collect_visual(VisualNode& node) const;

  // —— 通用属性面（控制通道 get/set 用；组件按需覆写）——
  /// 读取自定义属性（返回空表示不支持）。
  [[nodiscard]] virtual auto get_property(std::string_view name) const
      -> std::optional<std::string>;
  /// 写入自定义属性；返回是否生效。
  virtual auto set_property(std::string_view name, std::string_view value) -> bool;
  /// 组件暴露的可读写属性名。
  [[nodiscard]] virtual auto property_names() const -> std::vector<std::string_view>;
  /// 动作触发（`invoke`；返回是否已处理）。
  [[nodiscard]] virtual auto invoke_action(std::string_view action, std::string_view argument)
      -> bool;

  /// 标记需要重新布局/重绘（由 UiRoot 消费）。
  void mark_dirty();
  void mark_layout_dirty();
  [[nodiscard]] auto dirty() const noexcept -> bool { return dirty_; }
  [[nodiscard]] auto layout_dirty() const noexcept -> bool { return layout_dirty_; }
  void clear_dirty() noexcept;

 protected:
  /// 子类绘制自身的"盒子"（背景/边框/圆角/阴影）。
  void paint_box(const RenderContext& context, raster::Canvas& canvas) const;
  /// 子类绘制文本（自动按 `text_align` 定位）。
  auto paint_text(const RenderContext& context, raster::Canvas& canvas, std::string_view text,
                  math::Rect box) const -> void;
  /// 布局子节点（Flex 子集：方向/间距/增长/对齐）。
  void layout_children(const RenderContext& context, math::Rect content);
  [[nodiscard]] auto content_box() const noexcept -> math::Rect;

  Style style_{};
  math::Rect bounds_{};
  math::Size measured_{};
  ElementId id_{};
  std::string key_{};
  Element* parent_{nullptr};
  std::vector<std::unique_ptr<Element>> children_{};
  bool visible_{true};
  bool enabled_{true};
  bool focusable_{false};
  bool hovered_{false};
  bool pressed_{false};
  bool focused_{false};
  bool dirty_{true};
  bool layout_dirty_{true};
};

/// 便捷容器：行/列布局面板。
class Panel : public Element {
 public:
  explicit Panel(FlexDirection direction = FlexDirection::Column);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Panel"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }
};

/// 空占位（用于弹性间距）。
class Spacer : public Element {
 public:
  explicit Spacer(float size = 0.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Spacer"; }
};

}  // namespace st::ui
