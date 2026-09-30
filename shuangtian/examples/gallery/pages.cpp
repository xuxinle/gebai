#include "pages.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>
#include <vector>

#include "battery/embed.hpp"  // 编译期资源嵌入（stpm 生成）
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/select.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/components/scene_view.hpp"
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
using st::ui::Chip;
using st::ui::Divider;
using st::ui::Element;
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
using st::ui::Panel;
using st::ui::ProgressBar;
using st::ui::Radio;
using st::ui::Select;
using st::ui::Slider;
using st::ui::Spinner;
using st::ui::Switch;
using st::ui::Table;
using st::ui::TableColumn;
using st::ui::Tabs;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::Tone;

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
                        "选择控件 · 滑块 · 标签页 · 下拉 · 反馈徽标 · 图标 · 文字 · 浮层 · 多行文本 · 禁用态");

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
  const auto& metrics = st::ui::Theme::light().metrics();
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

  // —— 浮层交互逻辑（Dialog / Toast：面板挂 UiRoot 叠加层，摘除经 hooks） ——
  dialog_button_ptr->on_click = [hooks, overlay_state_ptr]() {
    auto dialog = std::make_unique<st::ui::Dialog>(
        "确认操作",
        "对话框经 UiRoot 叠加层挂载：Esc、点击遮罩、底部按钮三条路径都能关闭。\n"
        "本例验证模态交互与自动测试可行性（tree 可见 Dialog 节点，invoke 可点按钮）。");
    dialog->set_id("demo-dialog");
    dialog->set_actions({"取消", "确认"});
    dialog->set_viewport_rect(hooks.viewport ? hooks.viewport() : st::math::Rect{});
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
  facts_card->add_child(std::make_unique<KeyValueRow>("编译", "stpm 直驱（MSVC 首选 / GCC / Clang）"));
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

}  // namespace

auto page_specs() -> const std::array<PageSpec, kPageCount>& {
  static const std::array<PageSpec, kPageCount> specs{{
      {.id = "overview", .icon = "grid", .label = "概览",
       .subtitle = "框架能力一览"},
      {.id = "components", .icon = "layers", .label = "组件",
       .subtitle = "控件全集"},
      {.id = "data", .icon = "database", .label = "数据",
       .subtitle = "表格 / 列表 / 键值"},
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
    case 3: return build_control(hooks);
    default: return build_about(hooks);
  }
}

}  // namespace gallery
