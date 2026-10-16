/// 裁云（Nimbus）— 视频剪辑器示例。
///
/// ## 这个示例检验什么（设计文档 §1）
///
/// **时间 / 动效应用面**——它是**唯一由 `UiRoot` 时间轴驱动持续动画的示例**。
/// 检验的是别处检验不到的东西：
///
/// - 播放中的**帧预算与续帧协议**（§11 性能目标的真实压测位）；
/// - **确定性**：`seek` 到同一时刻两次渲染逐像素相同——这是"预览真的跟着
///   播放头走"与"预览是静帧"的唯一可靠区分（截图肉眼看不出静帧）；
/// - 拖拽修剪这类**连续交互**（SplitView 拖手柄之外的又一类）。
///
/// ## 诚实边界
///
/// 框架已内置 `st::ext::VideoDecoder`（v0.1.7，ffmpeg 子进程：抽帧 + 读元信息，
/// 探测不到如实不支持）；但它的定位是**素材查看/预处理**，不是逐帧播放
///（每抽一帧起一个进程 ~30ms，见 st/ext/video.hpp 的代价表）。本示例的"素材"
/// 仍全部是**程序化生成片段**（本地光栅原语实时渲染）——真实视频素材导入
/// 需要缩略图/缓存策略配合，作为后续大单记在 BACKLOG；不假占位、不冒充能播。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "project.hpp"
#include "views.hpp"
#include "st/app/app.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/dsl.hpp"

namespace {

using caiyun::Clip;
using caiyun::format_timecode;
using caiyun::GeneratorKind;
using caiyun::kGenerators;
using caiyun::MonitorView;
using caiyun::Project;
using caiyun::TimelineView;
using st::ui::Button;
namespace dsl = st::ui::dsl;

struct Options {
  bool headless{false};
  std::string backend{};
  std::string theme{"light"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::string export_dir{};   ///< `--export-dir <dir>`：启动即导出帧序列（设计文档 §4.3）
  int bench_seconds{0};       ///< `--bench <秒>`：播放压测时长（设计文档 §7.4）
};

/// 用法说明。**必须有**：未知参数会报错退出，没有 `--help` 就等于"报错了也没处查"
///（承 gbcode 同名函数）。
auto print_usage(std::string_view program) -> void {
  st::print(
      "用法: {} [选项]\n"
      "\n"
      "窗口/主题\n"
      "  --headless            无头模式（无窗口；控制通道照常可用）\n"
      "  --theme light|dark    主题\n"
      "\n"
      "内容/自动化\n"
      "  --control-port N      控制通道端口（0 = 自动）\n"
      "  --control-file PATH   控制信息落盘路径\n"
      "  --shots DIR           无头截图目录\n"
      "  --export-dir DIR      启动即导出帧序列（真实 PNG + manifest）\n"
      "  --bench N             真开播放压测 N 秒后退出\n"
      "  -h, --help            显示本帮助\n",
      program);
}

auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string raw = argv[i];
    const auto value = [&](const char* fallback) -> std::string {
      if (i + 1 < argc) return argv[++i];
      return fallback;
    };
    // —— 数值参数统一走带校验的解析（承 gbcode 的同条纪律，逐字对齐）——
    //
    // 为何不能裸调 `std::atoi`/`stoi`：传非数字（如 `--control-port abc`）时
    // `stoi` 抛 `std::invalid_argument` 无人捕获 → `terminate`（退出码
    // 0xC0000409，屏幕上一行 what(): stoi）；`atoi` 更糟——静默返 0，错值
    // 变成"端口 0（自动）"。而同一命令行里的 `--bogus-flag` 走的是
    // 「报错 + exit 2」的干净路径。**非法值必须与未知参数同一条路径**
    //（报错可读、退出码稳定），不能一快一崩。
    const auto parse_int = [&](const char* flag, long long fallback) -> long long {
      const std::string text = value(std::format("{}", fallback).c_str());
      long long parsed = 0;
      std::size_t consumed = 0;
      const bool ok = !text.empty() && [&] {
        try {
          parsed = std::stoll(text, &consumed);
        } catch (const std::exception&) {
          return false;
        }
        return consumed == text.size();   // 尾巴（如 `8x`）也是非法值
      }();
      if (!ok) {
        st::eprint("参数 {} 需要一个整数，得到：{}（用 --help 看可用项）\n", flag, text);
        std::exit(2);
      }
      return parsed;
    };
    if (raw == "--help" || raw == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    // ⚠ headless 必须与下方 else-if 链同体（首个分支）：独立 if 命中后继续
    // 落进链尾 else 兜底会被误判「未知参数」。
    if (raw == "--headless") {
      options.headless = true;
    }
    else if (raw == "--backend") options.backend = value("");
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--control-port") {
      const long long port = parse_int("--control-port", 0);
      if (port < 0 || port > 65535) {
        st::eprint("参数 --control-port 超出端口范围：{}\n", port);
        std::exit(2);
      }
      options.control_port = static_cast<std::uint16_t>(port);
    }
    else if (raw == "--control-file") options.control_file = value("");
    else if (raw == "--shots") options.shots = value("");
    else if (raw == "--export-dir") options.export_dir = value("");
    else if (raw == "--bench") {
      const long long seconds = parse_int("--bench", 0);
      if (seconds < 0) {
        st::eprint("参数 --bench 不能为负：{}\n", seconds);
        std::exit(2);
      }
      options.bench_seconds = static_cast<int>(seconds);
    }
    // **未知参数报错，不静默忽略**（gbcode 实证过的温床：新加的开关被静默
    // 吃掉，表现是"三档渲染出来的图几乎一样"，差点被当成结论）。
    else {
      st::eprint("未知参数：{}（用 --help 看可用项）\n", raw);
      std::exit(2);
    }
  }
  return options;
}

