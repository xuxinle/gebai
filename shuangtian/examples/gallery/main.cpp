/// 霜天组件画廊（gallery）——框架的"美观"验证载体与无头协同开发的主要靶场。
///
/// 用法：
///   gallery --headless [--frames 3] [--scale 2.0] [--control-port 0] [--control-file path]
///           [--theme dark] [--shots DIR] [--bench N]
/// 无头模式下不创建窗口，控制通道（TCP）承担全部交互：tree / find / get / set / invoke /
/// input.* / capture / visual / wait / metrics —— 智能体可据此开发与验证界面。
///
/// 本文件只负责**装配**（顶部栏 / 侧栏导航 / 滚动容器 / 状态栏 / 导航切页）；
/// 页面内容在 `pages.cpp`（一页一个函数）。切页用 `set_visible`——
/// 框架在 measure/arrange/paint/hit_test/semantics/visual 六处都尊重 `visible`，
/// 所以隐藏页零布局零绘制，且**组件状态不丢**（输入框里的文字、表格选中行切回来还在）。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pages.hpp"

#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/window_frame.hpp"   // 自绘外壳（标题栏 + 内容槽 + 缩放边缘）
#include "st/ui/dsl.hpp"   // 声明式页的宿主（`dsl::mount_into`）
#include "st/ui/icon.hpp"

