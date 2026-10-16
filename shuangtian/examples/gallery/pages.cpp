#include "pages.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "battery/embed.hpp"  // 编译期资源嵌入（stpm 生成）
#include "st/core/fs.hpp"      // FileDialog 卡起始目录（st::fs::current_dir）
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/color_picker.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/completion_popup.hpp"
#include "st/ui/components/diff_view.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/select.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/components/timeline_view.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/components/viewport.hpp"
#include "st/ui/components/scene_view.hpp"
#include "st/ui/dsl.hpp"          // 声明式页（声明式 UI 的演示）
#include "st/ui/icon.hpp"
#include "st/ui/theme.hpp"

namespace gallery {
namespace {

using st::math::Insets;
using st::math::Point;
using st::math::Rect;
using st::ui::Align;
using st::ui::Avatar;
using st::ui::Badge;
using st::ui::Button;
using st::ui::Card;
using st::ui::Checkbox;
using st::ui::CodeEditor;
using st::ui::ColorPicker;
using st::ui::CommandPalette;
using st::ui::CompletionItemView;
using st::ui::CompletionPopup;
using st::ui::ContextMenu;
using st::ui::Chip;
using st::ui::DiffLayout;
using st::ui::DiffView;
using st::ui::Divider;
using st::ui::Element;
using st::ui::FileDialog;
using st::ui::FlexDirection;
using st::ui::FontWeight;
using st::ui::Heading;
using st::ui::Icon;
using st::ui::IconView;
using st::ui::Input;
using st::ui::Justify;
using st::ui::KeyValueRow;
using st::ui::List;
using st::ui::MarkdownView;
using st::ui::Menu;
using st::ui::MenuBar;
using st::ui::MenuItem;
using st::ui::MenuPanel;
using st::ui::MergeView;
using st::ui::Panel;
using st::ui::ProgressBar;
using st::ui::Radio;
using st::ui::Select;
using st::ui::Slider;
using st::ui::Spinner;
using st::ui::SplitView;
using st::ui::Switch;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::Tabs;
using st::ui::Terminal;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::TimelineView;
using st::ui::Tone;
using st::ui::Tooltip;
using st::ui::Tree;
using st::ui::TreeNode;
using st::ui::Viewport;
// 时间轴模型的四个名字（TimelineView 卡直接填 Model：与 caiyun 几何层回流同一套数据层）。
using st::ui::timeline::Clip;
using st::ui::timeline::Millis;
using st::ui::timeline::SnapOptions;
using st::ui::timeline::Track;

// ————————————————————————————————————————————————————————————————————————————
// 通用小组件：让每个页面用同一套节奏，避免"每页各写一种间距"
// ————————————————————————————————————————————————————————————————————————————

/// 页面容器：列 + 统一内边距/间距 + 页标题与副标题。
[[nodiscard]] auto make_page(std::string_view id, std::string_view title,
                             std::string_view subtitle) -> std::unique_ptr<Panel> {
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id(std::format("page-{}", id));
  page->style().padding = Insets::all(24.0f);
  page->style().gap = 16.0f;
  page->add_child(std::make_unique<Heading>(std::string(title), 2));
  auto sub = std::make_unique<Text>(std::string(subtitle));
  sub->set_tone(Tone::Muted);
  page->add_child(std::move(sub));
  return page;
}

/// 带标题的分组卡片（页面的基本分节单位）。
[[nodiscard]] auto make_card(std::string_view id, std::string_view title)
    -> std::unique_ptr<Card> {
  auto card = std::make_unique<Card>(18.0f);
  card->set_id(std::string(id));
  card->style().direction = FlexDirection::Column;
  card->style().gap = 12.0f;
  if (!title.empty()) {
    auto heading = std::make_unique<Text>(std::string(title));
    heading->set_weight(FontWeight::SemiBold);
    card->add_child(std::move(heading));
  }
  return card;
}

/// 横向容器（行 + 间距 + 可选换行）。
[[nodiscard]] auto make_row(float gap = 8.0f, bool wrap = false) -> std::unique_ptr<Panel> {
  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().gap = gap;
  row->style().wrap = wrap;
  row->style().align_items = Align::Center;
  return row;
}

/// 小节标签（卡片内的次级说明）。
[[nodiscard]] auto make_caption(std::string_view text) -> std::unique_ptr<Text> {
  auto caption = std::make_unique<Text>(std::string(text));
  caption->set_tone(Tone::Faint);
  caption->set_font_size(11.0f);
  return caption;
}

/// 统计卡片：大数字 + 标签 + 图标。
/// `value_out` 非空时回填大数字那个 `Text*`——调用方需要**运行时改值**时用得上
/// （比"再按 id 去全树里查一遍"直接得多，也不依赖 id 拼写约定）。
[[nodiscard]] auto make_stat_card(const std::string& id, std::string_view icon, std::string label,
                                  std::string value, Tone tone,
                                  st::ui::Text** value_out = nullptr) -> std::unique_ptr<Card> {
  auto card = std::make_unique<Card>(16.0f);
  card->set_id(id);
  card->style().grow = true;
  card->style().direction = FlexDirection::Column;
  card->style().gap = 6.0f;

  auto header = make_row(8.0f);
  auto icon_view = std::make_unique<IconView>(std::string(icon), 16.0f);
  icon_view->set_tone(tone);
  header->add_child(std::move(icon_view));
  auto label_text = std::make_unique<Text>(std::move(label));
  label_text->set_tone(Tone::Muted);
  label_text->set_font_size(12.0f);
  header->add_child(std::move(label_text));
  card->add_child(std::move(header));

  auto value_text = std::make_unique<Text>(std::move(value));
  value_text->set_font_size(26.0f);
  value_text->set_weight(FontWeight::SemiBold);
  value_text->set_id(id + "-value");
  if (value_out != nullptr) *value_out = value_text.get();
  card->add_child(std::move(value_text));
  return card;
}

/// 自绘组件示例：进度条（示范"组件即代码"——不依赖内置控件也能得到同一套视觉语言）。
class ProgressRow : public Element {
 public:
  ProgressRow(std::string label, float value, Tone tone = Tone::Primary)
      : label_(std::move(label)), value_(value), tone_(tone) {
    set_id("progress-" + label_);
  }
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ProgressRow"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override {
    return st::ui::Role::ProgressBar;
  }

  void set_value(float value) {
    value_ = st::math::clamp01(value);
    mark_dirty();
  }
  [[nodiscard]] auto semantics_value() const -> std::string override {
    return std::format("{:.0f}%", static_cast<double>(value_ * 100.0f));
  }
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override {
    if (name == "value") return std::format("{:.3f}", static_cast<double>(value_));
    if (name == "label") return label_;
    return std::nullopt;
  }
  auto set_property(std::string_view name, std::string_view value) -> bool override {
    if (name == "value") {
      if (const auto parsed = st::parse_f64(value); parsed.has_value()) {
        set_value(static_cast<float>(*parsed));
        return true;
      }
      return false;
    }
    if (name == "label") {
      label_ = std::string(value);
      mark_layout_dirty();
      return true;
    }
    return false;
  }
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override {
    return {"value", "label"};
  }
  void apply_theme(const st::ui::Theme& theme) override {
    style_.font_size = theme.metrics().font_sm;
    style_.color = theme.colors().text_muted;
  }
  void measure(const st::ui::RenderContext& context,
               const st::ui::Constraints& constraints) override {
    (void)context;
    measured_ = st::math::Size{constraints.max_width, 34.0f};
  }
  void paint_content(const st::ui::RenderContext& context,
                     st::raster::Surface& canvas) const override {
    const auto& colors = context.theme.colors();
    const float label_height = style_.font_size * 1.4f;
    const float track_y = bounds_.y + label_height + 2.0f;
    const float track_height = 6.0f;
    const st::math::Color accent = st::ui::tone_color(context.theme, tone_);

    canvas.fill_rect(Rect{bounds_.x, track_y, bounds_.width, track_height},
                     st::raster::Paint::solid(colors.surface_sunken), track_height * 0.5f);
    const float filled = std::max(track_height, bounds_.width * value_);
    st::raster::Gradient gradient = st::raster::Gradient::linear(
        Point{bounds_.x, track_y}, Point{bounds_.x + filled, track_y},
        {{0.0f, accent}, {1.0f, accent.mix(colors.primary_hover, 0.35f)}});
    canvas.fill_rect(Rect{bounds_.x, track_y, filled, track_height},
                     st::raster::Paint::with_gradient(std::move(gradient)), track_height * 0.5f);

    if (context.text != nullptr) {
      context.text->draw(canvas, label_, Point{bounds_.x, bounds_.y}, style_.font_size,
                         colors.text);
      const std::string percent = semantics_value();
      const float width = context.text->measure_width(percent, style_.font_size);
      context.text->draw(canvas, percent, Point{bounds_.right() - width, bounds_.y},
                         style_.font_size, accent);
    }
  }

 private:
  std::string label_{};
  float value_{0.0f};
  Tone tone_{Tone::Primary};
};

/// 图标格：图标 + 名字（画廊要能**逐个核对**图标描边是否完整——这正是一处曾经缺陷的现场：
/// 描边由"线段四边形 + 顶点圆"拼成，绕向不一致时会被非零环绕规则错误抵消，图标缺半截弧线）。
class IconCell : public Element {
 public:
  explicit IconCell(std::string name) : name_(std::move(name)) {
    set_id("icon-cell-" + name_);
  }
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "IconCell"; }
  [[nodiscard]] auto semantics_text() const -> std::string override { return name_; }
  void apply_theme(const st::ui::Theme& theme) override {
    style_.color = theme.colors().text_muted;
    // 字号取 9：最长的图标名（`chevron-down` 12 字符）也要能放进格子，
    // 否则名字会溢出到相邻格、看起来像"图标排错了位"
    style_.font_size = 9.0f;
    border_ = theme.colors().border;
  }
  void measure(const st::ui::RenderContext& context,
               const st::ui::Constraints& constraints) override {
    (void)context;
    (void)constraints;
    measured_ = st::math::Size{kCellWidth, kCellHeight};
  }
  void paint_content(const st::ui::RenderContext& context,
                     st::raster::Surface& canvas) const override {
    const float icon_size = 18.0f;
    const float icon_x = bounds_.x + (bounds_.width - icon_size) * 0.5f;
    Icon::draw(canvas, name_, Rect{icon_x, bounds_.y + 6.0f, icon_size, icon_size},
               style_.color, 0.0f);
    if (context.text != nullptr) {
      const float width = context.text->measure_width(name_, style_.font_size);
      context.text->draw(canvas, name_, Point{bounds_.x + (bounds_.width - width) * 0.5f,
                                              bounds_.y + 6.0f + icon_size + 4.0f},
                         style_.font_size, context.theme.colors().text_faint);
    }
  }

  static constexpr float kCellWidth{78.0f};
  static constexpr float kCellHeight{58.0f};

 private:
  std::string name_{};
  st::math::Color border_{};
};

/// SVG 图标格：名字 + 同图标三档尺寸（16/32/64px）——矢量重栅的直观对照。
/// 画法走 `Icon::draw("svg:<id>")`（与内置表同一入口；注册表补位）。
class SvgIconCell : public Element {
 public:
  explicit SvgIconCell(std::string id) : icon_id_(std::move(id)) {
    set_id("svg-cell-" + icon_id_);
  }
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SvgIconCell"; }
  [[nodiscard]] auto semantics_text() const -> std::string override { return icon_id_; }
  void apply_theme(const st::ui::Theme& theme) override {
    style_.color = theme.colors().text;
    style_.font_size = 9.0f;
    border_ = theme.colors().border;
  }
  void measure(const st::ui::RenderContext& context,
               const st::ui::Constraints& constraints) override {
    (void)context;
    (void)constraints;
    measured_ = st::math::Size{kCellWidth, kCellHeight};
  }
  void paint_content(const st::ui::RenderContext& context,
                     st::raster::Surface& canvas) const override {
    // 三档尺寸纵向排：16（基线对齐名区）/ 32 / 64——宽度按最大档占位
    const std::string name = "svg:" + icon_id_;
    float x = bounds_.x + 6.0f;
    const float top = bounds_.y + 6.0f;
    Icon::draw(canvas, name, Rect{x, top, 16.0f, 16.0f}, style_.color, 0.0f);
    Icon::draw(canvas, name, Rect{x + 20.0f, top, 32.0f, 32.0f}, style_.color, 0.0f);
    Icon::draw(canvas, name, Rect{x + 56.0f, top, 64.0f, 64.0f}, style_.color, 0.0f);
    if (context.text != nullptr) {
      const float width = context.text->measure_width(icon_id_, style_.font_size);
      context.text->draw(canvas, icon_id_,
                         Point{bounds_.x + (bounds_.width - width) * 0.5f, top + 64.0f + 2.0f},
                         style_.font_size, context.theme.colors().text_faint);
    }
  }

  static constexpr float kCellWidth{132.0f};
  static constexpr float kCellHeight{86.0f};

 private:
  std::string icon_id_{};  // 不叫 id_——遮蔽 Element::id_ 是 L13 违规（实测踩过）
  st::math::Color border_{};
};

/// 色板格：颜色块 + 名字（令牌核对用）。
class SwatchCell : public Element {
 public:
  SwatchCell(std::string name, st::math::Color color) : name_(std::move(name)), color_(color) {
    set_id("swatch-" + name_);
  }
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "SwatchCell"; }
  [[nodiscard]] auto semantics_text() const -> std::string override { return name_; }
  void measure(const st::ui::RenderContext& context,
               const st::ui::Constraints& constraints) override {
    (void)context;
    (void)constraints;
    measured_ = st::math::Size{kCellWidth, kCellHeight};
  }
  void paint_content(const st::ui::RenderContext& context,
                     st::raster::Surface& canvas) const override {
    const float swatch = 26.0f;
    const Rect chip{bounds_.x + (bounds_.width - swatch) * 0.5f, bounds_.y + 6.0f, swatch, swatch};
    canvas.fill_rect(chip, st::raster::Paint::solid(color_), 6.0f);
    // 描边保证浅色块在浅底上也有边界（同"输入框描边"的道理）
    st::raster::Path outline;
    outline.add_rounded_rect(chip, 6.0f);
    canvas.stroke_path(outline, st::raster::Paint::solid(context.theme.colors().border), 1.0f);
    if (context.text != nullptr) {
      const float width = context.text->measure_width(name_, 10.0f);
      context.text->draw(canvas, name_,
                         Point{bounds_.x + (bounds_.width - width) * 0.5f, chip.bottom() + 5.0f},
                         10.0f, context.theme.colors().text_faint);
    }
  }

  static constexpr float kCellWidth{92.0f};
  static constexpr float kCellHeight{62.0f};

 private:
  std::string name_{};
  st::math::Color color_{};
};

}  // namespace
/// 「关于」页渲染的 Markdown 原文。
///
/// 刻意覆盖**全部块级与行内语法**（标题/段落/列表/引用/代码块/表格/行内码/强调）：
/// 这样"关于页看起来对不对"就等价于"Markdown 渲染的各分支都落到了像素上"——
/// 一次截图核验多个分支，而不是只验证最容易写对的那种。
inline constexpr std::string_view kAboutMarkdown = R"md(
## 霜天是什么

原生桌面应用框架：**全自绘**、无系统控件、软硬件渲染兼容，并可在**没有桌面的服务器上**开发。

- 渲染：自研软件光栅器（扫描线覆盖抗锯齿），硬件后端可选加速
- 文本：TTF/OTF/CFF/CID 解析 + 整形，CJK 回退，字形按物理分辨率栅格化
- 布局：Flex 子集（间距/增长/对齐/换行）
- 组件：按钮/表单/列表/表格/开关/滑块/标签页/下拉/对话框/徽章…全部自绘

