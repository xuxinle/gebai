/// 镂月（Lunaris）— 图像编辑器示例。
///
/// ## 这个示例检验什么（设计文档 §1）
///
/// **像素 / 光栅创作面**——gallery 是组件能力面、gbcode 是文本编辑器应用面，
/// 而镂月是唯一以**自绘画布**为主表面的示例。它把光栅管线拉到真实用户路径上：
/// `Canvas`/`Paint`/`BlendMode`/`Surface` 合成、逐像素操作、PNG 编解码**往返**、
/// 撤销栈、九种混合模式的肉眼与像素双重检验。
///
/// ## 壳是声明式的，画布走逃生舱
///
/// 菜单/工具栏/图层面板/状态栏都用 `dsl` 描述；画布这类高频自绘走
/// `dsl::custom<T>`（与 gbcode 持有 `CodeEditor` 同构）。见设计文档 §9。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "canvas_view.hpp"
#include "document.hpp"
#include "battery/embed.hpp"   // 编译期资源嵌入（stpm 生成）
#include "st/app/app.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"   // `ST_MAIN`（跨平台入口：正规化 argv 编码）
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/dsl.hpp"

namespace {

using louyue::CanvasView;
using louyue::Document;
using louyue::Swatch;
using louyue::Tool;
using st::math::Color;
using st::ui::Button;
namespace dsl = st::ui::dsl;

/// 命令行选项（承 gallery/gbcode 的形态：只解析本示例关心的几个）。
struct Options {
  bool headless{false};
  std::string theme{"light"};
  std::string control_port{"0"};
  std::string control_file{};
  std::string shots{};
  std::string open{};   ///< `--open <png>`：启动即打开一个真实 PNG 文件
};

auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string raw = argv[i];
    const auto value = [&](const char* fallback) -> std::string {
      if (i + 1 < argc) return argv[++i];
      return fallback;
    };
    if (raw == "--headless") options.headless = true;
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--control-port") options.control_port = value("0");
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--shots") options.shots = value({});
    else if (raw == "--open") options.open = value({});
  }
  return options;
}

/// 主页面：整个编辑器是一个 `Component`。
///
/// ⚠ 生命周期契约（`docs/declarative.md` §5.5）：`build()` 会被**反复调用**
/// （任何 State 失效都触发重组），所以里面只做「声明」；`document_` 与
/// `canvas_` 是**跨帧存活**的真值源，不进声明式树。
class LouyuePage final : public dsl::Component {
 public:
  LouyuePage() : document_(std::make_unique<Document>(320, 200)) {
    // 启动即载入内嵌样例：PNG **解码**第一次跑在真实用户路径上（设计文档 §3.3）。
    //
    // 资源经 `st.pkg` 的 `embed` 声明、由 stpm 在编译期烤进二进制
    //（`battery/embed.hpp`）——内嵌路径相对**工程根**，与 gbcode/gallery 同口径。
    //
    // ⚠ 嵌入字节是 `char` 数组（`std::span<const char>`），而 `png_decode` 收
    // `std::span<const std::uint8_t>`——用 `std::transform` 拷一份，
    // **不用 `reinterpret_cast`**（lint L6 禁止；且 char→uint8_t 的别名转换
    // 在“char 为有符号”的平台上语义上就不干净）。样例图只有几 KB，开销可忽略。
    const auto raw = b::embed<"examples/louyue/assets/sample.png">();
    // ⚠ `EmbeddedFile` 不是容器：用 `data()` / `length()`（与 gallery 同口径）。
    const std::string_view raw_view(raw.data(), raw.length());
    st::codec::Bytes png;
    png.resize(raw_view.size());
    std::transform(raw_view.begin(), raw_view.end(), png.begin(), [](char raw_byte) {
      return static_cast<std::uint8_t>(static_cast<unsigned char>(raw_byte));
    });
    if (const auto image = st::codec::png_decode(png); image.has_value()) {
      (void)document_->load_png(*image);
      document_->set_file_path("sample.png");
    }
  }

  [[nodiscard]] auto document() noexcept -> Document& { return *document_; }
  [[nodiscard]] auto canvas() noexcept -> CanvasView* { return canvas_; }

  /// 载入一个真实 PNG 文件（`--open` 与菜单「打开」共用）。
  auto open_file(const std::string& path) -> bool {
    const auto image = st::codec::png_read_file(path);
    if (!image.has_value()) return false;
    if (!document_->load_png(*image)) return false;
    document_->set_file_path(path);
    canvas_version_.set(canvas_version_.value() + 1U);
    return true;
  }

