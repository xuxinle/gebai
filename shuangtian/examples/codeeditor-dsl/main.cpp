/// 代码编辑器演示（**声明式重写版**）：与 `examples/codeeditor` 同一界面，
/// 但五层骨架与所有交互状态全部用 `st::ui::dsl` 声明。
///
/// 对照点（为什么值得留两个版本）：
/// - 命令式版（1417 行）：`std::make_unique<...>` 逐层组装 + 手工持指针同步状态；
/// - 声明式版（本文件）：`state_` 驱动 + `build()` 里描述形态，**同步由框架做**——
///   改状态 → 下一帧重组 → 真值树按 diff 更新；不再有「忘了 set_content」这类漂移。
///
/// 声明式没能覆盖的少数点用 `custom<T>` 逃生舱（Tabs 的 key 语义已由 `dsl::tabs` 包好；
/// `CodeEditor` 用 `custom` 接一等接口）——诚实分工，不做属性面穷举。
///
/// 布局五层：标题栏 / 菜单栏 / 主体三栏（活动栏·侧栏·编辑区）/ 底部面板 / 状态栏。
/// 控制通道钩子沿用旧版：`#btn-theme`、`#editor`、`#status`。

#include <algorithm>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/text/highlight.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/icon.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;

// ── 数据模型（= 状态的一部分；声明式只管「怎么画」，这里管「是什么」）────────

struct Sample {
  std::string name;
  std::string language;
  std::string code;
  std::string tree_key;
};

/// 一个打开的编辑器缓冲区。
struct OpenBuffer {
  std::string key;
  std::string label;
  std::string language;
  std::string text;
  bool dirty{false};

  /// 相等：`State<T>::set` 用它判变化（整个 vector 的比较依赖它）。
  /// 文本比较成本可接受（只在写入时判一次）；真要省可就只比元数据，
  /// 但那样「内容变了却不重组」会让编辑器显示与数据脱节——宁可多比。
  auto operator==(const OpenBuffer& other) const -> bool = default;
};

[[nodiscard]] auto samples() -> std::vector<Sample> {
  std::vector<Sample> list;
  list.push_back(Sample{"renderer.cpp", "cpp",
                        "// 霜天光栅器：扫描线覆盖率抗锯齿\n#include <vector>\n\n"
                        "namespace st::raster {\n\nauto fill_path_aa(Surface& canvas) -> void {\n"
                        "  for (int y = first_y; y < last_y; ++y) {\n"
                        "    rasterize_row(canvas, polylines, y, paint);\n  }\n}\n\n}",
                        "src/raster/renderer.cpp"});
  list.push_back(Sample{"deploy.py", "python",
                        "\"\"\"部署脚本：把构建产物推到目标机\"\"\"\nimport subprocess\nfrom pathlib import Path\n\n"
                        "class Deployer:\n    def __init__(self, host: str, port: int = 22):\n"
                        "        self.host = host\n        self.port = port\n\n"
                        "if __name__ == \"__main__\":\n    print(\"部署开始\")\n",
                        "tools/deploy.py"});
  list.push_back(Sample{"control.rs", "rust",
                        "// 控制通道：帧 = uint32 大端长度 + JSON\nuse std::io::Write;\n\n"
                        "pub struct Frame {\n    pub id: u32,\n    pub method: String,\n}\n\n"
                        "fn main() -> std::io::Result<()> {\n    Ok(())\n}\n",
                        "src/control/frame.rs"});
  list.push_back(Sample{"theme.json", "json",
                        "{\n  \"name\": \"霜天 · 暗色\",\n  \"version\": \"0.1.0\",\n"
                        "  \"colors\": {\n    \"background\": \"#0E1626\",\n    \"keyword\": \"#C792EA\"\n"
                        "  },\n  \"enabled\": true\n}\n",
                        "assets/theme.json"});
  list.push_back(Sample{"pipeline.yaml", "yaml",
                        "# CI 流水线：构建 → 测试 → 打包\nname: shuangtian-ci\non:\n  push:\n"
                        "    branches: [main, dev]\njobs:\n  build:\n    runs-on: ubuntu-24.04\n",
                        ".github/pipeline.yaml"});
  list.push_back(Sample{"query.sql", "sql",
                        "-- 会话与工具调用统计\nSELECT\n    s.user_id,\n    COUNT(t.id) AS tool_calls\n"
                        "FROM sessions AS s\nGROUP BY s.user_id\nLIMIT 20;\n",
                        "stats/query.sql"});
  return list;
}