> 设计原则：**能被自动观察和操作的界面，才有资格被自动开发。**
> 因此无头模式与控制通道是一等公民，而不是调试附属品。

### 一段代码

```cpp
// 组件只需四个扩展点即可与内置控件共享同一套视觉语言
void measure(const RenderContext& ctx, const Constraints& box) override;
void arrange(const RenderContext& ctx, math::Rect rect) override;
void paint_content(const RenderContext& ctx, raster::Surface& canvas) const override;
auto on_event(const RenderContext& ctx, Event& event) -> bool override;
```

### 分层

| 层 | 职责 |
|---|---|
| `core` | 进程/文件/字符串/时间（平台差异单点封装） |
| `raster` | 画布、路径、覆盖率光栅化、SIMD 快路径 |
| `text` | 字体解析、整形、字形缓存 |
| `ui` | 元素树、布局、组件、主题令牌 |
| `app` | 生命周期、无头模式、控制通道 |

行内样式同样有效：`code`、**粗体**、*斜体*、[链接](https://example.com)。
)md";

// ————————————————————————————————————————————————————————————————————————————
// 概览页：一眼看全框架的主要能力（统计 + 按钮 + 表单 + 进度）
// ————————————————————————————————————————————————————————————————————————————

namespace {

[[nodiscard]] auto build_overview(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = make_page("overview", "概览",
                        "自绘 UI · 跨平台 · 软硬件渲染兼容 · 无头可控 · DPI 感知");

  auto stats = make_row(12.0f);
  stats->set_id("stats-row");
  // 前两张是**会变的**事实（渲染器 / DPI）：初值只占位，真实值由应用在刷新时填。
  // 写死的"软件光栅器"在切到 GPU 后会变成假信息——而这正是这一页要展示的可观测性。
  {
    st::ui::Text* renderer_text = nullptr;
    stats->add_child(make_stat_card("stat-backend", "cpu", "渲染器", "—", Tone::Primary,
                                    &renderer_text));
    if (renderer_text != nullptr && hooks.register_runtime_field) {
      hooks.register_runtime_field("stat_renderer",
                                   [renderer_text](std::string value) {
                                     renderer_text->set_content(std::move(value));
                                   });
    }
    st::ui::Text* dpi_text = nullptr;
    stats->add_child(make_stat_card("stat-dpi", "eye", "DPI 缩放", "—", Tone::Accent, &dpi_text));
    if (dpi_text != nullptr && hooks.register_runtime_field) {
      hooks.register_runtime_field("stat_dpi", [dpi_text](std::string value) {
        dpi_text->set_content(std::move(value));
      });
    }
  }
  // 组件节点数与控制端口也是**会变的事实**（切页/主题/DPI 都会改）：全部走运行时字段，
  // 不再写死——静态占位在演示里就是假信息（v0.1.5 补全）。
  {
    st::ui::Text* nodes_text = nullptr;
    stats->add_child(make_stat_card("stat-nodes", "layers", "组件节点", "—", Tone::Success,
                                    &nodes_text));
    if (nodes_text != nullptr && hooks.register_runtime_field) {
      hooks.register_runtime_field("stat_nodes", [nodes_text](std::string value) {
        nodes_text->set_content(std::move(value));
      });
    }
    st::ui::Text* port_text = nullptr;
    stats->add_child(make_stat_card("stat-control", "terminal", "控制通道", "—", Tone::Warning,
                                    &port_text));
    if (port_text != nullptr && hooks.register_runtime_field) {
      hooks.register_runtime_field("stat_port", [port_text](std::string value) {
        port_text->set_content(std::move(value));
      });
    }
  }
  page->add_child(std::move(stats));

  // 按钮与图标
  auto button_card = make_card("card-buttons", "按钮与图标");
  auto button_row = make_row(8.0f, /*wrap=*/true);
  for (const auto& [label, variant] : std::vector<std::pair<std::string, Button::Variant>>{
           {"主要操作", Button::Variant::Primary},
           {"次要", Button::Variant::Secondary},
           {"轻量", Button::Variant::Ghost},
           {"柔和", Button::Variant::Soft},
           {"危险", Button::Variant::Danger}}) {
    auto button = std::make_unique<Button>(label, variant);
    button->set_id("btn-" + st::ascii_lower(label));
    button_row->add_child(std::move(button));
  }
  button_card->add_child(std::move(button_row));
  button_card->add_child(std::make_unique<Divider>(false));

  // 图标按钮：**纯图标**（常用操作）与**图标 + 文字**（带语义的主/次操作）。
  // 两者都必须内容居中——纯图标按钮靠“只有图标”时的宽度规则，
  // 图标+文字靠“图标 + 间距 + 文字”整块居中（见 `Button::paint_content`）。
  auto icon_button_row = make_row(8.0f, /*wrap=*/true);
  icon_button_row->set_id("icon-button-row");
  for (const auto& [icon_name, variant] : std::vector<std::pair<std::string, Button::Variant>>{
           {"refresh", Button::Variant::Secondary},
           {"settings", Button::Variant::Secondary},
           {"search", Button::Variant::Ghost},
           {"edit", Button::Variant::Ghost},
           {"trash", Button::Variant::Ghost},
           {"send", Button::Variant::Primary}}) {
    // 空标签 = 纯图标按钮；`set_icon` 的名字必须在内置表里（否则不画也不占位）
    auto icon_button = std::make_unique<Button>("", variant);
    icon_button->set_id("btn-icon-" + icon_name);
    icon_button->set_icon(icon_name);
    icon_button_row->add_child(std::move(icon_button));
  }
  button_card->add_child(std::move(icon_button_row));
  button_card->add_child(std::make_unique<Divider>(false));

  // 图标 + 文字按钮：三种变体各一个，验证“合成整块居中”而非“文字居中”。
  auto icon_text_row = make_row(8.0f, /*wrap=*/true);
  icon_text_row->set_id("icon-text-row");
  for (const auto& [icon_name, label, variant] : std::vector<std::tuple<std::string, std::string, Button::Variant>>{
           {"plus", "新建", Button::Variant::Primary},
           {"upload", "上传", Button::Variant::Secondary},
           {"download", "导出", Button::Variant::Secondary},
           {"refresh", "刷新", Button::Variant::Ghost},
           {"trash", "删除", Button::Variant::Danger}}) {
    auto icon_text = std::make_unique<Button>(label, variant);
    icon_text->set_id("btn-icon-text-" + icon_name);
    icon_text->set_icon(icon_name);
    icon_text_row->add_child(std::move(icon_text));
  }
  button_card->add_child(std::move(icon_text_row));
  button_card->add_child(std::make_unique<Divider>(false));

  auto icon_strip = make_row(14.0f);
  icon_strip->set_id("icon-strip");
  for (const auto* icon_name :
       {"search", "settings", "user", "folder", "code", "terminal", "bell", "calendar", "star",
        "heart", "bolt", "shield", "cloud", "database", "send", "tag"}) {
    auto icon = std::make_unique<IconView>(icon_name, 18.0f);
    icon->set_tone(Tone::Muted);
    icon->set_id(std::string("icon-") + icon_name);
    icon_strip->add_child(std::move(icon));
  }
  button_card->add_child(std::move(icon_strip));
  page->add_child(std::move(button_card));

  // 表单
  auto form_card = make_card("card-form", "表单");
  auto search_input = std::make_unique<Input>();
  search_input->set_id("input-search");
  search_input->set_placeholder("搜索组件、图标或文档…");
  search_input->set_icon_prefix("search");
  auto* search_ptr = search_input.get();
  form_card->add_child(std::move(search_input));

  auto password_input = std::make_unique<Input>();
  password_input->set_id("input-password");
  password_input->set_placeholder("密码");
  password_input->set_password(true);
  form_card->add_child(std::move(password_input));

  auto form_actions = make_row(8.0f);
  auto submit_button = std::make_unique<Button>("提交", Button::Variant::Primary, Button::Size::Small);
  submit_button->set_id("btn-submit");
  auto* submit_ptr = submit_button.get();
  auto reset_button = std::make_unique<Button>("重置", Button::Variant::Ghost, Button::Size::Small);
  reset_button->set_id("btn-reset");
  auto* reset_ptr = reset_button.get();
  form_actions->add_child(std::move(submit_button));
  form_actions->add_child(std::move(reset_button));
  form_card->add_child(std::move(form_actions));
  page->add_child(std::move(form_card));

  // 进度
  auto progress_card = make_card("card-progress", "进度与状态（示例自绘组件）");
  progress_card->add_child(std::make_unique<ProgressRow>("光栅器覆盖率", 0.86f, Tone::Primary));
  progress_card->add_child(std::make_unique<ProgressRow>("字体缓存命中", 0.72f, Tone::Success));
  progress_card->add_child(std::make_unique<ProgressRow>("协议实现度", 0.94f, Tone::Accent));
  page->add_child(std::move(progress_card));

  submit_ptr->on_click = [search_ptr, hooks]() {
    hooks.set_status("已提交: " + search_ptr->value());
  };
  reset_ptr->on_click = [search_ptr, hooks]() {
    search_ptr->set_text("");
    hooks.set_status("已重置");
  };
  return page;
}

// ————————————————————————————————————————————————————————————————————————————
// 组件页：控件全集（此前一半组件从未被人看过）
// ————————————————————————————————————————————————————————————————————————————

[[nodiscard]] auto build_components(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = make_page("components", "组件",
                        "选择控件 · 滑块 · 标签页 · 下拉 · 反馈徽标 · 图标 · 文字 · 浮层 · 多行文本 · 禁用态 · 树 · 提示气泡 · 取色器 · 缩放视口 · 代码编辑器 · 差异视图 · 补全弹层 · 命令面板 · 时间轴 · 三方合并 · 菜单栏 · 右键菜单 · 文件对话框 · 终端");

  // —— 选择控件 ——
  auto choice_card = make_card("card-choice", "选择控件");
  auto choice_row = make_row(28.0f, /*wrap=*/true);
  auto check_group = std::make_unique<Panel>(FlexDirection::Column);
  check_group->style().gap = 8.0f;
  check_group->add_child(make_caption("Checkbox"));
  for (const auto& [label, checked, enabled] :
       std::vector<std::tuple<std::string, bool, bool>>{
           {"启用硬件加速", true, true}, {"记录调试日志", false, true}, {"不可用项", false, false}}) {
    auto box = std::make_unique<Checkbox>(label);
    box->set_id("cb-" + st::ascii_lower(label));
    box->set_checked(checked);
    box->set_enabled(enabled);
    check_group->add_child(std::move(box));
  }
  choice_row->add_child(std::move(check_group));

  auto radio_group = std::make_unique<Panel>(FlexDirection::Column);
  radio_group->style().gap = 8.0f;
  radio_group->add_child(make_caption("Radio"));
  for (const auto& [label, checked] : std::vector<std::pair<std::string, bool>>{
           {"软件光栅器", true}, {"X11 后端", false}, {"Wayland 后端", false}}) {
    auto radio = std::make_unique<Radio>(label);
    radio->set_id("radio-" + st::ascii_lower(label));
    radio->set_checked(checked);
    radio_group->add_child(std::move(radio));
  }
  choice_row->add_child(std::move(radio_group));

  auto switch_group = std::make_unique<Panel>(FlexDirection::Column);
  switch_group->style().gap = 8.0f;
  switch_group->add_child(make_caption("Switch"));
  for (const auto& [label, checked, enabled] : std::vector<std::tuple<std::string, bool, bool>>{
           {"无头模式", true, true}, {"垂直同步", false, true}, {"只读", false, false}}) {
    auto toggle = std::make_unique<Switch>(label);
    toggle->set_id("sw-" + st::ascii_lower(label));
    toggle->set_checked(checked);
    toggle->set_enabled(enabled);
    switch_group->add_child(std::move(toggle));
  }
  choice_row->add_child(std::move(switch_group));
  choice_card->add_child(std::move(choice_row));
  page->add_child(std::move(choice_card));

  // —— 滑块 ——
  auto slider_card = make_card("card-slider", "滑块");
  for (const auto& [label, value] : std::vector<std::pair<std::string, float>>{
           {"不透明度", 0.72f}, {"圆角半径", 0.35f}, {"动画时长", 0.90f}}) {
    auto slider = std::make_unique<Slider>(value);
    slider->set_id("slider-" + st::ascii_lower(label));
    slider->set_label(label);
    slider->style().width = 420.0f;
    slider_card->add_child(std::move(slider));
  }
  page->add_child(std::move(slider_card));

  // —— 标签页 ——
  auto tabs_card = make_card("card-tabs", "标签页");
  auto tabs = std::make_unique<Tabs>();
  tabs->set_id("demo-tabs");
  tabs->set_tabs({"外观", "行为", "数据"});
  auto* tabs_ptr = tabs.get();
  tabs_card->add_child(std::move(tabs));

  auto tabs_body = std::make_unique<Panel>(FlexDirection::Column);
  tabs_body->set_id("tabs-body");
  tabs_body->style().gap = 6.0f;
  std::array<Text*, 3> tab_texts{};
  for (std::size_t index = 0; index < 3; ++index) {
    auto text = std::make_unique<Text>(std::vector<std::string>{
        "外观页：颜色/圆角/阴影全部来自设计令牌，改令牌即改全局——且对比度有测试兜底。",
        "行为页：键盘 Tab 可遍历、Enter 激活、滚轮与方向键滚动，焦点环是键盘可达性的唯一线索。",
        "数据页：列表用业务 key 对齐（sync_items），刷新不会让选中态与 id 漂移。",
    }[index]);
    text->set_tone(Tone::Muted);
    text->set_id(std::format("tab-body-{}", index));
    text->set_visible(index == 0);
    tab_texts[index] = text.get();
    tabs_body->add_child(std::move(text));
  }
  tabs_card->add_child(std::move(tabs_body));
  page->add_child(std::move(tabs_card));
  tabs_ptr->on_change = [tab_texts, hooks](std::size_t index) {
    for (std::size_t each = 0; each < tab_texts.size(); ++each) {
      tab_texts[each]->set_visible(each == index);
    }
    hooks.set_status(std::format("标签页切到 #{}", index));
  };

  // —— 下拉选择（浮层：面板挂到 UiRoot 叠加层，绘制在内容之上、事件优先命中）——
  auto select_card = make_card("card-select", "下拉选择");
  select_card->add_child(make_caption("点击展开：面板是叠加层，不占内容布局"));
  auto select = std::make_unique<Select>();
  select->set_id("demo-select");
  select->style().width = 260.0f;
  select->set_placeholder("选择渲染后端…");
  select->add_option("headless", "headless（无头离屏）");
  select->add_option("win32", "win32（真实窗口）");
  select->add_option("x11", "x11（Linux 窗口）");
  select->add_option("wayland", "wayland（Linux 窗口）");
  select->set_selected_index(0);
  auto* select_ptr = select.get();
  select_card->add_child(std::move(select));
  auto select_state = std::make_unique<Text>("当前：headless（无头离屏）");
  select_state->set_id("select-state");
  select_state->set_tone(Tone::Faint);
  select_state->set_font_size(12.0f);
  auto* select_state_ptr = select_state.get();
  select_card->add_child(std::move(select_state));
  page->add_child(std::move(select_card));

  select_ptr->overlay_host = [hooks](std::unique_ptr<Element> panel) {
    hooks.add_overlay(std::move(panel));
  };
  select_ptr->overlay_remove = [hooks](Element* panel) { hooks.remove_overlay(panel); };
  select_ptr->on_change = [select_ptr, select_state_ptr, hooks](std::string_view value) {
    const std::string label(select_ptr->selected_label());
    select_state_ptr->set_content(std::format("当前：{}（值 {}）", label, value));
    hooks.set_status(std::format("已选择 {}（值 {}）", label, value));
  };

  // —— 反馈与徽标 ——
  auto feedback_card = make_card("card-feedback", "反馈与徽标");
  feedback_card->add_child(make_caption("Badge（各语义色）"));
  auto badge_row = make_row(8.0f, true);
  badge_row->set_id("badge-row");
  for (const auto& [text, tone] : std::vector<std::pair<std::string, Tone>>{
           {"默认", Tone::Default}, {"主色", Tone::Primary}, {"成功", Tone::Success},
           {"警告", Tone::Warning}, {"危险", Tone::Danger}, {"强调", Tone::Accent}}) {
    auto badge = std::make_unique<Badge>(text, tone);
    badge->set_id("badge-" + st::ascii_lower(text));
    badge_row->add_child(std::move(badge));
  }
  feedback_card->add_child(std::move(badge_row));

  feedback_card->add_child(make_caption("Chip（可关闭）× Avatar（三档尺寸）× Spinner"));
  auto chip_row = make_row(10.0f, true);
  auto* chip_row_ptr = chip_row.get();
  for (const auto& [text, closable] : std::vector<std::pair<std::string, bool>>{
           {"C++20", false}, {"零依赖", false}, {"可关闭", true}, {"再点一次消失", true}}) {
    auto chip = std::make_unique<Chip>(text, closable);
    auto* chip_self = chip.get();
    chip->set_id("chip-" + st::ascii_lower(text));
    if (closable) {
      // `on_close` 是 `void()`，不带自身参数：在 lambda 外先取到裸指针即可
      //（组件不掌握自己的所有权，"移不移除"由宿主决定）。
      chip_self->on_close = [chip_row_ptr, chip_self, text, hooks]() {
        if (Element* parent = chip_self->parent(); parent != nullptr && parent == chip_row_ptr) {
          (void)parent->remove_child(chip_self);
          hooks.set_status(std::format("已移除标签「{}」", text));
        }
      };
    }
    chip_row->add_child(std::move(chip));
  }
  for (const float size : {28.0f, 36.0f, 48.0f}) {
    auto avatar = std::make_unique<Avatar>("霜天", size);
    avatar->set_id(std::format("avatar-{}", static_cast<int>(size)));
    chip_row->add_child(std::move(avatar));
  }
  auto spinner = std::make_unique<Spinner>();
  spinner->set_id("demo-spinner");
  chip_row->add_child(std::move(spinner));
  feedback_card->add_child(std::move(chip_row));

  feedback_card->add_child(make_caption("ProgressBar（内置组件，对比页面上的自绘 ProgressRow）"));
  for (const auto& [id, value, tone] :
       std::vector<std::tuple<std::string, float, Tone>>{{"progress-build", 0.62f, Tone::Primary},
                                                         {"progress-sync", 0.28f, Tone::Warning},
                                                         {"progress-done", 1.0f, Tone::Success}}) {
    auto bar = std::make_unique<ProgressBar>(value);
    bar->set_id(id);
    bar->set_tone(tone);
    bar->style().width = 420.0f;
    feedback_card->add_child(std::move(bar));
  }
  page->add_child(std::move(feedback_card));

  // —— 窗框（自绘标题栏）——
  //
  // 为什么摆在这一页：窗框是**组件**而不是平台特性（`CONVENTIONS.md` §10 第 7 条），
  // 而它跨平台长得一样——正是本页"组件集巡检"要盯的东西。
  //
  // ⚠ 这里的三个按钮在无头/不支持窗口控制时**照样画、动作如实拒绝**：
  // 这就是"画面跨平台一致、能力如实上报"那份契约在示例里的可见形态。
  auto titlebar_card = make_card("card-titlebar", "窗框（自绘标题栏）");
  titlebar_card->add_child(make_caption(
      "窗口装饰一律自绘：不依赖系统标题栏。标题区可拖动/双击最大化，边缘 6px 内为缩放带；"
      "右侧三按钮经 WindowControl 端口生效（无窗口时如实拒绝，但画面不变）"));
  auto demo_bar = std::make_unique<st::ui::TitleBar>("gallery · 霜天");
  demo_bar->set_id("demo-titlebar");
  demo_bar->set_icon("layers");
  demo_bar->set_window_control(hooks.window_control);
  demo_bar->style().width = 640.0f;
  titlebar_card->add_child(std::move(demo_bar));
  titlebar_card->add_child(make_caption(
      "注：真实窗口的窗框在应用最顶层（`gbcode` 示例）；此处是同一个组件在卡片里的一面"));
  page->add_child(std::move(titlebar_card));

  // —— 图标全集（逐个核对描边完整性）——
  auto icons_card = make_card("card-icons", std::format("图标全集（{} 个：描边由线段四边形与顶点圆求并，"
                                                        "绕向不一致时会被非零环绕规则抵消——这正是曾经的缺陷现场）",
                                                        Icon::names().size()));
  auto icon_grid = make_row(2.0f, /*wrap=*/true);
  icon_grid->set_id("icon-grid");
  for (const auto& name : Icon::names()) {
    icon_grid->add_child(std::make_unique<IconCell>(std::string(name)));
  }
  icons_card->add_child(std::move(icon_grid));

  // —— SVG 图标集（矢量数据源：同一图标多尺寸渲染，任意缩放清晰）——
  // 首次构建该卡时把编译期嵌入的 sprite 装进进程级注册表（幂等；随后
  // IconView/Icon::draw 走「内置表优先、SVG 补位」的统一入口）。
  static const bool sprite_loaded = [] {
    const auto sprite = b::embed<"examples/gallery/assets/icons.svg">();
    return st::ui::svg_registry().load_sprite(
        std::string_view(sprite.data(), sprite.length()));
  }();
  const auto svg_ids = st::ui::svg_registry().ids();
  auto svg_card = make_card("card-svg-icons",
                            std::format("SVG 图标集（{} 个：矢量数据源 · 按目标尺寸重新光栅化 · "
                                        "任意缩放清晰 · Lucide 风格 sprite）",
                                        svg_ids.size() - (sprite_loaded ? 0 : 0)));
  auto svg_head = std::make_unique<st::ui::Text>(
      std::format("装载 {} 个图标；同一图标从 16px 到 64px 逐级放大——边缘始终平滑（位图放大会糊）",
                  svg_ids.size()));
  svg_head->set_font_size(11.5f);
  svg_head->set_tone(st::ui::Tone::Muted);
  svg_card->add_child(std::move(svg_head));
  auto svg_grid = make_row(2.0f, /*wrap=*/true);
  svg_grid->set_id("svg-icon-grid");
  for (const auto& id : svg_ids) {
    svg_grid->add_child(std::make_unique<SvgIconCell>(id));
  }
  svg_card->add_child(std::move(svg_grid));
  page->add_child(std::move(svg_card));
  {
    auto gl_card = make_card("card-gl", "三维视图（OpenGL）");
    auto view = std::make_unique<st::ui::SceneView>(st::ui::SceneShape::Cube);
    view->set_id("gl-view");
    view->set_preferred_height(200.0f);
    gl_card->add_child(std::move(view));
    page->add_child(std::move(gl_card));
  }

  page->add_child(std::move(icons_card));

  // —— 文字样式 ——
  auto type_card = make_card("card-typography", "文字样式");
  for (std::uint32_t level = 1; level <= 3; ++level) {
    auto heading = std::make_unique<Heading>(std::format("Heading {} · 标题层级", level), level);
    heading->set_id(std::format("type-heading-{}", level));
    type_card->add_child(std::move(heading));
  }
  const st::ui::Theme light_theme = st::ui::Theme::light();
  const auto& metrics = light_theme.metrics();
  for (const auto& [label, size] : std::vector<std::pair<std::string, float>>{
           {"font_base 14 · 正文", metrics.font_base},
           {"font_sm 13 · 次要", metrics.font_sm},
           {"font_xs 12 · 辅助", metrics.font_xs}}) {
    auto text = std::make_unique<Text>(label);
    text->set_font_size(size);
    text->set_id("type-body-" + st::ascii_lower(label.substr(0, 9)));
    type_card->add_child(std::move(text));
  }
  auto tone_row = make_row(16.0f, true);
  for (const auto& [label, tone] : std::vector<std::pair<std::string, Tone>>{
           {"Default", Tone::Default}, {"Muted", Tone::Muted}, {"Faint", Tone::Faint},
           {"Primary", Tone::Primary}, {"Accent", Tone::Accent}, {"Success", Tone::Success},
           {"Warning", Tone::Warning}, {"Danger", Tone::Danger}}) {
    auto text = std::make_unique<Text>(label);
    text->set_tone(tone);
    text->set_id("tone-" + st::ascii_lower(label));
    tone_row->add_child(std::move(text));
  }
  type_card->add_child(std::move(tone_row));
  page->add_child(std::move(type_card));

  // —— 分栏（SplitView；v0.1.5 补全：由「示例级自绘升为框架组件」）——
  auto split_card = make_card("card-split", "可拖拽分栏（SplitView）");
  split_card->add_child(make_caption(
      "拖拽中间手柄改比例（悬停变粗、拖拽主色）；键盘 ←/→ 步进、双击归位；"
      "控制通道：set ratio / invoke step_forward·reset——两个面板是真实子元素（tree 可查）"));
  auto split_demo = std::make_unique<SplitView>();
  split_demo->set_id("demo-split");
  split_demo->style().height = 140.0f;
  split_demo->set_ratio(0.4f, false);
  split_demo->set_min_ratio(0.15f);
  {
    auto left = std::make_unique<Panel>(FlexDirection::Column);
    left->set_id("demo-split-left");
    left->style().padding = Insets::all(10.0f);
    left->style().gap = 6.0f;
    auto left_title = std::make_unique<Text>("左面板");
    left_title->set_weight(FontWeight::SemiBold);
    left_title->set_font_size(12.5f);
    left->add_child(std::move(left_title));
    auto left_note = std::make_unique<Text>("拖右边的手柄\n改变两侧比例");
    left_note->set_tone(Tone::Faint);
    left_note->set_font_size(12.0f);
    left->add_child(std::move(left_note));
    split_demo->set_first(std::move(left));
  }
  {
    auto right = std::make_unique<Panel>(FlexDirection::Column);
    right->set_id("demo-split-right");
    right->style().padding = Insets::all(10.0f);
    right->style().gap = 6.0f;
    auto right_title = std::make_unique<Text>("右面板");
    right_title->set_weight(FontWeight::SemiBold);
    right_title->set_font_size(12.5f);
    right->add_child(std::move(right_title));
    auto right_note = std::make_unique<Text>("双击手柄回 50%；\n上下方向用 orientation=vertical");
    right_note->set_tone(Tone::Faint);
    right_note->set_font_size(12.0f);
    right->add_child(std::move(right_note));
    split_demo->set_second(std::move(right));
  }
  SplitView* split_demo_ptr = split_demo.get();
  auto split_actions = make_row(8.0f, /*wrap=*/true);
  const auto make_split_button = [&](const char* id, const char* label,
                                     const char* argument) {
    auto button = std::make_unique<Button>(label, Button::Variant::Secondary,
                                           Button::Size::Small);
    button->set_id(id);
    button->on_click = [split_demo_ptr, argument]() {
      (void)split_demo_ptr->invoke_action("set", argument);
    };
    return button;
  };
  split_actions->add_child(make_split_button("split-set-30", "30%", "0.3"));
  split_actions->add_child(make_split_button("split-set-50", "50%", "0.5"));
  split_actions->add_child(make_split_button("split-set-70", "70%", "0.7"));
  auto vertical_button = std::make_unique<Button>("切上下分栏", Button::Variant::Ghost,
                                                 Button::Size::Small);
  vertical_button->set_id("split-toggle-orientation");
  vertical_button->on_click = [split_demo_ptr]() {
    const bool horizontal =
        split_demo_ptr->get_property("orientation").value_or("horizontal") == "horizontal";
    (void)split_demo_ptr->set_property("orientation", horizontal ? "vertical" : "horizontal");
    split_demo_ptr->style().height = horizontal ? 220.0f : 140.0f;
    split_demo_ptr->mark_layout_dirty();
  };
  split_actions->add_child(std::move(vertical_button));
  split_card->add_child(std::move(split_actions));
  split_card->add_child(std::move(split_demo));
  page->add_child(std::move(split_card));

  // —— 浮层与反馈（Dialog / Toast / Tooltip；v0.1.5 补全：此前三个组件在画廊零出现） ——
  auto overlay_card = make_card("card-overlay", "浮层与反馈（对话框 / 轻提示 / 提示气泡）");
  overlay_card->add_child(make_caption(
      "Dialog：遮罩 + 居中卡片，Esc / 点遮罩 / 按钮三条关闭路径；Toast：自动消失（时间轴驱动）；Tooltip：悬浮跟随"));

  auto overlay_actions = make_row(8.0f, /*wrap=*/true);
  // —— Dialog 触发 ——
  auto dialog_button = std::make_unique<Button>("打开对话框", Button::Variant::Primary,
                                                Button::Size::Small);
  dialog_button->set_id("btn-open-dialog");
  auto* dialog_button_ptr = dialog_button.get();
  overlay_actions->add_child(std::move(dialog_button));
  // —— Toast 触发（两种色调 + 自动消失） ——
  auto toast_ok_button = std::make_unique<Button>("成功 Toast", Button::Variant::Secondary,
                                                  Button::Size::Small);
  toast_ok_button->set_id("btn-toast-ok");
  auto toast_warn_button = std::make_unique<Button>("警告 Toast", Button::Variant::Soft,
                                                    Button::Size::Small);
  toast_warn_button->set_id("btn-toast-warn");
  auto* toast_ok_ptr = toast_ok_button.get();
  auto* toast_warn_ptr = toast_warn_button.get();
  overlay_actions->add_child(std::move(toast_ok_button));
  overlay_actions->add_child(std::move(toast_warn_button));
  overlay_card->add_child(std::move(overlay_actions));

  auto overlay_state = std::make_unique<Text>("尚未触发浮层");
  overlay_state->set_id("overlay-state");
  overlay_state->set_tone(Tone::Faint);
  overlay_state->set_font_size(12.0f);
  auto* overlay_state_ptr = overlay_state.get();
  overlay_card->add_child(std::move(overlay_state));
  page->add_child(std::move(overlay_card));

  // —— 多行文本（TextArea：折行 / 光标 / 内部滚动；此前画廊零覆盖） ——
  auto multiline_card = make_card("card-multiline", "多行文本（TextArea）");
  auto textarea = std::make_unique<st::ui::TextArea>();
  textarea->set_id("demo-textarea");
  textarea->set_placeholder("这里支持多行：Enter 换行，方向键移动光标，滚轮滚动…");
  textarea->set_text("第一行：多行文本域自动折行，\n第二行：光标上下左右可移动，\n"
                     "第三行：内容超出可视高时内部滚动，光标始终可见。");
  textarea->style().height = 96.0f;
  auto* textarea_ptr = textarea.get();
  multiline_card->add_child(std::move(textarea));
  auto textarea_state = std::make_unique<Text>("内容 0 字");
  textarea_state->set_id("textarea-state");
  textarea_state->set_tone(Tone::Faint);
  textarea_state->set_font_size(12.0f);
  auto* textarea_state_ptr = textarea_state.get();
  multiline_card->add_child(std::move(textarea_state));
  page->add_child(std::move(multiline_card));

  // —— 禁用态（Button/Input：控件全状态的最后一环） ——
  auto disabled_card = make_card("card-disabled", "禁用态");
  auto disabled_row = make_row(10.0f, true);
  auto disabled_button = std::make_unique<Button>("不可用按钮", Button::Variant::Primary);
  disabled_button->set_id("btn-disabled");
  disabled_button->set_enabled(false);
  disabled_row->add_child(std::move(disabled_button));
  auto disabled_input = std::make_unique<Input>();
  disabled_input->set_id("input-disabled");
  disabled_input->set_placeholder("不可输入的输入框");
  disabled_input->set_enabled(false);
  disabled_input->style().width = 200.0f;
  disabled_row->add_child(std::move(disabled_input));
  disabled_card->add_child(std::move(disabled_row));
  disabled_card->add_child(make_caption(
      "语义树可见 disabled 标记；点击/键盘均不响应（选择器 `Button:disabled` 可命中）"));
  page->add_child(std::move(disabled_card));

  // —— 树（Tree）：无子元素、扁平数组驱动；展开/选中状态由调用方数据表达 ——
  auto tree_card = make_card("card-tree", "树（Tree）");
  tree_card->add_child(make_caption(
      "目录行点击展开/收起（状态由调用方数据表达，组件不持有层级）；"
      "文件行点击选中；↑↓ 移动、Enter 激活、←→ 收起/展开；"
      "控制通道：get rows·selected_key，invoke click——本树静态演示不接懒加载"));
  auto tree_demo = std::make_unique<Tree>();
  tree_demo->set_id("demo-tree");
  tree_demo->set_row_height(26.0f);
  tree_demo->style().height = 130.0f;
  auto* tree_ptr = tree_demo.get();
  auto tree_state = std::make_unique<Text>("未选中");
  tree_state->set_id("tree-state");
  tree_state->set_tone(Tone::Faint);
  tree_state->set_font_size(12.0f);
  auto* tree_state_ptr = tree_state.get();
  tree_card->add_child(std::move(tree_demo));
  tree_card->add_child(std::move(tree_state));
  page->add_child(std::move(tree_card));

  // —— 提示气泡（Tooltip）——
  auto tooltip_card = make_card("card-tooltip", "提示气泡（Tooltip）");
  tooltip_card->add_child(make_caption(
      "按钮“显示气泡”把一张 Tooltip 挂到 UiRoot 叠加层：它按目标矩形自动定位到上方居中（"
      "空间不足翻到下方），set_active 控制显隐——UiRoot 悬停 450ms 的 hint-bubble 走的是"
      "同一张组件（Element::set_hint，见右侧按钮，无头验证控制面可用）"));
  auto tooltip_actions = make_row(8.0f, /*wrap=*/true);
  auto tooltip_button = std::make_unique<Button>("显示气泡", Button::Variant::Secondary,
                                                 Button::Size::Small);
  tooltip_button->set_id("btn-tooltip");
  auto tooltip_hide_button = std::make_unique<Button>("隐藏", Button::Variant::Ghost,
                                                       Button::Size::Small);
  tooltip_hide_button->set_id("btn-tooltip-hide");
  auto* tooltip_button_ptr = tooltip_button.get();
  auto* tooltip_hide_ptr = tooltip_hide_button.get();
  tooltip_button->set_hint("悬停 450ms 也会弹出这张气泡（UiRoot 共享气泡，与本卡同一组件）");
  tooltip_hide_button->set_hint("Ghost 变体 + 提示气泡");
  tooltip_actions->add_child(std::move(tooltip_button));
  tooltip_actions->add_child(std::move(tooltip_hide_button));
  tooltip_card->add_child(std::move(tooltip_actions));
  auto tooltip_state = std::make_unique<Text>("气泡未挂出");
  tooltip_state->set_id("tooltip-state");
  tooltip_state->set_tone(Tone::Faint);
  tooltip_state->set_font_size(12.0f);
  auto* tooltip_state_ptr = tooltip_state.get();
  tooltip_card->add_child(std::move(tooltip_state));
  page->add_child(std::move(tooltip_card));

  // —— 取色器（ColorPicker）：HSV 交互、RGB 真值 ——
  auto color_card = make_card("card-color", "取色器（ColorPicker）");
  color_card->add_child(make_caption(
      "色相条 + 饱和度/明度块可拖拽；十六进制输入只认 #RRGGBB(AABB)、"
      "解析失败如实拒绝并保留原色；拖拽与输入均经 on_change 回显——真值源是 RGB，HSV 只是交互量"));
  auto picker_row = make_row(24.0f, /*wrap=*/true);
  auto picker = std::make_unique<ColorPicker>();
  picker->set_id("demo-colorpicker");
  picker->set_color(st::math::Color::rgb(37, 99, 235), /*notify=*/false);
  auto* picker_ptr = picker.get();
  picker_row->add_child(std::move(picker));
  auto picker_state = std::make_unique<Text>("#2563EB（rgba(37, 99, 235, 255)）");
  picker_state->set_id("colorpicker-state");
  picker_state->set_tone(Tone::Faint);
  picker_state->set_font_size(12.0f);
  auto* picker_state_ptr = picker_state.get();
  auto picker_swatch = std::make_unique<Panel>(FlexDirection::Column);
  picker_swatch->set_id("colorpicker-swatch");
  picker_swatch->style().gap = 6.0f;
  auto* picker_swatch_ptr = picker_swatch.get();
  picker_swatch->add_child(std::move(picker_state));
  picker_swatch->add_child(make_caption("拖 SV 块/色相条或改十六进制，色板与回显实时联动"));
  picker_row->add_child(std::move(picker_swatch));
  color_card->add_child(std::move(picker_row));
  page->add_child(std::move(color_card));

  // —— 视口（Viewport）：二维缩放/平移，与 ScrollView 的一维滚动分工 ——
  auto viewport_card = make_card("card-viewport", "缩放视口（Viewport）");
  viewport_card->add_child(make_caption(
      "滚轮以光标为锚缩放、中键/空格拖拽平移、fit 适配内容；棋盘格是透明底不是白像素；"
      "控制通道：get/set zoom·pan_x·pan_y，invoke zoom_in·zoom_out·fit；"
      "内容是一张真实子元素（矢量图标），二维缩放平移都是它的变化"));
  auto viewport_demo = std::make_unique<Viewport>();
  viewport_demo->set_id("demo-viewport");
  viewport_demo->style().height = 160.0f;
  {
    auto image = std::make_unique<st::ui::IconView>("image", 120.0f);
    image->set_id("demo-viewport-content");
    viewport_demo->set_content(std::move(image));
  }
  auto* viewport_ptr = viewport_demo.get();
  viewport_card->add_child(std::move(viewport_demo));
  auto viewport_state = std::make_unique<Text>("缩放 100% · pan (0, 0)——滚轮/拖拽后实时回显");
  viewport_state->set_id("viewport-state");
  viewport_state->set_tone(Tone::Faint);
  viewport_state->set_font_size(12.0f);
  auto* viewport_state_ptr = viewport_state.get();
  viewport_card->add_child(std::move(viewport_state));
  page->add_child(std::move(viewport_card));

  // —— 代码编辑器（CodeEditor）：语法高亮 + 查找高亮 + 只读切换 ——
  // 四卡第二包（编辑器族；对账兑现见 tests/support/gallery_exhibits.hpp）：四组件
  // 的组件页首现；交互面按「真实可交互」标准——每个操作都有稳定 id 与回显。
  auto editor_card = make_card("card-code-editor", "代码编辑器（CodeEditor）");
  editor_card->add_child(make_caption(
      "语法高亮（内置 30+ 语言）· 行号/缩进参考线· 只读切换 · 查找高亮；"
      "控制通道：get text·language·read_only·lines，invoke select_all·find·goto_line——本卡只演示一层交互面，完整能力见 gbcode"));
  auto editor_demo = std::make_unique<CodeEditor>();
  editor_demo->set_id("demo-code-editor");
  editor_demo->set_language("cpp");
  editor_demo->set_tab_width(2);
  editor_demo->set_text(
      "// 编辑器内直接敲字、拖选、Ctrl+Z 撤销——都走真值树\n"
      "int main() {\n"
      "  int answer = 42;\n"
      "  return answer;  // 控制通道可断言 selection/read_only\n"
      "}\n");
  editor_demo->style().height = 150.0f;
  auto* editor_ptr = editor_demo.get();
  editor_card->add_child(std::move(editor_demo));
  auto editor_actions = make_row(8.0f, /*wrap=*/true);
  auto editor_ro_button = std::make_unique<Button>("切只读", Button::Variant::Secondary,
                                                   Button::Size::Small);
  editor_ro_button->set_id("btn-editor-readonly");
  auto* editor_ro_button_ptr = editor_ro_button.get();
  editor_actions->add_child(std::move(editor_ro_button));
  auto editor_find_button = std::make_unique<Button>("查找 answer", Button::Variant::Ghost,
                                                     Button::Size::Small);
  editor_find_button->set_id("btn-editor-find");
  auto* editor_find_button_ptr = editor_find_button.get();
  editor_actions->add_child(std::move(editor_find_button));
  editor_card->add_child(std::move(editor_actions));
  auto editor_state = std::make_unique<Text>("（装载后回显行数/字符数）");
  editor_state->set_id("editor-state");
  editor_state->set_tone(Tone::Faint);
  editor_state->set_font_size(12.0f);
  auto* editor_state_ptr = editor_state.get();
  editor_card->add_child(std::move(editor_state));
  page->add_child(std::move(editor_card));

  // —— 差异视图（DiffView）：两段文本 → 高亮差异 + hunk 导航 + 布局切换 ——
  auto diff_card = make_card("card-diff", "差异视图（DiffView）");
  diff_card->add_child(make_caption(
      "两段文本→行级差异（删除红/新增绿/占位对齐）；←→ 在 hunk 间环绕跳转；"
      "并列/内联两布局；控制通道：get hunk_count·current_hunk，invoke next_hunk·set_layout"));
  auto diff_demo = std::make_unique<DiffView>();
  diff_demo->set_id("demo-diff");
  diff_demo->set_read_only(true);
  diff_demo->set_texts(
      "int main() {\n  int a = 1;\n  int b = 2;\n  return a + b;\n}\n",
      "int main() {\n  int a = 1;\n  int b = 4;\n  int c = 5;\n  return a + b + c;\n}\n");
  diff_demo->style().height = 170.0f;
  auto* diff_ptr = diff_demo.get();
  diff_card->add_child(std::move(diff_demo));
  auto diff_actions = make_row(8.0f, /*wrap=*/true);
  auto diff_next_button = std::make_unique<Button>("下一处差异", Button::Variant::Secondary,
                                                   Button::Size::Small);
  diff_next_button->set_id("btn-diff-next");
  diff_next_button->set_hint("DiffView::next_hunk（环绕）");
  auto diff_layout_button = std::make_unique<Button>("切内联/并列", Button::Variant::Ghost,
                                                     Button::Size::Small);
  diff_layout_button->set_id("btn-diff-layout");
  diff_layout_button->set_hint("DiffLayout::Unified / SideBySide");
  auto* diff_next_button_ptr = diff_next_button.get();
  auto* diff_layout_button_ptr = diff_layout_button.get();
  diff_actions->add_child(std::move(diff_next_button));
  diff_actions->add_child(std::move(diff_layout_button));
  diff_card->add_child(std::move(diff_actions));
  auto diff_state = std::make_unique<Text>("（装载后回显差异块数）");
  diff_state->set_id("diff-state");
  diff_state->set_tone(Tone::Faint);
  diff_state->set_font_size(12.0f);
  auto* diff_state_ptr = diff_state.get();
  diff_card->add_child(std::move(diff_state));
  page->add_child(std::move(diff_card));

  // —— 补全弹层（CompletionPopup）：候选列表 + 键盘导航 + 选中回显 ——
  // 浮层组件：按钮触发时才构造并挂叠加层（锚点 = 触发按钮实时 bounds）；
  // ⚠ 构造默认不可见，挂载后必须显式 set_visible(true)（否则绘制与键盘派发都被跳过）。
  auto completion_card = make_card("card-completion", "补全弹层（CompletionPopup）");
  completion_card->add_child(make_caption(
      "按钮唤起：候选列表（徽标+类型）贴锚点下方、空间不足翻上方；↑↓ 导航、Enter/Tab 接受、Esc 关闭；"
      "控制通道：get items·visible_items·selected，invoke next·prev·select·accept"));
  auto completion_open_button = std::make_unique<Button>("唤起补全弹层", Button::Variant::Secondary,
                                                         Button::Size::Small);
  completion_open_button->set_id("btn-completion-open");
  completion_open_button->set_hint("锚点 = 本按钮实时 bounds（点击时已排布）");
  auto* completion_open_ptr = completion_open_button.get();
  completion_card->add_child(std::move(completion_open_button));
  auto completion_state = std::make_unique<Text>("未唤起（LSP 场景见 gbcode；本卡静态候选表）");
  completion_state->set_id("completion-state");
  completion_state->set_tone(Tone::Faint);
  completion_state->set_font_size(12.0f);
  auto* completion_state_ptr = completion_state.get();
  completion_card->add_child(std::move(completion_state));
  page->add_child(std::move(completion_card));

  // —— 命令面板（CommandPalette）：Ctrl+Shift+P 形态的命令入口 ——
  auto palette_card = make_card("card-palette", "命令面板（CommandPalette）");
  palette_card->add_child(make_caption(
      "顶部居中卡片 + 输入过滤（title/detail 子串、不分大小写）；↑↓ 环绕导航、Enter 执行、Esc/遮罩关闭；"
      "挂载后必须 grab_focus（否则敲的字跑进底层焦点元素）；控制通道：get query·match_count·active，invoke activate·close"));
  auto palette_open_button = std::make_unique<Button>("唤起命令面板", Button::Variant::Primary,
                                                       Button::Size::Small);
  palette_open_button->set_id("btn-palette-open");
  auto* palette_open_ptr = palette_open_button.get();
  palette_card->add_child(std::move(palette_open_button));
  auto palette_state = std::make_unique<Text>("未唤起（选命令后回显；Esc/点遮罩关闭）");
  palette_state->set_id("palette-state");
  palette_state->set_tone(Tone::Faint);
  palette_state->set_font_size(12.0f);
  auto* palette_state_ptr = palette_state.get();
  palette_card->add_child(std::move(palette_state));
  page->add_child(std::move(palette_card));

  // —— 时间轴（TimelineView）：标尺 + 轨道 + 片段块 + 播放头，几何层为
  // timeline::Model（与 caiyun 几何层回流同一套数据层）。
  auto timeline_card = make_card("card-timeline", "时间轴（TimelineView）");
  timeline_card->add_child(make_caption(
      "两轨道三片段：拖播放头/片段体/左右边缘（吸附到边缘/播放头，阈值 8px）；"
      "滚轮缩放（指针下那刻不动）、Shift+滚轮平移、空白拖拽平移；"
      "控制通道：get playhead·pixels_per_second·snap，invoke zoom_in·zoom_out·play·scroll_to"));
  auto timeline_demo = std::make_unique<TimelineView>();
  timeline_demo->set_id("demo-timeline");
  {
    // 填 Model：两轨道（视频/音频）×三片段；与 caiyun 同构的毫秒制数据层。
    auto& model = timeline_demo->model();
    Track video;
    video.label = "V";
    video.height = 40.0f;
    video.locked = false;
    video.clips = {Clip{.id = "v1", .label = "开场", .start = 0, .duration = 4000,
                        .source_in = 0, .selected = true},
                   Clip{.id = "v2", .label = "中段", .start = 4500, .duration = 3000,
                        .source_in = 0, .selected = false}};
    Track audio;
    audio.label = "A";
    audio.height = 40.0f;
    audio.locked = false;
    audio.clips = {Clip{.id = "a1", .label = "配音", .start = 0, .duration = 7500,
                        .source_in = 0, .selected = false}};
    model.set_tracks({std::move(video), std::move(audio)});
    model.set_playhead(2000);
    model.set_pixels_per_second(48.0f);
    SnapOptions snap_options;
    snap_options.threshold_px = 8.0f;
    snap_options.to_zero = true;
    snap_options.to_playhead = true;
    snap_options.to_clip_edges = true;
    timeline_demo->set_snap_options(snap_options);
  }
  timeline_demo->style().height = 150.0f;
  auto* timeline_ptr = timeline_demo.get();
  timeline_card->add_child(std::move(timeline_demo));
  auto timeline_state = std::make_unique<Text>("（装载后回显播放头时间码）");
  timeline_state->set_id("timeline-state");
  timeline_state->set_tone(Tone::Faint);
  timeline_state->set_font_size(12.0f);
  auto* timeline_state_ptr = timeline_state.get();
  timeline_card->add_child(std::move(timeline_state));
  page->add_child(std::move(timeline_card));

  // —— 三方合并（MergeView）：base/ours/theirs 三栏并列，冲突段逐段取舍 ——
  auto merge_card = make_card("card-merge", "三方合并（MergeView）");
  merge_card->add_child(make_caption(
      "与 Git 无关的纯文本三方合并：只有冲突段需要人来选（未变/单侧改/双侧同改自动可解）；"
      "按钮逐段取舍或一键全取；控制通道：get conflicts·resolved·pending·current，"
      "invoke take_ours·take_theirs·take_ours_all·take_theirs_all·next_conflict·previous_conflict"));
  auto merge_demo = std::make_unique<MergeView>();
  merge_demo->set_id("demo-merge");
  merge_demo->set_sides(
      "第一行：共同祖先\n第二行：我方保留\n第三行：双改冲突\n第四行：他方新增\n",
      "第一行：共同祖先\n第二行：我方保留\n第三行：我方修改\n第四行：他方新增\n第五行：我方收尾\n",
      "第一行：共同祖先\n第二行：我方保留\n第三行：他方改法\n第四行：他方新增\n第五行：我方收尾\n");
  merge_demo->style().height = 180.0f;
  auto* merge_ptr = merge_demo.get();
  merge_card->add_child(std::move(merge_demo));
  auto merge_actions = make_row(8.0f, /*wrap=*/true);
  auto merge_ours_button = std::make_unique<Button>("当前段取我方", Button::Variant::Secondary,
                                                    Button::Size::Small);
  merge_ours_button->set_id("btn-merge-ours");
  merge_ours_button->set_hint("take_ours：作用于当前聚焦段");
  auto merge_theirs_button = std::make_unique<Button>("当前段取他方", Button::Variant::Secondary,
                                                      Button::Size::Small);
  merge_theirs_button->set_id("btn-merge-theirs");
  merge_theirs_button->set_hint("take_theirs：作用于当前聚焦段");
  auto merge_next_button = std::make_unique<Button>("下一处冲突", Button::Variant::Ghost,
                                                    Button::Size::Small);
  merge_next_button->set_id("btn-merge-next");
  merge_next_button->set_hint("next_pending_conflict（未决定的才算）");
  auto* merge_ours_button_ptr = merge_ours_button.get();
  auto* merge_theirs_button_ptr = merge_theirs_button.get();
  auto* merge_next_button_ptr = merge_next_button.get();
  merge_actions->add_child(std::move(merge_ours_button));
  merge_actions->add_child(std::move(merge_theirs_button));
  merge_actions->add_child(std::move(merge_next_button));
  merge_card->add_child(std::move(merge_actions));
  auto merge_state = std::make_unique<Text>("（装载后回显冲突/已解计数）");
  merge_state->set_id("merge-state");
  merge_state->set_tone(Tone::Faint);
  merge_state->set_font_size(12.0f);
  auto* merge_state_ptr = merge_state.get();
  merge_card->add_child(std::move(merge_state));
  page->add_child(std::move(merge_card));

  // —— 菜单栏（MenuBar）：顶部标题条 + 下拉面板（overlay；锚点惰性跟标题）——
  auto menubar_card = make_card("card-menubar", "菜单栏（MenuBar）");
  menubar_card->add_child(make_caption(
      "三顶级菜单：点标题/悬停切换/键盘 ←→+Enter 都能开；面板是叠加层，"
      "Esc/点外/激活后关闭；控制通道：面板 get anchor，invoke activate_item（参数=条目 id）"));
  auto menubar_demo = std::make_unique<MenuBar>();
  menubar_demo->set_id("demo-menubar");
  menubar_demo->set_menus(std::vector<Menu>{
      Menu{.id = "file", .label = "文件", .items = {
                MenuItem{.id = "file.new", .label = "新建"},
                MenuItem{.id = "file.open", .label = "打开…"},
                MenuItem{.id = "sep1", .label = "", .separator = true},
                MenuItem{.id = "file.quit", .label = "退出"},
            }},
      Menu{.id = "edit", .label = "编辑", .items = {
                MenuItem{.id = "edit.undo", .label = "撤销"},
                MenuItem{.id = "edit.redo", .label = "重做"},
                MenuItem{.id = "edit.find", .label = "查找…"},
            }},
      Menu{.id = "view", .label = "查看", .items = {
                MenuItem{.id = "view.zoom_in", .label = "放大"},
                MenuItem{.id = "view.zoom_out", .label = "缩小"},
                MenuItem{.id = "view.icons", .label = "查看图标页"},
            }},
  });
  auto* menubar_ptr = menubar_demo.get();
  // 面板身份记在 shared_ptr 单元格里：与首包 Tooltip 同一套防悬垂/防重复挂层手段
  //（on_open_menu 生命周期长于本函数栈帧，裸捕栈引用会悬垂）。
  auto menubar_panel_cell = std::make_shared<MenuPanel*>(nullptr);
  menubar_card->add_child(std::move(menubar_demo));
  auto menubar_state = std::make_unique<Text>("点菜单标题展开；条目点击回显到这里");
  menubar_state->set_id("menubar-state");
  menubar_state->set_tone(Tone::Faint);
  menubar_state->set_font_size(12.0f);
  auto* menubar_state_ptr = menubar_state.get();
  menubar_card->add_child(std::move(menubar_state));
  page->add_child(std::move(menubar_card));

  // —— 右键菜单（ContextMenu）：按钮触发 + dismiss barrier（点外/Esc 关闭）——
  auto context_card = make_card("card-context", "右键菜单（ContextMenu）");
  context_card->add_child(make_caption(
      "按钮唤起（锚点=按钮实时 bounds；真实场景是右键，gbcode 编辑器右键同款）："
      "面板外不拦截命中，点击面板外或 Esc 触发 on_close；"
      "控制通道：面板 get anchor，invoke activate_item（参数=条目 id）"));
  auto context_open_button = std::make_unique<Button>("唤起右键菜单", Button::Variant::Secondary,
                                                       Button::Size::Small);
  context_open_button->set_id("btn-context-open");
  context_open_button->set_hint("锚点 = 本按钮实时 bounds（点击时已排布）");
  auto* context_open_ptr = context_open_button.get();
  context_card->add_child(std::move(context_open_button));
  auto context_state = std::make_unique<Text>("未唤起（条目激活后回显；Esc/点外关闭）");
  context_state->set_id("context-state");
  context_state->set_tone(Tone::Faint);
  context_state->set_font_size(12.0f);
  auto* context_state_ptr = context_state.get();
  context_card->add_child(std::move(context_state));
  page->add_child(std::move(context_card));

  // —— 文件对话框（FileDialog）：打开模式，真实目录浏览 ——
  auto filedialog_card = make_card("card-filedialog", "文件对话框（FileDialog）");
  filedialog_card->add_child(make_caption(
      "打开模式：左侧常用位置/盘符 + 面包屑 + 目录列表（双击进入）+ 文件名行 +"
      "确认/取消；Esc/遮罩/取消三路关闭；控制通道：get directory·filename，"
      "invoke select·activate·confirm·up·goto_place"));
  auto filedialog_open_button = std::make_unique<Button>("打开文件对话框", Button::Variant::Secondary,
                                                          Button::Size::Small);
  filedialog_open_button->set_id("btn-filedialog-open");
  filedialog_open_button->set_hint("起始目录 = 进程 CWD（失败兑底主目录）");
  auto* filedialog_open_ptr = filedialog_open_button.get();
  filedialog_card->add_child(std::move(filedialog_open_button));
  auto filedialog_state = std::make_unique<Text>("未打开（确认后回显全路径；Esc/取消关闭）");
  filedialog_state->set_id("filedialog-state");
  filedialog_state->set_tone(Tone::Faint);
  filedialog_state->set_font_size(12.0f);
  auto* filedialog_state_ptr = filedialog_state.get();
  filedialog_card->add_child(std::move(filedialog_state));
  page->add_child(std::move(filedialog_card));

  // —— 终端（Terminal）：行模式静态回显 + 按钮注入（真 PTY 形态见 gbcode）——
  auto terminal_card = make_card("card-terminal", "终端（Terminal）");
  terminal_card->add_child(make_caption(
      "行模式（非 PTY）：等宽字体、滚回截断上限 4000 行、标签栏可关；"
      "真终端（PTY/ConPTY、字节级输入、vim 可开）见 gbcode 的 Ctrl+`；"
      "控制通道与属性面详 terminal.cpp；本卡演示 append 静态回显 + 按钮注入"));
  auto terminal_demo = std::make_unique<Terminal>();
  terminal_demo->set_id("demo-terminal");
  terminal_demo->set_tabs_visible(true);
  auto* terminal_ptr = terminal_demo.get();
  terminal_card->add_child(std::move(terminal_demo));
  auto terminal_actions = make_row(8.0f, /*wrap=*/true);
  auto terminal_send_button = std::make_unique<Button>("注入一条命令回显", Button::Variant::Secondary,
                                                        Button::Size::Small);
  terminal_send_button->set_id("btn-terminal-send");
  terminal_send_button->set_hint("append()：行模式宿主写入输出");
  auto* terminal_send_button_ptr = terminal_send_button.get();
  terminal_actions->add_child(std::move(terminal_send_button));
  auto terminal_clear_button = std::make_unique<Button>("清屏", Button::Variant::Ghost,
                                                        Button::Size::Small);
  terminal_clear_button->set_id("btn-terminal-clear");
  terminal_clear_button->set_hint("clear()：行模式清滚回");
  auto* terminal_clear_button_ptr = terminal_clear_button.get();
  terminal_actions->add_child(std::move(terminal_clear_button));
  terminal_card->add_child(std::move(terminal_actions));
  auto terminal_state = std::make_unique<Text>("（装载后回显行数）");
  terminal_state->set_id("terminal-state");
  terminal_state->set_tone(Tone::Faint);
  terminal_state->set_font_size(12.0f);
  auto* terminal_state_ptr = terminal_state.get();
  terminal_card->add_child(std::move(terminal_state));
  page->add_child(std::move(terminal_card));

  // —— 浮层交互逻辑（Dialog / Toast：面板挂 UiRoot 叠加层，摘除经 hooks） ——
  dialog_button_ptr->on_click = [hooks, overlay_state_ptr]() {
    auto dialog = std::make_unique<st::ui::Dialog>(
        "确认操作",
        "对话框经 UiRoot 叠加层挂载：Esc、点击遮罩、底部按钮三条路径都能关闭。\n"
        "本例验证模态交互与自动测试可行性（tree 可见 Dialog 节点，invoke 可点按钮）。");
    dialog->set_id("demo-dialog");
    dialog->set_actions({"取消", "确认"});
    auto* dialog_ptr = dialog.get();
    dialog->on_dismiss = [hooks, dialog_ptr, overlay_state_ptr]() {
      if (hooks.remove_overlay) hooks.remove_overlay(dialog_ptr);
      overlay_state_ptr->set_content("对话框已关闭（Esc / 遮罩 / 按钮）");
    };
    dialog->on_action = [hooks, dialog_ptr, overlay_state_ptr](std::size_t index) {
      overlay_state_ptr->set_content(std::format("对话框按钮 #{} 已点", index));
      if (hooks.remove_overlay) hooks.remove_overlay(dialog_ptr);
    };
    if (hooks.add_overlay) hooks.add_overlay(std::move(dialog));
    overlay_state_ptr->set_content("对话框已打开（叠加层，点遮罩或按钮关闭）");
  };

  // Toast：自动消失（2600ms 默认）；到期回调里经 hooks 摘除（延迟摘除协议）
  const auto spawn_toast = [hooks, overlay_state_ptr](std::string message, st::ui::Tone tone) {
    auto toast = st::ui::Toast::make(std::move(message), tone);
    toast->set_id("demo-toast");
    toast->set_auto_dismiss_ms(st::ui::Toast::kDefaultDismissMs);
    auto* toast_ptr = toast.get();
    toast->on_dismiss = [hooks, toast_ptr, overlay_state_ptr]() {
      if (hooks.remove_overlay) hooks.remove_overlay(toast_ptr);
      overlay_state_ptr->set_content("Toast 已自动消失（2600ms 到期）");
    };
    if (hooks.add_overlay) hooks.add_overlay(std::move(toast));
    overlay_state_ptr->set_content("Toast 展示中…（2.6 秒后自动消失）");
  };
  toast_ok_ptr->on_click = [spawn_toast]() { spawn_toast("已保存到本地 ✓", st::ui::Tone::Success); };
  toast_warn_ptr->on_click =
      [spawn_toast]() { spawn_toast("磁盘空间不足 10%", st::ui::Tone::Warning); };

  // TextArea：内容变化回显（字符数 + 行数）
  textarea_ptr->on_change = [textarea_state_ptr](std::string_view text) {
    std::size_t lines = 1;
    for (const char ch : text) {
      if (ch == '\n') ++lines;
    }
    textarea_state_ptr->set_content(
        std::format("内容 {} 字 · {} 行", st::utf8_length(text), lines));
  };
  // 初值也回显一次（静态构造时不触发 on_change）
  if (const std::string& initial = textarea_ptr->value(); !initial.empty()) {
    std::size_t lines = 1;
    for (const char ch : initial) {
      if (ch == '\n') ++lines;
    }
    textarea_state_ptr->set_content(
        std::format("内容 {} 字 · {} 行", st::utf8_length(initial), lines));
  }

  // —— 四卡交互接线（Tree/Tooltip/ColorPicker/Viewport；静态展示部分在建卡处） ——

  // Tree：静态两层树（组件只要求“可见行扁平数组”，本卡不接懒加载；点目录行只回报 toggle 意图）
  tree_ptr->sync_nodes(std::vector<TreeNode>{
      {"src", "src", true, true, 0},
      {"src/ui", "ui", true, true, 1},
      {"tree.hpp", "tree.hpp", false, false, 2},
      {"viewport.hpp", "viewport.hpp", false, false, 2},
      {"examples", "examples", true, true, 0},
      {"gallery", "gallery", false, true, 1},
      {"README.md", "README.md", false, false, 0},
  });
  tree_ptr->on_select = [tree_state_ptr, hooks](std::string_view key) {
    tree_state_ptr->set_content(std::format("选中 {}（目录行不选，只展开）", key));
    hooks.set_status(std::format("树选中：{}", key));
  };
  tree_ptr->on_toggle = [tree_state_ptr](std::string_view key, bool expanded) {
    tree_state_ptr->set_content(std::format("{} {}（静态演示不重灌数据）", key,
                                           expanded ? "展开" : "收起"));
  };

  // Tooltip：按挂出时按钮的实时 bounds 定位（FillViewport 形态：气泡在 arrange 里自定位）。
  // 生命周期：气泡挂出后由 UiRoot 叠加层持有，本卡不再移除（唯一所有者恒为叠加层，
  // 裸指针不悬垂）；指针存 shared_ptr 单元格是因为 on_click 生命周期长于本函数栈帧，
  // 直接捕栈引用会悬垂。显隐走 set_active：语义树上恒在（get id 可见），绘制/命中随 active 开关。
  auto bubble_cell = std::make_shared<Tooltip*>(nullptr);
  const auto tooltip_show = [hooks, bubble_cell, tooltip_state_ptr](Element* target) {
    if (Tooltip* live = *bubble_cell; live != nullptr) {  // 已挂出：只按最新目标矩形重新定位
      live->set_target_rect(target->bounds());
      live->set_active(true);
      tooltip_state_ptr->set_content("气泡已重新定位（set_target_rect → 上方居中）");
      return;
    }
    auto bubble = std::make_unique<Tooltip>(
        "组件级 Tooltip：挂 UiRoot 叠加层，按目标矩形自动定位");
    bubble->set_id("demo-tooltip");
    bubble->set_target_rect(target->bounds());
    bubble->set_active(true);
    *bubble_cell = bubble.get();
    if (hooks.add_overlay) hooks.add_overlay(std::move(bubble));
    tooltip_state_ptr->set_content("气泡已挂出（set_target_rect 后自动上方居中）");
  };
  tooltip_button_ptr->on_click = [tooltip_show, tooltip_button_ptr]() {
    tooltip_show(tooltip_button_ptr);
  };
  tooltip_hide_ptr->on_click = [bubble_cell, tooltip_state_ptr]() {
    if (Tooltip* live = *bubble_cell; live != nullptr) {
      live->set_active(false);
      tooltip_state_ptr->set_content("气泡已收起（set_active(false)：绘制与命中关，语义树仍在）");
    } else {
      tooltip_state_ptr->set_content("气泡尚未挂出");
    }
  };

  // ColorPicker：颜色变化回显（拖拽/十六进制输入均触发；程序 set_color(..., false) 不触发）。
  // 色板直接改 style().background（Panel 默认 surface=None，不会被主题重刷覆盖）。
  picker_ptr->on_change = [picker_swatch_ptr, picker_state_ptr, hooks](st::math::Color color) {
    picker_swatch_ptr->style().background = color;
    picker_state_ptr->set_content(std::format("当前 rgba({}, {}, {}, {})",
                                              static_cast<int>(color.r),
                                              static_cast<int>(color.g),
                                              static_cast<int>(color.b),
                                              static_cast<int>(color.a)));
    hooks.set_status(std::format("取色器：rgba({}, {}, {}, {})",
                                  static_cast<int>(color.r), static_cast<int>(color.g),
                                  static_cast<int>(color.b), static_cast<int>(color.a)));
  };

  // Viewport：缩放/平移变化回显（滚轮、拖拽、控制通道 set/invoke 均经此回调）。
  const auto refresh_viewport_state = [viewport_ptr, viewport_state_ptr]() {
    const float zoom = viewport_ptr->zoom();
    const st::math::Point pan = viewport_ptr->pan();
    viewport_state_ptr->set_content(std::format(
        "缩放 {:.0f}% · pan ({:.0f}, {:.0f})", zoom * 100.0f, pan.x, pan.y));
  };
  viewport_ptr->on_view_changed = refresh_viewport_state;

  // —— 编辑器族四卡接线（CodeEditor/DiffView/CompletionPopup/CommandPalette）——

  // CodeEditor：只读切换（按钮文案与状态回显同步）；查找高亮 + 命中导航。
  // on_change 只在用户编辑时触发（程序化 set_text 不冒充——与 Input/TextArea 同决议），
  // 所以回显状态条在切只读/查找时由各自按钮直接刷新；字符数则在真编辑时更新。
  const auto refresh_editor_state = [editor_ptr, editor_state_ptr, hooks]() {
    std::string finds;
    if (editor_ptr->find_match_count() > 0U) {
      finds = std::format("，命中 {} 处", editor_ptr->find_match_count());
      if (editor_ptr->find_active_index() != CodeEditor::kNoFindMatch) {
        finds += std::format("（当前 #{}）", editor_ptr->find_active_index() + 1);
      }
    }
    editor_state_ptr->set_content(std::format(
        "{}（高亮 {}；内容 {} 字 · {} 行{}）",
        editor_ptr->read_only() ? "只读（仍可选中/复制）" : "可编辑",
        editor_ptr->language(), st::utf8_length(editor_ptr->text()),
        editor_ptr->line_count(), finds));
    hooks.set_status(std::format("编辑器：{} 行 · {}只读", editor_ptr->line_count(),
                                 editor_ptr->read_only() ? "" : "非"));
  };
  editor_ro_button_ptr->on_click = [editor_ptr, editor_ro_button_ptr, refresh_editor_state]() {
    const bool next = !editor_ptr->read_only();
    editor_ptr->set_read_only(next);
    editor_ro_button_ptr->set_label(next ? "切可编辑" : "切只读");
    refresh_editor_state();
  };
  editor_find_button_ptr->on_click = [editor_ptr, refresh_editor_state]() {
    if (editor_ptr->find_needle() == "answer") {
      (void)editor_ptr->find_next();  // 再点：在命中间环绕跳
    } else {
      (void)editor_ptr->set_find("answer");  // 首点：全部命中高亮
      (void)editor_ptr->find_next();
    }
    refresh_editor_state();
  };
  editor_ptr->on_change = [refresh_editor_state](std::string_view) { refresh_editor_state(); };

  // DiffView：hunk 导航回显 + 布局切换（并列 ⇄ 内联，重建两侧文本）。
  const auto refresh_diff_state = [diff_ptr, diff_state_ptr]() {
    diff_state_ptr->set_content(std::format(
        "差异块 {} 处 · 当前 #{}（{}布局）", diff_ptr->hunk_count(),
        diff_ptr->current_hunk() + 1,
        diff_ptr->layout() == DiffLayout::Unified ? "内联" : "并列"));
  };
  diff_ptr->on_hunk_change = [refresh_diff_state](std::size_t) { refresh_diff_state(); };
  diff_next_button_ptr->on_click = [diff_ptr, refresh_diff_state, hooks]() {
    if (diff_ptr->hunk_count() == 0U) return;
    (void)diff_ptr->next_hunk();
    refresh_diff_state();
    hooks.set_status(std::format("差异跳到 #{} / {}", diff_ptr->current_hunk() + 1,
                                 diff_ptr->hunk_count()));
  };
  diff_layout_button_ptr->on_click = [diff_ptr, refresh_diff_state, hooks]() {
    const bool unified = diff_ptr->layout() == DiffLayout::Unified;
    diff_ptr->set_layout(unified ? DiffLayout::SideBySide : DiffLayout::Unified);
    refresh_diff_state();
    hooks.set_status(unified ? "差异切回并列两栏" : "差异切到内联单栏");
  };

  // CompletionPopup：按钮唤起（锚点=按钮实时 bounds）→ 候选导航/接受回显；
  // 关闭路径（Esc/点别处/接受后）统一走 on_dismiss 摘除浮层。
  completion_open_ptr->on_click = [hooks, completion_open_ptr, completion_state_ptr]() {
    auto popup = std::make_unique<CompletionPopup>();
    popup->set_id("demo-completion");
    popup->set_anchor(completion_open_ptr->bounds());
    popup->set_items(std::vector<CompletionItemView>{
        {.label = "gallery", .detail = "str", .badge = "\u25cb"},
        {.label = "gallery_exhibits", .detail = "file", .badge = "\u25cb"},
        {.label = "gbcode", .detail = "dir", .badge = "\u25cb"},
        {.label = "grab_focus", .detail = "method", .badge = "\u0192"},
        {.label = "goto_line", .detail = "method", .badge = "\u0192"},
    });
    popup->set_visible(true);
    auto* popup_ptr = popup.get();
    popup->on_accept = [popup_ptr, completion_state_ptr, hooks](const std::string& insert) {
      completion_state_ptr->set_content(std::format("已接受候选「{}」（insert_text 回报）", insert));
      hooks.set_status(std::format("补全接受：{}", insert));
      if (hooks.remove_overlay) hooks.remove_overlay(popup_ptr);  // 接受后关闭（gbcode 同策略）
    };
    popup->on_dismiss = [hooks, popup_ptr, completion_state_ptr](bool accepted) {
      if (hooks.remove_overlay) hooks.remove_overlay(popup_ptr);
      if (!accepted) {
        completion_state_ptr->set_content("已取消（Esc / 点击弹层外）");
      }
    };
    if (hooks.add_overlay) hooks.add_overlay(std::move(popup));
    completion_state_ptr->set_content("弹层展示中：↑↓ 导航、Enter/Tab 接受、Esc 取消");
  };

  // CommandPalette：按钮唤起 → 命令表过滤/执行回显；Esc/遮罩关闭。
  // 挂载后必须 grab_focus（否则键盘进不来——组件注释里的实测坑）；
  // on_command 只回 id，执行的语义在各 handler 里（本表把 id 回显到状态条）。
  palette_open_ptr->on_click = [hooks, palette_state_ptr]() {
    auto palette = std::make_unique<CommandPalette>(std::vector<CommandPalette::Command>{
        {.id = "demo.a", .title = "演示: 打个招呼", .detail = "状态栏回显命令 id"},
        {.id = "demo.b", .title = "演示: 报个时辰", .detail = "证明 handler 真在跑"},
        {.id = "demo.c", .title = "演示: 什么都不做", .detail = "空 handler（如实回报）"},
        {.id = "view.icons", .title = "查看: 图标全集", .detail = "本页下方图标卡"},
        {.id = "view.split", .title = "查看: 可拖拽分栏", .detail = "本页分栏卡"},
    });
    palette->set_id("demo-palette");
    // CommandPalette 构造即可见（与 CompletionPopup 相反——那个构造默认隐藏，
    // 必须显式 set_visible(true)，见上一卡的坑）；这里无需再设。
    auto* palette_ptr = palette.get();
    palette->on_command = [palette_state_ptr, hooks, palette_ptr](std::string_view id) {
      palette_state_ptr->set_content(std::format("已执行「{}」（{} 条命中，active）", id,
                                                 palette_ptr->match_count()));
      hooks.set_status(std::format("命令面板执行：{}", id));
    };
    palette->on_close = [hooks, palette_ptr, palette_state_ptr]() {
      if (hooks.remove_overlay) hooks.remove_overlay(palette_ptr);
      palette_state_ptr->set_content("面板已关闭（Esc / 遮罩 / 执行后）");
    };
    if (hooks.add_overlay) hooks.add_overlay(std::move(palette));
    // 挂树之后才能把焦点交给过滤框：grab_focus 经 UiRoot::set_focus 生效
    // （gbcode 实测坑：挂前调不生效，敲的字会跑进底层焦点元素）。
    // （命令面板不自动关：执行后由用户 Esc——本表是演示，多选几条看回显。）
    palette_ptr->grab_focus();
    palette_state_ptr->set_content("面板展示中：输入过滤 · ↑↓ 导航 · Enter 执行 · Esc 关闭");
  };

  // —— 六卡接线（TimelineView/MergeView/MenuBar/ContextMenu/FileDialog/Terminal；
  // 静态展示部分在建卡处）——

  // TimelineView：播放头/片段拖动与修剪回显（组件改 Model 后回调；拖拽期间
  // 不吸附到自己——组件内已按约定传 exclude）。
  const auto refresh_timeline_state = [timeline_ptr, timeline_state_ptr, hooks]() {
    const Millis playhead = timeline_ptr->model().playhead();
    timeline_state_ptr->set_content(std::format(
        "播放头 {:02}:{:02}.{:03} · 缩放 {:.0f}px/s",
        static_cast<int>(playhead / 60000), static_cast<int>((playhead / 1000) % 60),
        static_cast<int>(playhead % 1000), timeline_ptr->model().pixels_per_second()));
    hooks.set_status(std::format("时间轴：播放头 {}ms", static_cast<long long>(playhead)));
  };
  timeline_ptr->on_playhead_changed = [refresh_timeline_state](Millis) { refresh_timeline_state(); };
  timeline_ptr->on_clip_moved = [timeline_state_ptr, hooks](std::size_t track, std::size_t clip) {
    timeline_state_ptr->set_content(std::format("片段移动：轨道 #{} · #{}（吸附候选拉齐）",
                                                track, clip));
    hooks.set_status(std::format("时间轴：轨道 #{} 片段 #{} 已移动", track, clip));
  };
  timeline_ptr->on_clip_trimmed = [timeline_state_ptr, hooks](std::size_t track, std::size_t clip) {
    timeline_state_ptr->set_content(std::format("片段修剪：轨道 #{} · #{}（左右边缘）",
                                                track, clip));
    hooks.set_status(std::format("时间轴：轨道 #{} 片段 #{} 已修剪", track, clip));
  };

  // MergeView：冲突计数回显 + 当前段取舍按钮（作用于当前聚焦段，非冲突段如实拒绝）。
  const auto refresh_merge_state = [merge_ptr, merge_state_ptr]() {
    merge_state_ptr->set_content(std::format(
        "冲突 {} 段 · 已解 {} · {}（当前段 #{}）", merge_ptr->conflict_count(),
        merge_ptr->resolved_count(),
        merge_ptr->has_pending_conflicts() ? "还有待决" : "全部已决",
        merge_ptr->current_segment()));
  };
  merge_ptr->on_choice_changed = [refresh_merge_state]() { refresh_merge_state(); };
  merge_ours_button_ptr->on_click = [merge_ptr, refresh_merge_state, hooks]() {
    (void)merge_ptr->invoke_action("take_ours", {});  // 与控制通道同一条路径
    refresh_merge_state();
    hooks.set_status("合并：当前段取我方");
  };
  merge_theirs_button_ptr->on_click = [merge_ptr, refresh_merge_state, hooks]() {
    (void)merge_ptr->invoke_action("take_theirs", {});
    refresh_merge_state();
    hooks.set_status("合并：当前段取他方");
  };
  merge_next_button_ptr->on_click = [merge_ptr, refresh_merge_state, hooks]() {
    (void)merge_ptr->invoke_action("next_conflict", {});
    refresh_merge_state();
    hooks.set_status(std::format("合并：跳到未决冲突段 #{}", merge_ptr->current_segment()));
  };

  // MenuBar：开面板（make_panel 锚定到标题下方）→ 挂 overlay；条目激活回显。
  // 切换菜单时框架先触发 on_menu_close（尽旧面板）再回调本处（参数=目标菜单序号，
  // 此时 open_index_ 已改到目标——直接用参数，不反读内部状态）。
  menubar_ptr->on_open_menu = [menubar_ptr, menubar_panel_cell, hooks](std::size_t index) {
    if (MenuPanel* live = *menubar_panel_cell; live != nullptr) {
      if (hooks.remove_overlay) hooks.remove_overlay(live);  // 兜底：首开/未走 close 路径
      *menubar_panel_cell = nullptr;
    }
    if (auto panel = menubar_ptr->make_panel(index); panel != nullptr) {
      panel->set_id("demo-menubar-panel");
      MenuPanel* panel_ptr = panel.get();
      *menubar_panel_cell = panel_ptr;
      panel->on_close = [menubar_panel_cell, hooks]() {
        if (MenuPanel* live = *menubar_panel_cell; live != nullptr) {
          if (hooks.remove_overlay) hooks.remove_overlay(live);
          *menubar_panel_cell = nullptr;
        }
      };
      if (hooks.add_overlay) hooks.add_overlay(std::move(panel));
    }
  };
  menubar_ptr->on_menu_close = [menubar_panel_cell, hooks]() {
    if (MenuPanel* live = *menubar_panel_cell; live != nullptr) {
      if (hooks.remove_overlay) hooks.remove_overlay(live);
      *menubar_panel_cell = nullptr;
    }
  };
  menubar_ptr->on_action = [menubar_state_ptr, hooks](const std::string& menu_id,
                                                       const std::string& item_id) {
    menubar_state_ptr->set_content(std::format("已执行 {}.{}（on_action 回报）", menu_id, item_id));
    hooks.set_status(std::format("菜单执行：{}.{}", menu_id, item_id));
  };

  // ContextMenu：按钮唤起（锚点=按钮实时 bounds）→ 条目激活/关闭回显。
  // 工厂 make 已接面板 on_close → 转发 on_close；条目激活需自己接（on_activate）。
  context_open_ptr->on_click = [hooks, context_open_ptr, context_state_ptr]() {
    auto menu = ContextMenu::make(
        Point{context_open_ptr->bounds().x, context_open_ptr->bounds().y},
        std::vector<MenuItem>{
            MenuItem{.id = "ctx.copy", .label = "复制"},
            MenuItem{.id = "ctx.paste", .label = "粘贴"},
            MenuItem{.id = "sep", .label = "", .separator = true},
            MenuItem{.id = "ctx.rename", .label = "重命名…"},
            MenuItem{.id = "ctx.delete", .label = "删除"},
        });
    menu->set_id("demo-context");
    auto* menu_ptr = menu.get();
    menu->panel()->on_activate = [menu_ptr, context_state_ptr, hooks](std::size_t index) {
      context_state_ptr->set_content(std::format(
          "已激活条目 #{}（id={}，invoke activate_item 同路径）", index,
          menu_ptr->panel()->item_id(index)));
      hooks.set_status(std::format("右键菜单：条目 #{}", index));
    };
    menu->on_close = [hooks, menu_ptr, context_state_ptr]() {
      if (hooks.remove_overlay) hooks.remove_overlay(menu_ptr);
      context_state_ptr->set_content("已关闭（Esc / 点击面板外 / 激活条目后）");
    };
    if (hooks.add_overlay) hooks.add_overlay(std::move(menu));
    context_state_ptr->set_content("菜单展示中：点击条目激活 · Esc/点外关闭");
  };

  // FileDialog：按钮打开（Open 模式；起始目录 = 进程 CWD）→ 确认回显全路径。
  // ⚠ 构造即建一个会话并进入目录（reload 同步 fs），所以只在打开时构造，
  // 不预建常驻（每帧重组不存在——本页是命令式，面板生命周期由叠加层持有）。
  filedialog_open_ptr->on_click = [hooks, filedialog_state_ptr]() {
    auto dialog = std::make_unique<FileDialog>(FileDialog::Mode::Open, "打开文件");
    dialog->set_id("demo-filedialog");
    std::string start_dir = ".";
    if (const auto cwd = st::fs::current_dir(); cwd) start_dir = *cwd;
    dialog->set_directory(start_dir);
    auto* dialog_ptr = dialog.get();
    dialog->on_confirm = [dialog_ptr, filedialog_state_ptr, hooks](const std::string& full_path) {
      filedialog_state_ptr->set_content(std::format("已确认：{}", full_path));
      hooks.set_status(std::format("文件对话框：{}", full_path));
      if (hooks.remove_overlay) hooks.remove_overlay(dialog_ptr);
    };
    dialog->on_cancel = [hooks, dialog_ptr, filedialog_state_ptr]() {
      if (hooks.remove_overlay) hooks.remove_overlay(dialog_ptr);
      filedialog_state_ptr->set_content("已取消（Esc / 取消按钮 / 遮罩点击）");
    };
    if (hooks.add_overlay) hooks.add_overlay(std::move(dialog));
    filedialog_state_ptr->set_content("对话框展示中：双击进入目录 · Enter 确认 · Esc 取消");
  };

  // Terminal：行模式静态输出 + 按钮注入/清屏（行模式不需要每帧 pump）。
  terminal_ptr->append("$ stpm run gallery --headless");
  terminal_ptr->append("[就绪] 行模式静态回显（无 PTY；真终端见 gbcode Ctrl+`）");
  terminal_ptr->append("[提示] 点「注入一条命令回显」看 append；「清屏」走 clear()");
  const auto refresh_terminal_state = [terminal_ptr, terminal_state_ptr]() {
    terminal_state_ptr->set_content(std::format(
        "行模式 · 当前会话 {} 行（标签栏可见，单会话演示）",
        terminal_ptr->line_count(terminal_ptr->active_session())));
  };
  terminal_send_button_ptr->on_click = [terminal_ptr, refresh_terminal_state, hooks]() {
    terminal_ptr->append("$ echo gallery-terminal（按钮注入）");
    terminal_ptr->append("gallery-terminal");
    refresh_terminal_state();
    hooks.set_status("终端：已注入一条命令回显");
  };
  terminal_clear_button_ptr->on_click = [terminal_ptr, refresh_terminal_state, hooks]() {
    terminal_ptr->clear();
    refresh_terminal_state();
    hooks.set_status("终端：已清屏");
  };
  refresh_terminal_state();  // 初值回显（建卡处占位换成实测行数）

  // 编辑器/差异卡初值回显（建卡处的占位文案在此换成实测值；与首包 TextArea 同姿态）。
  refresh_editor_state();
  refresh_diff_state();
  refresh_timeline_state();
  refresh_merge_state();

  return page;
}

// ————————————————————————————————————————————————————————————————————————————
// 数据页：表格 / 列表 / 键值
// ————————————————————————————————————————————————————————————————————————————

[[nodiscard]] auto build_data(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = make_page("data", "数据", "表格（斑马纹/对齐/横向滚动）· 列表（业务 key 对齐）· 键值对");

  // —— 表格 ——
  auto table_card = make_card("card-table", "数据表格");
  table_card->add_child(make_caption("固定列宽 + 自适应列 + 右对齐数字列；表头、斑马纹、hover 全部自绘"));
  std::vector<TableColumn> columns{
      TableColumn{.name = "组件", .width = 130.0f},
      TableColumn{.name = "层", .width = 90.0f},
      TableColumn{.name = "状态", .width = 90.0f},
      TableColumn{.name = "说明"},
      TableColumn{.name = "用例数", .width = 80.0f, .align = TextAlign::End},
  };
  auto table = std::make_unique<Table>(std::move(columns));
  table->set_id("demo-table");
  table->style().height = 248.0f;
  table->set_row_height(34.0f);
  for (const auto& row : std::vector<std::vector<std::string>>{
           {"光栅器", "raster", "稳定", "扫描线覆盖抗锯齿 · 运行段覆盖率", "18"},
           {"字体引擎", "text", "稳定", "TTF/OTF/CFF/CID · CJK 回退 · 字形缓存", "12"},
           {"布局引擎", "ui", "稳定", "Flex 子集 · 间距/增长/对齐/换行", "24"},
           {"控制通道", "control", "稳定", "组件树/视觉树/属性读写/键鼠注入", "9"},
           {"脚本层", "ext", "可选", "QuickJS（默认关闭）· 四重配额", "7"},
           {"包管理器", "pkg", "稳定", "求解/lock/获取/校验/构建/lint", "15"},
           {"Markdown", "md", "稳定", "解析 · 流式增量 · 零依赖高亮", "11"},
           {"窗口后端", "shell", "部分", "win32 完整；x11/wayland 探测 + 回退", "4"}}) {
    table->add_row(row);
  }
  auto* table_ptr = table.get();
  table_card->add_child(std::move(table));
  page->add_child(std::move(table_card));
  // 点击行即选中（数据页语义：单选导航）——选中态由表格自绘（primary_soft 底 + 主色左缘条），
  // 语义树/协议可断言（get selected_row）。
  table_ptr->set_on_row_click([table_ptr, hooks](std::size_t row) {
    table_ptr->set_selected_row(row);
    const std::string_view name = table_ptr->cell(row, 0U);
    hooks.set_status(std::format("选中表格第 {} 行（组件列：{}）", row + 1, name));
  });

  // —— 列表 ——
  auto list_card = make_card("card-list", "列表");
  list_card->add_child(make_caption(
      "`sync_items` 用业务 key 对齐：刷新数据不会让选中态与元素 id 漂移（clear+add 会）"));
  auto list = std::make_unique<List>();
  list->set_id("demo-list");
  list->style().height = 220.0f;
  const std::vector<std::pair<std::string, std::string>> items{
      {"布局引擎", "Flex 子集 · 间距/增长/对齐"},
      {"光栅器", "扫描线覆盖抗锯齿 · SIMD 快路径"},
      {"字体引擎", "TTF/CFF/CID · CJK 回退"},
      {"控制通道", "组件树 · 视觉树 · 键鼠注入"},
      {"Markdown", "流式增量 · 代码高亮"},
      {"脚本层", "QuickJS · 默认关闭"},
      {"包管理器", "求解 · lock · 校验 · 构建"},
      {"图标集", "72 个内置图标 · 矢量描边"},
  };
  for (const auto& [label, subtitle] : items) list->add_item(label, subtitle);
  auto* list_ptr = list.get();
  list_card->add_child(std::move(list));
  page->add_child(std::move(list_card));
  list_ptr->set_on_select([list_ptr, hooks](std::size_t index) {
    // 回读真实 label（旧实现只输出索引，自动化拿不到"选了什么"）
    const st::ui::ListItem* item = list_ptr->item(index);
    const std::string_view label = item != nullptr ? std::string_view{item->label()} : "?";
    hooks.set_status(std::format("选中列表项 #{}：{}", index, label));
  });

  // —— 键值 ——
  auto facts_card = make_card("card-facts", "键值对");
  facts_card->add_child(std::make_unique<KeyValueRow>("语言", "C++20"));
  {
    // 真实文件的编译期嵌入：与控制通道读回交叉验证
    const auto embedded = b::embed<"examples/gallery/assets/about.txt">();
    const std::string_view body(embedded.data(), embedded.length());
    const std::size_t newline = body.find('\n');
    st::print("嵌入资源 about.txt: {} 字节 · 首行「{}」\n", embedded.length(),
              body.substr(0, newline == std::string_view::npos ? body.size() : newline));
    auto row =
        std::make_unique<KeyValueRow>("嵌入资源", std::format("about.txt · {} 字节", embedded.length()));
    row->set_id("embedded-about");
    facts_card->add_child(std::move(row));
  }
  facts_card->add_child(std::make_unique<KeyValueRow>("渲染", "软件光栅器"));
  facts_card->add_child(std::make_unique<KeyValueRow>("后端", "headless / x11 / wayland / win32"));
  facts_card->add_child(std::make_unique<KeyValueRow>("协议", "st-control/1"));
  facts_card->add_child(std::make_unique<KeyValueRow>("编译", "stpm 直驱（g++ 默认 / MSVC 回退）"));
  page->add_child(std::move(facts_card));
  return page;
}

// ————————————————————————————————————————————————————————————————————————————
// 控制通道页：把"这个界面能被远程驱动"本身展示出来
// ————————————————————————————————————————————————————————————————————————————

[[nodiscard]] auto build_control(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  auto page = make_page("control", "控制通道",
                        "TCP + st-control/1：组件树 / 选择器 / 属性读写 / 动作触发 / 键鼠注入 / 截图");

  // —— 端点与实时信息 ——
  auto endpoint_card = make_card("card-endpoint", "端点");
  auto runtime_row = make_row(16.0f, true);
  runtime_row->set_id("runtime-row");
  // 四个实时字段：页面只声明"这里要实时值"，具体值由应用在刷新时填（见 `PageHooks`）。
  const auto add_runtime_field = [&hooks, &runtime_row](std::string_view field,
                                                       std::string_view icon,
                                                       std::string_view label) {
    st::ui::Text* value_text = nullptr;
    runtime_row->add_child(make_stat_card(std::format("rt-{}", field), icon, std::string(label),
                                          "—", Tone::Primary, &value_text));
    if (value_text != nullptr && hooks.register_runtime_field) {
      hooks.register_runtime_field(field, [value_text](std::string value) {
        value_text->set_content(std::move(value));
      });
    }
  };
  // "渲染后端" = 窗口后端（headless/win32）；"渲染器" = 谁在画这一帧（software/gpu）。
  // 两者是**不同的问题**：同一台机器上 headless+GPU 与 win32+软件都合法。
  // 之前这里写死"软件光栅器"，切到 GPU 后就成了假信息。
  add_runtime_field("backend", "cpu", "窗口后端");
  add_runtime_field("renderer", "zap", "渲染器");
  add_runtime_field("dpi", "eye", "DPI 缩放");
  add_runtime_field("frames", "activity", "累计帧");
  add_runtime_field("port", "terminal", "控制端口");
  endpoint_card->add_child(std::move(runtime_row));
  endpoint_card->add_child(make_caption(
      "这些值由状态栏的刷新按钮从 `metrics` 实时回读——界面自己展示自己的可观测性"));
  page->add_child(std::move(endpoint_card));

  // —— 方法与选择器（说明性内容）——
  auto api_card = make_card("card-api", "协议方法");
  struct ApiRow {
    std::string_view method;
    std::string_view description;
  };
  const std::array<ApiRow, 10> api_rows{{
      {.method = "tree", .description = "语义树（只含可见节点——切页后即可据此判断当前在哪一页）"},
      {.method = "find", .description = "选择器查询（`#id` / `Type` / `[text~=关键词]` / `:focused`）"},
      {.method = "get / set", .description = "属性读写（属性面与脚本 `$('#x').set()` 共用同一实现）"},
      {.method = "invoke", .description = "动作触发（click / focus / select / submit …）"},
      {.method = "input.*", .description = "真实鼠标键盘注入（click / scroll / text / key）"},
      {.method = "capture", .description = "截图 PNG（物理分辨率；无头模式下的唯一“看见”通道）"},
      {.method = "visual", .description = "视觉树（实际绘制层：bounds / 填充色 / 圆角 / 文本）"},
      {.method = "wait", .description = "条件等待（element / gone / text / stable）——替代轮询截图"},
      {.method = "metrics", .description = "运行指标（后端 / 无头 / DPI / 帧耗时 / 组件数）"},
      {.method = "app", .description = "应用控制（主题、DPI 切换、退出）"},
  }};
  for (const ApiRow& entry : api_rows) {
    auto row = std::make_unique<KeyValueRow>(std::string(entry.method),
                                             std::string(entry.description));
    row->set_id("api-" + st::ascii_lower(entry.method));
    api_card->add_child(std::move(row));
  }
  page->add_child(std::move(api_card));

  // —— 可远程操作靶区 ——
  auto target_card = make_card("card-target", "操作靶区（供自动化练手）");
  target_card->add_child(make_caption(
      "自动化可在这里完成\"点击 → 回读 → 断言\"的闭环：控件都有稳定 id，属性可读可写"));
  auto target_row = make_row(10.0f, true);
  auto drill_button = std::make_unique<Button>("点我计数", Button::Variant::Primary,
                                               Button::Size::Small);
  drill_button->set_id("drill-button");
  auto* drill_ptr = drill_button.get();
  auto drill_input = std::make_unique<Input>();
  drill_input->set_id("drill-input");
  drill_input->set_placeholder("输入点什么…");
  drill_input->style().width = 220.0f;
  auto* drill_input_ptr = drill_input.get();
  auto drill_switch = std::make_unique<Switch>("拨动我");
  drill_switch->set_id("drill-switch");
  auto* drill_switch_ptr = drill_switch.get();
  target_row->add_child(std::move(drill_button));
  target_row->add_child(std::move(drill_input));
  target_row->add_child(std::move(drill_switch));
  target_card->add_child(std::move(target_row));

  auto drill_state = std::make_unique<Text>("计数 0 · 输入空 · 开关 关");
  drill_state->set_id("drill-state");
  drill_state->set_tone(Tone::Muted);
  drill_state->set_font_size(12.0f);
  auto* drill_state_ptr = drill_state.get();
  target_card->add_child(std::move(drill_state));
  page->add_child(std::move(target_card));

  auto counter = std::make_shared<int>(0);
  const auto refresh = [drill_input_ptr, drill_switch_ptr, drill_state_ptr, counter]() {
    drill_state_ptr->set_content(std::format("计数 {} · 输入「{}」· 开关 {}",
                                             *counter, drill_input_ptr->value(),
                                             drill_switch_ptr->checked() ? "开" : "关"));
  };
  drill_ptr->on_click = [hooks, counter, refresh]() {
    ++(*counter);
    refresh();
    hooks.set_status(std::format("计数 → {}", *counter));
  };
  drill_input_ptr->on_change = [hooks, refresh](std::string_view text) {
    refresh();
    hooks.set_status(std::format("输入变为「{}」", text));
  };
  drill_switch_ptr->on_change = [hooks, refresh](bool checked) {
    refresh();
    hooks.set_status(checked ? "开关 → 开" : "开关 → 关");
  };
  return page;
}

// ————————————————————————————————————————————————————————————————————————————
// 关于页：版本 / 分层 / 真实 Markdown 渲染
// ————————————————————————————————————————————————————————————————————————————

[[nodiscard]] auto build_about(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  (void)hooks;
  auto page = make_page("about", "关于", "霜天（Shuangtian）· 原生桌面应用框架 · 全自绘");

  auto version_card = make_card("card-version", "版本");
  version_card->add_child(std::make_unique<KeyValueRow>("名称", "霜天 Shuangtian"));
  version_card->add_child(std::make_unique<KeyValueRow>("版本", "0.1.0"));
  version_card->add_child(std::make_unique<KeyValueRow>("语言", "C++20（全现代子集 + 静态禁令扫描）"));
  version_card->add_child(std::make_unique<KeyValueRow>("依赖", "框架本体零依赖"));
  version_card->add_child(std::make_unique<KeyValueRow>("目标平台", "Linux / Windows / macOS"));
  page->add_child(std::move(version_card));

  // Markdown 实时渲染（真实解析 + 零依赖代码高亮）
  auto md_card = make_card("card-markdown", "Markdown 渲染（真实解析，非拼字符串）");
  auto markdown = std::make_unique<MarkdownView>(std::string(kAboutMarkdown));
  markdown->set_id("about-markdown");
  markdown->style().height = 420.0f;
  md_card->add_child(std::move(markdown));
  page->add_child(std::move(md_card));

  // 嵌入资源原文（与 Markdown 渲染对照）
  auto embed_card = make_card("card-embed", "编译期嵌入资源（assets/about.txt 原文）");
  {
    const auto embedded = b::embed<"examples/gallery/assets/about.txt">();
    auto text = std::make_unique<Text>(std::string(std::string_view(embedded.data(), embedded.length())));
    text->set_id("about-embedded-text");
    text->set_tone(Tone::Muted);
    text->set_multiline(true);
    embed_card->add_child(std::move(text));
  }
  page->add_child(std::move(embed_card));
  return page;
}

// ————————————————————————————————————————————————————————————
// 声明式页：壳（手搭） + 内容区（声明式组件）
//
// 这一页同时承担两件事：
// ① 演示声明式 UI（状态驱动重组 / key 对齐列表 / 条件内容 / 异步 resource）；
// ② 验证「声明式与手搭可以待在同一个界面里」——这是它区别于独立示例的意义：
//    声明式不是一个只能写整个应用的孤岛，而是可以填进任何容器的编排层。
// ————————————————————————————————————————————————————————————

/// 页壳：标题 + 说明卡 + 一个**空的锚点容器**（`declarative-host`）——
/// 声明式树由应用侧 `dsl::mount_into` 挂到它下面（框架的单根语义见 pages.hpp 注释）。
[[nodiscard]] auto build_declarative_shell(const PageHooks& hooks) -> std::unique_ptr<Panel> {
  (void)hooks;
  auto page = make_page("declarative", "声明式",
                        "描述 |状态 → 界面|，重组由框架做（对标 Compose / ArkTS）");

  auto intro = make_card("card-declarative-intro", "为什么值得这样写");
  intro->add_child(make_caption(
      "下面的内容不是手搭的：它由一段 `Component::build()` 描述，改状态 → 下一帧重组 → "
      "真值树按 diff 更新。再也不会有「改了数据忘了同步界面」这类漂移。"));
  page->add_child(std::move(intro));

  // 声明式宿主：空容器，挂载点（应用侧填）。用 `Column` 包一层避免锚点自身被裁剪语义影响。
  auto host = std::make_unique<Panel>(FlexDirection::Column);
  host->set_id("declarative-host");
  host->style().gap = 12.0f;
  page->add_child(std::move(host));

  auto note = make_card("card-declarative-note", "控制通道照常可驱动");
  note->add_child(make_caption(
      "声明式产出的元素与手搭的**在同一棵真值树上**——tree / find / get / set / invoke "
      "对它们完全一致（不变式 2：一套语义、多个入口）。"));
  page->add_child(std::move(note));
  return page;
}

// —— 声明式内容：一个带表单 + 列表 + 异步 + 条件卡片的页 ——
//
// 四种典型形态各占一块（每一块都对应 docs/declarative.md 里的一项能力）：
//   ① 表单：输入 → 状态 → 回显（状态驱动重组的最小闭环）；
//   ② 列表：key 对齐复用（增删不悳动已有项的元素身份）；
//   ③ 异步：`resource` 在工作线程上算，结果回主线程写状态；
//   ④ 条件内容：开关控制一块卡片的在场与缺席（裁剪，不是隐藏）。

struct TodoRow {
  std::string key;
  std::string text;
  bool done{false};
  auto operator==(const TodoRow& other) const -> bool = default;
};

struct DeclarativePage : st::ui::dsl::Component {
  using Component::Component;

