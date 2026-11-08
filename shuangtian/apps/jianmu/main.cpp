/// **建木（jianmu）** —— 三维建模与演示工作台：实时编辑并预览 **three.js / bpy** 两种脚本。
///
/// 名字取意《山海经》的建木：「众帝所自上下」——它是天地的**通道**，
/// 正合这个应用的定位：一边是建模（bpy/参数化几何），一边是演示（three.js 组装/布光）。
/// 旧名 `threelab` 只说了 three.js 那一半，所以改名。
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
/// 默认 `apps/jianmu/scenes/`，可用 `--dir` 指定。两种脚本同权：
/// · `*.js` —— **普通 three.js 代码**（组装、布光、动画、实例化）
/// · `*.py` —— **bpy 脚本**（Blender 的建模 API：布尔/倒角/挤出/参数化齿形）
/// 两者都不需要任何本框架的 API——这是整件事的关键：
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
#include "st/app/cli.hpp"
#include "st/core/string.hpp"   // parse_i64（参数取值校验）   // 通用参数解析（框架统一入口；本应用只声明特有参数）
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/raster/scene3d_script.hpp"
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
  /// 框架的通用解析结果（`--headless`/`--width`/`--height`/`--theme`/
  /// `--control-*`/`--shots`/`--frames`…全在这里）。
  ///
  /// 为何整包带进来：这些开关的语义由框架定义，抄一遍就多一份口径——本文件原来
  /// 把它们逐个手写了**两遍**（分离式 + 等号式两条 if 链），改一个开关要动四处。
  st::app::CommonOptions common{};

  /// 场景目录（`--dir`）。
  std::string dir{};
  /// 启动即打开的场景文件（`--scene`）。
  std::string scene{};
  /// 监控场景文件并在保存时热重载（缺省**开**：本应用的核心工作流就是边写边看）。
  bool watch{true};
  /// 帧剖析采样帧数（`--profile-frames`）：本应用的剖析入口，与框架的
  /// `--perf-frames` 同义但保留旧名（工具脚本在用）。
  int probe_frames{0};
};

/// 本应用特有参数表（框架通用开关**不在这里**）。
[[nodiscard]] auto jianmu_options() -> std::vector<st::app::AppOption> {
  return {
      {.name = "--dir", .value_count = 1, .help = "场景目录"},
      {.name = "--scene", .value_count = 1, .help = "启动即打开的场景文件"},
      {.name = "--watch", .value_count = 0, .help = "监控场景文件并热重载（缺省开）"},
      {.name = "--no-watch", .value_count = 0, .help = "关闭热重载"},
      {.name = "--profile-frames", .value_count = 1, .help = "帧剖析采样帧数"},
      {.name = "--screenshot-dir", .value_count = 1, .help = "--shots 的别名"},
  };
}

/// 把 `--key=value` 归一化成 `--key` `value` 两个 argv。
///
/// 为何要这一层：本应用历来**同时收**分离式与等号式。这不是习惯问题——只认等号式
/// 时框架 e2e 传的 `--control-file PATH` 被无视，控制文件永不落盘，e2e 等到 60 秒
/// 报"应用未写出控制文件"，看起来像启动失败（真事故）。
///
/// 放在入口处做，而不是让框架认识 `=`：**全仓参数口径必须一致**，多一种语法就要
/// 在每个应用、每个脚本、每份文档里多考虑一次。归一化只服务本应用的历史用法。
[[nodiscard]] auto expand_equals_forms(int argc, char** argv)
    -> std::pair<std::vector<std::string>, std::vector<const char*>> {
  std::vector<std::string> storage;
  storage.reserve(static_cast<std::size_t>(argc) * 2);
  storage.emplace_back(argc > 0 ? argv[0] : "jianmu");
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    // 只拆 `--name=value`：单个 `-` 开头或没有 `=` 的原样保留。
    const auto equals = argument.find('=');
    if (equals != std::string::npos && equals > 2 && argument[0] == '-' &&
        argument[1] == '-') {
      storage.push_back(argument.substr(0, equals));
      storage.push_back(argument.substr(equals + 1));
      continue;
    }
    storage.push_back(argument);
  }
  std::vector<const char*> pointers;
  pointers.reserve(storage.size());
  for (const std::string& item : storage) pointers.push_back(item.c_str());
  return {std::move(storage), std::move(pointers)};
}