/// 自定义语言：业务日志（运行时注册，演示「规则即数据」）。
void register_custom_language() {
  st::text::LanguageSpec spec = st::text::make_language("stlog");
  st::text::with_aliases(spec, {"slog", "applog"});
  st::text::with_keywords(spec, {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"});
  st::text::with_types(spec, {"service", "component", "trace_id", "span_id", "duration"});
  st::text::with_builtins(spec, {"true", "false", "null", "req", "resp"});
  st::text::with_line_comment(spec, "#");
  spec.key_value_keys = true;
  (void)st::text::global_languages().register_language(spec, /*replace=*/true);
}

constexpr const char* kStlogSample =
    "# 业务日志（自定义语言：运行时注册的规则）\n"
    "INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms\n"
    "WARN  service=renderer component=font 字形缓存接近上限\n"
    "ERROR service=net trace_id=9f2c 连接失败 resp=null  # 需要重试\n";

// ── 页面：整个 IDE 就是一个 Component ────────────────────────────────────
//
// 状态（≈ ArkTS @State 成员）：
//   buffers_       打开的编辑器（数据）
//   active_        当前标签索引
//   activity_      活动栏选中项
//   bottom_        底部面板选中项
//   status_        状态栏文案
//   dark_/sidebar_ 主题与侧栏可见性
//   cursor_text_   光标位置文案
//
// build() 只描述「这些状态对应的界面形态」——所有「改完要点哪里」的同步都没了。

struct CodeEditorPage : Component {
 public:
  explicit CodeEditorPage(std::vector<Sample> files) : files_(std::move(files)) {}

  // —— 状态 ——
  State<std::vector<OpenBuffer>> buffers_{std::vector<OpenBuffer>{}};
  State<std::size_t> active_{0};
  State<std::size_t> activity_{0};
  State<std::size_t> bottom_{2};            // 默认「终端」（VSCode 用户最常用的落点）
  State<std::string> status_{"就绪"};
  State<bool> dark_{false};
  State<bool> sidebar_visible_{true};
  State<std::string> cursor_text_{"Ln 1, Col 1"};
  State<std::string> language_{"cpp"};
  State<std::vector<std::string>> terminal_log_{
      std::vector<std::string>{"shuangtian dev terminal", "> 输入 help 查看可用命令，Enter 提交"}};
  State<std::string> output_{"[启动] codeeditor-dsl · 声明式构建"};
  State<std::string> terminal_draft_{""};
  State<bool> palette_open_{false};
  State<std::string> palette_query_{""};
  State<std::size_t> open_menu_{kNoMenu};

  static constexpr std::size_t kNoMenu{static_cast<std::size_t>(-1)};

  std::vector<Sample> files_{};

  // —— 非状态引用（逃生舱的强类型句柄；每次 build 重新取得）——
  CodeEditor* editor{nullptr};
  Element* editor_host{nullptr};
  Tabs* tab_bar{nullptr};
  MenuBar* menu_bar_ptr{nullptr};

  // —— 动作（写状态；同步由框架做）——

  auto open(std::string_view name) -> void {
    auto list = buffers_.value();
    std::size_t found = list.size();
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (list[index].key == name) {
        found = index;
        break;
      }
    }
    if (found == list.size()) {
      for (const auto& sample : files_) {
        if (sample.name != name) continue;
        // 已打开的标签先存回当前编辑内容
        if (editor != nullptr && active_.value() < list.size()) {
          list[active_.value()].text = editor->text();
        }
        OpenBuffer buffer;
        buffer.key = sample.name;
        buffer.label = sample.name;
        buffer.language = sample.language;
        buffer.text = sample.code;
        list.push_back(std::move(buffer));
        found = list.size() - 1;
        break;
      }
    }
    if (found == list.size()) return;   // 没找到
    buffers_.set(std::move(list));
    active_.set(found);
    status_.set(std::string(name) + " · 已打开");
  }

  auto close(std::string_view key) -> void {
    auto list = buffers_.value();
    if (editor != nullptr && !list.empty()) list[active_.value()].text = editor->text();
    const auto before = list.size();
    std::erase_if(list, [&key](const OpenBuffer& buffer) { return buffer.key == key; });
    if (list.size() == before) return;
    std::size_t next = active_.value();
    if (next >= list.size() && !list.empty()) next = list.size() - 1;
    buffers_.set(std::move(list));
    active_.set(list.empty() ? 0 : next);
    status_.set("已关闭 " + std::string(key));
  }

  auto save() -> void {
    auto list = buffers_.value();
    if (list.empty()) {
      status_.set("没有可保存的编辑器");
      return;
    }
    if (editor != nullptr) list[active_.value()].text = editor->text();
    list[active_.value()].dirty = false;
    output_.set(output_.value() + "\n[保存] " + list[active_.value()].label);
    status_.set("已保存 " + list[active_.value()].label + "（内存模拟）");
    buffers_.set(std::move(list));
  }

  auto toggle_theme() -> void {
    const bool next = !dark_.value();
    dark_.set(next);
    status_.set(next ? "主题 dark · 视觉令牌已切换" : "主题 light · 视觉令牌已切换");
  }

  auto run_terminal(std::string command) -> void {
    std::string reply;
    if (command == "help") {
      reply = "可用命令：help · langs · open <file> · save · theme · clear";
    } else if (command == "langs") {
      for (const auto& name : CodeEditor::available_languages()) reply += std::string(name) + " ";
      reply = "已注册语言：" + reply;
    } else if (command == "save") {
      save();
      reply = "已执行保存";
    } else if (command == "theme") {
      toggle_theme();
      reply = "已切换主题";
    } else if (command == "clear") {
      terminal_log_.set({"shuangtian dev terminal"});
      terminal_draft_.set("");
      return;
    } else if (command.starts_with("open ")) {
      const std::string want = command.substr(5);
      open(want);
      reply = "已尝试打开 " + want;
    } else if (!command.empty()) {
      reply = "未知命令：" + command + "（help 查看）";
    }
    if (!command.empty()) {
      auto log = terminal_log_.value();
      log.push_back("❯ " + command);
      if (!reply.empty()) log.push_back(reply);
      terminal_log_.set(std::move(log));
    }
    terminal_draft_.set("");
  }

  // —— build()：只描述形态 ——

  void build(Composer& c) override {
    const auto& buffers = buffers_.value();
    const std::size_t active = active_.value();
    const bool has_editor = !buffers.empty();

    column(c, {.gap = 0.0f, .id = "editor-page"}, [&] {
      build_title_bar(c, buffers, active);
      build_menu(c);
      build_main(c, buffers, active, has_editor);
      build_bottom(c);
      build_status_bar(c, buffers, active);
    });

    // 命令面板：**条件声明**（关掉 = 本帧不声明 → 框架 sweep 移除）
    if (palette_open_.value()) build_palette(c);
  }

  // 命令面板（overlay + 过滤列表）
  void build_palette(Composer& c) {
    (void)overlay(c, "palette", {.padding = 60.0f, .id = "command-palette"}, [&] {
      (void)card(c, {.gap = 8.0f, .padding = 12.0f}, [&] {
        text(c, [] { return std::string("> 输入命令…"); });
        input(c, palette_query_.value(),
              [this](std::string next) { palette_query_.set(std::move(next)); },
              {.id = "palette-input", .key = "palette-input"});
        // 过滤后的命令列表（命令集合从当前状态生成）
        const std::string query = palette_query_.value();
        for (const auto& command : commands()) {
          if (!query.empty() && command.title.find(query) == std::string::npos) continue;
          (void)button(c, command.title + "   —   " + command.detail,
                       [this, action = command.action] {
                         palette_open_.set(false);
                         action();
                       },
                       {.key = command.title});
        }
        (void)button(c, "关闭（Esc）", [this] { palette_open_.set(false); },
                     {.id = "palette-close"});
      });
    });
  }

  /// 命令集合（与旧版功能对齐：保存 / 侧栏 / 主题 / 关于 / 打开每个文件 / stlog 样例）。
  struct Command {
    std::string title;
    std::string detail;
    std::function<void()> action;
  };
  [[nodiscard]] auto commands() -> std::vector<Command> {
    std::vector<Command> list;
    list.push_back(Command{"文件: 保存", "把当前编辑器标记为已保存", [this] { save(); }});
    list.push_back(Command{"查看: 切换侧栏可见性", "显示/隐藏活动栏与侧栏（Ctrl+B）",
                           [this] { sidebar_visible_.set(!sidebar_visible_.value()); }});
    list.push_back(Command{"查看: 切换亮/暗主题", "主题令牌整体切换",
                           [this] { toggle_theme(); }});
    list.push_back(Command{"帮助: 关于", "codeeditor-dsl · 霜天声明式示例", [this] {
                             status_.set("codeeditor-dsl 0.1.0 · 全内置组件 + 声明式");
                           }});
    for (const auto& sample : files_) {
      list.push_back(Command{"文件: 打开 " + sample.name, "语言 " + sample.language,
                             [this, name = sample.name] { open(name); }});
    }
    list.push_back(Command{"语言: 打开 stlog 样例", "自定义语言（运行时注册）", [this] {
                             open_stlog();
                           }});
    return list;
  }

  /// 自定义语言样例（运行时注册的 stlog）。
  auto open_stlog() -> void {
    // 自定义语言样例（运行时注册的 stlog）
    static const Sample kStlog{"stlog.log", "stlog",
                               "INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms\n"
                               "WARN  service=renderer component=font 字形缓存接近上限\n"
                               "ERROR service=net trace_id=9f2c 连接失败 resp=null\n",
                               "logs/stlog.log"};
    files_.push_back(kStlog);
    open(kStlog.name);
  }

  /// 关闭当前标签（快捷键 Ctrl+W 与菜单共用）。
  auto close_active() -> void {
    const auto list = buffers_.value();
    if (list.empty()) return;
    close(list[active_.value()].key);
  }

  // —— 非状态引用（逃生舱的强类型句柄；每次 build 重新取得）——

 private:
  // 1. 标题栏
  void build_title_bar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    row(c, {.gap = 8.0f, .padding = 12.0f, .height = 36.0f, .id = "titlebar"}, [&] {
      // 标题栏居中：align_items=Center 由 BoxProps 之外的 style 直写（逃生舱示例）
      (void)icon(c, "code", 16.0f);
      const std::string title = buffers.empty() ? "codeeditor" : buffers[active].label + " - codeeditor";
      text(c, [title] { return title; }, {.id = "title-text"});
      // 弹性空隙把窗口控制推到右侧
      (void)spacer(c, 0.0f);
      for (const char* glyph : {"minus", "square", "close"}) {
        (void)icon(c, glyph, 14.0f);
      }
    });
  }

  // 2. 菜单栏（MenuBar 声明式；下拉面板经 overlay 挂）
  void build_menu(Composer& c) {
    static const std::vector<MenuData> kMenus = {
        {"file", "文件",
         {{.id = "new", .label = "新建文件"},
          {.id = "open", .label = "打开文件…"},
          {.separator = true},
          {.id = "save", .label = "保存（Ctrl+S）"},
          {.id = "close-tab", .label = "关闭编辑器（Ctrl+W）"}}},
        {"edit", "编辑",
         {{.id = "undo", .label = "撤销"},
          {.id = "redo", .label = "重做"},
          {.separator = true},
          {.id = "select-all", .label = "全选（Ctrl+A）"}}},
        {"view", "查看",
         {{.id = "command-palette", .label = "命令面板…（Ctrl+Shift+P）"},
          {.separator = true},
          {.id = "toggle-sidebar", .label = "切换侧栏（Ctrl+B）"},
          {.id = "toggle-theme", .label = "切换亮/暗主题"}}},
        {"help", "帮助", {{.id = "about", .label = "关于 codeeditor-dsl"}}}};
    // 菜单栏：动作写到状态（on_action）；打开哪个菜单也写到状态（on_open_menu）
    menu_bar_ptr = dsl::menu_bar(
        c, kMenus,
        [this](const std::string& menu, const std::string& item) { on_menu(menu, item); },
        [this](std::size_t index) {
          open_menu_.set(open_menu_.value() == index ? kNoMenu : index);
        },
        {.id = "menubar"});
    // 打开状态 → 下一帧声明面板 overlay（不声明 = 自动消失，框架 sweep）
    const std::size_t open = open_menu_.value();
    if (open != kNoMenu && menu_bar_ptr != nullptr) dsl::menu_panel_overlay(c, *menu_bar_ptr, open);
  }

  void on_menu(const std::string& menu, const std::string& item) {
    open_menu_.set(kNoMenu);   // 选完即关
    if (menu == "file" && item == "save") {
      save();
    } else if (menu == "file" && item == "close-tab") {
      close_active();
    } else if (menu == "file" && (item == "new" || item == "open")) {
      palette_open_.set(true);
    } else if (menu == "view" && item == "command-palette") {
      palette_open_.set(true);
    } else if (menu == "view" && item == "toggle-sidebar") {
      sidebar_visible_.set(!sidebar_visible_.value());
    } else if (menu == "view" && item == "toggle-theme") {
      toggle_theme();
    } else if (menu == "help" && item == "about") {
      status_.set("codeeditor-dsl 0.1.0 · 霜天声明式示例");
    }
  }

  // 3. 主体三栏
  void build_main(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                  bool has_editor) {
    row(c, {.grow = true, .id = "main-row"}, [&] {
      if (sidebar_visible_.value()) {
        build_activity_bar(c);
        build_sidebar(c);
      }
      build_editor_area(c, buffers, active, has_editor);
    });
  }

  void build_activity_bar(Composer& c) {
    struct Activity {
      const char* icon;
      const char* id;
    };
    static constexpr Activity kActivities[] = {{"folder", "explorer"}, {"search", "search"},
                                               {"git-branch", "scm"}, {"play", "run"},
                                               {"package", "extensions"}};
    column(c, {.gap = 2.0f, .width = 44.0f, .id = "activity-bar"}, [&] {
      for (std::size_t index = 0; index < std::size(kActivities); ++index) {
        // 图标按钮：`custom<Button>`（Button 的 icon 是一等接口，属性面没有）
        const std::string id = std::string("activity-") + kActivities[index].id;
        (void)custom<Button>(c, [&](Button& b) {
          b.set_id(id);
          b.set_icon(kActivities[index].icon);
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this, index] { activity_.set(index); };
        }, {.width = 40.0f, .height = 40.0f, .key = kActivities[index].id});
      }
    });
  }

  void build_sidebar(Composer& c) {
    column(c, {.gap = 8.0f, .padding = 12.0f, .width = 240.0f, .id = "sidebar"}, [&] {
      const std::size_t activity = activity_.value();
      if (activity == 0) {
        text(c, [] { return std::string("资源管理器"); });
        for (const auto& sample : files_) {
          // 文件项：Ghost + 小尺寸（与活动栏按钮同一观感；custom 逃生舱设 icon/变体）
          (void)custom<Button>(c, [this, &sample](Button& b) {
            b.set_label(sample.tree_key);
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this, name = sample.name] { open(name); };
          }, {.key = sample.tree_key});
        }
      } else if (activity == 1) {
        text(c, [] { return std::string("搜索"); });
        // 搜索结果：数据驱动 List（key 复用）
        std::vector<ListItemData> hits;
        for (const auto& sample : files_) {
          hits.push_back({.key = sample.name, .label = sample.name, .subtitle = sample.language});
        }
        (void)list(c, hits, [this, files = files_](std::size_t index) {
          if (index < files.size()) open(files[index].name);
        }, {.id = "search-results"});
      } else {
        text(c, [] { return std::string("（示例占位面板）"); });
      }
    });
  }

  void build_editor_area(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                         bool has_editor) {
    column(c, {.grow = true, .id = "editor-column"}, [&] {
      // 标签栏：数据驱动（key 复用，活动态跟 key）
      std::vector<TabData> tabs;
      for (const auto& buffer : buffers) {
        tabs.push_back({.key = buffer.key, .label = buffer.label, .modified = buffer.dirty,
                        .closable = true});
      }
      (void)dsl::tabs(c, tabs, active,
                      [this](std::size_t index) {
                        // 切标签：先存回当前编辑内容，再切（数据在状态里，切换即改 active）
                        auto list = buffers_.value();
                        if (editor != nullptr && active_.value() < list.size()) {
                          list[active_.value()].text = editor->text();
                        }
                        buffers_.set(std::move(list));
                        active_.set(index);
                      },
                      [this](const std::string& key) { close(key); },
                      {.id = "editor-tabs"});

      if (has_editor) {
        // 编辑器：custom<T> 逃生舱（CodeEditor 的一等接口有多处属性面覆盖不到）
        const auto& buffer = buffers[active < buffers.size() ? active : 0];
        (void)custom<CodeEditor>(c, [this, &buffer](CodeEditor& ed) {
          ed.set_id("editor");     // 控制通道钩子：st_visual_check.py 依赖
          // 属性经**属性面**写入（`set_property`）：这样协议 `get` 与脚本 `ui_get`
          // 能看到同样的值——声明式不绕过「一套语义」（不变式 2）。
          ed.set_property("font_size", "13.5");
          ed.set_property("tab_width", "4");
          ed.set_property("language", buffer.language);
          ed.set_property("text", buffer.text);
          ed.set_property("read_only", "false");
          ed.style().grow = true;
          ed.on_change = [this](std::string_view) { status_.set("编辑中…"); };
          ed.on_cursor_change = [this] {
            if (editor == nullptr) return;
            cursor_text_.set(std::format("Ln {}, Col {}", editor->cursor_line() + 1,
                                         editor->cursor_column() + 1));
          };
          editor = &ed;
        }, {.grow = true, .id = "editor-host"});
      } else {
        column(c, {.grow = true, .id = "empty-hint"}, [&] {
          (void)icon(c, "code", 40.0f);
          text(c, [] {
            return std::string("从左侧资源管理器打开一个文件，或 Ctrl+Shift+P 打开命令面板");
          });
        });
      }
    });
  }

  // 4. 底部面板（问题 / 输出 / 终端）
  void build_bottom(Composer& c) {
    column(c, {.height = 170.0f, .id = "bottom-panel"}, [&] {
      std::vector<TabData> labels{{.key = "problems", .label = "问题", .closable = false},
                                  {.key = "output", .label = "输出", .closable = false},
                                  {.key = "terminal", .label = "终端", .closable = false}};
      (void)dsl::tabs(c, labels, bottom_.value(),
                      [this](std::size_t index) { bottom_.set(index); },
                      {}, {.id = "bottom-tabs"});
      const std::size_t which = bottom_.value();
      if (which == 0) {
        row(c, {.gap = 16.0f, .padding = 8.0f, .grow = true, .id = "panel-problems"}, [&] {
          (void)icon(c, "error", 14.0f);
          text(c, [] { return std::string("0 个错误"); });
          (void)icon(c, "warning", 14.0f);
          text(c, [] { return std::string("0 个警告"); });
          (void)spacer(c, 0.0f);
          text(c, [] { return std::string("工作区干净，没有发现问题"); });
        });
      } else if (which == 1) {
        (void)custom<ScrollView>(c, [this](ScrollView& scroll) {
          scroll.set_id("output-scroll");
          scroll.style().grow = true;
          auto label = std::make_unique<Text>(output_.value());
          label->set_id("output-text");
          label->set_font_size(12.0f);
          label->set_tone(Tone::Muted);
          label->style().monospace = true;
          scroll.add_child(std::move(label));
        }, {.grow = true, .id = "panel-output"});
      } else {
        column(c, {.gap = 6.0f, .padding = 8.0f, .grow = true, .id = "panel-terminal"}, [&] {
          std::string joined;
          for (const auto& line : terminal_log_.value()) joined += line + "\n";
          text(c, [joined] { return joined; }, {.id = "terminal-output"});
          row(c, {.gap = 6.0f}, [&] {
            text(c, [] { return std::string("❯"); });
            // 终端输入：Enter 提交（Input::on_submit 是一等接口）
            (void)custom<Input>(c, [this](Input& field) {
              field.set_id("terminal-input");
              field.set_placeholder("help");
              field.set_property("text", terminal_draft_.value());
              field.on_change = [this](std::string_view next) {
                terminal_draft_.set(std::string(next));
              };
              field.on_submit = [this](std::string_view command) {
                run_terminal(std::string(command));
              };
            }, {.key = "terminal-input"});
          });
        });
      }
    });
  }

  // 5. 状态栏
  void build_status_bar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    row(c, {.gap = 12.0f, .padding = 10.0f, .height = 26.0f, .id = "statusbar"}, [&] {
      (void)icon(c, "git-branch", 12.0f);
      text(c, [] { return std::string("main*"); }, {.id = "branch-label"});
      (void)icon(c, "error", 12.0f);
      text(c, [] { return std::string("0"); });
      (void)icon(c, "warning", 12.0f);
      text(c, [] { return std::string("0"); });
      (void)spacer(c, 0.0f);
      text(c, [this] { return cursor_text_.value(); }, {.id = "cursor-label"});
      text(c, [] { return std::string("·  空格: 4  ·  UTF-8"); });
      const std::string lang = buffers.empty() ? std::string("—") : buffers[active].language;
      text(c, [lang] { return lang; }, {.id = "language-label"});
      text(c, [this] { return status_.value(); }, {.id = "status"});   // 控制通道钩子
      button(c, dark_.value() ? "亮色" : "暗色", [this] { toggle_theme(); },
             {.id = "btn-theme"});   // 控制通道钩子
    });
  }
};