  // —— 状态（≈ @State）——
  st::ui::dsl::State<std::string> name{"霜天"};
  st::ui::dsl::State<int> clicks{0};
  st::ui::dsl::State<std::vector<TodoRow>> todos{std::vector<TodoRow>{
      TodoRow{"a", "读 DESIGN.md", true},
      TodoRow{"b", "写一个声明式页面", false},
      TodoRow{"c", "跑通控制通道验证", false},
  }};
  st::ui::dsl::State<std::string> draft{""};
  st::ui::dsl::State<bool> show_stats{true};
  st::ui::dsl::State<std::string> query{"shuangtian"};
  st::ui::dsl::State<std::string> filter{""};        // ⑤ 派生：过滤词（memo 的依赖）
  st::ui::dsl::State<std::string> effect_log{""};    // ⑤ 副作用写的行（effect 的产物）

  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;

    // ⑤ hooks 集中在 build 开头：hooks 按**调用点序号**对齐槽位，顺序必须每帧一致
    //    （写在条件分支里会错位——同 React Hooks 的规则）。
    //
    // 下面两个 `Deps` 与一个 lambda 都**具名**传——不是为了好看，是绕开 GCC 的
    // `-Wdangling-reference` 误报：该启发式看到「返回引用的函数」+「实参里有绑定到
    // 临时的东西」就报（它**不看**被引用的对象其实是 `Composer` 里活得更久的缓存槽）。
    // 实测触发点先后踩过两个：临时 `Deps`（已用局部变量回避）、以及这里把 lambda
    // 直接写在实参里（闭包对象本身是临时——“a temporary bound to a reference parameter”）。
    // 具名变量把两个触发条件都消掉，改动面最小，也不像 `-Wno-dangling-reference`
    // 那样把本文件里**真正**的悬垂风险一并静默掉。
    const Deps memo_deps{{&todos, &filter}};
    const Deps effect_deps{{&name}};
    const auto visible_source = [&] {
      std::vector<TodoRow> out;
      for (const auto& item : todos.value()) {
        if (filter.value().empty() || item.text.find(filter.value()) != std::string::npos) {
          out.push_back(item);
        }
      }
      return out;
    };
    const auto& visible = memo<std::vector<TodoRow>>(c, visible_source, memo_deps);
    int& builds = ref<int>(c, 0);
    ++builds;   // ref：每次重组自增（它**不触发**重组——要驱动界面得用 State）
    effect(c, [this] { effect_log.set("name 变了 → " + name.value()); }, effect_deps);