  void build(dsl::Composer& c) override {
    (void)layers_version_.value();
    (void)canvas_version_.value();
    dsl::column(c, {.gap = 0.0f, .grow = true, .id = "louyue-root"}, [&] {
      dsl::row(c, {.gap = 0.0f, .grow = true, .id = "main-row"}, [&] {
        build_toolbar(c);
        build_viewport(c);
        build_side_panels(c);
      });
      build_status_bar(c);
    });
  }

  // —— 供 e2e 的动作面（设计文档 §3.4，**不可改名**）——

  auto invoke_export(const std::string& path) -> bool { return document_->export_png(path); }
  auto invoke_undo() -> bool { return document_->undo(); }
  auto invoke_redo() -> bool { return document_->redo(); }
  auto add_layer() -> bool {
    if (document_->layers().size() >= 16U) return false;   // 上限护栏：图层面板放不下更多
    document_->add_layer(std::format("图层 {}", document_->layers().size() + 1U));
    invalidate_all();
    return true;
  }
  auto remove_layer(std::size_t index) -> bool {
    const bool removed = document_->remove_layer(index);
    if (removed) invalidate_all();
    return removed;
  }
  auto set_blend(std::size_t index, int mode) -> bool {
    if (index >= document_->layers().size()) return false;
    if (mode < 0 || mode >= static_cast<int>(louyue::kBlendModeCount)) return false;
    document_->layers()[index].blend = louyue::kBlendModes[mode];
    invalidate_all();
    return true;
  }
  [[nodiscard]] auto layer_summary() const -> std::string {
    std::string out;
    for (const auto& layer : document_->layers()) {
      out += std::format("{}|{}|{:.2f}|{}\n", layer.name,
                         st::raster::to_string(layer.blend), static_cast<double>(layer.opacity),
                         layer.visible ? "1" : "0");
    }
    return out;
  }
  [[nodiscard]] auto history_summary() const -> std::string {
    std::string out;
    for (const auto& name : document_->history_names()) out += name + "\n";
    return out;
  }

 private:
  void invalidate_all() {
    canvas_version_.set(canvas_version_.value() + 1U);
    layers_version_.set(layers_version_.value() + 1U);
    if (canvas_ != nullptr) canvas_->mark_dirty();
  }

