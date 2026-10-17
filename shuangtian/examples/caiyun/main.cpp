/// 裁云（Nimbus）— **AI 驱动的视频生成器**（霜天示例）。
///
/// ## 定位（本次升级）
///
/// 旧版是「时间轴剪辑器」：素材是测试卡，成品是帧序列。现在的定位是
/// **「AI 控制生成高质量视频 + 人工微调」**：
///
/// - **AI 的入口是一份导演脚本**（`director.hpp` 的 JSON）：分镜、文案、配色、节奏、
///   配乐全在里面。智能体写脚本 → `invoke load_script` / `invoke script` 应用 →
///   `invoke render` 出成片，全程不需要人类点界面；
/// - **人工操作面保留但收敛**（以 AI 为主）：时间轴（修剪/移动/分割/波纹删除/吸附）、
///   检查器（不透明度/速度/字幕文案）、传输控制、播放头擦洗；
/// - **渲染管线**（`render.hpp`）：1080p PNG 帧序列 + 内置合成配乐 + ffmpeg 封装 mp4，
///   工作线程执行、进度可查、可中止。
///
/// 默认工程就是**「歌白 10 秒宣传片」**——启动即可预览，一键即出成片。
///
/// ## 这个示例检验什么（`docs/LOUYUE_CAIYUN.md` §1）
///
/// **时间 / 动效应用面**——它是**唯一由 `UiRoot` 时间轴驱动持续动画的示例**：
/// 播放中的帧预算与续帧协议、`seek` 到同一时刻两次渲染逐像素相同（确定性）、
/// 拖拽修剪这类连续交互；外加**跨线程长任务**（渲染作业 → 进度回 UI）的真实位。

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

#include "director.hpp"
#include "director_bridge.hpp"
#include "project.hpp"
#include "render.hpp"
#include "views.hpp"
#include "st/app/app.hpp"
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/dsl.hpp"