    // ① 表单 + 计数（状态驱动重组）
    (void)card(c, {.gap = 10.0f, .padding = 16.0f, .key = "form"}, [&] {
      (void)heading(c, "① 状态驱动：输入即回显", 3);
      (void)input(c, name.value(),
                  [this](std::string next) { name.set(std::move(next)); },
                  {.id = "decl-name", .key = "name-input"});
      (void)text(c, [&] { return "你好，" + name.value() + "！（改上面输入框，这行自己变）"; },
                 {.id = "decl-greeting", .key = "greeting"});
      (void)row(c, {.gap = 8.0f}, [&] {
        (void)button(c, "点击 +1", [this] { clicks.set(clicks.value() + 1); },
                     {.id = "decl-inc", .key = "inc"});
        (void)button(c, "重置", [this] { clicks.set(0); }, {.id = "decl-reset", .key = "reset"});
        (void)text(c, [&] { return "计数 " + std::to_string(clicks.value()); },
                   {.id = "decl-clicks", .key = "clicks"});
      });
    });

    // ② 列表：key 对齐复用 + 增删
    (void)card(c, {.gap = 10.0f, .padding = 16.0f, .key = "list"}, [&] {
      (void)heading(c, "② 列表：key 对齐复用", 3);
      (void)row(c, {.gap = 8.0f}, [&] {
        (void)input(c, draft.value(), [this](std::string next) { draft.set(std::move(next)); },
                    {.id = "decl-draft", .key = "draft"});
        (void)button(c, "添加", [this] { add_item(); }, {.id = "decl-add", .key = "add"});
        (void)button(c, "清空", [this] { todos.set({}); }, {.id = "decl-clear", .key = "clear"});
      });
      const auto items = todos.value();
      if (items.empty()) {
        (void)text(c, [] { return std::string("（空列表）"); }, {.key = "empty"});
      }
      for (const auto& item : items) {
        (void)row(c, {.gap = 8.0f, .key = item.key}, [&, key = item.key] {
          (void)checkbox(c, item.text, item.done,
                         [this, key](bool next) { set_done(key, next); },
                         {.key = key + "-box"});
          (void)button(c, "×", [this, key] { remove_item(key); }, {.key = key + "-del"});
        });
      }
    });

