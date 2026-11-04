/// **threelab** —— 实时编辑并预览 three.js 场景的示例应用。
///
/// ## 这个示例检验什么
///
/// 一个"三维脚本实时预览器"必须同时成立四件事，缺一条就用不了：
///
/// | 能力 | 为什么是硬要求 |
/// |---|---|
/// | **热重载**：改 `.js` 文件 → 画面跟着变 | 否则每改一行都要重启，编辑循环断掉 |
/// | **交互**：旋转 / 缩放 / 平移 / 悬浮 / 点击选取 | 否则只是个"会旋转的图片"，判断不了模型对不对 |
/// | **错误可见**：脚本写错时画面不清空、错误显示在视口上 | 否则改坏一行就白屏，人会以为程序崩了 |
/// | **状态自检**：物体数 / 后端 / 拾取计数能读 | 否则"为什么没反应"只能靠猜 |
///
/// 前三条是产品能力，第四条是**可验证性**：本仓一贯的立场是
/// "不可断言的观感 = 不可复用"（见 `docs/VERIFY-PLAYBOOK.md`）。
/// 所以视图把 `objects`/`hovered`/`selected`/`hover_events`/`click_events`
/// 都暴露成属性，控制通道与测试都能直接读。
///
/// ## 为什么不做成纯 XML + JS
///
/// 与 `xmlapp` 相反：这个示例的**主体是一个自绘组件**（三维视口），它要在
/// `paint_content` 里驱动三维后端。XML 层负责的是"视口 + 文件列表 + 提示条"
/// 这类**外围**布局，那些用声明式写更省事。两条通路各做自己擅长的事。
///
/// ## 文件在哪
///
/// 默认 `examples/threelab/scenes/*.js`，可用 `--dir` 指定。
/// 脚本是**普通 three.js 代码**，不需要任何本框架的 API——这是整件事的关键：
/// 模型/用户写的是它已经会写的东西。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/raster/scene_report.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/three_view.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"

namespace {

using st::Json;

/// 命令行选项（与其它示例同一套形态：`--key=value`）。
struct Options {
  std::string dir{};
  std::string scene{};
  bool watch{true};
  int width{1100};
  int height{760};
  std::string theme{"dark"};
  int control_port{0};
  std::string control_file{};
  std::string shots{};
  std::uint32_t max_frames{0};
  bool headless{false};
  int probe_frames{0};
};

auto parse_options(int argc, char** argv) -> Options {
  Options options;
  // ## 同时支持 `--key=value` 与 `--key value`
  //
  // 只支持前者是一个**真踩到的坑**：框架的 e2e 脚本（以 caiyun 为模版）用的是
  // 分离形式 `--control-file PATH`，而本程序只认 `= 形式`——于是控制文件
  // 永远不落盘，e2e 在 60 秒后报“应用未写出控制文件”，看起来像启动失败。
  //
  // 两种都收：分离式是 Unix 惯例（脚本/工具链在用），等号式是个人习惯。
  bool boolean_flag = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    (void)boolean_flag;
    const auto next_value = [&]() -> std::string {
      if (i + 1 >= argc) return {};
      return argv[++i];
    };
    // 先处理分离形式（键名完全匹配）
    if (arg == "--dir") { options.dir = next_value(); continue; }
    if (arg == "--scene") { options.scene = next_value(); continue; }
    if (arg == "--width") { options.width = std::atoi(next_value().c_str()); continue; }
    if (arg == "--height") { options.height = std::atoi(next_value().c_str()); continue; }
    if (arg == "--theme") { options.theme = next_value(); continue; }
    if (arg == "--control-port") {
      options.control_port = std::atoi(next_value().c_str());
      continue;
    }
    if (arg == "--control-file") { options.control_file = next_value(); continue; }
    if (arg == "--screenshot-dir" || arg == "--shots") {
      options.shots = next_value();
      continue;
    }
    if (arg == "--frames") { options.max_frames = static_cast<std::uint32_t>(std::atoi(next_value().c_str())); continue; }
    if (arg == "--profile-frames") { options.probe_frames = std::atoi(next_value().c_str()); continue; }
    if (arg == "--headless") { options.headless = true; continue; }
    if (arg == "--no-watch") { options.watch = false; continue; }

    // 再处理等号形式
    const auto value_of = [&](std::string_view key) -> std::string {
      if (arg.size() <= key.size() + 1) return {};
      return std::string(arg.substr(key.size() + 1));
    };
    if (arg.rfind("--dir=", 0) == 0) options.dir = value_of("--dir");
    else if (arg.rfind("--scene=", 0) == 0) options.scene = value_of("--scene");
    else if (arg.rfind("--width=", 0) == 0) options.width = std::atoi(value_of("--width").c_str());
    else if (arg.rfind("--height=", 0) == 0) options.height = std::atoi(value_of("--height").c_str());
    else if (arg.rfind("--theme=", 0) == 0) options.theme = value_of("--theme");
    else if (arg.rfind("--control-port=", 0) == 0) {
      options.control_port = std::atoi(value_of("--control-port").c_str());
    } else if (arg.rfind("--control-file=", 0) == 0) {
      options.control_file = value_of("--control-file");
    } else if (arg.rfind("--screenshot-dir=", 0) == 0) {
      options.shots = value_of("--screenshot-dir");
    } else if (arg.rfind("--shots=", 0) == 0) {
      options.shots = value_of("--shots");
    } else if (arg.rfind("--frames=", 0) == 0) {
      options.max_frames = static_cast<std::uint32_t>(std::atoi(value_of("--frames").c_str()));
    } else if (arg.rfind("--profile-frames=", 0) == 0) {
      options.probe_frames = std::atoi(value_of("--profile-frames").c_str());
    }
  }
  return options;
}

