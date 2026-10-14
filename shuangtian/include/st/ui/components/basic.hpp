#pragma once

/// 基础组件：文本 / 标题 / 图标 / 按钮 / 卡片 / 分隔线 / 徽标行。
/// 全部自绘：颜色与尺度一律取自 `Theme`（`apply_theme` 时落进 `style_`），不在组件里硬编码。

#include <functional>
#include <string>
#include <string_view>

#include "st/ui/element.hpp"
#include "st/ui/icon.hpp"

namespace st::ui {

/// 文本标签（单行省略，或 `set_multiline` 折行显示）。
class Text : public Element {
 public:
  explicit Text(std::string content = {});
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Text"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_content(std::string content);
  [[nodiscard]] auto content() const -> const std::string& { return content_; }
  /// 事件：仅在 `set_selectable(true)` 时处理拖选 / `Ctrl+A` / `Ctrl+C`（其余一律放行）。
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void set_multiline(bool multiline);
  void set_max_lines(std::size_t lines);
  void set_tone(Tone tone);
  void set_font_size(float size);
  void set_weight(FontWeight weight);

  /// **可选**（默认关）：开启后文本可用鼠标拖选、`Ctrl+A` 全选、`Ctrl+C` 复制选中。
  ///
  /// 为何默认关（而不是像 MarkdownView 那样默认开）：
  /// 静态标签遍布界面（表单说明、列表副标题、状态文字…），若默认抢拖选手势，
  /// 用户想拖动窗口/划选整个区域时会被逐个标签截断——那不是“能力”，是干扰。
  /// 需要“能选中某一小段”的展示型文本显式打开。
  void set_selectable(bool selectable);
  [[nodiscard]] auto selectable() const noexcept -> bool { return selectable_; }
  /// 选中文本（无选区时为空串）。
  [[nodiscard]] auto selected_text() const -> std::string;
  /// 是否有非空选区。
  [[nodiscard]] auto has_selection() const -> bool;
  /// 清空选区 / 全选（自动化与宿主动作面用）。
  void clear_selection() noexcept;
  void select_all();

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return content_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  /// 只读属性（`selection`）：`get` 能读、`set` 不生效。
  [[nodiscard]] auto readonly_property_names() const -> std::vector<std::string_view> override;
  /// `copy`：把文本内容写进宿主剪贴板（2026-10-12——文本标签此前完全无法复制：
  /// 键盘、动作面、协议全无路径，右键菜单也无从谈起）。
  /// 控制通道 `invoke <id> copy` 即可达；写失败（无宿主）返回 false 如实上报。
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

 private:
  /// 一条可见行（含它在本控件内的 x 区间）——选区与绘制共用同一份。
  struct RenderedLine {
    std::string text{};   ///< 本行的**显示文本**（折行/截断后的结果，不是原文切片）
    float x{0.0f};        ///< 行首 x（受 `text_align` 影响）
    float width{0.0f};
    float y{0.0f};
  };
  /// 算出当前应当显示的每一行（与 `paint_content` / `measure` 同一套口径）。
  ///
  /// 为何要抽出来：选区命中必须建在**实际显示的行**上，而不是原文字符串的
  /// 下标——折行与 `max_lines_` 截断会让两者彻底不同（截断后多出的字根本不展示，
  /// 选中它就会出现“选了但看不见”）。
  [[nodiscard]] auto rendered_lines(const RenderContext& context) const -> std::vector<RenderedLine>;
  /// 坐标 → `{行下标, 行内字节偏移}`（越界夹到首/末行）。
  [[nodiscard]] auto caret_at(const RenderContext& context, math::Point point) const
      -> std::pair<std::size_t, std::size_t>;
  /// `{行下标, 行内偏移}` → 屏幕 x。
  [[nodiscard]] auto offset_to_x(const RenderContext& context, std::size_t line,
                                 std::size_t offset) const -> float;

  std::string content_{};
  bool multiline_{false};
  std::size_t max_lines_{0};
  Tone tone_{Tone::Default};
  float font_size_override_{0.0f};
  FontWeight weight_{FontWeight::Regular};