class CaiyunPage final : public dsl::Component {
 public:
  CaiyunPage() {
    project_.duration = 10.0;
    // 预置一条**真实可用**的时间轴：四类生成器各一段，分布在两条轨道上。
    // 不做"空轨道 + 占位块"——设计文档 §3.3 的标准（内容都是真的）。
    const int a = project_.add_clip(GeneratorKind::TestChart, 0, 0.0, 3.0);
    (void)a;
    (void)project_.add_clip(GeneratorKind::ColorBars, 0, 3.0, 6.5);
    (void)project_.add_clip(GeneratorKind::Shapes, 0, 6.5, 10.0);
    (void)project_.add_clip(GeneratorKind::Subtitle, 1, 1.0, 4.0);
    (void)project_.add_clip(GeneratorKind::ColorBars, 1, 5.0, 8.0);
    project_.selected = project_.clips.empty() ? -1 : project_.clips.front().id;
    project_.playhead = 1.5;
  }

  [[nodiscard]] auto project() noexcept -> Project& { return project_; }

  /// 导出帧序列到目录（`frame_0001.png` + `manifest.json`）。
  ///
  /// ⚠ `text_port` 必须传入：不传时字幕类片段退化为几何占位块，于是
  /// **导出的帧与预览不一致**（预览有字幕、导出没有）——对剪辑器而言这是
  /// 硬缺陷（发布的片子丢字幕），而不是渲染细节。
  auto export_frames(const std::string& dir, const st::ui::TextPort* text_port) -> int {
    const int frames = static_cast<int>(std::ceil(project_.duration * project_.fps));
    st::raster::Canvas canvas{320, 180, 1.0f};
    std::string manifest = std::format(
        "{{\n  \"fps\": {:.3f},\n  \"duration\": {:.3f},\n  \"frames\": {},\n  \"clips\": [\n",
        project_.fps, project_.duration, frames);
    for (std::size_t i = 0; i < project_.clips.size(); ++i) {
      const Clip& clip = project_.clips[i];
      manifest += std::format(
          "    {{\"id\": {}, \"track\": {}, \"t_in\": {:.3f}, \"t_out\": {:.3f}, "
          "\"generator\": \"{}\"}}{}\n",
          clip.id, clip.track, clip.t_in, clip.t_out, caiyun::to_string(clip.generator),
          i + 1 < project_.clips.size() ? "," : "");
    }
    manifest += "  ]\n}\n";

    int written = 0;
    for (int index = 0; index < frames; ++index) {
      const double time = static_cast<double>(index) / project_.fps;
      project_.render_frame(canvas, time, text_port);
      st::codec::PngImage image;
      image.width = 320;
      image.height = 180;
      image.rgba.resize(320U * 180U * 4U);
      for (int y = 0; y < 180; ++y) {
        for (int x = 0; x < 320; ++x) {
          const auto pixel = canvas.pixel_at(x, y);
          const std::size_t at = (static_cast<std::size_t>(y) * 320U + x) * 4U;
          image.rgba[at + 0] = pixel.r;
          image.rgba[at + 1] = pixel.g;
          image.rgba[at + 2] = pixel.b;
          image.rgba[at + 3] = pixel.a;
        }
      }
      const std::string path = std::format("{}/frame_{:04}.png", dir, index + 1);
      if (!st::codec::png_write_file(path, image).has_value()) break;
      ++written;
    }
    (void)st::fs::write_text(std::format("{}/manifest.json", dir), manifest);
    return written;
  }

