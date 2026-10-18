/// 歌白代码（gbcode）：按 VSCode 信息架构组装的**原生 GUI 代码编辑器**。
///
/// **定位（2026-10-18 起）**：从“框架演示”转为**正式可用的编辑器**。
/// 因此去掉了全部演示壳：内置样例工作区（内存假文件）、启动自动打开样例、
/// 内置 JS 逻辑（心跳/状态栏回显）。
/// 现在工作区一律来自磁盘：`--workspace <dir>`，不传就是**当前目录**。
///
/// 布局五层（自上而下）：
/// - 标题栏：文件名（脏标记）+ 应用名 + 装饰性窗口控制（— □ ×）
/// - 菜单栏：文件 / 编辑 / 选择 / 查看 / 运行 / 帮助（`MenuBar` + 下拉面板 overlay）
/// - 主体三栏：活动栏（资源管理器·搜索·变更·运行·扩展）+ 侧栏（`SplitView` 可拖）
///   + 编辑区（`Tabs` 多标签 + `CodeEditor`）
/// - 底部面板：问题 / 输出 / 终端（`Tabs` 切换）
/// - 状态栏：分支 · 错误/警告计数 · 光标 Ln,Col · 选区 · 缩进 · 编码 · 语言 · 主题
///
/// **为什么用声明式**：整份界面由 `CodeEditorPage::build()` 描述「状态对应的形态」，
/// 「改完要点哪里」的手工同步全部消失——改状态 → 下一帧重组 → 真值树按 diff 更新。
///
/// **真值树唯一**：声明式不绕过 `apply_properties` —— `tree`/`get`/`set`/`invoke`
/// 对这里的元素照常可用（协议自动化与手搭界面完全一致）。少数属性面覆盖不到的一等接口
/// （`CodeEditor::on_change`、`MenuBar` 下拉、`CommandPalette` 的命令表…）走 `custom<T>`
/// 逃生舱，不做属性面穷举。
///
/// 核心能力（均为**真实实现**，非演示）：
/// - **多标签编辑模型**：打开/切换/关闭/脏标记（`dsl::tabs` 按 key 对齐复用）
/// - **真实文件工作区**：资源树 `fs::list_dir`、点文件打开、Ctrl+S `fs::write_text`
///   真实写盘（脏标记/标题栏/状态栏全联动）
/// - **查找替换**：Ctrl+F / Ctrl+H 浮条（命中高亮、计数 n/m、环绕、替换/全部）
/// - **命令面板**：Ctrl+Shift+P（全部命令）/ Ctrl+P（快速打开文件）
/// - **问题检查**：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行（真检查，点了跳行）
/// - **LSP / Git / 终端**：clangd 诊断与补全、git 状态与 blame、真 PTY shell
/// - **进程内脚本**：`--enable-script` 开放控制通道 `script` 方法（AI 可读写界面）；
///   不再自动载入任何内置脚本
///
/// 控制通道兼容钩子（`tools/*.py` 与子代理依赖，**不可改名**）：
/// `#btn-theme`（主题）、`#editor`（活动编辑器）、`#status`（状态栏文案）、
/// `#sidebar-split`（可拖分栏）、`#sidebar`、`#activity-search`（活动栏）、
/// `#titlebar`（自绘窗框：`ui::TitleBar`，属性面 `title`、动作面 `minimize`/`maximize`/`close`）、
/// `#terminal-input` / `#terminal-output`（终端）、`#command-palette`。
///
/// 坐标系：全部逻辑像素；文本索引为 UTF-8 字节偏移且落在码点边界。

#include "battery/embed.hpp"  // 编译期资源嵌入（示例 JS 逻辑层）

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/ext/json.hpp"   // dsl::custom<T> 实例化需要 Json 完整类型（模板体里按值传 Json）
#include "st/text/diff.hpp"
#include "st/text/highlight.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/completion_popup.hpp"

#include "git_service.hpp"   // Git 集成（版本管理重构）
#include "lsp_bridge.hpp"   // 语言服务集成（LSP 阶段 3）
#include "shortcut_help.hpp"   // 快捷键单源表（注册 + 帮助卡 + 对账三面同源，BACKLOG 3982）
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/diff_view.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"  // 自绘外壳（与 gallery 同一形态）
#include "st/ui/components/tree.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/icon.hpp"
#include "st/ui/svg.hpp"   // 品牌标记：`svg_registry()` 注册（图标表只收录单色描边字形）


#include "page.hpp"
#include "page_data.hpp"
#include "session_store.hpp"   // 会话恢复（restore_session 的入参类型）

using namespace gbcode_ui;

// ════════════════════════════════════════════════════════════════════════════
// 三、入口：命令行、快捷键、主循环
// ════════════════════════════════════════════════════════════════════════════