/// 一个场景文件的登记项。
struct SceneFile {
  std::string path{};
  std::string name{};
  /// 上次读到的修改时间（纳秒）；-1 表示还没读过。
  std::int64_t stamp{-1};
};

/// 列举目录下的 `.js` 场景（按文件名排序，顺序稳定才可复现）。
auto list_scenes(const std::string& dir) -> std::vector<SceneFile> {
  std::vector<SceneFile> out;
  std::error_code error;
  std::filesystem::directory_iterator iterator(dir, error);
  if (error) return out;
  for (const auto& entry : iterator) {
    if (!entry.is_regular_file(error)) continue;
    const auto extension = entry.path().extension().string();
    if (extension != ".js") continue;
    SceneFile file;
    file.path = entry.path().string();
    file.name = entry.path().stem().string();
    out.push_back(std::move(file));
  }
  std::sort(out.begin(), out.end(),
            [](const SceneFile& a, const SceneFile& b) { return a.name < b.name; });
  return out;
}

/// 示例应用的"业务状态"（留在 C++，JS 侧只做联动）。
struct LabState {
  std::vector<SceneFile> scenes{};
  std::size_t current{0};
  st::ui::ThreeView* view{nullptr};
  std::uint64_t reloads{0};
  std::string last_error{};

  [[nodiscard]] auto current_scene() const -> const SceneFile* {
    if (current >= scenes.size()) return nullptr;
    return &scenes[current];
  }

  /// 读当前场景文件并装进视口。
  ///
  /// 返回 true 表示**这次真的发生了变化**（用于决定是否重绘/续帧）。
  auto reload_current(bool force) -> bool {
    const SceneFile* scene = current_scene();
    if (scene == nullptr || view == nullptr) return false;
    if (!force) {
      // 时间戳没变就不重读：文件监听每秒几十次，每次重读+重求值会白白烧 CPU
      const auto stamp = st::fs::modified_ns(scene->path);
      if (!stamp.has_value() || *stamp == scenes[current].stamp) return false;
      scenes[current].stamp = *stamp;
    } else {
      const auto stamp = st::fs::modified_ns(scene->path);
      if (stamp.has_value()) scenes[current].stamp = *stamp;
    }

    const auto source = st::fs::read_text(scene->path);
    if (!source.has_value()) {
      last_error = std::format("读不到 {}：{}", scene->path, source.error().message);
      return false;
    }
    ++reloads;
    const bool ok = view->load_script(*source, scene->path);
    last_error = ok ? std::string() : view->last_error();
    return true;
  }

  /// 切到第 `index` 个场景（越界忽略）。
  auto select(std::size_t index) -> bool {
    if (index >= scenes.size() || index == current) return false;
    current = index;
    scenes[current].stamp = -1;   // 强制重读
    return reload_current(true);
  }
};