  // —— 选区（2026-10-14）——
  // 用 `{行, 行内字节偏移}`（而不是全串字节偏移）：`Text` 的显示行是**离散字符串
  // 数组**（折行/截断产生），没有“全串偏移 ↔ 显示位置”的线性映射。
  bool selectable_{false};
  /// 上次排版得到的**显示行文本**（`measure`/`paint_content` 时写）。
  ///
  /// 为何缓存：① `selected_text()` 无上下文、无从折行；② 更重要的是
  /// “选中的字”必须与“屏幕上看到的字”是同一份——现场重折行会在窗口尺寸
  /// 变化后选出别处的字。
  /// `mutable`：在 `paint_content`（const）里也要写。
  mutable std::vector<std::string> display_lines_{};
  /// 显示行的**行宽缓存**（与 `display_lines_` 一一对应）。
  ///
  /// 为何需要（2026-10-14，实测）：多行 `Text` 的 `paint_content` 过去每帧
  /// 都 `port.wrap(content_)` **重新折行全部内容**、并逐行 `measure_width`、
  /// 再逐行 `draw`——全不看视口。把整份日志交给一个多行 `Text` 的场景
  /// （`Terminal` 行模式就是这么做的）因而以**内容长度**为每帧成本：
  /// 实测稳态帧 500 行 **47 ms** / 2000 行 **705 ms** / 20000 行 **3299 ms**
  /// （`tools/ui_big_editors_bench.cpp`）。
  ///
  /// 修法：折行结果与行宽都**按 (`content_`, `bounds_.width`, 字号) 缓存**，
  /// 三者不变就复用（滚动时它们不变——滚动是内容顶的偏移，不是几何变化），
  /// 且 `paint_content` 按裁剪区**只画可见那几行**。
  ///
  /// 为何缓存的三元组是这几个：折行宽度只取决于可用宽度与字号
  /// （`wrap(content, size, max_width)`），内容变了才需重折；对齐只影响行首 x
  /// （不参与缓存，每帧现算）。
  mutable std::vector<float> display_line_widths_{};
  mutable std::string layout_cache_content_{};
  mutable float layout_cache_width_{-1.0F};
  mutable float layout_cache_font_size_{-1.0F};
  /// 选区两端（已归一）；`line == npos` = 无选区。
  std::size_t select_begin_line_{std::string::npos};
  std::size_t select_begin_offset_{0};
  std::size_t select_end_line_{0};
  std::size_t select_end_offset_{0};
  bool selecting_{false};
};

/// 标题（按层级取字号与字重）。
class Heading : public Text {
 public:
  explicit Heading(std::string content = {}, std::uint32_t level = 1);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Heading"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Heading; }
  void set_level(std::uint32_t level);
  [[nodiscard]] auto level() const noexcept -> std::uint32_t { return level_; }
  void apply_theme(const Theme& theme) override;

