/// 霜天组件画廊（gallery）——框架的"美观"验证载体与无头协同开发的主要靶场。
///
/// 用法：
///   gallery --headless [--frames 3] [--scale 2.0] [--control-port 0] [--control-file path]
///           [--theme dark] [--shots DIR]
/// 无头模式下不创建窗口，控制通道（TCP）承担全部交互：tree / find / get / set / invoke /
/// input.* / capture / visual / wait / metrics —— 智能体可据此开发与验证界面。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "battery/embed.hpp"  // 编译期资源嵌入（stpm 生成；见 third_party/battery/UPSTREAM.md）
#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/print.hpp"
#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::math::Color;
using st::math::Insets;
using st::math::Point;
using st::math::Rect;
using st::ui::Align;
using st::ui::Button;
using st::ui::Card;
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
using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::Tone;

/// 自绘组件示例：进度条（示范「组件即代码」——不依赖内置控件也能得到同一套视觉语言）。
class ProgressRow : public Element {
 public:
  ProgressRow(std::string label, float value, Tone tone = Tone::Primary)
      : label_(std::move(label)), value_(value), tone_(tone) {
    set_id("progress-" + label_);
  }
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ProgressRow"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::ProgressBar; }

  void set_value(float value) {
    value_ = st::math::clamp01(value);
    mark_dirty();
  }
  [[nodiscard]] auto semantics_value() const -> std::string override {
    return std::format("{:.0f}%", static_cast<double>(value_ * 100.0f));
  }
  [[nodiscard]] auto semantics_text() const -> std::string override { return label_; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override {
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
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override {
    (void)context;
    measured_ = st::math::Size{constraints.max_width, 34.0f};
  }
  void paint_content(const st::ui::RenderContext& context, st::raster::Canvas& canvas) const override {
    const auto& colors = context.theme.colors();
    const float label_height = style_.font_size * 1.4f;
    const float track_y = bounds_.y + label_height + 2.0f;
    const float track_height = 6.0f;
    const Color accent = st::ui::tone_color(context.theme, tone_);

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

/// 统计卡片：大数字 + 标签 + 图标。
[[nodiscard]] auto make_stat_card(const std::string& id, std::string_view icon, std::string label,
                                  std::string value, Tone tone) -> std::unique_ptr<Card> {
  auto card = std::make_unique<Card>(16.0f);
  card->set_id(id);
  card->style().grow = true;
  card->style().direction = FlexDirection::Column;
  card->style().gap = 6.0f;

  auto header = std::make_unique<Panel>(FlexDirection::Row);
  header->style().gap = 8.0f;
  header->style().align_items = Align::Center;
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
  card->add_child(std::move(value_text));
  return card;
}

[[nodiscard]] auto make_nav_item(std::string_view icon, std::string label, bool active)
    -> std::unique_ptr<Button> {
  auto button = std::make_unique<Button>(std::string(label), active ? Button::Variant::Soft
                                                                    : Button::Variant::Ghost,
                                         Button::Size::Medium);
  button->set_icon(std::string(icon));
  button->style().justify = Justify::Start;
  button->style().text_align = TextAlign::Start;
  auto text = std::make_unique<Text>(std::string(label));
  (void)text;
  return button;
}

struct Options {
  bool headless{false};
  float scale{0.0f};
  std::string theme{"light"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::uint32_t frames{0};
  /// 渲染基准帧数（`--bench N`）：每帧**强制全量重绘**并报告分阶段分位耗时。
  std::uint32_t bench{0};
  int max_ms{0};
};

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view raw = argv[index];
    const auto next = [&](std::string fallback) -> std::string {
      if (index + 1 < argc) return argv[++index];
      return fallback;
    };
    if (raw == "--headless") options.headless = true;
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(next("1")));
    else if (raw == "--dpi") options.scale = static_cast<float>(std::stod(next("1")));
    else if (raw == "--theme") options.theme = next("light");
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(next("0")));
    else if (raw == "--control-file") options.control_file = next("");
    else if (raw == "--shots") options.shots = next("");
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(next("0")));
  else if (raw == "--bench") options.bench = static_cast<std::uint32_t>(std::stoi(next("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(next("0"));
    else if (raw == "--help" || raw == "-h") {
      st::print("用法: gallery [--headless] [--scale 2.0] [--theme dark] [--control-port 0]\n"
                  "               [--control-file PATH] [--shots DIR] [--frames N] [--ms N]\n");
      std::exit(0);
    }
  }
  return options;
}

}  // namespace

/// 渲染基准（性能优化的可重复标尺）：连续 `frames` 帧**强制全量重绘**，
/// 报告总帧耗时与分阶段（排版 / 绘制 / 送显）的 p50、p95。
///
/// 为什么放在示例而不是框架：基准要跑的"内容"是应用自己的界面；
/// 框架只需提供可组合的原语（`root().mark_dirty_all()` + `render_frame()` + `metrics()`）。
/// 每帧都先标脏整棵树——这是**最坏情况**，也正因为最坏才可复现：
/// 否则"第二帧开始不重绘"会把排版成本静默地排除在测量之外（实测被坑过）。
[[nodiscard]] auto run_render_bench(st::app::Application& app, std::uint32_t frames) -> int {
  std::vector<double> total;
  std::vector<double> layout;
  std::vector<double> paint;
  std::vector<double> present;
  total.reserve(frames);
  layout.reserve(frames);
  paint.reserve(frames);
  present.reserve(frames);
  for (std::uint32_t index = 0; index < frames; ++index) {
    app.root().mark_dirty_all();
    app.render_frame();
    const st::control::Metrics metrics = app.metrics();
    total.push_back(metrics.last_frame_ms);
    layout.push_back(metrics.layout_ms);
    paint.push_back(metrics.paint_ms);
    present.push_back(metrics.present_ms);
  }
  const auto summarize = [&frames](const char* name, std::vector<double>& samples) {
    std::ranges::sort(samples);
    const auto at = [&samples](double ratio) -> double {
      const auto index = static_cast<std::size_t>(ratio * static_cast<double>(samples.size() - 1));
      return samples[index];
    };
    double sum = 0.0;
    for (const double value : samples) sum += value;
    st::print("  {:<8} p50 {:6.2f} ms · p95 {:6.2f} ms · 均 {:6.2f} ms · 最大 {:6.2f} ms\n", name,
              at(0.5), at(0.95), sum / static_cast<double>(samples.size()), samples.back());
  };
  st::print("渲染基准：{} 帧（每帧强制全量重绘）\n", frames);
  summarize("总帧", total);
  summarize("排版", layout);
  summarize("绘制", paint);
  summarize("送显", present);
  // 绘制分解（需 `ST_PAINT_PROFILE=1`）：只看“绘制 30ms”不知道该改哪里
  if (const st::raster::PaintProfiler* profile = app.paint_profile(); profile != nullptr) {
    st::print("绘制分解（最后一帧，按累计耗时排序）：\n");
    std::vector<std::pair<double, std::string>> rows;
    for (std::size_t index = 0; index < static_cast<std::size_t>(st::raster::PaintOp::Count);
         ++index) {
      const auto op = static_cast<st::raster::PaintOp>(index);
      const st::raster::PaintOpStat& stat = profile->op(op);
      if (stat.calls == 0) continue;
      rows.emplace_back(
          stat.ms, std::format("{:<10} 调用 {:>5} · {:>8.2f} ms · {:>9} 像素 · {:>7.3} ms/次",
                               st::raster::paint_op_name(op), stat.calls, stat.ms, stat.pixels,
                               stat.ms / static_cast<double>(stat.calls)));
    }
    std::ranges::sort(rows, [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    for (const auto& [ms, text] : rows) st::print("  {}\n", text);
  }
  return 0;
}

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  st::app::AppOptions app_options;
  app_options.width = 1280;
  app_options.height = 800;
  app_options.scale = options.scale;
  app_options.title = "霜天 · 组件画廊";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;

  st::app::Application app("gallery", "0.1.0", app_options);

  // —— 根布局 ——
  auto root_panel = std::make_unique<Panel>(FlexDirection::Column);
  root_panel->set_id("app-root");

  auto* app_ptr = &app;
  auto* root_ptr = &app.root();

  // 顶部栏
  auto top_bar = std::make_unique<Panel>(FlexDirection::Row);
  top_bar->set_id("top-bar");
  top_bar->style().height = 60.0f;
  top_bar->style().padding = Insets{24.0f, 0.0f, 24.0f, 0.0f};
  top_bar->style().gap = 10.0f;
  top_bar->style().align_items = Align::Center;
  auto brand_icon = std::make_unique<IconView>("sparkles", 22.0f);
  brand_icon->set_tone(Tone::Primary);
  top_bar->add_child(std::move(brand_icon));
  auto title = std::make_unique<Heading>("霜天 · 组件画廊", 3);
  title->set_id("app-title");
  top_bar->add_child(std::move(title));
  auto top_spacer = std::make_unique<Panel>(FlexDirection::Row);
  top_spacer->style().grow = true;
  top_bar->add_child(std::move(top_spacer));

  auto theme_button = std::make_unique<Button>("暗色", Button::Variant::Ghost, Button::Size::Small);
  theme_button->set_id("theme-toggle");
  theme_button->set_icon("moon");
  auto* theme_button_ptr = theme_button.get();
  auto scale_button = std::make_unique<Button>("DPI 1x", Button::Variant::Secondary, Button::Size::Small);
  scale_button->set_id("dpi-toggle");
  scale_button->set_icon("cpu");
  auto* scale_button_ptr = scale_button.get();
  auto shot_button = std::make_unique<Button>("截图", Button::Variant::Primary, Button::Size::Small);
  shot_button->set_id("btn-capture");
  shot_button->set_icon("image");
  auto* shot_button_ptr = shot_button.get();

  top_bar->add_child(std::move(theme_button));
  top_bar->add_child(std::move(scale_button));
  top_bar->add_child(std::move(shot_button));
  root_panel->add_child(std::move(top_bar));

  // 主体
  auto body = std::make_unique<Panel>(FlexDirection::Row);
  body->set_id("body");
  body->style().grow = true;

  auto sidebar = std::make_unique<Panel>(FlexDirection::Column);
  sidebar->set_id("sidebar");
  sidebar->style().width = 224.0f;
  sidebar->style().padding = Insets{12.0f, 16.0f, 12.0f, 16.0f};
  sidebar->style().gap = 4.0f;
  auto nav_label = std::make_unique<Text>("导航");
  nav_label->set_tone(Tone::Faint);
  nav_label->set_font_size(11.0f);
  sidebar->add_child(std::move(nav_label));
  sidebar->add_child(make_nav_item("grid", "概览", true));
  sidebar->add_child(make_nav_item("layers", "组件", false));
  sidebar->add_child(make_nav_item("database", "数据", false));
  sidebar->add_child(make_nav_item("cpu", "控制通道", false));
  sidebar->add_child(make_nav_item("info", "关于", false));
  auto sidebar_spacer = std::make_unique<Panel>(FlexDirection::Column);
  sidebar_spacer->style().grow = true;
  sidebar->add_child(std::move(sidebar_spacer));
  auto version_text = std::make_unique<Text>("霜天 v0.1.0 · C++20");
  version_text->set_tone(Tone::Faint);
  version_text->set_font_size(11.0f);
  sidebar->add_child(std::move(version_text));
  body->add_child(std::move(sidebar));

  auto scroll = std::make_unique<ScrollView>();
  scroll->set_id("content-scroll");
  scroll->style().grow = true;
  auto content = std::make_unique<Panel>(FlexDirection::Column);
  content->set_id("content");
  content->style().padding = Insets::all(24.0f);
  content->style().gap = 16.0f;

  auto page_title = std::make_unique<Heading>("概览", 2);
  content->add_child(std::move(page_title));
  auto page_sub = std::make_unique<Text>("自绘 UI · 跨平台 · 软硬件渲染兼容 · 无头可控 · DPI 感知");
  page_sub->set_tone(Tone::Muted);
  content->add_child(std::move(page_sub));

  // 统计行
  auto stats = std::make_unique<Panel>(FlexDirection::Row);
  stats->set_id("stats-row");
  stats->style().gap = 12.0f;
  stats->add_child(make_stat_card("stat-backend", "cpu", "渲染后端", "软件光栅器", Tone::Primary));
  stats->add_child(make_stat_card("stat-dpi", "eye", "DPI 缩放", "1.0x", Tone::Accent));
  stats->add_child(make_stat_card("stat-nodes", "layers", "组件节点", "自绘", Tone::Success));
  stats->add_child(make_stat_card("stat-control", "terminal", "控制通道", "TCP", Tone::Warning));
  content->add_child(std::move(stats));

  // 按钮与图标
  auto button_card = std::make_unique<Card>(18.0f);
  button_card->set_id("card-buttons");
  button_card->style().gap = 14.0f;
  button_card->style().direction = FlexDirection::Column;
  auto button_title = std::make_unique<Text>("按钮与图标");
  button_title->set_weight(FontWeight::SemiBold);
  button_card->add_child(std::move(button_title));
  auto button_row = std::make_unique<Panel>(FlexDirection::Row);
  button_row->style().gap = 8.0f;
  button_row->style().wrap = true;
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
  auto divider = std::make_unique<Divider>(false);
  button_card->add_child(std::move(divider));
  auto icon_row = std::make_unique<Panel>(FlexDirection::Row);
  icon_row->style().gap = 14.0f;
  icon_row->style().align_items = Align::Center;
  for (const auto* icon_name :
       {"search", "settings", "user", "folder", "code", "terminal", "bell", "calendar", "star",
        "heart", "bolt", "shield", "cloud", "database", "send", "tag"}) {
    auto icon = std::make_unique<IconView>(icon_name, 18.0f);
    icon->set_tone(Tone::Muted);
    icon->set_id(std::string("icon-") + icon_name);
    icon_row->add_child(std::move(icon));
  }
  button_card->add_child(std::move(icon_row));
  content->add_child(std::move(button_card));

  // 表单
  auto form_card = std::make_unique<Card>(18.0f);
  form_card->set_id("card-form");
  form_card->style().gap = 12.0f;
  form_card->style().direction = FlexDirection::Column;
  auto form_title = std::make_unique<Text>("表单");
  form_title->set_weight(FontWeight::SemiBold);
  form_card->add_child(std::move(form_title));
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
  auto form_actions = std::make_unique<Panel>(FlexDirection::Row);
  form_actions->style().gap = 8.0f;
  auto submit_button = std::make_unique<Button>("提交", Button::Variant::Primary,
                                                Button::Size::Small);
  submit_button->set_id("btn-submit");
  auto* submit_ptr = submit_button.get();
  auto reset_button = std::make_unique<Button>("重置", Button::Variant::Ghost, Button::Size::Small);
  reset_button->set_id("btn-reset");
  auto* reset_ptr = reset_button.get();
  form_actions->add_child(std::move(submit_button));
  form_actions->add_child(std::move(reset_button));
  form_card->add_child(std::move(form_actions));
  content->add_child(std::move(form_card));

  // 列表与数据
  auto data_card = std::make_unique<Card>(18.0f);
  data_card->set_id("card-data");
  data_card->style().gap = 12.0f;
  data_card->style().direction = FlexDirection::Column;
  auto data_title = std::make_unique<Text>("列表与数据");
  data_title->set_weight(FontWeight::SemiBold);
  data_card->add_child(std::move(data_title));
  auto data_row = std::make_unique<Panel>(FlexDirection::Row);
  data_row->style().gap = 20.0f;
  auto list = std::make_unique<List>();
  list->set_id("demo-list");
  list->style().grow = true;
  list->add_item("布局引擎", "Flex 子集 · 间距/增长/对齐");
  list->add_item("光栅器", "扫描线覆盖抗锯齿 · SIMD");
  list->add_item("字体引擎", "TTF/CFF/CID · CJK 回退");
  list->add_item("控制通道", "组件树 · 视觉树 · 键鼠注入");
  list->add_item("Markdown", "流式增量 · 代码高亮");
  auto* list_ptr = list.get();
  data_row->add_child(std::move(list));

  auto facts = std::make_unique<Panel>(FlexDirection::Column);
  facts->set_id("demo-facts");
  facts->style().width = 300.0f;
  facts->style().gap = 6.0f;
  facts->add_child(std::make_unique<KeyValueRow>("语言", "C++20"));
  // 这一行来自**编译期嵌入的真实文件**（`examples/gallery/assets/about.txt`）：
  // 文案在编辑器里写（有高亮、无需转义），构建时由 stpm 转成字节数组编译进可执行文件。
  {
    const auto embedded = b::embed<"examples/gallery/assets/about.txt">();
    // 启动时打印：与控制通道读回交叉验证（嵌入字节数应与源文件逐一相等）
    const std::string_view embedded_body(embedded.data(), embedded.length());
    const std::size_t newline = embedded_body.find('\n');
    st::print("嵌入资源 about.txt: {} 字节 · 首行「{}」\n", embedded.length(),
              embedded_body.substr(0, newline == std::string_view::npos ? embedded_body.size() : newline));
    auto row = std::make_unique<KeyValueRow>(
        "嵌入资源", std::format("about.txt · {} 字节", embedded.length()));
    row->set_id("embedded-about");
    facts->add_child(std::move(row));
  }
  facts->add_child(std::make_unique<KeyValueRow>("渲染", "软件光栅器"));
  facts->add_child(std::make_unique<KeyValueRow>("后端", "headless / x11 / wayland / win32"));
  facts->add_child(std::make_unique<KeyValueRow>("协议", "st-control/1"));
  data_row->add_child(std::move(facts));
  data_card->add_child(std::move(data_row));
  content->add_child(std::move(data_card));

  // 进度与状态
  auto progress_card = std::make_unique<Card>(18.0f);
  progress_card->set_id("card-progress");
  progress_card->style().gap = 14.0f;
  progress_card->style().direction = FlexDirection::Column;
  auto progress_title = std::make_unique<Text>("进度与状态（示例自绘组件）");
  progress_title->set_weight(FontWeight::SemiBold);
  progress_card->add_child(std::move(progress_title));
  progress_card->add_child(std::make_unique<ProgressRow>("光栅器覆盖率", 0.86f, Tone::Primary));
  progress_card->add_child(std::make_unique<ProgressRow>("字体缓存命中", 0.72f, Tone::Success));
  progress_card->add_child(std::make_unique<ProgressRow>("协议实现度", 0.94f, Tone::Accent));
  content->add_child(std::move(progress_card));

  auto content_spacer = std::make_unique<Panel>(FlexDirection::Column);
  content_spacer->style().height = 8.0f;
  content->add_child(std::move(content_spacer));

  scroll->add_child(std::move(content));
  body->add_child(std::move(scroll));
  root_panel->add_child(std::move(body));

  // 状态栏
  auto status_bar = std::make_unique<Panel>(FlexDirection::Row);
  status_bar->set_id("status-bar");
  status_bar->style().height = 30.0f;
  status_bar->style().padding = Insets{24.0f, 0.0f, 24.0f, 0.0f};
  status_bar->style().align_items = Align::Center;
  status_bar->style().gap = 8.0f;
  auto status_dot = std::make_unique<IconView>("dot", 8.0f);
  status_dot->set_tone(Tone::Success);
  status_bar->add_child(std::move(status_dot));
  auto status_text = std::make_unique<Text>("就绪");
  status_text->set_id("status-text");
  status_text->set_tone(Tone::Muted);
  status_text->set_font_size(12.0f);
  auto* status_ptr = status_text.get();
  status_bar->add_child(std::move(status_text));
  auto status_spacer = std::make_unique<Panel>(FlexDirection::Row);
  status_spacer->style().grow = true;
  status_bar->add_child(std::move(status_spacer));
  auto status_right = std::make_unique<Text>("等待控制通道…");
  status_right->set_id("status-right");
  status_right->set_tone(Tone::Faint);
  status_right->set_font_size(12.0f);
  auto* status_right_ptr = status_right.get();
  status_bar->add_child(std::move(status_right));
  root_panel->add_child(std::move(status_bar));

  // —— 交互 ——
  theme_button_ptr->on_click = [app_ptr, theme_button_ptr, status_right_ptr, root_ptr]() {
    const bool dark = app_ptr->root().theme().mode() == st::ui::ThemeMode::Light;
    app_ptr->set_theme_mode(dark ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light);
    theme_button_ptr->set_label(dark ? "亮色" : "暗色");
    theme_button_ptr->set_icon(dark ? "sun" : "moon");
    status_right_ptr->set_content(dark ? "主题 dark · 视觉令牌已切换" : "主题 light · 视觉令牌已切换");
    root_ptr->mark_dirty_all();
  };
  scale_button_ptr->on_click = [app_ptr, scale_button_ptr, status_right_ptr, root_ptr]() {
    const float current = app_ptr->device_scale();
    const float next = current < 1.25f ? 1.5f : (current < 1.75f ? 2.0f : 1.0f);
    if (auto status = app_ptr->set_device_scale(next); status.has_value()) {
      scale_button_ptr->set_label(std::format("DPI {}x", next == 1.0f ? 1 : (next == 1.5f ? 2 : 3)));
      status_right_ptr->set_content(std::format("DPI {:.1f}x · 字形已按物理分辨率重栅格化",
                                                static_cast<double>(next)));
    } else {
      status_right_ptr->set_content("DPI 切换失败: " + status.error().message);
    }
    root_ptr->mark_dirty_all();
  };
  shot_button_ptr->on_click = [app_ptr, status_ptr, root_ptr]() {
    auto saved = app_ptr->capture_to_file("", {});
    if (saved.has_value()) {
      status_ptr->set_content("已截图: " + st::fs::file_name(*saved));
    } else {
      status_ptr->set_content("截图失败: " + saved.error().message);
    }
    root_ptr->mark_dirty_all();
  };
  submit_ptr->on_click = [search_ptr, status_ptr, root_ptr]() {
    status_ptr->set_content("已提交: " + search_ptr->value());
    root_ptr->mark_dirty_all();
  };
  reset_ptr->on_click = [search_ptr, status_ptr, root_ptr]() {
    search_ptr->set_text("");
    status_ptr->set_content("已重置");
    root_ptr->mark_dirty_all();
  };
  list_ptr->set_on_select([status_ptr, root_ptr](std::size_t index) {
    status_ptr->set_content(std::format("选中列表项 #{}", index));
    root_ptr->mark_dirty_all();
  });

  // —— 启动 ——
  app.set_content(std::move(root_panel));
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }
  status_right_ptr->set_content(std::format("{} · headless={} · DPI {:.1f} · 控制通道 127.0.0.1:{}",
                                            app.backend_name(), app.headless(),
                                            static_cast<double>(app.device_scale()),
                                            app.control_port()));
  app.root().mark_dirty_all();
  app.render_frame();

  if (options.bench > 0) return run_render_bench(app, options.bench);

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  st::print("gallery 退出：{} 帧，DPI {:.1f}，后端 {}\n", frames,
              static_cast<double>(app.device_scale()), std::string(app.backend_name()));
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