/// 把能力暴露给脚本层：`lab.*`。
///
/// 与 `xmlapp` 的分工同形：**数据与能力在 C++，联动逻辑在 JS**。
/// 这样界面（列表选中、提示条）不必懂三维，三维也不必懂界面。
void install_bridge(st::ui::ScriptBridge& bridge, LabState& state) {
  (void)bridge.expose("lab.scenes", [&state](const std::vector<Json>&) -> st::Result<Json> {
    Json out = Json::array();
    for (const auto& scene : state.scenes) out.push_back(Json{{"name", scene.name}});
    return out;
  });
  (void)bridge.expose("lab.current", [&state](const std::vector<Json>&) -> st::Result<Json> {
    const auto* scene = state.current_scene();
    return Json(scene != nullptr ? scene->name : std::string());
  });
  (void)bridge.expose("lab.select", [&state](const std::vector<Json>& args) -> st::Result<Json> {
    if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "lab.select 需要下标");
    const auto index = static_cast<std::size_t>(args[0].get<double>());
    return Json(state.select(index));
  });
  (void)bridge.expose("lab.reload", [&state](const std::vector<Json>&) -> st::Result<Json> {
    return Json(state.reload_current(true));
  });
  (void)bridge.expose("lab.status", [&state](const std::vector<Json>&) -> st::Result<Json> {
    const auto* scene = state.current_scene();
    Json out = Json::object();
    out["scene"] = scene != nullptr ? scene->name : std::string();
    out["index"] = state.current;
    out["count"] = state.scenes.size();
    out["reloads"] = state.reloads;
    out["error"] = state.last_error;
    if (state.view != nullptr) {
      out["objects"] = state.view->object_count();
      out["backend"] = state.view->get_property("backend").value_or("");
      out["picked"] = state.view->get_property("picked").value_or("");
      out["selected"] = state.view->get_property("selected_name").value_or("");
      out["hover_events"] = state.view->hover_events();
      out["click_events"] = state.view->click_events();
      out["distance"] = state.view->get_property("distance").value_or("");
    }
    return out;
  });
  // 相机动作：脚本/控制通道都能直接调（这也是"交互可被驱动"的前提，
  // 否则自动化只能靠注入鼠标事件，那是更脆的测试面）
  (void)bridge.expose("lab.reset_view", [&state](const std::vector<Json>&) -> st::Result<Json> {
    if (state.view == nullptr) return Json(false);
    return Json(state.view->invoke_action("reset_view", ""));
  });
  (void)bridge.expose("lab.frame", [&state](const std::vector<Json>&) -> st::Result<Json> {
    if (state.view == nullptr) return Json(false);
    return Json(state.view->invoke_action("frame_scene", ""));
  });
}

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);

  // 场景目录：默认相对可执行文件/工作目录的两个常见位置都试
  std::string scene_dir = options.dir;
  if (scene_dir.empty()) {
    for (const char* candidate : {"examples/threelab/scenes", "scenes"}) {
      if (st::fs::exists(candidate)) {
        scene_dir = candidate;
        break;
      }
    }
    if (scene_dir.empty()) scene_dir = "examples/threelab/scenes";
  }

  st::app::AppOptions app_options;
  app_options.title = "threelab · three.js 实时预览";
  app_options.width = options.width;
  app_options.height = options.height;
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = static_cast<std::uint16_t>(std::clamp(options.control_port, 0, 65535));
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.max_frames = options.max_frames;
  app_options.profile_frames = static_cast<std::uint32_t>(options.probe_frames);
  app_options.headless = options.headless;
  // 脚本宿主：本示例的界面联动靠 JS（与 xmlapp 同一形态）
  app_options.enable_script = true;

  st::app::Application app("threelab", "0.1.0", app_options);

  auto frame = std::make_unique<st::ui::WindowFrame>("threelab · three.js 实时预览");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) frame->title_bar()->set_id("titlebar");

  // ── 内容：三维视口占满，底部一条状态栏 ──
  //
  // 为什么状态栏手写在 C++ 而不是 XML：本示例的重点是**视口能力**（热重载 + 交互），
  // 把界面也塞进 XML 会把读者的注意力从"三维怎么用"引到"声明式怎么写"。
  // 声明式那条通路由 `xmlapp` 演示，不必两处都演示。
  auto root = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  root->set_id("lab-root");

  auto view = std::make_unique<st::ui::ThreeView>();
  view->set_id("viewport");
  view->set_background(st::math::Color{0x0B, 0x0F, 0x16, 0xFF});
  // 视口是页面主体：`grow` 让它吃掉纵向剩余空间（否则下面会留一段黑底）。
  //
  // 为何需要显式声明：`Panel` 的纵向布局对**不 grow 的子项**只给它的自然高，
  // 剩下的空间就空着。实测截图里就是“上半截有画面、下半截黑块”。
  // 声明式那边对应的是 `flex: 1` / `grow`。
  view->style().grow = true;
  view->set_fill_height(true);
  auto* view_ptr = view.get();
  root->add_child(std::move(view));

  auto status = std::make_unique<st::ui::Text>("初始化…");
  status->set_id("status");
  auto* status_ptr = status.get();
  root->add_child(std::move(status));

  // ## 为何要显式声明纵向“吃满”
  //
  // 实测：窗口内容区 660 高，而 `lab-root` 只量到 340（= 视口 320 + 状态栏 20）。
  // 也就是说**面板自己没吃满**，剩下的 320 像素是窗口底色——
  // 截图里表现为“画面只占上半截、下半截黑块”，看很像是视口没铺开。
  //
  // 两处都要声明：面板`grow`（占满父级剩余）+ 视口`grow`（占满面板剩余）。
  root->style().grow = true;

  // `WindowFrame` 的内容槽：子节点进它的 `content()`（与 gedou 同一形态）
  if (frame->content() != nullptr) frame->content()->add_child(std::move(root));
  app.set_content(std::move(frame));

  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败：%s\n", started.error().message.c_str());
    return 1;
  }

  // ── 业务状态 ──
  LabState state;
  state.view = view_ptr;
  state.scenes = list_scenes(scene_dir);

  st::log::out("threelab：场景目录 {}（{} 个 .js）\n", scene_dir, state.scenes.size());
  if (state.scenes.empty()) {
    st::log::out("  提示：往该目录放一个 .js（普通 three.js 代码，最后一句是 scene）即可\n");
  }

  // 选初始场景：`--scene=<名字>` 优先；其次优先打开 04-watch（精机剖面，
  // 它最能体现这套能力：程序化齿形 + 啮合位置由模数算 + 振荡动画）；
  // 都没有则取第一个。
  if (options.scene.empty()) {
    for (std::size_t i = 0; i < state.scenes.size(); ++i) {
      if (state.scenes[i].name == "04-watch") {
        state.current = i;
        break;
      }
    }
  }
  if (!options.scene.empty()) {
    for (std::size_t i = 0; i < state.scenes.size(); ++i) {
      if (state.scenes[i].name == options.scene) {
        state.current = i;
        break;
      }
    }
  }
  if (!state.scenes.empty()) {
    view_ptr->load_script("const scene = new THREE.Scene(); scene;", "<placeholder>");
    state.reload_current(true);
  }

  // ── 脚本桥 ──
  if (st::ui::ScriptHost* script = app.script(); script != nullptr && script->valid()) {
    st::ui::ScriptBridge bridge(*script, &app.root());
    install_bridge(bridge, state);
  }

  // ── 状态栏内容（每帧或状态变化时更新） ──
  std::string last_status_text;
  std::uint64_t ticks = 0;
  while (!app.quit_requested()) {
    // ## `max_frames` 要自己判
    //
    // `AppOptions::max_frames` 只在 `Application::run()` 的内部循环里生效；
    // 本应用用 `start()` + `tick()` 自驱（因为要在每帧做文件监听），
    // 那条路不会替你数帧。不自判的话 `--frames=N` 完全无效——
    // 而症状是“无头跑不停”（不是报错），CI 里会直接挂到超时。
    if (options.max_frames > 0 && ticks >= options.max_frames) break;
    const std::int64_t frame_start = st::time::now_ms();

    // 文件监听：只在开了 watch 时轮询（未开时是常数级判断）
    bool changed = false;
    if (options.watch) changed = state.reload_current(false);

    // 状态栏：把"为什么没反应"直接写出来
    const auto* scene = state.current_scene();
    std::string text;
    if (state.scenes.empty()) {
      text = std::format("场景目录为空：{}（放一个 .js 进去）", scene_dir);
    } else if (!state.last_error.empty()) {
      text = std::format("[{}] 脚本错误：{}", scene != nullptr ? scene->name : "?", state.last_error);
    } else {
      text = std::format("[{}] {} 个物体 · {} · 悬浮 {} 次 / 点击 {} 次 · 重载 {} 次", 
                         scene != nullptr ? scene->name : "?", view_ptr->object_count(),
                         view_ptr->get_property("backend").value_or("?"),
                         view_ptr->hover_events(), view_ptr->click_events(), state.reloads);
    }
    if (text != last_status_text) {
      // `Text` 的内容可原地改（不重建元素：重建会丢掉选择状态与布局缓存）
      status_ptr->set_content(text);
      last_status_text = text;
      app.request_repaint();
    }

    // ## 三维视口必须由宿主每帧**登记损坏区**
    //
    // 光调 `request_repaint()` 不够：它只说“该出一帧了”，而帧内按损坏区做
    // **局部重绘**（带视口剔除）。视口不在损坏区里就被整块跳过，
    // 里面那句 `request_animation` 也跟着不执行——于是永远没 damage、永远被跳过。
    //
    // `tick_animation()` 在**绘制之外**把损坏登记上，打断那个死锁。
    // 它返回 false（场景里没有会动的东西）时不登记，静态场景不会空转。
    if (view_ptr->tick_animation()) app.request_repaint();
    if (changed) app.request_repaint();

    app.tick();
    ++ticks;
    app.pace_loop(frame_start);
  }

  st::log::out("threelab 退出：{} 帧，场景重载 {} 次\n", static_cast<int>(ticks), state.reloads);
  return 0;
}

ST_MAIN(run_app)