// ── 入口 ─────────────────────────────────────────────────────────────────

struct Options {
  std::string language{"cpp"};
  bool headless{false};
  std::uint32_t frames{0};
  int max_ms{0};
  float scale{0.0f};
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::string theme{"light"};
  std::string text_lcd{"auto"};
  std::string text_fit{"auto"};
};

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view raw = argv[index];
    const auto value = [&](std::string fallback) {
      return index + 1 < argc ? std::string(argv[++index]) : std::move(fallback);
    };
    if (raw == "--language") options.language = value("cpp");
    else if (raw == "--headless") options.headless = true;
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(value("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(value("0"));
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(value("1")));
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(value("0")));
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--shots") options.shots = value({});
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--text-lcd") options.text_lcd = value("auto");
    else if (raw == "--text-fit") options.text_fit = value("auto");
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
  app_options.title = "codeeditor-dsl · 霜天";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  app_options.text_lcd = options.text_lcd;
  app_options.text_fit = options.text_fit;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.theme = options.theme == "dark" ? ThemeMode::Dark : ThemeMode::Light;

  st::app::Application app("codeeditor-dsl", "0.1.0", app_options);

  // 声明式挂载：整个 IDE 是一个 Component
  auto page = std::make_shared<CodeEditorPage>(samples());
  auto host = dsl::mount(app.root(), page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
  }
  // 初始文件（--language 指定，默认 cpp）
  const Sample* initial = nullptr;
  for (const auto& sample : page->files_) {
    if (initial == nullptr) initial = &sample;
    if (sample.language == options.language) {
      initial = &sample;
      break;
    }
  }
  if (initial != nullptr) page->open(initial->name);
  page->dark_.set(options.theme == "dark");
  (void)host->tick();

  // —— 全局快捷键（声明式注册：转调 UiRoot，先于焦点链派发）——
  {
    auto* root = &app.root();
    const auto ctrl_only = UiRoot::Shortcut{"", true, false, false, false};
    const auto bind = [&](const char* key, bool shift, std::function<void()> action) {
      UiRoot::Shortcut mods = ctrl_only;
      mods.key = key;
      mods.shift = shift;
      // handler 返回 true = 消费（不再下沉）；这些是全局命令，总是消费
      (void)root->register_shortcut(key, mods, [action = std::move(action)]() {
        action();
        return true;
      });
    };
    bind("s", false, [page] { page->save(); });
    bind("w", false, [page] { page->close_active(); });
    bind("b", false, [page] { page->sidebar_visible_.set(!page->sidebar_visible_.value()); });
    bind("p", true, [page] { page->palette_open_.set(true); });   // Ctrl+Shift+P
    bind("tab", false, [page] {
      // Ctrl+Tab：循环切标签
      const auto count = page->buffers_.value().size();
      if (count > 1) page->active_.set((page->active_.value() + 1) % count);
    });
  }

  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败: %s\n", started.error().to_string().c_str());
    return 1;
  }
  page->output_.set(std::format("[启动] codeeditor-dsl 0.1.0 · 后端 {} · headless={}",
                                app.backend_name(), app.headless()));
  (void)host->tick();
  app.render_frame();

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    if (host->dirty()) (void)host->tick();
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    app.pace_loop(frame_start_ms);
  }
  st::print("codeeditor-dsl 退出：{} 帧，标签 {} 个\n", frames, page->buffers_.value().size());
  return 0;
}

}  // namespace

ST_MAIN(run_app)