  void build(dsl::Composer& c) override {
    (void)version_.value();
    dsl::column(c, {.gap = 0.0f, .grow = true, .id = "caiyun-root"}, [&] {
      // 上：监视器 + 时间码 + 传输控制
      build_monitor(c);
      // 中：素材箱（左） + 检查器（右）
      dsl::row(c, {.gap = 8.0f, .padding_x = 8.0f, .grow = true, .id = "middle-row"}, [&] {
        build_bin(c);
        build_inspector(c);
      });
      // 下（主表面）：时间轴
      build_timeline(c);
    });
  }

 private:
  void bump() { version_.set(version_.value() + 1U); }

  void build_monitor(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .padding = 8.0f, .id = "monitor-panel"}, [&] {
      (void)dsl::custom<MonitorView>(
          c,
          [this](MonitorView& monitor) {
            monitor.set_id("monitor");
            monitor.set_project(&project_);
            monitor_ = &monitor;
          },
          {.height = 260.0f, .key = "monitor"});
      dsl::row(c, {.gap = 12.0f, .id = "transport-row"}, [&] {
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("transport-play");
              b.set_icon("play");
              b.set_variant(project_.playing ? Button::Variant::Primary : Button::Variant::Soft);
              b.on_click = [this] {
                project_.playing = !project_.playing;
                bump();
              };
            },
            {.width = 36.0f, .height = 30.0f, .key = "play"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("transport-stop");
              b.set_icon("square");
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] {
                project_.playing = false;
                project_.playhead = 0.0;
                if (monitor_ != nullptr) monitor_->refresh();
                bump();
              };
            },
            {.width = 36.0f, .height = 30.0f, .key = "stop"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("transport-step-back");
              b.set_icon("chevron-left");
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] { step_frame(-1); };
            },
            {.width = 36.0f, .height = 30.0f, .key = "step-back"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("transport-step-forward");
              b.set_icon("chevron-right");
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] { step_frame(1); };
            },
            {.width = 36.0f, .height = 30.0f, .key = "step-fwd"});
        dsl::text(
            c,
            [this] { return format_timecode(project_.playhead); },
            {.id = "timecode", .key = "timecode"});
        (void)dsl::spacer(c, 1.0f);
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("btn-split");
              b.set_icon("diff");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] { do_split(); };
            },
            {.key = "split"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("btn-ripple-delete");
              b.set_icon("trash");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] { do_ripple_delete(); };
            },
            {.key = "ripple"});
      });
    });
  }

  void build_bin(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .padding = 8.0f, .width = 220.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "bin"}, [&] {
      dsl::text(c, [] { return std::string("素材箱"); }, {.id = "bin-title"});
      const char* icons[] = {"diff", "image", "circle", "grid"};
      for (std::size_t i = 0; i < caiyun::kGeneratorCount; ++i) {
        const auto kind = kGenerators[i];
        const std::string id = std::format("bin-{}", i);
        (void)dsl::custom<Button>(
            c,
            [this, kind, id, i, &icons](Button& b) {
              b.set_id(id);
              b.set_icon(icons[i]);
              b.set_label(caiyun::to_string(kind));
              b.set_variant(Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              // 双击语义的简化：点击即"加到时间轴"（拖拽另有时序问题，
              // 而控制通道驱动拖拽在 e2e 里很难稳定；`invoke add_clip` 是权威路径）。
              b.on_click = [this, kind] {
                // 落在播放头处、默认 2 秒长；超出工程时长则往前贴。
                const double length = 2.0;
                double t_in = project_.playhead;
                if (t_in + length > project_.duration) {
                  t_in = std::max(0.0, project_.duration - length);
                }
                const int created = project_.add_clip(kind, 0, t_in, t_in + length);
                if (created >= 0) {
                  if (timeline_ != nullptr) timeline_->set_project(&project_);
                  bump();
                }
              };
            },
            {.id = id, .key = id});
      }
      (void)dsl::spacer(c, 1.0f);
      dsl::text(c,
                [this] { return std::format("{} 个片段", project_.clips.size()); },
                {.id = "bin-clip-count", .key = "clip-count"});
    });
  }

  void build_inspector(dsl::Composer& c) {
    dsl::column(c, {.gap = 8.0f, .padding = 8.0f, .width = 220.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "inspector"}, [&] {
      dsl::text(c, [] { return std::string("属性"); }, {.id = "inspector-title"});
      const Clip* clip = project_.find_clip(selected_id());
      if (clip == nullptr) {
        dsl::text(c, [] { return std::string("（未选中片段）"); }, {.id = "inspector-empty"});
        return;
      }
      dsl::text(c,
                [clip] {
                  return std::format("#{} · {}", clip->id, caiyun::to_string(clip->generator));
                },
                {.id = "inspector-clip", .key = "clip"});
      dsl::text(c,
                [clip] {
                  return std::format("入 {:.2f}s  出 {:.2f}s", clip->t_in, clip->t_out);
                },
                {.id = "inspector-range", .key = "range"});
      dsl::text(c,
                [clip] { return std::format("时长 {:.2f}s", clip->duration()); },
                {.id = "inspector-duration", .key = "duration"});
      dsl::text(c, [] { return std::string("不透明度"); }, {});
      const int id = clip->id;
      (void)dsl::slider(
          c, clip->opacity,
          [this, id](float value) {
            Clip* target = project_.find_clip(id);
            if (target == nullptr) return;
            target->opacity = std::clamp(value, 0.0f, 1.0f);
            if (monitor_ != nullptr) monitor_->refresh();
            bump();
          },
          {.id = "inspector-opacity", .key = "opacity"});
      dsl::text(c, [] { return std::string("速度"); }, {});
      (void)dsl::slider(
          c, clip->speed,
          [this, id](float value) {
            Clip* target = project_.find_clip(id);
            if (target == nullptr) return;
            target->speed = std::clamp(value, 0.25f, 4.0f);
            if (monitor_ != nullptr) monitor_->refresh();
            bump();
          },
          {.id = "inspector-speed", .key = "speed"});
    });
  }

  void build_timeline(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .padding = 8.0f, .id = "timeline-panel"}, [&] {
      dsl::row(c, {.gap = 12.0f, .id = "timeline-header"}, [&] {
        dsl::text(c, [] { return std::string("时间轴"); }, {.id = "timeline-title"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("btn-snap");
              b.set_icon("link");
              b.set_size(Button::Size::Small);
              b.set_variant(project_.snapping ? Button::Variant::Soft : Button::Variant::Ghost);
              b.set_label(project_.snapping ? "吸附 开" : "吸附 关");
              b.on_click = [this] {
                project_.snapping = !project_.snapping;
                bump();
              };
            },
            {.key = "snap"});
      });
      (void)dsl::custom<TimelineView>(
          c,
          [this](TimelineView& timeline) {
            timeline.set_id("timeline");
            timeline.set_project(&project_);
            // 两个回调都在**每一步变化**后触发，含拖拽中的每一帧——
            // 监视器必须跟着重合成，否则拖动播放头时预览是静止的（最容易被
            // 误判成"功能已实现"的那类缺陷）。
            timeline.on_seek = [this] {
              if (monitor_ != nullptr) monitor_->refresh();
              bump();
            };
            timeline.on_project_changed = [this] {
              if (monitor_ != nullptr) monitor_->refresh();
              bump();
            };
            timeline_ = &timeline;
          },
          {.height = 132.0f, .key = "timeline"});
    });
  }

  [[nodiscard]] auto selected_id() const -> int {
    if (timeline_ != nullptr && timeline_->selected_clip() >= 0) {
      return timeline_->selected_clip();
    }
    return project_.selected;
  }

  void step_frame(int delta) {
    project_.playing = false;
    const double step = 1.0 / project_.fps;
    project_.playhead =
        std::clamp(project_.playhead + static_cast<double>(delta) * step, 0.0, project_.duration);
    if (timeline_ != nullptr) timeline_->set_project(&project_);
    if (monitor_ != nullptr) monitor_->refresh();
    bump();
  }

  void do_split() {
    if (project_.split_at(project_.playhead)) {
      if (timeline_ != nullptr) timeline_->set_project(&project_);
      if (monitor_ != nullptr) monitor_->refresh();
      bump();
    }
  }

  void do_ripple_delete() {
    if (project_.ripple_delete(selected_id())) {
      if (timeline_ != nullptr) timeline_->set_project(&project_);
      if (monitor_ != nullptr) monitor_->refresh();
      bump();
    }
  }

 public:
  /// 播放推进（由主循环每帧调）——**时间轴必须由应用推进**（设计文档 §4.3）。
  /// 返回是否真的推进了（调用方据此决定要不要请求重绘）。
  auto advance(double delta_seconds) -> bool {
    if (!project_.playing) return false;
    project_.playhead += delta_seconds;
    if (project_.playhead >= project_.duration) {
      project_.playhead = project_.loop ? 0.0 : project_.duration;
      if (!project_.loop) project_.playing = false;
    }
    if (monitor_ != nullptr) monitor_->refresh();
    return true;
  }

  [[nodiscard]] auto monitor() noexcept -> MonitorView* { return monitor_; }
  [[nodiscard]] auto timeline_widget() noexcept -> TimelineView* { return timeline_; }

 private:
  Project project_{};
  MonitorView* monitor_{nullptr};
  TimelineView* timeline_{nullptr};
  dsl::State<std::size_t> version_{0};
};

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  st::app::AppOptions app_options;
  app_options.width = 1120;
  app_options.height = 780;
  app_options.title = "裁云 Nimbus · 霜天";
  app_options.headless = options.headless;
  app_options.backend = !options.backend.empty() ? options.backend
                        : (options.headless ? std::string("headless") : std::string{});
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;

  st::app::Application app("caiyun", "0.1.0", app_options);
  // **自绘组件必须先注册**（`custom<T>` 用它之前）——见 louyue 同名注释。
  // ⚠ 注册名不能用 "TimelineView"：内置组件清单已新增同名时间线组件
  //（dsl.cpp ST_COMPONENT_LIST），重名会被拒（防静默失效的正确行为），
  // 表现是 caiyun 启动即退（实测 Windows 首跑发现）。改用带前缀的名字，
  // C++ 类名不动（它是视频剪辑时间轴，与内置 TimelineView 语义无关）。
  if (!dsl::register_element_type<TimelineView>("CaiyunTimeline") ||
      !dsl::register_element_type<MonitorView>("MonitorView")) {
    std::fprintf(stderr, "自绘组件注册失败（与内置组件重名？）\n");
    return 1;
  }

  auto page = std::make_shared<CaiyunPage>();

  auto frame = std::make_unique<st::ui::WindowFrame>("裁云 Nimbus · 霜天");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("image");
  }
  st::ui::Element* slot = frame->content();
  app.set_content(std::move(frame));
  auto host = dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  (void)host->tick();

  // 启动即导出（设计文档 §4.3 的 `--export-dir`）：真实产物、无假编码器。
  // 文本端口从 root 取（与预览走**同一个**端口——否则导出的帧会丢字幕）。
  if (!options.export_dir.empty()) {
    const int written = page->export_frames(options.export_dir, app.root().text_port());
    st::print("已导出 {} 帧到 {}\n", written, options.export_dir);
  }

  // `--bench`：**真开播放**再压测。为什么必须真播放：不播放时监视器不会每帧
  // 重合成（`frame_time_ == playhead` 直接命中缓存），量出来的是“空转主循环”的
  // 成本，而不是“持续动画下”的真实成本——而后者才是本示例要压的东西
  //（设计文档 §4.3：连续动画的帧预算与续帧协议）。
  if (options.bench_seconds > 0) {
    page->project().playing = true;
  }

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // 主循环：**时间轴必须由应用推进**（`ui_root.hpp` 明写的那条教训——
  // 框架不替应用推进时间，因为它不知道"这一帧该走多久"）。
  std::uint64_t frames = 0;
  const std::int64_t bench_deadline =
      options.bench_seconds > 0 ? st::time::now_ms() + options.bench_seconds * 1000 : 0;
  std::int64_t last_ms = st::time::now_ms();
  while (!app.quit_requested()) {
    const std::int64_t frame_start = st::time::now_ms();
    const double delta = static_cast<double>(frame_start - last_ms) / 1000.0;
    last_ms = frame_start;

    if (page->advance(delta)) app.request_repaint();
    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
    ++frames;
    if (bench_deadline != 0 && st::time::now_ms() >= bench_deadline) break;
    app.pace_loop(frame_start);
  }
  if (options.bench_seconds > 0) {
    // ⚠ 报告**两个口径**，别混：
    //   * **墙钟帧间隔**含 `pace_loop` 的帧预算睡眠（默认 16ms）——它反映“实际
    //     帧率”，不反映“渲染成本”；
    //   * **帧内耗时**（`frame_p50/p95`）才是“能不能抱住 60fps”的依据。
    // 实测（1680×1170 @1.5、两层合成）：墙钟 ~20ms 而真实 p50 = 10.4ms
    //——只看前者会误以为“重合成很慢”，实际是节流在起作用。
    //
    // 墙钟值改由 `metrics.wall_p50_ms` 给出（框架统一口径）：以前这里用
    // “总秒数 / 总帧数” 自算，那是**平均**而 p50/p95 是**分位**——两种统计量
    // 摆在一行里会被当成同类（平均会被一次长卡顿拉高，分位不会）。
    const auto metrics = app.metrics();
    st::print("压测：{} 秒 / {} 帧；墙钟间隔 p50={:.2f}ms p95={:.2f}ms（含帧预算睡眠），"
              "帧内耗时 p50={:.2f}ms p95={:.2f}ms（绘制 {:.2f}ms）\n",
              options.bench_seconds, frames, metrics.wall_p50_ms, metrics.wall_p95_ms,
              metrics.frame_p50_ms, metrics.frame_p95_ms, metrics.paint_ms);
  }
  st::print("裁云退出：{} 帧，后端 {}\n", frames, std::string(app.backend_name()));
  return 0;
}

ST_MAIN(run_app)