namespace {

using caiyun::Clip;
using caiyun::format_timecode;
using caiyun::GeneratorKind;
using caiyun::kGenerators;
using caiyun::DirectorBridge;
using caiyun::MonitorView;
using caiyun::Project;
using caiyun::RenderJob;
using caiyun::Renderer;
using caiyun::RenderStatus;
using caiyun::TimelineView;
using st::ui::Button;
using st::ui::ScrollView;
namespace dsl = st::ui::dsl;

struct Options {
  bool headless{false};
  std::string backend{};
  std::string theme{"light"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  /// `--script <路径>`：启动即装载导演脚本（AI/CI 的入口）。
  std::string script{};
  /// `--ai-control`：启动即进入 AI 控制态（人只能查看）。
  ///
  /// 为何要这个开关而不是“让 AI 自己发 edit_lock”：演示/人机同屏时，
  /// **启动那一刻就不该给人工编辑留窗口**——先把界面摆对，再开始放片子。
  bool ai_control{false};
  /// `--render <输出 mp4>` + `--render-size WxH` + `--render-frames N`：启动即渲染。
  std::string render_output{};
  int render_width{1920};
  int render_height{1080};
  int render_frames{0};
  std::string export_dir{};   ///< `--export-dir <dir>`：启动即导出 PNG 帧序列（旧口径，保留）
  int bench_seconds{0};       ///< `--bench <秒>`：播放压测时长
};

/// 用法说明。**必须有**：未知参数会报错退出，没有 `--help` 就等于"报错了也没处查"。
auto print_usage(std::string_view program) -> void {
  st::print(
      "用法: {} [选项]\n"
      "\n"
      "窗口/主题\n"
      "  --headless            无头模式（无窗口；控制通道照常可用）\n"
      "  --theme light|dark    主题\n"
      "\n"
      "内容/自动化\n"
      "  --script PATH         启动即装载导演脚本（JSON；缺省 = 内置歌白 10 秒宣传片）\n"
  "  --ai-control          启动即进入 AI 控制态（人工编辑控件置灰，只能查看/播放）\n"
      "  --render PATH         启动即渲染成片（mp4；需本机有 ffmpeg）\n"
      "  --render-size WxH     成片分辨率（缺省 1920x1080）\n"
      "  --render-frames N     只渲染前 N 帧（快速试渲）\n"
      "  --export-dir DIR      启动即导出 PNG 帧序列（320x180，无 ffmpeg 时的兜底）\n"
      "  --bench N             真开播放压测 N 秒后退出\n"
      "  --control-port N      控制通道端口（0 = 自动）\n"
      "  --control-file PATH   控制信息落盘路径\n"
      "  --shots DIR           无头截图目录\n"
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
    // 数值参数统一走带校验的解析（承 gbcode 的纪律）：非法值必须与未知参数
    // 同一条路径（报错可读、退出码稳定），不能"一快一崩"。
    const auto parse_int = [&](const char* flag, long long fallback) -> long long {
      const std::string text = value(std::format("{}", fallback).c_str());
      long long parsed = 0;
      std::size_t consumed = 0;
      bool ok = false;
      try {
        parsed = std::stoll(text, &consumed);
        ok = consumed == text.size();
      } catch (const std::exception&) {
        ok = false;
      }
      if (!ok) {
        st::eprint("参数 {} 需要一个整数，收到「{}」\n", flag, text);
        std::exit(2);
      }
      return parsed;
    };
    if (raw == "--headless") {
      options.headless = true;
    } else if (raw == "--theme") {
      options.theme = value("light");
    } else if (raw == "--backend") {
      options.backend = value("");
    } else if (raw == "--control-port") {
      options.control_port = static_cast<std::uint16_t>(std::clamp(parse_int("--control-port", 0), 0LL, 65535LL));
    } else if (raw == "--control-file") {
      options.control_file = value("");
    } else if (raw == "--shots") {
      options.shots = value("");
      } else if (raw == "--script") {
    options.script = value("");
  } else if (raw == "--ai-control") {
    options.ai_control = true;
  } else if (raw == "--render") {
      options.render_output = value("");
    } else if (raw == "--export-dir") {
      options.export_dir = value("");
    } else if (raw == "--render-size") {
      const std::string text = value("1920x1080");
      const auto at = text.find('x');
      if (at == std::string::npos) {
        st::eprint("参数 --render-size 需要 WxH（如 1920x1080），收到「{}」\n", text);
        std::exit(2);
      }
      try {
        options.render_width = std::clamp(std::stoi(text.substr(0, at)), 64, 7680);
        options.render_height = std::clamp(std::stoi(text.substr(at + 1)), 64, 4320);
      } catch (const std::exception&) {
        st::eprint("参数 --render-size 不是合法尺寸：「{}」\n", text);
        std::exit(2);
      }
    } else if (raw == "--render-frames") {
      options.render_frames = static_cast<int>(std::clamp(parse_int("--render-frames", 0), 0LL, 100000LL));
    } else if (raw == "--bench") {
      options.bench_seconds = static_cast<int>(std::clamp(parse_int("--bench", 0), 0LL, 3600LL));
    } else if (raw == "-h" || raw == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      st::eprint("未知参数：{}（用 --help 看可用项）\n", raw);
      std::exit(2);
    }
  }
  return options;
}

class CaiyunPage final : public dsl::Component {
 public:
  explicit CaiyunPage(std::string script_path) : script_path_(std::move(script_path)) {
    project_.apply_script(load_initial_script(script_path_));
    project_.playhead = 1.2;
  }

  [[nodiscard]] auto project() noexcept -> Project& { return project_; }
  [[nodiscard]] auto renderer() noexcept -> Renderer& { return renderer_; }
  void set_tasks(st::app::Tasks* tasks) { tasks_ = tasks; }
  void set_text_port(const st::ui::TextPort* port) { text_port_ = port; }

  /// 把脚本里的**相对路径**改成相对脚本所在目录的绝对路径（旁白 WAV 等）。
  ///
  /// 为何在这里做：脚本是外部输入，用自身位置引用素材（`vo/s1.wav`）才是可移植
  /// 的写法；而渲染可能在**任意工作目录**下跑（GUI 双击、CI、控制通道）。
  /// 只改这一处，渲染器就只管拿绝对路径读文件。
  static void rebase_relative_paths(caiyun::Script& script, const std::string& script_path) {
    if (script_path.empty()) return;
    const std::string base = st::fs::parent(script_path);
    if (base.empty()) return;
    for (auto& track : script.audio.voice) {
      if (track.path.empty() || st::fs::is_absolute(track.path)) continue;
      track.path = st::fs::join(base, track.path);
    }
  }