    // ③ 异步 resource：工作线程算，结果回主线程写状态
    (void)card(c, {.gap = 10.0f, .padding = 16.0f, .key = "async"}, [&] {
      (void)heading(c, "③ 异步资源：工作线程上算", 3);
      (void)input(c, query.value(), [this](std::string next) { query.set(std::move(next)); },
                  {.id = "decl-query", .key = "query"});
      const auto& data = resource<std::string>(c, [](const std::string& input,
                                                     AsyncCancel cancel) -> std::string {
          // 模拟一个不快的计算（改输入会翻旧代的牌——旧任务提前退出）
          for (int step = 0; step < 20; ++step) {
            if (cancel.is_cancelled()) return "（已取消）";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
          return "「" + input + "」长度 = " + std::to_string(input.size());
        }, query.value());
      const auto state = data.value();
      const std::string line = state.status == AsyncStatus::Pending ? std::string("计算中…")
                               : state.status == AsyncStatus::Error ? "出错：" + state.error
                                                                    : state.value;
      (void)text(c, [line] { return line; }, {.id = "decl-async", .key = "async-result"});
    });

    // ④ 条件内容：关掉 = 本帧不声明 = 框架裁剪掉（不是 hidden）
    (void)card(c, {.gap = 10.0f, .padding = 16.0f, .key = "cond"}, [&] {
      (void)heading(c, "④ 条件内容：不声明即移除", 3);
      (void)checkbox(c, "显示统计卡", show_stats.value(),
                     [this](bool next) { show_stats.set(next); }, {.key = "toggle-stats"});
      if (show_stats.value()) {
        (void)card(c, {.gap = 6.0f, .padding = 12.0f, .key = "stats"}, [&] {
          (void)text(c, [&] { return "待办 " + std::to_string(todos.value().size()) + " 项"; });
          (void)text(c, [&] { return "点击 " + std::to_string(clicks.value()) + " 次"; });
        });
      }
    });

    // ⑤ 状态系统的高层原语：memo（依赖未变不重算）/ effect（依赖变化跑一次）/
    //    ref（跨重组稳定、不触发重组）。三种在同一份 build 里共存，槽位按调用点对齐。
    (void)card(c, {.gap = 10.0f, .padding = 16.0f, .key = "hooks"}, [&] {
      (void)heading(c, "⑤ memo / effect / ref", 3);
      (void)input(c, filter.value(), [this](std::string next) { filter.set(std::move(next)); },
                  {.id = "decl-filter", .key = "filter"});
      (void)text(c,
                 [&] {
                   return "memo 过滤出 " + std::to_string(visible.size()) + " / " +
                          std::to_string(todos.value().size()) + " 项（每帧 " +
                          std::to_string(builds) + " 次 build）";
                 },
                 {.id = "decl-memo", .key = "memo-line"});
      (void)text(c, [&] { return "effect：" + effect_log.value(); },
                 {.id = "decl-effect", .key = "effect-line"});
    });
  }