struct Options {
  bool headless{false};
  std::string backend{};
  float scale{0.0f};
  std::string theme{"light"};
  std::string language{"cpp"};
  std::string text_lcd{"auto"};
  std::string text_fit{"auto"};
  /// 覆盖率 gamma 预校正：auto/off/数值（见 AppOptions::text_gamma）。
  ///
  /// 与 `--text-lcd`/`--text-fit` 同一姿态：应用层有自己的参数解析，
  /// 框架侧的 `st::app::parse_cli` 到不了这里——**漏接就是静默忽略**。
  std::string text_gamma{"auto"};
  std::string text_gamma_small{};
  /// 拟合的适用字号上限（物理 px；auto = 不限）。见 `AppOptions::text_fit_max_size`。
  std::string text_fit_max_size{"auto"};
  /// 逐字形类的覆盖率 gamma（auto = 内置默认档）。见 `AppOptions::text_digit_gamma`。
  std::string text_digit_gamma{"auto"};
  std::string text_letter_gamma{"auto"};
  std::string text_han_gamma{"auto"};
  /// 界面字号缩放：auto/数值（见 `AppOptions::ui_font_scale`）。
  ///
  /// 与 `--text-*` 同一姿态：应用层有自己的参数解析，框架侧的
  /// `st::app::parse_cli` 到不了这里——**漏接就是静默忽略**。
  std::string ui_font_scale{"auto"};
  /// 编辑器字号档位（`--editor-font-scale`）：相对正文 `font_base` 的倍数，auto = 用内置默认。
  ///
  /// 与 `--ui-font-scale` 的区别：后者按主题整体缩放（`Theme::scaled`，影响**所有**文字），
  /// 本项只动编辑器（代码区与行号）。两者可叠加。
  std::string editor_font_scale{"auto"};
  /// 编辑器**行距倍数**（`--editor-line-spacing`）：auto = 组件默认。
  std::string editor_line_spacing{"auto"};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::uint32_t frames{0};
  int max_ms{0};
  bool enable_script{false};
  /// 请求系统标题栏/边框（默认关：窗框一律自绘）。仅给排查用。
  bool decorations{false};
  /// 工作区目录：资源管理器/打开/保存全部走 `st::fs` 真实读写。
  /// 缺省 = 当前工作目录（正式形态：不再回退内存样例）。
  std::string workspace{};
  /// `--open <path>`：启动即打开的文件。
  std::string open_path{};
  /// 霜天仓库根（`--tool-root`）：跑任务/构建时找 `st` 工具链用。
  std::string tool_root{};
};