namespace {

using st::math::Insets;
using st::ui::Align;
using st::ui::Button;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Heading;
using st::ui::IconView;
using st::ui::Justify;
using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::Tone;
using st::ui::UiRoot;

struct Options {
  bool headless{false};
  float scale{0.0f};
  std::string theme{"light"};
  /// 渲染器：auto（按实测帧耗时选更快）/ gpu / software。
  std::string renderer{"auto"};
  /// 文字抗锯齿：auto（有窗口 → LCD 亚像素；无头 → 灰度）/ on / off。
  std::string text_lcd{"auto"};
  /// 字形网格拟合：auto/off/light/normal（见 AppOptions::text_fit）。
  std::string text_fit{"auto"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::uint32_t frames{0};
  /// 渲染基准帧数（`--bench N`）：每帧**强制全量重绘**并报告分阶段分位耗时。
  std::uint32_t bench{0};
  int max_ms{0};
  /// 请求系统标题栏/边框（默认关：窗框一律自绘，见 `CONVENTIONS.md` §10 第 7 条）。
  bool decorations{false};
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
    else if (raw == "--renderer") options.renderer = next("auto");
    else if (raw == "--text-lcd") options.text_lcd = next("auto");
    else if (raw == "--text-fit") options.text_fit = next("auto");
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(next("1")));
    else if (raw == "--dpi") options.scale = static_cast<float>(std::stod(next("1")));
    else if (raw == "--theme") options.theme = next("light");
    else if (raw == "--decorations") options.decorations = true;
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(next("0")));
    else if (raw == "--control-file") options.control_file = next("");
    else if (raw == "--shots") options.shots = next("");
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(next("0")));
    else if (raw == "--bench") options.bench = static_cast<std::uint32_t>(std::stoi(next("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(next("0"));
    else if (raw == "--help" || raw == "-h") {
      st::print("用法: gallery [--headless] [--scale 2.0] [--theme dark] [--control-port 0]\n"
                "               [--control-file PATH] [--shots DIR] [--frames N] [--bench N] [--ms N]\n"
                "               [--renderer auto|gpu|software] [--text-lcd auto|on|off] [--text-fit auto|off|light|normal]\n");
      std::exit(0);
    }
  }
  return options;
}

/// 导航项：图标 + 文字，左对齐（选中态由切页逻辑切 variant）。
[[nodiscard]] auto make_nav_item(std::string_view icon, const std::string& label)
    -> std::unique_ptr<Button> {
  auto button =
      std::make_unique<Button>(label, Button::Variant::Ghost, Button::Size::Medium);
  button->set_icon(std::string(icon));
  button->style().justify = Justify::Start;
  button->style().text_align = TextAlign::Start;
  return button;
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
  // 绘制分解（需 `ST_PAINT_PROFILE=1`）：只看"绘制 30ms"不知道该改哪里
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
  // 渲染器交给应用选：`auto` 会**实测**两条路径再定（见 shell/backend.cpp 的 create_surface）
  app_options.renderer = options.renderer;
  app_options.text_lcd = options.text_lcd;
  app_options.text_fit = options.text_fit;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;

  st::app::Application app("gallery", "0.1.0", app_options);

  auto* app_ptr = &app;
  auto* root_ptr = &app.root();

  // —— 根布局 ——
  //
  // 外壳用 `WindowFrame`（**组件化的窗口**：标题栏 + 内容槽 + 八向缩放边缘）。
  // 为什么不再手搭一条"顶部栏"：
  //   ① 窗框是**框架**该提供的东西（`CONVENTIONS.md` §10 第 7 条：装饰一律自绘）；
  //   ② 无边框窗口**只能**靠窗框提供拖动/缩放区域——手搭 shell 时窗口拖不动；
  //   ③ 主题/DPI/截图这些控件本来就长在标题栏那一行（VSCode / 浏览器同构），
  //      挂进标题栏的尾部槽即可，不必再叠一条 60px 的横条。
  auto frame = std::make_unique<st::ui::WindowFrame>("概览");
  frame->set_id("app-root");                       // 兼容钩子：原来的根 id 保持在根元素上
  auto root_panel = std::make_unique<Panel>(FlexDirection::Column);   // 内容槽内的主体
  root_panel->set_id("content-root");
  // 内容主体**铺满内容槽**：不声明 grow 时它按自身测量高排（导航 + 页 + 状态栏），
  // 而窗框的内容区是"扣掉标题栏之后的那一块"，主体本就该占满它。
  root_panel->style().grow = true;
  st::ui::WindowFrame* frame_ptr = frame.get();
  if (frame_ptr->title_bar() != nullptr) {
    frame_ptr->title_bar()->set_id("titlebar");
    frame_ptr->title_bar()->set_icon("sparkles");
    frame_ptr->title_bar()->set_window_control(&app);
  }
  // 品牌：挂标题栏**前部槽**（图标之后、标题之前）——与真实应用同一形态：
  //   `[图标][霜天 · 组件画廊][……][主题 DPI 截图 | — □ ×]`
  // 控制通道兼容钩子 `#app-title` 就落在这块品牌标题上。
  // 而标题栏自身的标题文本留作"当前位置"（切页时更新，与 VSCode 显示当前文件名同构）。
  auto brand = std::make_unique<Heading>("霜天 · 组件画廊", 3);
  brand->set_id("app-title");
  brand->style().font_size = 14.0f;
  if (frame_ptr->title_bar() != nullptr) frame_ptr->title_bar()->add_leading(std::move(brand));

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

  // 三个按钮进标题栏尾部槽（顺序：主题 · DPI · 截图，与控制按钮同排不重叠）。
  if (frame_ptr->title_bar() != nullptr) {
    frame_ptr->title_bar()->add_trailing(std::move(theme_button));
    frame_ptr->title_bar()->add_trailing(std::move(scale_button));
    frame_ptr->title_bar()->add_trailing(std::move(shot_button));
  }

  // —— 内容槽：主体（导航 + 页 + 状态栏）——
  // 子节点声明进**内容区**：`WindowFrame::add_child` 已按此语义转发，
  // 所以这里照旧写 `root_panel->add_child(...)`，但最后把 root_panel 挂到窗框内容槽。

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

  // 导航项与页面一一对应（清单来自 `page_specs()`，不在两处各写一遍）
  const auto& specs = gallery::page_specs();
  std::array<Button*, gallery::kPageCount> nav_ptrs{};
  for (std::size_t index = 0; index < gallery::kPageCount; ++index) {
    auto item = make_nav_item(specs[index].icon, std::string(specs[index].label));
    item->set_id(std::format("nav-{}", specs[index].id));
    nav_ptrs[index] = item.get();
    sidebar->add_child(std::move(item));
  }
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
  auto* scroll_ptr = scroll.get();

  // 页面容器：5 个页面都在里面，用 `visible` 切换（隐藏页零布局零绘制）
  auto pages_host = std::make_unique<Panel>(FlexDirection::Column);
  pages_host->set_id("pages");
  std::array<Element*, gallery::kPageCount> page_ptrs{};

  // 运行时字段：页面注册"这里要实时值"，应用在刷新时填（两边互不知道细节）
  std::vector<std::pair<std::string, std::function<void(std::string)>>> runtime_fields;

  gallery::PageHooks hooks;
  // ⚠ 状态栏回调必须**先接上再建页**：页面按值捕获 PageHooks（拷贝），
  // 后接的 set_status 对已建页面不可见——旧序（建页后才接）会让所有页面的
  // set_status 全部落到初始空操作上，状态栏永远不变（实测踩到）。
  auto status_text_early = std::make_unique<Text>("就绪");
  status_text_early->set_id("status-text");
  status_text_early->set_tone(Tone::Muted);
  status_text_early->set_font_size(12.0f);
  auto* status_ptr = status_text_early.get();
  hooks.set_status = [status_ptr](std::string text) { status_ptr->set_content(std::move(text)); };
  hooks.add_overlay = [root_ptr](std::unique_ptr<Element> overlay) {
    // FillViewport：模态遮罩/宽通知的标准形态（组件在 arrange 里自定位卡片）
    root_ptr->add_overlay(std::move(overlay), UiRoot::OverlayLayout::FillViewport);
  };
  hooks.remove_overlay = [root_ptr](Element* overlay) { root_ptr->remove_overlay(overlay); };
  // 窗框的窗口动作出口（页面把它注入 `ui::TitleBar`）。
  // ⚠ 同样必须在**建页之前**接上：页面按值捕获 `PageHooks`（与 `set_status` 同一个坑）。
  hooks.window_control = &app;
  hooks.register_runtime_field =
      [&runtime_fields](std::string_view field, std::function<void(std::string)> setter) {
        runtime_fields.emplace_back(std::string(field), std::move(setter));
      };

  for (std::size_t index = 0; index < gallery::kPageCount; ++index) {
    auto page = gallery::build_page(index, hooks);
    page->set_visible(index == 0);  // 默认第一页
    page_ptrs[index] = page.get();
    pages_host->add_child(std::move(page));
  }
  scroll->add_child(std::move(pages_host));
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
  // status_text 在建页前已创建（hooks.set_status 需要先拿到指针，见上方注释）
  status_bar->add_child(std::move(status_text_early));
  auto status_spacer = std::make_unique<Panel>(FlexDirection::Row);
  status_spacer->style().grow = true;
  status_bar->add_child(std::move(status_spacer));
  auto refresh_button =
      std::make_unique<Button>("刷新指标", Button::Variant::Ghost, Button::Size::Small);
  refresh_button->set_id("btn-refresh");
  refresh_button->set_icon("activity");
  auto* refresh_ptr = refresh_button.get();
  status_bar->add_child(std::move(refresh_button));
  auto status_right = std::make_unique<Text>("等待控制通道…");
  status_right->set_id("status-right");
  status_right->set_tone(Tone::Faint);
  status_right->set_font_size(12.0f);
  auto* status_right_ptr = status_right.get();
  status_bar->add_child(std::move(status_right));
  root_panel->add_child(std::move(status_bar));

  // —— 交互 ——（set_status 已在建页前接上；此处不再重复赋值）

  // 运行时字段刷新：从 `metrics` 与 `Application` 回读——界面自己展示自己的可观测性
  const auto refresh_runtime = [app_ptr, status_right_ptr, &runtime_fields]() {
    const st::control::Metrics metrics = app_ptr->metrics();
    for (auto& [field, setter] : runtime_fields) {
      if (field == "backend") setter(std::string(app_ptr->backend_name()));
      else if (field == "renderer") setter(metrics.renderer);
      else if (field == "stat_renderer") {
        // 卡片位置短：写"GPU (D3D11)"这类短名，长描述留在 renderer_note（控制通道可读）
        setter(metrics.renderer == "gpu" ? "GPU (D3D11)" : "软件光栅器");
      } else if (field == "stat_dpi") {
        setter(std::format("{:.1f}x", static_cast<double>(app_ptr->device_scale())));
      }
      else if (field == "dpi") setter(std::format("{:.1f}x", static_cast<double>(app_ptr->device_scale())));
      else if (field == "frames") setter(std::format("{}", metrics.frames));
      else if (field == "port") setter(std::format("{}", app_ptr->control_port()));
      else if (field == "stat_nodes") {
        // 语义树节点数（与控制通道 tree 同源）：切页/主题/DPI 变化后都会不同
        setter(std::format("{}", metrics.nodes));
      } else if (field == "stat_port") {
        setter(std::format("127.0.0.1:{}", app_ptr->control_port()));
      }
    }
    status_right_ptr->set_content(std::format("{} · DPI {:.1f} · {}x{} · 第 {} 帧",
                                              app_ptr->backend_name(),
                                              static_cast<double>(app_ptr->device_scale()),
                                              metrics.physical_width, metrics.physical_height,
                                              metrics.frames));
  };
  refresh_ptr->on_click = [app_ptr, refresh_runtime]() {
    refresh_runtime();
    app_ptr->root().mark_dirty_all();
  };

  // 切页：visible + 导航高亮 + 滚动复位 + 清焦点
  const auto show_page = [&](std::size_t index) {
    for (std::size_t each = 0; each < gallery::kPageCount; ++each) {
      page_ptrs[each]->set_visible(each == index);
      nav_ptrs[each]->set_variant(each == index ? Button::Variant::Soft : Button::Variant::Ghost);
    }
    // 窗框标题跟随当前页：与编辑器把当前文件名写进标题栏同构
    // （标题栏不只是装饰——它回答"我现在在哪"）。
    if (frame_ptr != nullptr && frame_ptr->title_bar() != nullptr) {
      frame_ptr->title_bar()->set_title(std::string(specs[index].label));
    }
    // 滚动复位：否则切到更短的页面会停在"上一页的中段"，看起来像内容缺失
    scroll_ptr->scroll_to(0.0f);
    // 清焦点：焦点元素若随页面被隐藏，键盘事件仍会送到它（"看不见的输入框在收字"）
    root_ptr->set_focus(nullptr);
    status_ptr->set_content(std::format("已切到「{}」", specs[index].label));
    root_ptr->mark_dirty_all();
  };
  for (std::size_t index = 0; index < gallery::kPageCount; ++index) {
    nav_ptrs[index]->on_click = [show_page, index]() { show_page(index); };
  }
  // 初始页的导航项也要**高亮**。
  //
  // `show_page` 只在点击时调用，而初始页是由 `page->set_visible(index == 0)` 定的——
  // 于是启动时侧栏 5 项全是 Ghost，**完全看不出当前在哪一页**（实测现象）。
  // 这里只设高亮、不改状态栏文案：启动时"就绪"比"已切到「概览」"更合适。
  nav_ptrs[0]->set_variant(Button::Variant::Soft);

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

  // —— 启动 ——
  //
  // 窗框接管根内容（**单根**）：`WindowFrame` 自带标题栏与内容槽，
  // 主体（`root_panel`）进内容槽。窗口动作端口已在上面注入（`set_window_control(&app)`），
  // 三个控制按钮与八向边缘条因此都接了真实后端。
  frame->content()->add_child(std::move(root_panel));
  app.set_content(std::move(frame));
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }

  // —— 声明式页：把声明式子树挂到该页的宿主容器下 ——
  //
  // `dsl::mount()` 是**单根语义**（会替掉 `UiRoot::content()`），gallery 是手搭壳，
  // 所以用 `mount_into()`：声明式树只占 `#declarative-host` 那一块。
  // 主循环每帧调一次 `decl_host->tick()`——帧首推进（先泵异步结果，再重组有失效的作用域）。
  // 时机在 `set_content` 之后：锚点得先上树，才拿得到稳定地址。
  st::ui::dsl::DeclarativeHost* decl_ptr = nullptr;
  std::unique_ptr<st::ui::dsl::DeclarativeHost> decl_host{};
  if (Element* decl_anchor = app.root().find("declarative-host"); decl_anchor != nullptr) {
    decl_host = st::ui::dsl::mount_into(app.root(), *decl_anchor,
                                        gallery::make_declarative_component());
    if (decl_host == nullptr) {
      st::print("声明式页挂载失败（该页将只显示壳）\n");
    } else {
      decl_ptr = decl_host.get();
    }
  }
  status_right_ptr->set_content(std::format("{} · headless={} · DPI {:.1f} · 控制通道 127.0.0.1:{}",
                                            app.backend_name(), app.headless(),
                                            static_cast<double>(app.device_scale()),
                                            app.control_port()));
  refresh_runtime();
  app.root().mark_dirty_all();
  app.render_frame();

  if (options.bench > 0) return run_render_bench(app, options.bench);

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    // 声明式页：帧首推进。
    //
    // 先 `pump_async()` 再判脏——**顺序不能反**：异步结果到达时状态还没写
    // （scope 不脏），只在 dirty 时才调 tick 的话 `pump_async` 永远不会被调到，
    // 任务结果就永远停在“计算中…”（实测踩到）。先泵再判，同帧就能落地。
    if (decl_ptr != nullptr) {
      (void)decl_ptr->pump_async();
      if (decl_ptr->dirty()) {
        (void)decl_ptr->tick();
        app.request_repaint();
      }
    }
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    // 节拍交给框架：有活 → 帧预算；空闲 → 4ms（控制通道响应节拍）。
    // 旧实现固定 `sleep_for(16ms)`：命令延迟被拉到 16~31ms（实测 ping p50=31ms）。
    app.pace_loop(frame_start_ms);
  }
  st::print("gallery 退出：{} 帧，DPI {:.1f}，后端 {}\n", frames,
            static_cast<double>(app.device_scale()), std::string(app.backend_name()));
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
