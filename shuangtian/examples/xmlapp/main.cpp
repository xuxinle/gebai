/// 纯 XML + JS 应用示例（xmlapp）——**零 C++ UI 代码**。
///
/// ## 这个示例检验什么
///
/// 界面**结构**写在 `ui/main.xml`，界面**逻辑**写在 `ui/app.js`；
/// 这个 C++ 文件只做三件与应用无关的事：
///
/// 1. **装配**（窗口/主题/控制通道）——所有应用都要这一段，理想形态是收进
///    `st::app` 的一个入口（见 `main()` 里 `--headless` 那一小段）；
/// 2. **暴露能力**：把 C++ 侧的真实能力（这里是内存里的任务表）经
///    `ScriptBridge::expose` 交给脚本——`app.tasks.add(…)` 那一族；
///    换成读文件/跑命令/查数据库，形状完全一样；
/// 3. **装载**：`build_from_xml_file` 建界面树 → `bridge.load(app.js)` 跑逻辑。
///
/// ## 与 gallery / gbcode / louyue 的分工
///
/// 那三个示例演示的是"用 C++（或 C++ + 声明式）写应用"的能力面；
/// 本示例演示的是**另一种开发形态**：不动 C++、只改资源文件就能改应用。
/// 它同时是 XML 通路与 JS 桥这两块框架能力的端到端回归——
/// 出问题时最先坏的就是"XML 建不出树"或"JS 调不到 C++"。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/time.hpp"
#include "st/ext/json.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using st::Json;

/// 示例的"业务状态"：一张任务表。
///
/// 为什么放在 C++ 而不是全写在 JS 里：**这才是这套架构的意义**。
/// 真实应用的数据在 C++（文件、数据库、网络），JS 只负责界面逻辑；
/// 本示例把"数据留 C++、逻辑留 JS"这条分工做成可运行的样子。
struct TaskStore {
  struct Item {
    std::string text;
    bool done{false};
  };
  std::vector<Item> items{};

  auto as_json() const -> Json {
    Json out = Json::array();
    for (const auto& item : items) out.push_back(Json{{"text", item.text}, {"done", item.done}});
    return out;
  }

  auto counts() const -> Json {
    std::size_t done = 0;
    for (const auto& item : items) {
      if (item.done) ++done;
    }
    return Json{{"total", items.size()}, {"done", done}};
  }

  /// 安装到桥：`app.tasks.*`。
  ///
  /// 注意每个函数都**返回新的全量列表**而不是原地改——JS 侧因此拿到的是一份
  /// 快照，不必与 C++ 共享可变状态（跨语言共享可变状态是缺陷温床）。
  void install(st::ui::ScriptBridge& bridge) {
    (void)bridge.expose("tasks.list", [this](const std::vector<Json>&) -> st::Result<Json> {
      return as_json();
    });
    (void)bridge.expose("tasks.count", [this](const std::vector<Json>&) -> st::Result<Json> {
      return counts();
    });
    (void)bridge.expose("tasks.add", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) {
        return st::unexpected(st::ErrorCode::Invalid, "tasks.add 需要文本");
      }
      const std::string text = st::json_as_string(args[0]);
      if (text.empty()) {
        return st::unexpected(st::ErrorCode::Invalid, "任务内容不能为空");
      }
      items.push_back(Item{text, false});
      return as_json();
    });
    (void)bridge.expose("tasks.toggle", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) {
        return st::unexpected(st::ErrorCode::Invalid, "tasks.toggle 需要下标");
      }
      const auto index = static_cast<std::size_t>(args[0].get<double>());
      if (index >= items.size()) {
        return st::unexpected(st::ErrorCode::NotFound, "下标越界");
      }
      items[index].done = !items[index].done;
      return as_json();
    });
    (void)bridge.expose("tasks.remove", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) {
        return st::unexpected(st::ErrorCode::Invalid, "tasks.remove 需要下标");
      }
      const auto index = static_cast<std::size_t>(args[0].get<double>());
      if (index >= items.size()) {
        return st::unexpected(st::ErrorCode::NotFound, "下标越界");
      }
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
      return as_json();
    });
    (void)bridge.expose("tasks.remove_done",
                        [this](const std::vector<Json>&) -> st::Result<Json> {
                          std::erase_if(items, [](const Item& item) { return item.done; });
                          return as_json();
                        });
  }
};

/// 命令行：**承 `st::app::CommonOptions` 的形态**（本示例只多一个 `--ui` 目录）。
struct Options {
  bool headless{false};
  std::string theme{"light"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::string ui_dir{"ui"};   ///< 放 main.xml 与 app.js 的目录
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
      "  --ui DIR              资源目录（main.xml 与 app.js 所在处，默认 ui）\n"
      "  --control-port N      控制通道端口（0 = 自动）\n"
      "  --control-file PATH   控制信息落盘路径\n"
      "  --shots DIR           无头截图目录\n"
      "  -h, --help            显示本帮助\n",
      program);
}

auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    const auto next = [&]() -> std::string {
      return index + 1 < argc ? std::string(argv[++index]) : std::string{};
    };
    // —— 数值参数统一走带校验的解析（承 gbcode 的同条纪律，逐字对齐）——
    //
    // 为何不能裸调 `std::stoi`：传非数字（如 `--control-port abc`）时它抛
    // `std::invalid_argument`，无人捕获 → `terminate`（实测退出码 0xC0000409，
    // 屏幕上一行 what(): stoi）——用户看不懂、脚本拿不到码。而同一命令行里的
    // `--bogus-flag` 走的是「报错 + exit 2」的干净路径。**非法值必须与未知参数
    // 同一条路径**（报错可读、退出码稳定），不能一快一崩。
    const auto parse_int = [&](const char* flag) -> long long {
      // ⚠ 与 gbcode 版的差别：本示例的 `next()` 无 fallback 形参（缺值返空串），
      // 于是缺值在此也走报错路径（gbcode 会静默用 fallback）——对"必填端口"更诚实。
      const std::string text = next();
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
        st::eprint("参数 {} 需要一个整数，得到：{}（用 --help 看可用项）\n",
                   flag, text.empty() ? "（缺值）" : text);
        std::exit(2);
      }
      return parsed;
    };
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (arg == "--headless") {
      options.headless = true;
    } else if (arg == "--theme") {
      options.theme = next();
    } else if (arg == "--control-port") {
      const long long port = parse_int("--control-port");
      if (port < 0 || port > 65535) {
        st::eprint("参数 --control-port 超出端口范围：{}\n", port);
        std::exit(2);
      }
      options.control_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--control-file") {
      options.control_file = next();
    } else if (arg == "--shots") {
      options.shots = next();
    } else if (arg == "--ui") {
      options.ui_dir = next();
    }
    // **未知参数报错，不静默忽略**（gbcode 实证过的温床：新加的开关被静默
    // 吃掉，表现是"三档渲染出来的图几乎一样"，差点被当成结论）。
    else {
      st::eprint("未知参数：{}（用 --help 看可用项）\n", arg);
      std::exit(2);
    }
  }
  return options;
}

/// 找资源目录：先看参数给的相对路径，再看可执行文件旁边的 `examples/xmlapp/ui`
/// （从仓库根跑 `st run xmlapp` 与从 `build/dev/bin` 跑都要能用）。
auto resolve_resource_dir(const std::string& requested) -> std::string {
  if (st::fs::is_regular_file(st::fs::join(requested, "main.xml"))) return requested;
  for (const char* candidate : {"examples/xmlapp/ui", "../examples/xmlapp/ui",
                                "../../examples/xmlapp/ui", "../../../examples/xmlapp/ui"}) {
    if (st::fs::is_regular_file(st::fs::join(candidate, "main.xml"))) return candidate;
  }
  return requested;   // 找不到就原样返回，让后面的报错说清路径
}

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);
  const std::string ui_dir = resolve_resource_dir(options.ui_dir);

  st::app::AppOptions app_options;
  app_options.width = 620;
  app_options.height = 620;
  app_options.title = "xmlapp · 霜天";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  // 脚本宿主只在需要时创建（默认关）——纯 C++ 应用不必付这份开销。
  app_options.enable_script = true;

  st::app::Application app("xmlapp", "0.1.0", app_options);

  // ① 建界面：**整棵树来自 XML**，这里没有一行 UI 代码。
  st::ui::XmlBuildReport report;
  auto tree = st::ui::build_from_xml_file(st::fs::join(ui_dir, "main.xml"), &report);
  if (tree == nullptr) {
    std::fprintf(stderr, "建界面失败：%s\n", report.parse_warning.c_str());
    for (const auto& tag : report.unknown_tags) {
      std::fprintf(stderr, "  认不出的标签：%s\n", tag.c_str());
    }
    return 1;
  }
  // 报告要**打印出来**：认不出的属性是"界面少了点东西"的常见原因，
  // 而静默跳过会让排查从"看日志"变成"读源码"。
  for (const auto& attr : report.unknown_attributes) {
    std::fprintf(stderr, "[xmlapp] 属性未生效：%s\n", attr.c_str());
  }
  if (!report.parse_warning.empty()) {
    std::fprintf(stderr, "[xmlapp] XML 提示：%s\n", report.parse_warning.c_str());
  }

  // ② 外壳：标题栏 + 内容槽（与其他示例同形态）。
  auto frame = std::make_unique<st::ui::WindowFrame>("xmlapp · 霜天");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    frame->title_bar()->set_icon("list");
  }
  frame->content()->add_child(std::move(tree));
  app.set_content(std::move(frame));

  // ③ 起服务（脚本宿主在 `start()` 里创建——所以桥必须在它之后建）。
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().message.c_str());
    return 1;
  }
  st::ui::ScriptHost* script = app.script();
  if (script == nullptr || !script->valid()) {
    std::fprintf(stderr, "脚本宿主未就绪（AppOptions::enable_script 打开了吗）\n");
    return 1;
  }

  // ④ 暴露 C++ 能力 + 装载 JS 逻辑。
  TaskStore store;
  st::ui::ScriptBridge bridge(*script, &app.root());
  store.install(bridge);
  const std::string script_path = st::fs::join(ui_dir, "app.js");
  if (auto status = bridge.load(script_path); !status) {
    std::fprintf(stderr, "装载 %s 失败：%s\n", script_path.c_str(), status.error().message.c_str());
    return 1;
  }
  st::print("xmlapp 就绪：界面 {}（{} 个元素）· 逻辑 {} · 桥 {} 个函数\n", ui_dir, report.elements,
            script_path, bridge.exposed().size());

  // 主循环：**承 gallery/louyue 形态**（需要每帧显式推进声明式宿主）。
  std::uint64_t frames = 0;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    if (app.root().tick_declarative_hosts() > 0U) app.request_repaint();
    app.tick();
    ++frames;
    app.pace_loop(frame_start_ms);
  }
  st::print("xmlapp 退出：{} 帧\n", frames);
  return 0;
}

}  // namespace

ST_MAIN(run_app)