  void build_toolbar(dsl::Composer& c) {
    dsl::column(c, {.gap = 2.0f, .padding_y = 4.0f, .width = 44.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "toolbar"}, [&] {
      const Tool tools[] = {Tool::Move,   Tool::Brush, Tool::Eraser, Tool::Bucket,
                            Tool::Eyedropper, Tool::Select, Tool::Crop};
      for (const Tool tool : tools) {
        const std::string id = std::format("tool-{}", louyue::tool_icon(tool));
        const bool active = canvas_ != nullptr && canvas_->tool() == tool;
        (void)dsl::custom<Button>(
            c,
            [this, tool, id, active](Button& b) {
              b.set_id(id);
              b.set_icon(louyue::tool_icon(tool));
              b.set_variant(active ? Button::Variant::Soft : Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              b.on_click = [this, tool] {
                if (canvas_ != nullptr) canvas_->set_tool(tool);
                invalidate_all();
              };
            },
            {.width = 40.0f, .height = 36.0f, .key = id});
      }
    });
  }

  void build_viewport(dsl::Composer& c) {
    // `custom<T>`：高频自绘不进重组树（状态变化走 `mark_dirty`）。
    (void)dsl::custom<CanvasView>(
        c, [this](CanvasView& view) { bind_canvas(view); },
        {.grow = true, .id = "canvas", .key = "canvas"});
  }

  void bind_canvas(CanvasView& view) {
    view.set_id("canvas");
    view.set_document(document_.get());
    view.on_document_changed = [this] { invalidate_all(); };
    canvas_ = &view;
  }

  void build_side_panels(dsl::Composer& c) {
    dsl::column(c, {.gap = 8.0f, .padding = 8.0f, .width = 260.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "side-panels"}, [&] {
      build_layer_panel(c);
      build_color_panel(c);
    });
  }

  void build_layer_panel(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .id = "layers-panel"}, [&] {
      dsl::text(c, [] { return std::string("图层"); }, {.id = "layers-title"});
    // 图层列表：**自上而下显示**（面板习惯），而模型是自底向上——此处反转。
    const auto& layers = document_->layers();
    std::vector<dsl::ListItemData> items;
    items.reserve(layers.size());
    for (std::size_t i = layers.size(); i-- > 0;) {
      items.push_back(dsl::ListItemData{
          .key = std::format("layer-{}", i),
          .label = std::format("{} · {}", layers[i].name,
                               st::raster::to_string(layers[i].blend)),
          .subtitle = std::format("{}%", static_cast<int>(layers[i].opacity * 100.0f)),
      });
    }
    (void)dsl::list(
        c, items,
        [this, &layers](std::size_t index) {
          // 显示序是反的：第 `index` 项对应模型里的 `size-1-index`。
          const std::size_t model = layers.size() - 1U - index;
          document_->set_active_layer(model);
          invalidate_all();
        },
        {.id = "layer-list", .key = std::format("layers-{}", layers_version_.value())});

    // 不透明度：调活动图层。
    const float opacity =
        document_->has_layer() ? document_->layers()[document_->active_layer()].opacity : 1.0f;
    (void)dsl::slider(
        c, opacity,
        [this](float v) {
          if (!document_->has_layer()) return;
          document_->layers()[document_->active_layer()].opacity = std::clamp(v, 0.0f, 1.0f);
          canvas_version_.set(canvas_version_.value() + 1U);
          if (canvas_ != nullptr) canvas_->mark_dirty();
        },
        {.id = "opacity-slider", .key = "opacity"});

    dsl::row(c, {.gap = 6.0f, .id = "layer-actions"}, [&] {
      (void)dsl::custom<Button>(
          c,
          [this](Button& b) {
            b.set_id("layer-add");
            b.set_icon("plus");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] { (void)add_layer(); };
          },
          {.width = 32.0f, .height = 28.0f, .key = "layer-add"});
      (void)dsl::custom<Button>(
          c,
          [this](Button& b) {
            b.set_id("layer-remove");
            b.set_icon("minus");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] {
              (void)remove_layer(document_->active_layer());
            };
          },
          {.width = 32.0f, .height = 28.0f, .key = "layer-remove"});
      (void)dsl::custom<Button>(
          c,
          [this](Button& b) {
            b.set_id("layer-merge");
            b.set_icon("layers");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] {
              (void)document_->merge_down(document_->active_layer());
              invalidate_all();
            };
          },
          {.width = 32.0f, .height = 28.0f, .key = "layer-merge"});
      });
    });
  }

  void build_color_panel(dsl::Composer& c) {
    dsl::text(c, [] { return std::string("颜色"); }, {.id = "color-title"});
    const Color color = canvas_ != nullptr ? canvas_->foreground() : Color{0, 0, 0, 255};
    dsl::row(c, {.gap = 8.0f, .id = "swatch-row"}, [&] {
      // 色块：**自绘元素**（`Swatch`）——不用 `Panel` + `style().background`，
      // 理由见 `Swatch` 定义处（`apply_theme` 每帧覆盖调用方写的背景色）。
      (void)dsl::custom<Swatch>(c, [color](Swatch& swatch) { swatch.set_color(color); },
                                {.id = "swatch"});
      dsl::text(c,
                [color] { return std::format("#{:02X}{:02X}{:02X}", color.r, color.g, color.b); },
                {.id = "hex-label", .key = "hex"});
    });
    build_hsv_slider(c, "hue", "色相", hue_);
    build_hsv_slider(c, "saturation", "饱和度", saturation_);
    build_hsv_slider(c, "value", "明度", value_);
  }

  void build_hsv_slider(dsl::Composer& c, std::string_view id, std::string_view label,
                        dsl::State<float>& value) {
    dsl::row(c, {.gap = 8.0f, .id = std::format("{}-row", id)}, [&] {
      dsl::text(c, [label] { return std::string(label); }, {.width = 52.0f});
      (void)dsl::slider(
          c, value.value(),
          [this, id](float v) {
            if (id == "hue") {
              hue_.set(v);
            } else if (id == "saturation") {
              saturation_.set(v);
            } else {
              value_.set(v);
            }
            apply_hsv();
          },
          {.grow = true, .id = std::format("{}-slider", id), .key = "hsv"});
    });
  }

  /// HSV → RGB 并推给画布（前景色）。
  void apply_hsv() {
    const float h = hue_.value() / 360.0f;
    const float s = saturation_.value();
    const float v = value_.value();
    const float sector = h * 6.0f;
    const int i = static_cast<int>(std::floor(sector));
    const float f = sector - static_cast<float>(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    switch (i % 6) {
      case 0: r = v; g = t; b = p; break;
      case 1: r = q; g = v; b = p; break;
      case 2: r = p; g = v; b = t; break;
      case 3: r = p; g = q; b = v; break;
      case 4: r = t; g = p; b = v; break;
      default: r = v; g = p; b = q; break;
    }
    const auto channel = [](float value) {
      return static_cast<std::uint8_t>(std::clamp(value * 255.0f, 0.0f, 255.0f));
    };
    if (canvas_ != nullptr) {
      canvas_->set_foreground(Color{channel(r), channel(g), channel(b), 255});
    }
    canvas_version_.set(canvas_version_.value() + 1U);
  }

  void build_status_bar(dsl::Composer& c) {
    dsl::row(c, {.gap = 16.0f, .padding_x = 10.0f, .height = 26.0f,
                 .surface = st::ui::Element::Surface::Alt, .id = "status"}, [&] {
      dsl::text(
          c,
          [this] {
            if (canvas_ == nullptr) return std::string("—");
            const auto pos = canvas_->cursor_image_pos();
            return std::format("{},{}", static_cast<int>(pos.x), static_cast<int>(pos.y));
          },
          {.id = "status-coords", .key = "coords"});
      dsl::text(
          c, [this] { return std::format("{}×{}", document_->width(), document_->height()); },
          {.id = "status-size", .key = "size"});
      dsl::text(
          c,
          [this] {
            const float zoom = canvas_ != nullptr ? canvas_->zoom() : 1.0f;
            return std::format("{:.0f}%", static_cast<double>(zoom) * 100.0);
          },
          {.id = "status-zoom", .key = "zoom"});
      dsl::text(
          c,
          [this] {
            return std::string(canvas_ != nullptr ? louyue::tool_name(canvas_->tool()) : "-");
          },
          {.id = "status-tool", .key = "tool"});
      dsl::text(
          c,
          [this] {
            return std::format("{} 步历史", document_->history_size());
          },
          {.id = "status-history", .key = "history"});
    });
  }

  std::unique_ptr<Document> document_{};
  CanvasView* canvas_{nullptr};
  dsl::State<std::size_t> canvas_version_{0};
  dsl::State<std::size_t> layers_version_{0};
  dsl::State<float> hue_{210.0f};
  dsl::State<float> saturation_{0.75f};
  dsl::State<float> value_{0.85f};
};

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  st::app::AppOptions app_options;
  app_options.width = 1180;
  app_options.height = 760;
  app_options.title = "镂月 Lunaris · 霜天";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = static_cast<std::uint16_t>(std::stoi(options.control_port));
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;

  st::app::Application app("louyue", "0.1.0", app_options);
  // **自绘组件必须先注册**（在 `custom<T>` 用它之前，即建界面之前）。
  // `CanvasView` 是示例级类型，不进框架组件表（那会让框架的公开组件面
  // 被具体示例的需求撑大）；`register_element_type` 就是为此留的口子。
  if (!dsl::register_element_type<CanvasView>("CanvasView")) {
    std::fprintf(stderr, "CanvasView 注册失败（与内置组件重名？）\n");
    return 1;
  }
  if (!dsl::register_element_type<Swatch>("Swatch")) {
    std::fprintf(stderr, "Swatch 注册失败（与内置组件重名？）\n");
    return 1;
  }
  auto page = std::make_shared<LouyuePage>();
  if (!options.open.empty()) {
    if (!page->open_file(options.open)) {
      std::fprintf(stderr, "无法打开 %s\n", options.open.c_str());
    }
  }

  // 外壳：`WindowFrame`（标题栏 + 内容槽 + 八向缩放边缘），与其他示例同形态。
  auto frame = std::make_unique<st::ui::WindowFrame>("镂月 Lunaris · 霜天");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("brush");
  }
  st::ui::Element* slot = frame->content();
  app.set_content(std::move(frame));
  auto host = dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  // **重组错误必须可见**（承 gbcode 的做法）：单根契约被破坏、护栏报表或
  // 预算耗尽时界面会静默地少一块——那时看截图只能看到“一片空白”。
  {
    const dsl::ReconcileStats& stats = host->stats();
    if (!stats.error.empty()) {
      std::fprintf(stderr, "[louyue] 声明式重组错误：%s\n", stats.error.c_str());
    }
    for (const auto& collision : stats.key_collisions) {
      std::fprintf(stderr, "[louyue] 重名 key：%s\n", collision.c_str());
    }
  }

  // 首帧重组**必须在 `start()` 之前**：`start()` 内会开控制通道，
  // 客户端一看到控制文件就连——那时界面还没建好就会读到半成品状态。
  (void)host->tick();

  // ⚠ 控制通道是在 `Application::start()` 里起的（不在 `tick()` 里）。
  // 自定义主循环（下面是 gallery 同款）必须**显式调 `start()`**——
  // 漏了它的表现是“进程在跑、界面也在画，但控制文件永远不会出现”，
  // 从外部看就是“启动超时”（实测踩到）。
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // 主循环形态**承 gallery**：需要每帧显式推进声明式宿主
  // （`tick_declarative_hosts`）——`DeclarativeHost` 登记的帧回调里已经内建
  // “先 pump_async 再判 dirty”的顺序，调用方不再重写（见 gallery 同段注释）。
  std::uint64_t frames = 0;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
    ++frames;
    app.pace_loop(frame_start_ms);
  }
  st::print("镂月退出：{} 帧，DPI {:.1f}，后端 {}\n", frames,
            static_cast<double>(app.device_scale()), std::string(app.backend_name()));
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页。
ST_MAIN(run_app)