  /// 装载脚本（路径为空则用内置歌白宣传片）。失败时**保留原工程**并返回错误。
  auto load_script(const std::string& path) -> st::Result<std::string> {
    caiyun::Script script = caiyun::builtin_gebai_promo();
    if (!path.empty()) {
      const auto text = st::fs::read_text(path);
      if (!text) return st::forward_error(text.error());
      const auto parsed = caiyun::parse_script(text.value());
      if (!parsed) return st::forward_error(parsed.error());
      script = parsed.value();
      rebase_relative_paths(script, path);
    }
    if (!project_.apply_script(script)) {
      return st::unexpected(st::ErrorCode::Invalid, "脚本没有分镜（scenes 为空）");
    }
    script_path_ = path;
    selected_scene_ = 0;
    if (monitor_ != nullptr) monitor_->refresh();
    if (timeline_ != nullptr) timeline_->set_project(&project_);
    bump();
    log(std::format("已应用脚本：{}（{} 个场景 / {:.2f}s）", project_.script.name,
                    project_.script.scenes.size(), project_.duration));
    return project_.script.name;
  }

  /// 把当前工程**回写**成脚本文件（人工微调后再落盘，让脚本成为真值源）。
  auto save_script(const std::string& path) const -> bool {
    return st::fs::write_text(path, caiyun::script_to_json(project_.script)).has_value();
  }

  /// 启动渲染（工作线程）。返回作业是否真的起来了。
  auto start_render(RenderJob job) -> bool {
    if (tasks_ == nullptr) return false;
    if (text_port_ == nullptr) {
      log("渲染失败：文本端口未就绪");
      return false;
    }
    if (renderer_.running()) {
      log("已有渲染作业在跑，忽略本次请求");
      return false;
    }
    job.output_dir = job.output_dir.empty() ? st::fs::join(st::fs::temp_dir(), "caiyun-render")
                                            : job.output_dir;
    const bool started = renderer_.start(*tasks_, project_, job, text_port_, [this] {
      // 工作线程 → 主线程：只 marker + 请求重绘（不碰 UI 树）。
      if (tasks_ != nullptr) tasks_->post_ui([this] { bump(); });
    });
    if (started) {
      log(std::format("渲染开始：{}x{} @{:.0f}fps → {}", job.width, job.height, job.fps,
                      job.output_file.empty() ? job.output_dir : job.output_file));
      bump();
    }
    return started;
  }