 private:
  std::uint32_t level_{1};
};

/// 图标。
class IconView : public Element {
 public:
  explicit IconView(std::string name = {}, float size = 18.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Icon"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Icon; }

  void set_icon(std::string name);
  [[nodiscard]] auto icon() const -> const std::string& { return icon_; }
  void set_size(float size);
  void set_tone(Tone tone);
  [[nodiscard]] auto size() const noexcept -> float { return size_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return icon_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  // ⚠ `property_names()` 必须与 `get/set_property` **对称**：
  //
  // `ui::apply_properties` 的判据是“属性名在元素自己声明的清单里”（那是为了
  // 消除“读得到、改不了”的分裂）。只实现 get/set 而不报名字，就意味着
  // **这条路永远命不中它**——`set {icon: …}` 会静默什么都不做。
  // 实测：`<Icon name="search"/>` 走 XML 建树时报“值没被接受”，根因就在这里。
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

 private:
  std::string icon_{};
  float size_{18.0f};
  Tone tone_{Tone::Default};
};

/// 按钮（含变体与尺寸；支持前导图标与点击回调）。
class Button : public Element {
 public:
  enum class Variant : std::uint8_t { Primary, Secondary, Ghost, Soft, Danger };
  /// 内容横向对齐（与布局侧的 `st::ui::Align` 同名同义，但作为按钮自己的接口
  /// 暴露：调用方写 `Button::Align::Start` 才读得出“这是按钮的内容对齐”）。
  ///
  /// 为何需要它（2026-10-11，实测）：默认**居中**对单个按钮是对的，但
  /// **列表行**用的也是 Button（`Ghost` + `grow` 铺满一行）。居中的行文字在
  /// 窄栏里会被推到中间，几十行叠在一起左缘参差不齐——列表是拿来“扫”的，
  /// 扫的是左缘。实测截图：Git 日志三行各自居中，看着像三个独立按钮
  /// 而不是一个列表。
  ///
  /// 默认 `Center` 保证既有界面零变化；列表行显式写 `Start`。
  using Align = st::ui::Align;
  enum class Size : std::uint8_t { Small, Medium, Large };

  explicit Button(std::string label, Variant variant = Variant::Primary,
                  Size size = Size::Medium);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Button"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Button; }

  void set_label(std::string label);
  [[nodiscard]] auto label() const -> const std::string& { return label_; }
  void set_icon(std::string icon);
  void set_variant(Variant variant);
  void set_size(Size size);
  void set_tone(Tone tone);
  /// 内容横向对齐（默认居中；列表行传 `Start`）。
  void set_align(Align align) noexcept { content_align_ = align; }
  [[nodiscard]] auto align() const noexcept -> Align { return content_align_; }
  [[nodiscard]] auto variant() const noexcept -> Variant { return variant_; }

  /// **图标盒径**（逻辑 px）——与 `apply_theme` 同一入口写入，供测试/宿主读取。
  ///
  /// 为何是一等读取面：图标尺寸是观感量，需要一个**被测对象自报的值**
  /// （`perceptual_changes.md` §14：不要信旁路探针）。测试直接读它，
  /// 就不必在测试里复刻一遍公式（复刻的公式一旦与实现脱钩就成假护栏）。
  [[nodiscard]] auto icon_box() const noexcept -> float { return icon_size_; }

  /// 当前图标在 `icon_box()` 盒里的**实际墨迹尺寸**（逻辑 px）。
  ///
  /// 排版（`measure`）与绘制（`paint_content`）都必须用它而不是盒宽：盒里
  /// 有透明留白，按盒宽排版会让内容重心偏向文字一侧（见 `measure` 里的推导）。
  [[nodiscard]] auto icon_ink_size() const -> math::Size {
    return Icon::ink_size(icon_, math::Size{icon_size_, icon_size_});
  }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;
  void activate() override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_flags() const -> SemanticsFlags override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  std::function<void()> on_click{};
  /// 图标在左（默认）或右。
  bool icon_leading{true};

 private:
  std::string label_{};
  std::string icon_{};
  Variant variant_{Variant::Primary};
  Size size_{Size::Medium};
  Tone tone_{Tone::Default};
  /// 图标盒径（由 `apply_theme` 按 Size 档从 `Metrics::icon_size*` 取）。
  float icon_size_{22.0f};
  /// 内容横向对齐（**与 `style_.align_items` 区分**：那个管的是“图标与文字在
  /// 交叉轴上如何对齐”，本字段管的是“整块内容在按钮里靠哪边”）。
  Align content_align_{Align::Center};
};

/// 卡片容器：`surface` 底 + 描边 + 圆角 + 阴影 + 内边距。
class Card : public Element {
 public:
  explicit Card(float padding = 16.0f);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Card"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Panel; }

  void set_padding(float padding);
  void set_radius(float radius);
  void set_shadow_level(std::uint8_t level);
  void set_elevated(bool elevated);
  void apply_theme(const Theme& theme) override;

 private:
  float padding_{16.0f};
  float radius_override_{0.0f};
  /// 0=无阴影、1=sm、≥2=md（`elevated_` 为真时一律 lg）。
  /// 卡片默认用 **md**：浅底上的卡片需要“真的浮起来”，而 sm 更像一道描边。
  std::uint8_t shadow_level_{2};
  bool elevated_{false};
};

/// 水平分隔线。
class Divider : public Element {
 public:
  explicit Divider(bool vertical = false);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Divider"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Separator; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

 private:
  bool vertical_{false};
};

/// 键值行（左标签右值，用于详情面板）。
class KeyValueRow : public Element {
 public:
  KeyValueRow(std::string key, std::string value);
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "KeyValueRow"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Text; }

  void set_value(std::string value);
  [[nodiscard]] auto value() const -> const std::string& { return value_; }
  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto semantics_value() const -> std::string override { return value_; }

 private:
  /// 显示标签（**不是** `Element::key_`）：基类的 key 是稳定逻辑身份（参与自动 id
  /// `Type@key`），把界面文案当身份会让 id 随文案变动而漂移。
  std::string label_{};
  std::string value_{};
};

}  // namespace st::ui