 private:
  void add_item() {
    std::string text = draft.value();
    if (text.empty()) return;
    auto list = todos.value();
    list.push_back(TodoRow{std::to_string(next_key_++), std::move(text), false});
    todos.set(std::move(list));
    draft.set({});
  }

  void set_done(const std::string& key, bool done) {
    auto list = todos.value();
    for (auto& item : list) {
      if (item.key == key) item.done = done;
    }
    todos.set(std::move(list));
  }

  void remove_item(const std::string& key) {
    auto list = todos.value();
    std::erase_if(list, [&key](const TodoRow& item) { return item.key == key; });
    todos.set(std::move(list));
  }

  int next_key_{100};
};

}  // namespace

// 工厂在 `gallery` 命名空间（与 pages.hpp 的声明对应）——组件定义在匿名空间无妨：
// 返回类型是基类指针，定义与使用都在本翻译单元内。
[[nodiscard]] auto make_declarative_component() -> std::shared_ptr<st::ui::dsl::Component> {
  return std::make_shared<DeclarativePage>();
}

auto page_specs() -> const std::array<PageSpec, kPageCount>& {
  static const std::array<PageSpec, kPageCount> specs{{
      {.id = "overview", .icon = "grid", .label = "概览",
       .subtitle = "框架能力一览"},
      {.id = "components", .icon = "layers", .label = "组件",
       .subtitle = "控件全集"},
      {.id = "data", .icon = "database", .label = "数据",
       .subtitle = "表格 / 列表 / 键值"},
      {.id = "declarative", .icon = "sparkles", .label = "声明式",
       .subtitle = "状态驱动（Compose / ArkTS 风）"},
      {.id = "control", .icon = "cpu", .label = "控制通道",
       .subtitle = "远程可驱动"},
      {.id = "about", .icon = "info", .label = "关于",
       .subtitle = "版本与分层"},
  }};
  return specs;
}

auto build_page(std::size_t index, const PageHooks& hooks) -> std::unique_ptr<Element> {
  switch (index) {
    case 0: return build_overview(hooks);
    case 1: return build_components(hooks);
    case 2: return build_data(hooks);
    // 声明式页：页壳（标题/说明卡）手搭，内容区交给声明式子树（应用侧 mount_into 挂）——
    // 两者边界清楚：页面模块管「页里有什么」，应用管「声明式宿主接在哪」。
    case 3: return build_declarative_shell(hooks);
    case 4: return build_control(hooks);
    default: return build_about(hooks);
  }
}

}  // namespace gallery