  /// 旧的 `--export-dir` 路径（320×180 PNG 帧序列 + manifest）：无 ffmpeg 时的兜底，
  /// 也是既有 e2e 的契约面。
  auto export_frames(const std::string& dir, const st::ui::TextPort* text_port) -> int {
    const int frames = static_cast<int>(std::ceil(project_.duration * project_.fps));
    st::raster::Canvas canvas{320, 180, 1.0f};
    std::string manifest = std::format(
        "{{\n  \"fps\": {:.3f},\n  \"duration\": {:.3f},\n  \"frames\": {},\n  \"scenes\": {},\n"
        "  \"clips\": [\n",
        project_.fps, project_.duration, frames, project_.script.scenes.size());
    for (std::size_t i = 0; i < project_.clips.size(); ++i) {
      const Clip& clip = project_.clips[i];
      const char* kind = clip.is_scene() ? "scene" : caiyun::to_string(clip.generator);
      manifest += std::format(
          "    {{\"id\": {}, \"track\": {}, \"t_in\": {:.3f}, \"t_out\": {:.3f}, "
          "\"kind\": \"{}\"}}{}\n",
          clip.id, clip.track, clip.t_in, clip.t_out, kind,
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
      image.rgba = canvas.to_rgba8();
      const std::string path = std::format("{}/frame_{:04}.png", dir, index + 1);
      if (!st::codec::png_write_file(path, image, 4).has_value()) break;
      ++written;
    }
    (void)st::fs::write_text(std::format("{}/manifest.json", dir), manifest);
    return written;
  }

  /// 控制面钩子（元素 #director 的动作面）。全部转发到本页的真实能力上——
  /// 桥里**不做任何业务判断**（脚本解析/渲染调度都只有一份实现）。
  [[nodiscard]] auto make_hooks() -> DirectorBridge::Hooks {
    DirectorBridge::Hooks hooks{};
    hooks.apply_script = [this](const std::string& json) -> std::string {
      const auto result = apply_script_text(json);
      return result ? std::string{} : result.error().message;
    };
    hooks.load_script = [this](const std::string& path) -> std::string {
      const auto result = load_script(path);
      return result ? std::string{} : result.error().message;
    };
    hooks.save_script = [this](const std::string& path) -> std::string {
      if (path.empty()) return std::string("save_script 需要文件路径");
      return save_script(path) ? std::string{} : std::format("写入失败：{}", path);
    };
    hooks.start_render = [this](const std::string& spec) -> std::string {
      // spec 形如 "<输出 mp4 路径>" 或 "<输出 mp4 路径> 1280x720"。
      RenderJob job{};
      std::string path = spec;
      const std::size_t space = spec.rfind(' ');
      if (space != std::string::npos) {
        const std::string tail = spec.substr(space + 1);
        const std::size_t x = tail.find('x');
        if (x != std::string::npos) {
          try {
            job.width = std::clamp(std::stoi(tail.substr(0, x)), 64, 7680);
            job.height = std::clamp(std::stoi(tail.substr(x + 1)), 64, 4320);
            path = spec.substr(0, space);
          } catch (...) {
            // 尾巴不是尺寸：整串当路径（按脚本分辨率渲染）。
          }
        }
      }
      if (path.empty()) return std::string("render 需要输出路径（如 /tmp/out.mp4）");
      job.output_file = path;
      job.output_dir = st::fs::parent(path);
      if (job.output_dir.empty()) job.output_dir = ".";
      if (job.width == 1920 && job.height == 1080 && project_.script.width > 0) {
        job.width = project_.script.width;
        job.height = project_.script.height;
      }
      job.fps = project_.fps;
      job.audio = project_.script.audio.enabled;
      return start_render(job) ? std::string{} : std::string("渲染未能启动（已有作业在跑？）");
    };
    hooks.stop_render = [this] { renderer_.cancel(); };
    hooks.script_json = [this] { return caiyun::script_to_json(project_.script); };
    hooks.set_edit_lock = [this](bool on) -> std::string {
      if (!project_.set_ai_control(on)) {
        // 重复设置不算错（幂等），但也不报“失败”。
        return {};
      }
      // 交接时**已经重建了片段**（以脚本为准），所以这里同步时间轴与监视器。
      if (timeline_ != nullptr) timeline_->set_project(&project_);
      if (monitor_ != nullptr) monitor_->refresh();
      selected_scene_ = 0;
      log(on ? "AI 接管编辑：人工控件已置灰（仍可查看/播放）"
             : "AI 交还编辑权：人工控件已恢复");
      bump();
      return {};
    };
    hooks.edit_locked = [this] { return project_.ai_controlled; };
    hooks.project_value = [this](const std::string& key) -> std::string {
      if (key == "scene_count") return std::to_string(project_.script.scenes.size());
      if (key == "duration") return std::format("{:.3f}", project_.duration);
      if (key == "fps") return std::format("{:.3f}", project_.fps);
      if (key == "size") return std::format("{}x{}", project_.script.width, project_.script.height);
      if (key == "name") return project_.script.name;
      if (key == "clip_count") return std::to_string(project_.clips.size());
      return {};
    };
    hooks.render_value = [this](const std::string& key) -> std::string {
      const RenderStatus status = renderer_.status();
      if (key == "stage") return status.stage;
      if (key == "progress") return std::format("{:.4f}", status.progress());
      if (key == "output") return status.output;
      if (key == "error") return status.error;
      if (key == "frames_done") return std::to_string(status.frames_done);
      if (key == "frames_total") return std::to_string(status.frames_total);
      if (key == "elapsed") return std::format("{:.2f}", status.elapsed_s);
      if (key == "log") {
        std::string joined;
        for (const std::string& line : status.log) {
          if (!joined.empty()) joined.push_back('\n');
          joined += line;
        }
        return joined;
      }
      return {};
    };
    return hooks;
  }

  void build(dsl::Composer& c) override {
    (void)version_.value();
    dsl::column(c, {.gap = 0.0f, .grow = true, .id = "caiyun-root"}, [&] {
      // 控制面（0×0）：AI 经 invoke apply_script / invoke render 驱动本应用的唯一
      // 稳定寻址点。放在根列第一个，保证任何布局下都存在。
      (void)dsl::custom<DirectorBridge>(
          c,
          [this](DirectorBridge& bridge) {
            bridge.set_id("director");
            bridge.set_hooks(make_hooks());
          },
          {.width = 0.0f, .height = 0.0f, .key = "director-bridge"});
      build_monitor(c);
      dsl::row(c, {.gap = 8.0f, .padding_x = 8.0f, .height = 340.0f, .id = "middle-row"}, [&] {
        build_director(c);
        build_inspector(c);
        (void)dsl::spacer(c, 1.0f);
      });
      build_timeline(c);
    });
  }

 private:
  void bump() { version_.set(version_.value() + 1U); }

  void log(std::string line) {
    log_lines_.push_back(std::move(line));
    if (log_lines_.size() > 40) log_lines_.erase(log_lines_.begin());
    bump();
  }

  /// 启动时装载脚本（构造函数路径）。
  ///
  /// **必须与 `load_script` 走同一个解析/重基入口**：早先这里自己又写了一遍
  /// “读文件 + parse”，漏了 `rebase_relative_paths`——于是命令行启动的工程里
  /// 旁白仍是 `vo/s1.wav`（相对 cwd），而 UI 按钮装载的却是绝对路径。结果
  /// 同一个脚本两条路径两种行为（实测：命令行出片 0/4 轨配音）。
  static auto load_initial_script(const std::string& path) -> caiyun::Script {
    if (path.empty()) return caiyun::builtin_gebai_promo();
    const auto text = st::fs::read_text(path);
    if (!text) {
      st::eprint("脚本读取失败：{}（改用内置歌白宣传片）\n", text.error().message);
      return caiyun::builtin_gebai_promo();
    }
    const auto parsed = caiyun::parse_script(text.value());
    if (!parsed) {
      st::eprint("脚本解析失败：{}（改用内置歌白宣传片）\n", parsed.error().message);
      return caiyun::builtin_gebai_promo();
    }
    caiyun::Script script = parsed.value();
    rebase_relative_paths(script, path);
    return script;
  }

  void build_monitor(dsl::Composer& c) {
    dsl::column(c, {.gap = 6.0f, .padding = 8.0f, .id = "monitor-panel"}, [&] {
      (void)dsl::custom<MonitorView>(
          c,
          [this](MonitorView& monitor) {
            monitor.set_id("monitor");
            monitor.set_project(&project_);
            monitor_ = &monitor;
          },
          {.height = 236.0f, .key = "monitor"});
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
            c, [this] { return format_timecode(project_.playhead); },
            {.id = "timecode", .key = "timecode"});
        (void)dsl::spacer(c, 1.0f);
        // 编辑类按钮：AI 控制态下置灰（门禁在 Project，这里只是让用户**看得见**）。
        const bool editable = project_.can_edit();
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("btn-split");
              b.set_icon("diff");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.set_enabled(editable);
              b.on_click = [this] { do_split(); };
            },
            {.key = "split"});
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("btn-ripple-delete");
              b.set_icon("trash");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.set_enabled(editable);
              b.on_click = [this] { do_ripple_delete(); };
            },
            {.key = "ripple"});
      });
    });
  }

  /// 左侧：**AI 导演面板**（脚本 / 渲染 / 进度 / 日志）——AI 与人的共用入口。
  void build_director(dsl::Composer& c) {
    // 面板内容比可视高度长（脚本 + 渲染 + 进度 + 日志），**必须能滚**：
    // 不包滚动容器时底部（日志/错误行）会被直接裁掉——表现是"功能没做完"。
    (void)dsl::custom_container<ScrollView>(
        c,
        [&] {
    dsl::column(c, {.gap = 8.0f, .padding = 8.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "director-panel"}, [&] {
      dsl::text(c, [] { return std::string("AI 导演"); },
                {.size = 15.0f, .weight = st::ui::FontWeight::SemiBold, .id = "director-title"});
      // **控制态横幅**：AI 接管时这一行是用户唯一能看出“为什么点不动”的地方。
      // 所以它必须是常驻的一行（不是 toostip）——置灰的控件不自解释。
      dsl::text(
          c,
          [this] {
            return project_.ai_controlled
                       ? std::string("AI 控制中 · 仅可查看（播放/暂停/拖动时间轴仍可用）")
                       : std::string("人工编辑模式");
          },
          {.size = 12.0f, .id = "director-control-state", .key = "control-state"});
      dsl::text(c, [this] { return std::format("工程：{}", project_.script.name); },
                {.id = "director-project", .key = "project-name"});
      dsl::text(c,
                [this] {
                  return std::format("{}×{} · {:.0f}fps · {} 场景 · {:.2f}s",
                                     project_.script.width, project_.script.height,
                                     project_.script.fps, project_.script.scenes.size(),
                                     project_.duration);
                },
                {.id = "director-meta", .key = "meta"});
      // 脚本路径（AI 侧通常直接用控制通道 apply_script，这里是人工换片子的入口）。
      dsl::text(c, [] { return std::string("脚本路径"); }, {});
      const bool editable = project_.can_edit();
      (void)dsl::input(
          c, script_path_,
          [this](std::string value) {
            script_path_ = std::move(value);
          },
          {.id = "director-script-path", .key = "script-path"})
          .set_enabled(editable);
      dsl::row(c, {.gap = 6.0f, .id = "director-buttons"}, [&] {
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("btn-apply-script");
              b.set_label("应用脚本");
              b.set_icon("play");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Soft);
              b.set_enabled(editable);
              b.on_click = [this] { apply_script_clicked(); };
            },
            {.key = "apply"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("btn-save-script");
              b.set_label("另存");
              b.set_icon("download");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] {
                const std::string path =
                    script_path_.empty() ? std::string("caiyun-project.json") : script_path_;
                log(save_script(path) ? std::format("脚本已保存：{}", path)
                                      : std::format("脚本保存失败：{}", path));
              };
            },
            {.key = "save"});
      });
      // 渲染控制。
      dsl::text(c, [] { return std::string("渲染"); },
                {.size = 13.0f, .weight = st::ui::FontWeight::SemiBold});
      dsl::row(c, {.gap = 6.0f, .id = "render-buttons"}, [&] {
        (void)dsl::custom<Button>(
            c,
            [this, editable](Button& b) {
              b.set_id("btn-render");
              b.set_label("渲染成片");
              b.set_icon("sparkles");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Primary);
              // 渲染不是“编辑”，但 AI 正在驱动时也不该被重开第二个作业
              //（渲染器一次只跑一个，重开只会得一句失败）。
              b.set_enabled(editable);
              b.on_click = [this] { render_clicked(); };
            },
            {.key = "render"});
        (void)dsl::custom<Button>(
            c,
            [this](Button& b) {
              b.set_id("btn-render-stop");
              b.set_label("中止");
              b.set_icon("square");
              b.set_size(Button::Size::Small);
              b.set_variant(Button::Variant::Ghost);
              b.on_click = [this] {
                renderer_.cancel();
                log("已请求中止渲染");
              };
            },
            {.key = "render-stop"});
      });
      const RenderStatus status = renderer_.status();
      (void)dsl::progress(c, status.progress(), {.id = "director-progress", .key = "progress"});
      dsl::text(c,
                [this] {
                  const RenderStatus now = renderer_.status();
                  return std::format("{} · 帧 {}/{} · {:.2f}s", now.stage, now.frames_done,
                                     now.frames_total, now.elapsed_s);
                },
                {.id = "director-status", .key = "status"});
      if (!status.output.empty()) {
        dsl::text(c, [this] { return std::format("成片：{}", renderer_.status().output); },
                  {.size = 11.0f, .id = "director-output", .key = "output"});
      }
      if (!status.error.empty()) {
        dsl::text(c, [this] { return renderer_.status().error; },
                  {.color = st::ui::Tone::Danger, .size = 11.0f, .id = "director-error", .key = "error"});
      }
      (void)dsl::divider(c);
      dsl::text(c, [] { return std::string("日志"); }, {.size = 11.0f});
      dsl::column(c, {.gap = 2.0f, .id = "director-log"}, [&] {
        const std::size_t begin = log_lines_.size() > 6 ? log_lines_.size() - 6 : 0;
        for (std::size_t i = begin; i < log_lines_.size(); ++i) {
          const std::string line = log_lines_[i];
          dsl::text(c, [line] { return line; }, {.size = 11.0f, .key = line});
        }
        const RenderStatus now = renderer_.status();
        for (std::size_t i = 0; i < now.log.size(); ++i) {
          if (i + 6 < now.log.size()) continue;
          const std::string line = now.log[i];
          dsl::text(c, [line] { return line; }, {.size = 11.0f, .key = "r" + line});
        }
      });
    });
        },
        [](ScrollView& view) { view.set_id("director-scroll"); },
        {.width = 280.0f, .height = 340.0f, .key = "director-scroll"});
  }

  void apply_script_clicked() {
    const auto name = load_script(script_path_);
    if (!name) log(std::format("应用失败：{}", name.error().message));
  }

  void render_clicked() {
    RenderJob job{};
    job.output_dir = st::fs::join(st::fs::temp_dir(), "caiyun-render");
    job.output_file = st::fs::join(job.output_dir, "caiyun-film.mp4");
    job.width = project_.script.width;
    job.height = project_.script.height;
    job.fps = project_.fps;
    job.audio = project_.script.audio.enabled;
    (void)start_render(job);
  }

  void build_inspector(dsl::Composer& c) {
    (void)dsl::custom_container<ScrollView>(
        c,
        [&] {
    dsl::column(c, {.gap = 8.0f, .padding = 8.0f,
                    .surface = st::ui::Element::Surface::Alt, .id = "inspector"}, [&] {
      dsl::text(c, [] { return std::string("属性"); },
                {.size = 15.0f, .weight = st::ui::FontWeight::SemiBold, .id = "inspector-title"});
      const Clip* clip = project_.find_clip(selected_id());
      if (clip == nullptr) {
        dsl::text(c, [] { return std::string("（未选中片段）"); }, {.id = "inspector-empty"});
        return;
      }
      const caiyun::Scene* scene = project_.scene_of(*clip);
      dsl::text(c,
                [clip, scene] {
                  return scene != nullptr
                             ? std::format("#{} · 场景 {}", clip->id,
                                           caiyun::scene_kind_name(scene->kind))
                             : std::format("#{} · {}", clip->id, caiyun::to_string(clip->generator));
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
      const bool editable = project_.can_edit();
      (void)dsl::slider(
          c, clip->opacity,
          [this, id](float value) {
            // 门禁在 Project 里（`set_opacity` 自己拒绝）——这里不再重复判断，
            // 否则“两处都要改”迟早漏一处。
            if (!project_.set_opacity(id, value)) return;
            if (monitor_ != nullptr) monitor_->refresh();
            bump();
          },
          {.id = "inspector-opacity", .key = "opacity"})
          .set_enabled(editable);
      dsl::text(c, [] { return std::string("速度"); }, {});
      (void)dsl::slider(
          c, clip->speed,
          [this, id](float value) {
            if (!project_.set_speed(id, value)) return;
            if (monitor_ != nullptr) monitor_->refresh();
            bump();
          },
          {.id = "inspector-speed", .key = "speed"})
          .set_enabled(editable);
      if (scene != nullptr) {
        (void)dsl::divider(c);
        dsl::text(c, [] { return std::string("文案（人工微调）"); }, {.size = 12.0f});
        const int scene_index = clip->scene_index;
        (void)dsl::input(
            c, scene->title,
            [this, scene_index](std::string value) {
              if (!project_.set_scene_text(scene_index, 0, std::move(value))) return;
              if (monitor_ != nullptr) monitor_->refresh();
              bump();
            },
            {.id = "inspector-title-input", .key = "scene-title"})
            .set_enabled(editable);
        (void)dsl::input(
            c, scene->subtitle,
            [this, scene_index](std::string value) {
              if (!project_.set_scene_text(scene_index, 1, std::move(value))) return;
              if (monitor_ != nullptr) monitor_->refresh();
              bump();
            },
            {.id = "inspector-subtitle-input", .key = "scene-subtitle"})
            .set_enabled(editable);
      }
    });
        },
        [](ScrollView& view) { view.set_id("inspector-scroll"); },
        {.width = 260.0f, .height = 340.0f, .key = "inspector-scroll"});
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
        (void)spacer(c, 1.0f);
        dsl::text(c,
                  [this] {
                    return std::format("V1 · {} 段", project_.clips.size());
                  },
                  {.id = "timeline-clip-count", .key = "clip-count"});
      });
      (void)dsl::custom<TimelineView>(
          c,
          [this](TimelineView& timeline) {
            timeline.set_id("timeline");
            timeline.set_project(&project_);
            // 两个回调都在**每一步变化**后触发，含拖拽中的每一帧——
            // 监视器必须跟着重合成，否则拖动播放头时预览是静止的。
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
  /// 播放推进（由主循环每帧调）——**时间轴必须由应用推进**。
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

  /// 应用脚本文本（控制通道 `invoke script` 的落点；走 `parse_script` 同一口径）。
  auto apply_script_text(std::string_view json) -> st::Result<std::string> {
    const auto parsed = caiyun::parse_script(json);
    if (!parsed) return st::forward_error(parsed.error());
    caiyun::Script script = parsed.value();
    if (!project_.apply_script(script)) {
      return st::unexpected(st::ErrorCode::Invalid, "脚本没有分镜");
    }
    if (monitor_ != nullptr) monitor_->refresh();
    if (timeline_ != nullptr) timeline_->set_project(&project_);
    bump();
    log(std::format("已应用内联脚本：{}（{} 场景）", project_.script.name,
                    project_.script.scenes.size()));
    return project_.script.name;
  }

  [[nodiscard]] auto script_json() const -> std::string {
    return caiyun::script_to_json(project_.script);
  }

 private:
  Project project_{};
  Renderer renderer_{};
  MonitorView* monitor_{nullptr};
  TimelineView* timeline_{nullptr};
  st::app::Tasks* tasks_{nullptr};
  const st::ui::TextPort* text_port_{nullptr};
  std::string script_path_{};
  std::vector<std::string> log_lines_{};
  int selected_scene_{0};
  dsl::State<std::size_t> version_{0};
};

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  st::app::AppOptions app_options;
  app_options.width = 1180;
  app_options.height = 900;
  app_options.title = "裁云 Nimbus · 霜天";
  app_options.headless = options.headless;
  app_options.backend = !options.backend.empty() ? options.backend
                        : (options.headless ? std::string("headless") : std::string{});
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;

  st::app::Application app("caiyun", "0.2.0", app_options);
  // **自绘组件必须先注册**（`custom<T>` 用它之前）。注册名不能与内置组件重名。
  if (!dsl::register_element_type<TimelineView>("CaiyunTimeline") ||
      !dsl::register_element_type<MonitorView>("MonitorView") ||
      !dsl::register_element_type<DirectorBridge>("CaiyunDirector")) {
    std::fprintf(stderr, "自绘组件注册失败（与内置组件重名？）\n");
    return 1;
  }

  auto page = std::make_shared<CaiyunPage>(options.script);
  // 启动即接管（`--ai-control`）：必须在**首帧之前**设好，否则会出现“先能改、
  // 锁上”的一瞬——而那一瞬恰好是演示时镜头对着屏幕的时候。
  if (options.ai_control) (void)page->project().set_ai_control(true);
  if (!options.export_dir.empty()) {
    (void)st::fs::create_directories(options.export_dir);
  }

  auto frame = std::make_unique<st::ui::WindowFrame>("裁云 Nimbus · AI 视频导演");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("sparkles");
  }
  st::ui::Element* slot = frame->content();
  app.set_content(std::move(frame));
  auto host = dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  (void)host->tick();

  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }

  // 启动期自动化：导出帧序列 / 渲染成片（在控制通道起来之后做，便于观察进度）。
  page->set_tasks(&app.tasks());
  page->set_text_port(app.root().text_port());
  if (!options.export_dir.empty()) {
    const int written = page->export_frames(options.export_dir, app.root().text_port());
    st::print("已导出 {} 帧到 {}\n", written, options.export_dir);
  }
  bool render_launched = false;
  if (!options.render_output.empty()) {
    RenderJob job{};
    job.output_dir = st::fs::parent(options.render_output);
    job.output_file = options.render_output;
    job.width = options.render_width;
    job.height = options.render_height;
    job.fps = page->project().fps;
    job.audio = page->project().script.audio.enabled;
    job.last_frame = options.render_frames;
    render_launched = page->start_render(job);
    st::print("渲染启动：{}x{} → {}\n", job.width, job.height, job.output_file);
  }

  // `--bench`：真开播放再压测（背景：不播放时监视器不重合成，量到的是空转成本）。
  if (options.bench_seconds > 0) page->project().playing = true;

  // 主循环：时间轴必须由应用推进。
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
    // 一次性渲染作业：完成后收尾退出（CI/CLI 用）。
    if (render_launched) {
      const RenderStatus status = page->renderer().status();
      if (!status.running && (status.finished || status.failed || status.cancelled)) {
        for (const std::string& line : status.log) {
          // 逐帧进度只显示尾部（40 条环形缓冲里大半是它，会把"每帧耗时"这类
          // 真正有用的汇总行挤掉）。
          if (line.starts_with("帧 ") && !line.starts_with("帧完成")) continue;
          st::print("  {}\n", line);
        }
        st::print("渲染结果：{}（{} 帧 / {:.2f}s）{}\n", status.stage, status.frames_done,
                  status.elapsed_s, status.error.empty() ? "" : " · " + status.error);
        if (!status.output.empty()) st::print("成片：{}\n", status.output);
        if (options.bench_seconds <= 0) break;
        render_launched = false;
      }
    }
    if (bench_deadline != 0 && st::time::now_ms() >= bench_deadline) break;
    app.pace_loop(frame_start);
  }
  if (options.bench_seconds > 0) {
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