/// 用法说明。**必须有**：未知参数会报错退出，没有 `--help` 就等于"报错了也没处查"。
auto print_usage(std::string_view program) -> void {
  st::print(
      "用法: {} [选项]\n"
      "\n"
      "窗口/主题\n"
      "  --headless            无头模式（无窗口；控制通道照常可用）\n"
      "  --scale N             DPI 缩放（如 2.0）\n"
      "  --theme light|dark    主题\n"
      "  --decorations         用系统标题栏（默认自绘）\n"
      "\n"
      "字体与字形（观感调参）\n"
      "  --editor-font-scale N 编辑器字号**档位**（相对正文的倍数，默认 1.0 = 与正文同级）\n"
      "  --editor-line-spacing N 编辑器行距倍数（相对字体自然行高，默认 1.15）\n"
      "  --ui-font-scale N     界面整体字号缩放（auto = 跟随系统/默认；影响所有文字）\n"
      "  --text-fit-max-size N 拟合的适用字号上限（物理 px，0 = 不限）\n"
      "  --text-digit-gamma V  数字类的覆盖率 gamma（auto = 内置默认）\n"
      "  --text-letter-gamma V 字母类的覆盖率 gamma（auto = 内置默认）\n"
      "  --text-han-gamma V    汉字类的覆盖率 gamma（auto = 内置默认）\n"
      "  --text-fit MODE       网格拟合：auto / off / light / normal\n"
      "  --text-lcd MODE       亚像素抗锯齿：auto / on / off\n"
      "  --text-gamma V        覆盖率 gamma（auto / off / 数值）\n"
      "  --text-gamma-small V  小字号的覆盖率 gamma（同上）\n"
      "\n"
      "内容/自动化\n"
      "  --language NAME       初始语言（默认 cpp；--list-languages 可列）\n"
      "  --workspace DIR       工作区目录（缺省 = 当前目录）\n"
      "  --tool-root DIR       霜天仓根（PATH 无 st 时到它下面 build/bin/st 找；跑任务用）\n"
      "  --control-port N      控制通道端口（0 = 自动）\n"
      "  --control-file PATH   控制信息落盘路径\n"
      "  --shots DIR           无头截图目录\n"
      "  --frames N / --ms N   跑够 N 帧 / N 毫秒后退出\n"
      "  --enable-script       开启进程内脚本能力（默认关）\n"
      "  -h, --help            显示本帮助\n",
      program);
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view raw = argv[index];
    const auto value = [&](std::string fallback) {
      return index + 1 < argc ? std::string(argv[++index]) : std::move(fallback);
    };
    // —— 数值参数统一走带校验的解析 ——
    //
    // 为何不能裸调 `std::stod`/`stoi`：传非数字（如 `--scale abc`）时它们抛
    // `std::invalid_argument`，无人捕获 → `terminate`（实测退出码 0xC0000409，
    // 屏幕上一行 what(): stod）——用户看不懂、脚本拿不到码。而同一命令行里的
    // `--bogus-flag` 走的是「报错 + exit 2」的干净路径。**非法值必须与未知参数
    // 同一条路径**（报错可读、退出码稳定），不能一快一崩。
    const auto parse_double = [&](const char* flag, double fallback) -> double {
      const std::string text = value(std::format("{}", fallback));
      double parsed = 0.0;
      std::size_t consumed = 0;
      const bool ok = !text.empty() && [&] {
        try {
          parsed = std::stod(text, &consumed);
        } catch (const std::exception&) {
          return false;
        }
        return consumed == text.size();   // 尾巴（如 `1.5x`）也是非法值
      }();
      if (!ok) {
        st::eprint("参数 {} 需要一个数值，得到：{}（用 --help 看可用项）\n", flag, text);
        std::exit(2);
      }
      return parsed;
    };
    const auto parse_int = [&](const char* flag, long long fallback) -> long long {
      const std::string text = value(std::format("{}", fallback));
      long long parsed = 0;
      std::size_t consumed = 0;
      const bool ok = !text.empty() && [&] {
        try {
          parsed = std::stoll(text, &consumed);
        } catch (const std::exception&) {
          return false;
        }
        return consumed == text.size();
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
    // `--headless` 必须接在 else-if 链的链首：独立 if 会在设置 headless 后
    // 继续落入链尾的「未知参数」分支（实测 `--headless` 报 exit 2，
    // 而所有 gbcode e2e 的启动参数都以它开关头——整条 e2e 生态全灭）。
    if (raw == "--headless") options.headless = true;

    // `--backend` 必须照接：应用层有**自己的**参数解析（不过 `st::app::parse_cli`），

    // 漏接的开关会被静默忽略——"我明明指定了 x11/wayland"会变成谜案

    //（Linux 上验证两个窗口后端全靠它）。

    else if (raw == "--backend") options.backend = value("");
    else if (raw == "--enable-script") options.enable_script = true;
    // `--scale` 夹到 [0.1, 10]：0 会让一切尺寸归零（窗口/字号/命中区全灭），
    // 负数在多数后端是非法 DPI。夹取而非报错——意图明确（想大想小），只是数值过头。
    else if (raw == "--scale") {
      options.scale = static_cast<float>(std::clamp(parse_double("--scale", 1.0), 0.1, 10.0));
    }
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--language") options.language = value("cpp");
    else if (raw == "--text-lcd") options.text_lcd = value("auto");
    else if (raw == "--text-fit") options.text_fit = value("auto");
    else if (raw == "--text-fit-max-size") options.text_fit_max_size = value("auto");
    else if (raw == "--text-digit-gamma") options.text_digit_gamma = value("auto");
    else if (raw == "--text-letter-gamma") options.text_letter_gamma = value("auto");
    else if (raw == "--text-han-gamma") options.text_han_gamma = value("auto");
    else if (raw == "--text-gamma") options.text_gamma = value("auto");
    else if (raw == "--text-gamma-small") options.text_gamma_small = value("auto");
    else if (raw == "--ui-font-scale") options.ui_font_scale = value("auto");
    else if (raw == "--editor-font-scale") options.editor_font_scale = value("auto");
    else if (raw == "--editor-line-spacing") options.editor_line_spacing = value("auto");
    else if (raw == "--control-port") {
      const long long port = parse_int("--control-port", 0);
      if (port < 0 || port > 65535) {
        st::eprint("参数 --control-port 超出端口范围：{}\n", port);
        std::exit(2);
      }
      options.control_port = static_cast<std::uint16_t>(port);
    }
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--shots") options.shots = value({});
    else if (raw == "--frames") {
      const long long frames = parse_int("--frames", 0);
      if (frames < 0) {
        st::eprint("参数 --frames 不能为负：{}\n", frames);
        std::exit(2);
      }
      options.frames = static_cast<std::uint32_t>(frames);
    }
    else if (raw == "--ms") {
      const long long ms = parse_int("--ms", 0);
      if (ms < 0) {
        st::eprint("参数 --ms 不能为负：{}\n", ms);
        std::exit(2);
      }
      options.max_ms = static_cast<int>(ms);
    }
    else if (raw == "--workspace") options.workspace = value(".");
    // `--open <path>`：启动即打开一个文件（编辑器历来都有这条——`code file.cpp`）。
    // 缺它时“打开某个文件”只能靠 UI 点，自动化与命令行使用都不方便。
    else if (raw == "--open") options.open_path = value({});
    else if (raw == "--tool-root") options.tool_root = value("");
    else if (raw == "--decorations") options.decorations = true;
    // 诊断开关（--log-file/--log-level/--crash-dir）：由 `startup_configure_diagnostics`
    // 在 `main` 更早处生效（才能盖住启动早期日志）。这里只“认识它”并把值吃掉，
    // 否则下面那句“未知参数”会把整个命令行拒掉——实测踩到：
    // 加了这些开关后 `--log-file X` 直接 exit 2。
    else if (raw == "--log-file" || raw == "--log-level" || raw == "--crash-dir") {
      (void)value("");
    }
    // **未知参数报错，不静默忽略**：本应用曾经自带一份参数解析器（不走
    // `parse_common_options`），于是新加的开关被静默吃掉——
    // 表现是"三档渲染出来的图几乎一样"（实测 B vs C 只差 20 像素），
    // 差点被当成"三档观感接近"的结论。静默忽略未知参数正是这类事故的温床。
    else {
      st::eprint("未知参数：{}（用 --help 看可用项）\n", raw);
      std::exit(2);
    }
  }
  return options;
}

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);
  register_custom_language();

  st::app::AppOptions app_options;
  app_options.width = 1280;
  app_options.height = 800;
  app_options.scale = options.scale;
  app_options.title = "歌白代码 · 霜天";
  app_options.headless = options.headless;
  // 窗框一律自绘：窗口不带系统标题栏（`CONVENTIONS.md` §10 第 7 条）。
  // 装饰开关仍留给宿主——`--decorations` 是给"就想看系统窗框"的排查场景用的。
  app_options.decorations = options.decorations;
  app_options.backend = !options.backend.empty() ? options.backend
                        : (options.headless ? std::string("headless") : std::string{});
  // 脚本能力显式开启：默认关闭，控制通道的 `script` 方法仅在开启后可用
  app_options.enable_script = options.enable_script;
  app_options.text_lcd = options.text_lcd;
  app_options.text_fit = options.text_fit;
  app_options.text_gamma = options.text_gamma;
  app_options.text_gamma_small = options.text_gamma_small;
  app_options.text_fit_max_size = options.text_fit_max_size;
  app_options.text_digit_gamma = options.text_digit_gamma;
  app_options.text_letter_gamma = options.text_letter_gamma;
  app_options.text_han_gamma = options.text_han_gamma;
  app_options.ui_font_scale = options.ui_font_scale;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.theme = options.theme == "dark" ? ThemeMode::Dark : ThemeMode::Light;

  // —— 品牌资产（编译期嵌入，两处各取所需）——
  //
  // ① 窗口/任务栏图标：PNG 字节交给 `AppOptions`，由 `Application` 在建窗后
  //    调 `Backend::set_window_icon`（后端不支持时只记告警）。
  // ② 标题栏左上角的品牌标记：SVG 注册进 `svg_registry()`（下面再做，
  //    但**必须在首帧渲染之前**）。
  //
  // 两份不是重复：一个是给**窗口系统**的位图（要 PNG，系统 API 只吃位图），
  // 一个是给**自绘标题栏**的矢量（要清晰，任意 DPI 下重新光栅化）。
  // 它们由 `tools/gebai_logo_probe.cpp` 从同一份 `assets/gbcode-logo.svg` 导出。
  {
    const auto icon = b::embed<"examples/gbcode/assets/gbcode-icon.png">();
    app_options.window_icon_png.assign(icon.data(), icon.data() + icon.length());
  }

  st::app::Application app("gbcode", "0.1.0", app_options);

  // —— 品牌标记（标题栏左上角）——
  //
  // **必须在首帧渲染之前注册**：SVG 图标注册表是显式装载的（没有静态初始化器），
  // 而 `TitleBar` 只在重绘时才向它取图形；晚一步注册不会报任何错，
  // 只表现为“图标就是不出来”（实测：写在 `app.render_frame()` 之后，
  // 界面已画完且不再标脏，等再久也看不见）。
  //
  // 来源是**框架仓级**的 `examples/assets/gebai-logo.svg`（与 Web 端 favicon 同一记号）：
  // 品牌资产跳应用共享，不在 gbcode 自己的 `assets/` 里再存一份。
  {
    const auto logo = b::embed<"examples/gbcode/assets/gbcode-logo.svg">();
    const std::string_view source(logo.data(), logo.length());
    if (!st::ui::svg_registry().add_single("gbcode-logo", source)) {
      st::print("品牌标记注册失败（{} 字节）\n", logo.length());
    }
  }
  // 窗口/任务栏图标：同一记号的 PNG 版本（由 `gebai_logo_probe` 从上面的 SVG 导出），
  // 交给后端 `set_window_icon`。

  // 声明式页面（整个 IDE 是一个 Component：内容槽里的一切由 `build()` 描述）。
  // 字号档位/行距倍数：非 auto 时必须也是「非法值报错 + exit 2」而不是
  // `stod` 裸抛（与 parse_options 里的口径一致——两处都是用户可传入的数值）。
  const auto parse_late_double = [](const char* flag, const std::string& text,
                                   [[maybe_unused]] double fallback) -> double {
    double parsed = 0.0;
    std::size_t consumed = 0;
    const bool valid = !text.empty() && [&] {
      try {
        parsed = std::stod(text, &consumed);
      } catch (const std::exception&) {
        return false;
      }
      return consumed == text.size();
    }();
    if (!valid) {
      st::eprint("参数 {} 需要一个数值，得到：{}（用 --help 看可用项）\n", flag, text);
      std::exit(2);
    }
    return parsed;
  };
  // 工作区缺省 = **当前工作目录**（正式形态）。
  //
  // 为何不留在空串：空串在过去意味着“进内置样例模式”（内存假文件）。
  // 样例表已删，空串现在只会得到一个空资源管理器——而“双击启动就站在
  // 当前目录”是编辑器的正常预期（`code .` 同款语义）。
  //
  // ⚠ `explicit_workspace` 必须单独记：下面“是否恢复上次会话”依赖
  // **用户有没有显式指定**，而不是“最终工作区是不是空的”。把两者混为一谈
  // 会让“无参数启动”永远不恢复会话（那是没人要的行为）。
  const bool explicit_workspace = !options.workspace.empty();
  std::string workspace = options.workspace;
  if (workspace.empty()) {
    auto cwd = st::fs::current_dir();
    if (cwd && !cwd->empty()) workspace = *cwd;
  }
  float editor_font_scale = kEditorFontScale;
  if (options.editor_font_scale != "auto") {
    editor_font_scale = static_cast<float>(
        parse_late_double("--editor-font-scale", options.editor_font_scale, kEditorFontScale));
  }
  float editor_line_spacing = CodeEditor::kDefaultLineSpacing;
  if (options.editor_line_spacing != "auto") {
    editor_line_spacing = static_cast<float>(parse_late_double(
        "--editor-line-spacing", options.editor_line_spacing, CodeEditor::kDefaultLineSpacing));
  }
  auto page = std::make_shared<CodeEditorPage>(workspace, options.tool_root,
                                               editor_font_scale, editor_line_spacing);
  // **命令行 --workspace 也要告知语言服务**：`switch_workspace()`（UI 对话框路径）
  // 会调 `lsp_.set_workspace`，而命令行启动这条路从没调过——后果是 clangd 拿着
  // 空 rootPath 起（无 compile_commands.json 可寻、相对 include 解不开），
  // 诊断永远不回（实测：真仓库 + clangd 在 PATH，打开 .cpp 等 25s 仍 0 诊断，
  // 而 clangd 进程活着——BACKLOG「clangd 已装但语言服务从未就绪」的真根因之一）。
  if (!workspace.empty()) page->set_language_workspace(workspace);
  // —— 会话（最近打开 + 关窗恢复）——
  //
  // 路径在构造后定（`session_path_` 是普通成员，不在构造列表里——两个构造重载
  // 不值得为它各改一遍）。`ST_HOME` 可重定向（e2e 用它把会话文件关进临时目录）。
  page->session_path_ = gbcode::default_session_path();
  // 会话状态分两层，载入顺序很重要（后面的赋值会覆盖前面）：
  // ① 先从会话文件拿「最近清单」——它是跨会话记忆，无论哪种启动方式都该在；
  // ② 再把本次的 `--workspace` 头插进 recent_folders（命令行打开也算一次
  //    「打开文件夹」，switch_workspace 的埋点只覆盖 UI 对话框路径）。
  // 标签/工作区恢复只在**无 --workspace 且无 --open** 时做（命令行意图优先于
  // 上次会话——VSCode 同语义；恢复不动 UI，pending 在首帧后落地）。
  {
    const gbcode::SessionState saved = gbcode::load_session(page->session_path_);
    page->recent_files_ = saved.recent_files;
    page->recent_folders_ = saved.recent_folders;
    if (explicit_workspace) {
      gbcode::push_recent(page->recent_folders_, workspace);
    } else if (options.open_path.empty() && page->restore_session(saved)) {
      st::print("已恢复上次会话（工作区 {}，{} 个标签）\n", saved.workspace,
                saved.files.size());
    }
  }
  // `--open <path>`：**不在构造期改状态**。
  //
  // 为何：在声明式构建期写 `State` 会触发递归重组（日志实测：`[dsl] 构建期写入
  // State…已延迟到帧末落地`），而那一延迟让“当前标签”与“缓冲区表”错开一帧——
  // 编辑器最终装回了**另一个文件的内容**（标签高亮 sample.cpp、正文是 renderer.cpp）。
  // 改成把路径记下来、**第一帧之后**再打开（那时宿主已就绪，时序干净）。
  if (!options.open_path.empty()) page->set_pending_open_path(options.open_path);
  // 主题的两个口子（**同一处装配**、依赖方向依然是单向的）：
  // 读 = `UiRoot::theme()`（真值源）；写 = `Application::set_theme_mode`
  // （它会连带处理字号缩放与文本 gamma——直接改 `root().theme()` 会把
  // `--ui-font-scale` 调好的字号悄悄还原）。
  page->theme_mode = [&app] { return app.root().theme().mode(); };
  page->theme_setter = [&app](ThemeMode mode) { app.set_theme_mode(mode); };
  // 外壳：`WindowFrame`（与 gallery 同一形态：标题栏 + 内容槽 + 八向缩放边缘）。
  // 为什么由应用装配而不是写进 `build()`：窗框是**外壳**（也是窗口唯一的拖动/缩放区），
  // 应当先于声明式内容存在、且不随页面重组而重建；页面只把标题推给它。
  auto frame = std::make_unique<WindowFrame>("歌白代码 · 霜天");
  frame->set_id("window-frame");
  frame->set_window_control(&app);       // 三控制按钮 + 边缘条接真实后端
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    // 品牌标记：歌白 logo（SVG 源，上面已注册为 `gebai-logo`），画在标题栏最左。
    frame->title_bar()->set_icon("svg:gbcode-logo");
  }
  // 内容挂进**窗框内容槽**（`mount_into` 的子树语义正好：声明式只占内容槽）。
  // 指针在 `set_content` 搬移前取好——之后从根部按 id 取回（与容器无关、更稳）。
  Element* slot = frame->content();
  // 菜单栏挂进**标题栏的前部槽**：标题栏与菜单栏合并成一行（菜单在左、标题跟在其后）。
  // 同样在 `set_content` 搬移前取好指针；两个 `mount_into` 各占一处树位，
  // 都属同一个声明式页面（`page`）。
  Element* menu_slot = frame->title_bar();
  app.set_content(std::move(frame));
  page->frame_ = dynamic_cast<WindowFrame*>(app.root().find("window-frame"));
  auto host = dsl::mount_into(app.root(), *slot, page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  // 异步统一走 app::Tasks（主线程只渲染契约）：resource 的 fetcher 不再养私池。
  host->composer().set_executor([tasks = &app.tasks()](std::function<void()> job) {
    tasks->submit(std::move(job));
  });
  // 菜单头部：单独一个 `Component`（标题栏那一行归它描述），挂进 `leading` 槽。
  // 先 `add_leading` 一个占位容器，再 `mount_into` 它——`mount_into` 需要
  // 一个**既有元素**作宿主，而 `leading` 槽按语义是「标题前的一块」。
  if (menu_slot != nullptr) {
    auto menu_block = std::make_unique<Panel>(FlexDirection::Row);
    menu_block->set_id("menu-block");
    Element* menu_root = page->frame_->title_bar()->add_leading(std::move(menu_block));
    auto header = std::make_shared<CodeEditorPage::MenuHeader>();
    header->page = page.get();
    page->menu_host_owned_ = dsl::mount_into(app.root(), *menu_root, header);
    if (page->menu_host_owned_ == nullptr) {
      std::fprintf(stderr, "菜单头部挂载失败\n");
      return 1;
    }
    page->menu_host_ = page->menu_host_owned_.get();
  }
  // 启动时的初始文件：
  // - 会话恢复已装回真文件 → 不再插手（旧实现会再 open_sample 把活动项夺走，
  //   实测表现为“恢复后活动标签被 renderer.cpp 覆盖、标题栏显示错文件”）。
  // - 否则：若指定了 `--open`，它由页面的帧首泵打开；除此之外不主动开任何文件
  //   ——正式形态下“启动即打开一个凭空造的文件”是演示行为，不是产品行为。
  //   想看内容就用资源管理器 / Ctrl+P / `--open`。
  // 首帧重组 + 聚焦编辑器：**必须在 `start()` 之前完成**。
  // `start()` 内会开控制通道，而客户端（脚本）一看到控制文件就连——
  // 那时如果界面还没建好（或焦点还没设），它拿到的是半成品状态
  // （实测踩到：`st_editor_smoke.py` 断言“启动即聚焦”，偶发失败）。
  (void)host->tick();
  // **重组错误必须可见**：DSL 单根契约被破坏、护栏报表、或作用域预算耗尽时，
  // 界面会静默地少一块——那时看截图只能看到“一片空白”，完全不知从哪里查。
  // 把 `ReconcileStats` 的结论打到 stderr（日志文件里有），排障就不需要再猜。
  {
    const dsl::ReconcileStats& stats = host->stats();
    if (!stats.error.empty()) std::fprintf(stderr, "[gbcode] 声明式重组错误：%s\n", stats.error.c_str());
    if (stats.budget_exceeded) std::fprintf(stderr, "[gbcode] 重组超帧预算（%.1fms），剩余作用域顺延\n", 4.0);
    for (const auto& collision : stats.key_collisions) {
      std::fprintf(stderr, "[gbcode] 重名 key：%s\n", collision.c_str());
    }
  }
  app.root().set_focus(page->editor);
  app.root().mark_dirty_all();

  // —— 全局快捷键（注册到 UiRoot：先于焦点链派发，文本组件吞键也拦得住）——
  //
  // **键位/修饰键/守卫口径全部来自单源表** `shortcut_help.hpp`（kShortcutSpecs）：
  // 这里只给「按了做什么」（动作返回值 = 消费与否），键位不再手抄第二份——
  // 帮助卡（page_dialogs.cpp）与对账测试（gbcode_shortcut_card_test.cpp）
  // 从同一张表/同一份注册表读，「表与代码分叉」从机制上消灭（BACKLOG 3982）。
  //
  // **除 Esc 外的一律先让路给浮层**（`LetOverlayDecide` 形态）：浮层是当前交互的
  // “前台”（对话框、查找条、命令面板、转到行、菜单面板），用户在浮层里的按键
  // 不应被编辑区快捷键抢走。具体的坑：浮层带输入框时，Ctrl+S/Ctrl+D 这类
  // “编辑器动作”会在输入框收到字符之前就被根级处理器吃掉——用户以为自己
  // 只是在浮层里打字。
  //
  // **Consume 形态**（无 overlay 守卫）的键位与原因见各动作处的注释
  //（补全弹层自己要用的键不能被守卫挡；F7 族无差异视图时让路；Esc 无事可关
  // 时让路——动作自己的返回值表达，包装层不越权）。
  {
    UiRoot* root = &app.root();
    const auto overlay_open = [root]() -> bool { return root->overlay_count() > 0; };
    // 按 id 注册（键位/修饰键/守卫从表来）：失败 = id 拼错或 Never 条被误用——
    // 都是调用方 bug，会被启动对账（下方 shortcut_drift）的 missing 名单当场
    // 点名，这里不再逐条判返回值。
    const auto bind = [&root, &overlay_open](std::string_view id,
                                            std::function<bool()> action) {
      (void)register_shortcut_by_id(*root, id, std::move(action), overlay_open);
    };
    bind("save", [page] { page->save(); return true; });
    bind("save-as", [page] { page->save_as(); return true; });
    // 全部保存 Ctrl+Alt+S：为何不是 VSCode 的 Ctrl+K S（和弦）——本框架没有
    // 和弦键位机制（与 Ctrl+Shift+O 同因），单键可发现性反而更好，且
    // Ctrl+S / Ctrl+Shift+S 已占，附上 Alt 是唯一不冲突的档位。
    bind("save-all", [page] { page->save_all(); return true; });
    bind("close", [page] { page->close_active(); return true; });
    bind("open", [page] { page->open_file_dialog(); return true; });
    // Ctrl+Shift+O：打开文件夹（换工作区）。与 VSCode 的 Ctrl+K Ctrl+O 同义，
    // 用单键是因为本框架没有和弦键位机制——单键的可发现性反而更好。
    bind("open-folder", [page] { page->open_folder_dialog(); return true; });
    bind("new", [page] { page->new_file_request(); return true; });
    // Ctrl+T：工作区符号（跨文件搜符号）。VSCode 同键位。
    bind("symbols", [page] { page->symbols_request(false); return true; });
    bind("toggle-sidebar", [page] {
      page->sidebar_visible_.set(!page->sidebar_visible_.value());
      return true;
    });
    // Ctrl+J：切换底部面板（与 Ctrl+` 同一条路径，后者另收 Backquote 布局）。
    bind("toggle-bottom", [page] { page->toggle_terminal_panel(); return true; });
    // Ctrl+Shift+V：Markdown 预览/源码切换（VSCode 同键位；非 md 文件上如实提示）。
    bind("md-preview", [page] { page->toggle_md_preview(); return true; });
    // Ctrl+` / Ctrl+Backquote（VSCode 的终端默认键位）：与活动栏那个按钮同一条
    // 路径。`` ` `` 在不同键盘布局上键名不一，两种都收（表里 alias 两条）。
    bind("toggle-terminal", [page] { page->toggle_terminal_panel(); return true; });
    bind("toggle-terminal-alt", [page] { page->toggle_terminal_panel(); return true; });
    bind("help", [page] {
      page->shortcuts_open_.set(!page->shortcuts_open_.value());
      return true;
    });
    // F2：重命名符号；Shift+Alt+F：格式化（都与 VSCode 同键位）。
    bind("rename", [page] { page->rename_request(); return true; });
    bind("format", [page] { page->format_request(); return true; });
    // —— 差异视图导航（F7 / Shift+F7，与主流 IDE 同键位）——
    // “下一个差异 / 上一个差异”是差异视图最常用的两个动作，必须有键盘入口：
    // 否则就只能靠鼠标滚——而“一个差异一个差异地看”本来就是顺序行为。
    // 无差异视图时返回 false 让路（原行为：不能消费不属于自己场景的键）。
    bind("next-hunk", [page] {
      if (page->diff_view_ptr == nullptr) return false;
      page->diff_view_ptr->next_hunk();
      return true;
    });
    bind("prev-hunk", [page] {
      if (page->diff_view_ptr == nullptr) return false;
      page->diff_view_ptr->previous_hunk();
      return true;
    });
    // F12：跳定义；Shift+F12：找引用；Alt+F12：声明；Ctrl+F12：实现；
    // Alt+Left：返回上一个光标位置（浏览器同源的“返回”）。
    // 键位对齐 VSCode 的 F12 家族直觉；同键位族靠修饰键区分（登记名同为 f12）。
    bind("definition", [page] { page->definition_request(); return true; });
    bind("references", [page] { page->references_request(); return true; });
    bind("declaration", [page] { page->declaration_request(); return true; });
    bind("implementation", [page] { page->implementation_request(); return true; });
    bind("navigate-back", [page] { page->navigate_back_request(); return true; });
    // Ctrl+Space：手动触发补全（VSCode/IDEA 的通用键位）。
    //
    // ⚠ 这里**不能装 `overlay_open` 守卫**（表的 reg=Consume）：用户按 Ctrl+Space
    // 时补全弹层通常已经开着（想重新触发/换一批候选），走守卫就变成“按了没反应”。
    bind("completion", [page] { page->completion_request(); return true; });
    // Ctrl+G：真的“转到行…”浮层（与选择菜单那一条同一条链路）。
    bind("goto-line", [page] { page->open_goto_line(); return true; });
    // Ctrl+D：选中下一处同词。
    //
    // 为何要**重复**注册（组件 `handle_key` 里已经处理过一遍）：这组键在编辑器
    // 未聚焦时（刚点过侧栏/终端）也应当生效，而全局表先于焦点链派发——
    // 焦点在编辑器时先被根吃下，不会出现“一次按键选两处”。
    bind("select-next-occurrence", [page] {
      if (page->editor == nullptr) return true;   // 无编辑器：消费但无事可做（原行为）
      // 状态栏如实报“选了哪个词 / 找不到下一处”——两者都不是错误，但必须能区分。
      if (page->editor->select_next_occurrence()) {
        page->status_.set("已选中「" + page->editor->selected_text() + "」");
      } else {
        page->status_.set("没有下一处同词");
      }
      return true;
    });
    // Alt+↑/↓：上/下移当前行。根级登记名 "alt-up"/"alt-down" 与后端键名
    // （ArrowUp/ArrowDown）不匹配——真移行是 CodeEditor 自己的 Alt+方向键分支；
    // 这两条是编辑器未聚焦时的备份（靠动作里的 editor->move_lines 兜底），
    // 从未以根级身份被派发命中过。原样保留（键位归一另单处理，见表头注释）。
    bind("move-line-up", [page] {
      if (page->editor != nullptr && page->editor->move_lines(-1)) {
        page->status_.set("已上移当前行");
      }
      return true;
    });
    bind("move-line-down", [page] {
      if (page->editor != nullptr && page->editor->move_lines(1)) {
        page->status_.set("已下移当前行");
      }
      return true;
    });
    // Ctrl+Shift+E / F / G：切到三个主视图（与歌白文件工作台同一套键位）。
    bind("view-explorer", [page] {
      page->activity_.set(0);
      page->sidebar_visible_.set(true);
      page->mark_sidebar_visited(0);
      return true;
    });
    bind("view-search", [page] {
      page->activity_.set(1);
      page->sidebar_visible_.set(true);
      page->mark_sidebar_visited(1);
      return true;
    });
    bind("view-git", [page] {
      page->activity_.set(2);
      page->sidebar_visible_.set(true);
      page->mark_sidebar_visited(2);
      page->refresh_git_request();
      return true;
    });
    bind("palette", [page] { page->open_palette(false); return true; });   // Ctrl+Shift+P：命令表
    bind("quick-open", [page] { page->open_palette(true); return true; }); // Ctrl+P：快速打开文件
    bind("find", [page] { page->open_find(); return true; });              // Ctrl+F：查找
    bind("replace", [page] { page->open_find(); return true; });           // Ctrl+H：查找替换
    bind("next-tab", [page] {                                              // Ctrl+Tab：循环切标签
      const auto count = page->buffers_.value().size();
      if (count > 1) page->active_.set((page->active_.value() + 1) % count);
      return true;
    });
    // F8：跳到当前文件的第一处问题（与 VSCode 的“下一处问题”同语义）。
    bind("next-problem", [page] {
      page->refresh_problems();
      if (page->problems_.empty()) {
        page->status_.set("没有问题可跳转");
        return true;
      }
      page->jump_to_problem_request(0);
      return true;
    });
    // Esc 关闭查找条与转到行浮层（`Input` 不认 Esc，用根级快捷键；**只在自己
    // 可见时接管**，免得吃掉别的场景的 Esc——比如命令面板自己要用的那个）。
    //
    // 为何两条要排在一起：它们是同一类“小浮层”，Esc 是它们共同的出口。
    // 只给查找条接 Esc 会让另一个变成“关不掉的浮层”（实测：转到行浮层开着时
    // 连 Ctrl+F 都打不开——快捷键被“浮层优先”让路，而它自己又不认 Esc）。
    bind("escape", [page]() {
      if (page->goto_line_open_.value()) {
        page->goto_line_open_.set(false);
        page->goto_line_error_.set("");
        return true;
      }
      // 新建/重命名/删除浮层：它是瞬态动作，Esc 必须能关。
      // 曾经漏了这一条：它既不受全局 Esc 管、又没自己的 Esc 实现，
      // 于是“关不掉的浮层”（只能点它自己的 ×）。
      if (page->fs_action_open_.value()) {
        page->fs_action_open_.set(false);
        page->fs_action_kind_.set(CodeEditorPage::FsAction::None);
        return true;
      }
      if (!page->find_open_.value()) return false;   // 无事可关：让路（不消费）
      page->dismiss_find();
      return true;
    });
    // —— 启动对账：注册面 ⇄ 单源表（表外注册/漏注册当场点名到日志）——
    //
    // 注册块零手抄键位之后，剩下的风险是「绕过表另写 register_shortcut」
    //（unknown）与「表改了、某条 bind 被删」（missing）——这里全部点名到
    // stderr（日志文件里可查），对账测试（gbcode_shortcut_card_test.cpp）
    // 则把同一份 diff 钉红。
    {
      const ShortcutDrift drift = shortcut_drift(root->shortcuts());
      for (const std::string& name : drift.unknown) {
        std::fprintf(stderr, "[gbcode] 表外快捷键（不在 shortcut_help.hpp 单源表）：%s\n",
                     name.c_str());
      }
      for (const std::string& id : drift.missing) {
        std::fprintf(stderr, "[gbcode] 快捷键漏注册（表有但没 bind）：%s\n", id.c_str());
      }
      for (const std::string& name : drift.reserved) {
        std::fprintf(stderr, "[gbcode] 编辑器内键位被根级抢注：%s\n", name.c_str());
      }
    }
  }
  // —— 编辑器右键菜单的触发链路 ——
  //
  // 为什么走 `set_event_handler` 而不是给 `CodeEditor` 加回调：右键菜单是**宿主**的
  // 责任（编辑器不应该自己知道弹哪张菜单）。而 `Element::on_event` 基类实现会在
  // 组件自身不消费事件时把事件交给注入的 handler——右键（`MouseDown`/`MouseUp`
  // 且 `button == 2`）编辑器一律不处理，正好是这条路径。
  //
  // 时机：在 `MouseUp`（不是 Down）上弹，与 Windows/macOS 的习惯一致；
  // 否则先弹菜单、再把“松开”送到新弹的菜单上，手感不对。
  if (page->editor != nullptr) {
    // 右键链路已在 `build_editor_area` 里接好（`CodeEditor::on_context_menu`）——
    // 这里不再装 `set_event_handler`：那个回调永远轮不到（组件对任何按钮的按下都消费）。
  }

  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败: %s\n", started.error().to_string().c_str());
    return 1;
  }
  page->note_startup(std::format("歌白代码 0.1.0 · 后端 {} · headless={} · DPI {:.1f} · 控制通道 127.0.0.1:{}",
                                    app.backend_name(), app.headless(),
                                    static_cast<double>(app.device_scale()), app.control_port()));
  (void)host->tick();
  app.render_frame();

  // 进程内脚本能力（`--enable-script`）：**能力保留，演示脚本已删除**。
  //
  // 旧实现启动时自动载入 `assets/logic.js`——那份脚本的作用是“证明脚本能跑”
  //（状态栏回显行数、每 2 秒刷心跳）。正式形态下这属于演示噪声：
  // 用户没要求的心跳会不断改写状态栏。
  // 能力本身（控制通道 `script` 方法 / AI 读写界面）与 `--enable-script` 开关
  // 都原样保留——只是不再有内置脚本自动运行。

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    // 帧首推进：先在编辑器上补做「上一帧记下的跳转」（切标签/打开文件后
    // 编辑器实例要等重组才拿到，所以跳转请求排队到这一帧落地）。
    // 声明式树统一推进（**全部已登记的树**，不只本页那棵）：
    // 一个页面可以占多处树位（如菜单挂在标题栏的附属槽），各自是一棵树；
    // 靠调用方逐个记得 `host->tick()` 就会漏（漏了的表现是“点击命中、状态也变、
    // 面板不出现”）。登记由 `DeclarativeHost` 构造时自动完成。
    app.root().tick_declarative_hosts();
    // 终端作业的实时输出：在 `tick`（重组）**之前**把工作线程攒下的行搬进状态，
    // 本帧就能排进布局——放在后面就永远慢一帧（连续输出时看着像卡）。
    page->update_terminal();
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    app.pace_loop(frame_start_ms);
  }
  // 退出前先收终端：可能在跑的作业得停掉并 join 读线程，
  // 否则 `jthread` 在析构时发现 `joinable` 会直接 `std::terminate`。
  page->shutdown();
  st::print("歌白代码 退出：{} 帧，标签 {} 个，可用语言 {} 种\n", frames,
            page->buffers_.value().size(), CodeEditor::available_languages().size());
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