/// 解析命令行 → `(应用特有选项, 可直接交给 Application 的 AppOptions)`。
[[nodiscard]] auto parse_options(int argc, char** argv)
    -> st::Result<std::pair<Options, st::app::AppOptions>> {
  // ⚠ 归一化结果的两个容器要**活到解析结束**：`pointers` 里存的是
  // `storage` 中每个字符串的 `data()`，提前析构就是悬垂指针。
  auto expanded = expand_equals_forms(argc, argv);
  // storage 看着没被读，但它是 pointers 里每个 data() 的**所有者**——
  // 提前析构就是悬垂指针。用 (void) 明说这是生命周期担保而非无用变量。
  const std::vector<std::string>& storage = expanded.first;
  (void)storage;
  const std::vector<const char*>& pointers = expanded.second;

  st::app::CommonOptions common;
  st::app::AppOptionValues values;
  auto parsed = st::app::parse_options(static_cast<int>(pointers.size()),
                                      const_cast<char**>(pointers.data()),
                                      jianmu_options(), common, values);
  if (!parsed) return st::forward_error(parsed.error());
  if (common.show_help) {
    st::log::out("{}", st::app::options_usage("jianmu", jianmu_options()));
    std::exit(0);
  }

  Options options;
  options.dir = values.get("--dir");
  options.scene = values.get("--scene");
  // `--watch` / `--no-watch` 是同一件事的两个方向：**后出现者胜**（从后往前找）。
  for (auto entry = values.values.rbegin(); entry != values.values.rend(); ++entry) {
    if (entry->first == "--watch") { options.watch = true; break; }
    if (entry->first == "--no-watch") { options.watch = false; break; }
  }
  if (const std::string frames = values.get("--profile-frames"); !frames.empty()) {
    auto number = st::parse_i64(frames);
    if (!number.has_value() || *number < 0) {
      return st::unexpected(st::ErrorCode::Invalid,
                            std::format("--profile-frames 的取值不是非负整数: {}", frames));
    }
    options.probe_frames = static_cast<int>(*number);
  }
  // `--screenshot-dir` 是 `--shots` 的别名：给后者作废前者的优先。
  if (const std::string alias = values.get("--screenshot-dir"); !alias.empty()) {
    common.app.screenshot_dir = alias;
  }

  st::app::AppOptions app = common.app;
  if (app.width <= 0) app.width = 1100;
  if (app.height <= 0) app.height = 760;
  if (app.theme_name.empty()) app.theme_name = "dark";
  if (app.backend.empty() && app.headless) app.backend = "headless";
  options.common = std::move(common);
  return std::make_pair(std::move(options), std::move(app));
}

/// 一个场景文件的登记项。
struct SceneFile {
  std::string path{};
  /// 展示名（**带扩展名**：`quad.js` / `quad.py`）。
  ///
  /// 为何不用裸主名：同一个场景常常有两种写法并存（`quad.js` 与 `quad.py`
  /// 逐参数同构，就是用来对比的），裸主名会让清单里出现两个 "quad"——
  /// 选中哪个无法分辨，`--scene=quad` 也只能命中前一个（实测：e2e 选 bpy 场景
  /// 却选中了 js，于是“改 .py 画面没变”看起来像热重载坏了）。
  std::string name{};
  /// 不带扩展名的主名（`quad`）——`--scene` 的宽容匹配用它。
  std::string stem{};
  /// 上次读到的修改时间（纳秒）；-1 表示还没读过。
  std::int64_t stamp{-1};
  /// 是否为**混合场景**声明（`.scene.json`）：内容是 SceneSpec 的一段 JSON，
  /// 里面用 `bpy_script` / `three_script` 引用同目录的相对路径。
  bool scene_json{false};
};

/// 列举目录下的三维场景（按文件名排序，顺序稳定才可复现）。
///
/// 三种输入都能装：
/// · `.js` —— three.js 表达层（组装/层级/材质/动画）
/// · `.py` —— bpy（Blender 的建模 API，精确建模）
/// · `.scene.json` —— **混合场景**：一小份声明它引用哪个 `.py`（产几何）与哪个 `.js`
///   （组装）。内容就是 `{"bpy_script":"a.py","three_script":"b.js"}`，
///   与裁云/探针吃的是同一套 `SceneSpec` 字段。
///
/// 三种都产出 `raster::ThreeScene`，所以下游（相机/拾取/高亮/两条渲染腿）完全同源
/// ——一个预览器同时看懂两条表达层及它们的组合，正是本应用存在的意义。
///
/// ⚠ 只认这三种（不扫 `.*`）：目录里会放着资产 JSON、说明文档、中间产物
///（`__ASSETS__` 展开结果），把它们列成“场景”会让列表变得没法用。
auto list_scenes(const std::string& dir) -> std::vector<SceneFile> {
  std::vector<SceneFile> out;
  std::error_code error;
  std::filesystem::directory_iterator iterator(dir, error);
  if (error) return out;
  for (const auto& entry : iterator) {
    if (!entry.is_regular_file(error)) continue;
    const std::string filename = entry.path().filename().string();
    const auto extension = entry.path().extension().string();
    const bool is_scene_json = filename.size() > 11 &&
                               filename.compare(filename.size() - 11, 11, ".scene.json") == 0;
    if (extension != ".js" && extension != ".py" && !is_scene_json) continue;
    SceneFile file;
    file.path = entry.path().string();
    // 混合场景的主名要去掉两段扩展名（`mixed_gears.scene.json` → `mixed_gears`）
    // 否则 `--scene=mixed_gears` 匹配不上，而“带扩展名的全名”又长得像文件。
    file.stem = is_scene_json ? filename.substr(0, filename.size() - 11)
                              : entry.path().stem().string();
    file.name = filename;
    file.scene_json = is_scene_json;
    out.push_back(std::move(file));
  }
  std::sort(out.begin(), out.end(),
            [](const SceneFile& a, const SceneFile& b) { return a.name < b.name; });
  return out;
}

/// 应用的"业务状态"（留在 C++，JS 侧只做联动）。
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
    // 按**扩展名**分派（`.py` → bpy，其余 → three.js）：分派逻辑住在
    // `ThreeView`，调用方不需要知道有几种表达层（见 `load_script_file_kind`）。
    bool ok = false;
    if (scene->scene_json) {
      // 混合场景：文件里是一份 `SceneSpec` JSON，其中 `bpy_script` / `three_script`
      // 是**相对本文件目录**的路径。先把它展开成完整脚本内容再交给视口——
      // 与裁云/探针走完全同一套 `parse_scene_spec_text`，不另写一份“混合”解释。
      ok = load_scene_json(*source, *scene);
      if (!ok) {
        // ⚠ **不要把 `last_error` 盖掉**：`load_scene_json` 把具体原因（JSON 字段错、
        // 引用文件读不到…）写进了本结构的 `last_error`，而视口的 `last_error()`
        // 在这条路上是空的。实测踩到：混合场景加载失败，状态栏却显示“无错误”，
        // 而画面留着上一个场景——现象是“切了场景但画面没变，也不报错”。
        return true;
      }
    } else {
      ok = view->load_script_file_kind(*source, scene->path);
    }
    last_error = ok ? std::string() : view->last_error();
    return true;
  }

  /// 装载 `.scene.json`：把引用到的脚本读进来填进 JSON，再交给视口。
  ///
  /// ## 为何在这里“展开”，而不是让视口认识 JSON
  ///
  /// 视口的契约是“**脚本进、场景出**”（`ThreeView::load_script*`）。让"读同目录文件"
  /// 进到视口会把文件系统带进 UI 层（而它现在完全无副作用，测试也好写）。
  /// 展开也顺带让**热重载**天然成立：被引用的 `.py`/`.js` 改了，用户回到这个
  /// 混合场景（或它自己的时间戳变了）就会重新读一遍。
  auto load_scene_json(const std::string& text, const SceneFile& scene) -> bool {
    const std::string dir = st::fs::parent(scene.path);
    // 引用的脚本相对**这个 JSON** 的目录，而不是进程工作目录——
    // 后者会“在仓库根能跑、双击就不行”。
    // 做法是用 `asset_basis`：SceneSpec 解析器会把相对路径按它解析。
    std::string json_text = text;
    if (json_text.find("\"asset_basis\"") == std::string::npos) {
      // 没写就补上（写了的尊重作者）：插在第一个 `{` 之后。
      if (const std::size_t brace = json_text.find('{'); brace != std::string::npos) {
        json_text.insert(brace + 1, std::format("\"asset_basis\":\"{}\",", dir));
      }
    }
    auto spec = st::raster::parse_scene_spec_text(json_text);
    if (!spec.has_value()) {
      last_error = std::format("{}：{}", scene.name, spec.error().message);
      return false;
    }
    // 把两个脚本文本读进 spec（解析器只认内联源码，不认路径——那是刻意的：
    // 渲染层不做 IO，也不该因为文件消失而默默渲个空场景）。
    if (!spec->bpy_script.empty()) {
      const std::string path = st::fs::is_absolute(spec->bpy_script)
                                   ? spec->bpy_script
                                   : st::fs::join(dir, spec->bpy_script);
      auto content = st::fs::read_text(path);
      if (!content.has_value()) {
        last_error = std::format("{}：读不到 {}", scene.name, path);
        return false;
      }
      spec->bpy_script = *content;
    }
    if (!spec->three_script.empty()) {
      const std::string path = st::fs::is_absolute(spec->three_script)
                                   ? spec->three_script
                                   : st::fs::join(dir, spec->three_script);
      auto content = st::fs::read_text(path);
      if (!content.has_value()) {
        last_error = std::format("{}：读不到 {}", scene.name, path);
        return false;
      }
      spec->three_script = *content;
    }
    if (spec->bpy_script.empty() && spec->three_script.empty()) {
      last_error = std::format("{}：既没写 bpy_script 也没写 three_script", scene.name);
      return false;
    }
    // 视口吃的是"脚本"这个粒度：混合场景就把两份脚本拼成一份真正的混合调用。
    // 用 `load_script_file_kind` 不行（它只认一个文件），所以直接走混合入口。
    const bool ok = view->load_mixed(spec->bpy_script, spec->three_script, scene.name);
    last_error = ok ? std::string() : view->last_error();
    return ok;
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
  auto parsed = parse_options(argc, argv);
  if (!parsed) {
    st::log::err("jianmu: {}\\n", parsed.error().to_string());
    return 2;
  }
  const Options options = parsed->first;

  // 场景目录：默认相对可执行文件/工作目录的几个常见位置都试
  //
  // `scenes` 这一档是给**单文件分发**用的：exe 与 `scenes/` 放同一目录即可。
  // （场景脚本故意不内嵌：它们要能改一行就重载，且用户就靠改它们干活。）
  std::string scene_dir = options.dir;
  if (scene_dir.empty()) {
    for (const char* candidate : {"apps/jianmu/scenes", "scenes"}) {
      if (st::fs::exists(candidate)) {
        scene_dir = candidate;
        break;
      }
    }
    if (scene_dir.empty()) scene_dir = "apps/jianmu/scenes";
  }

  // 框架的解析结果**直接**成为启动参数；下面只补本应用的默认值。
  st::app::AppOptions app_options = std::move(parsed->second);
  app_options.title = "建木 · 三维建模与演示";
  if (app_options.theme_name.empty()) app_options.theme_name = "dark";
  app_options.theme = app_options.theme_name == "dark" ? st::ui::ThemeMode::Dark
                                                       : st::ui::ThemeMode::Light;
  app_options.max_frames = options.common.max_frames;
  if (options.probe_frames > 0) {
    app_options.profile_frames = static_cast<std::uint32_t>(options.probe_frames);
  }
  // 脚本宿主：本示例的界面联动靠 JS（与 xmlapp 同一形态）
  app_options.enable_script = true;

  st::app::Application app("jianmu", "0.1.0", app_options);

  auto frame = std::make_unique<st::ui::WindowFrame>("建木 · 三维建模与演示");
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

  st::log::out("建木：场景目录 {}（{} 个脚本）\n", scene_dir, state.scenes.size());
  if (state.scenes.empty()) {
    st::log::out("  提示：往该目录放一个 .js（three.js）或 .py（bpy）即可\n");
  }

  // 选初始场景：`--scene=<名字>` 优先；其次优先打开 04-watch（精机剖面，
  // 它最能体现这套能力：程序化齿形 + 啮合位置由模数算 + 振荡动画）；
  // 都没有则取第一个。
  if (options.scene.empty()) {
    for (std::size_t i = 0; i < state.scenes.size(); ++i) {
      if (state.scenes[i].stem == "04-watch") {
        state.current = i;
        break;
      }
    }
  }
  if (!options.scene.empty()) {
    // `--scene` 接受**文件名**（`quad.py`）或**裸主名**（`quad`）。
    // 裸主名有歧义时（同名 js/py 并存）优先精确文件名——这是调用方
    // 能表达的“我要那个具体的”。
    for (std::size_t i = 0; i < state.scenes.size(); ++i) {
      if (state.scenes[i].name == options.scene) {
        state.current = i;
        break;
      }
    }
    if (state.scenes[state.current].name != options.scene) {
      for (std::size_t i = 0; i < state.scenes.size(); ++i) {
        if (state.scenes[i].stem == options.scene) {
          state.current = i;
          break;
        }
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
  // --ms 退出判据的基准时刻（框架通用开关，见循环内注释）。
  const std::int64_t started_ms = st::time::now_ms();
  while (!app.quit_requested()) {
    // ## `max_frames` 要自己判
    //
    // `AppOptions::max_frames` 只在 `Application::run()` 的内部循环里生效；
    // 本应用用 `start()` + `tick()` 自驱（因为要在每帧做文件监听），
    // 那条路不会替你数帧。不自判的话 `--frames=N` 完全无效——
    // 而症状是“无头跑不停”（不是报错），CI 里会直接挂到超时。
    if (options.common.max_frames > 0 && ticks >= options.common.max_frames) break;
    // --ms 同属框架通用开关：同样要自己判（理由同上）。
    if (options.common.max_ms > 0 &&
        st::time::now_ms() - started_ms >=
            static_cast<std::int64_t>(options.common.max_ms)) {
      break;
    }
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

  st::log::out("建木退出：{} 帧，场景重载 {} 次\n", static_cast<int>(ticks), state.reloads);
  return 0;
}

ST_MAIN(run_app)
