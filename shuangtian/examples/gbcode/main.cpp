/// 代码编辑器演示：按 VSCode 的信息架构组装的 IDE 形态界面（**声明式版**）。
///
/// 布局五层（自上而下）：
/// - 标题栏：文件名（脏标记）+ 应用名 + 装饰性窗口控制（— □ ×）
/// - 菜单栏：文件 / 编辑 / 选择 / 查看 / 运行 / 帮助（`MenuBar` + 下拉面板 overlay）
/// - 主体三栏：活动栏（资源管理器·搜索·源代码管理·运行·扩展）+ 侧栏（`SplitView` 可拖）
///   + 编辑区（`Tabs` 多标签 + `CodeEditor`）
/// - 底部面板：问题 / 输出 / 终端（`Tabs` 切换）
/// - 状态栏：分支 · 错误/警告计数 · 光标 Ln,Col · 选区 · 缩进 · 编码 · 语言 · 主题
///
/// **为什么用声明式**：整份界面由 `CodeEditorPage::build()` 描述「状态对应的形态」，
/// 「改完要点哪里」的手工同步全部消失——改状态 → 下一帧重组 → 真值树按 diff 更新。
/// 旧的命令式实现（1966 行：逐层 `std::make_unique` + 手工持指针、七处重复同步标题栏）
/// 已在本次整合中被它替换；`examples/gbcode-dsl`（声明式重写版）同时并入，不再有分身。
///
/// **真值树唯一**：声明式不绕过 `apply_properties` —— `tree`/`get`/`set`/`invoke`
/// 对这里的元素照常可用（协议自动化与手搭界面完全一致）。少数属性面覆盖不到的一等接口
/// （`CodeEditor::on_change`、`MenuBar` 下拉、`CommandPalette` 的命令表…）走 `custom<T>`
/// 逃生舱，不做属性面穷举。
///
/// 演示点：
/// - **多标签编辑模型**：打开/切换/关闭/脏标记（`dsl::tabs` 按 key 对齐复用）
/// - **真实文件工作区**：`--workspace <dir>` 下资源树 `fs::list_dir`、点文件打开、
///   Ctrl+S `fs::write_text` 真实写盘（脏标记/标题栏/状态栏全联动）
/// - **查找替换**：Ctrl+F / Ctrl+H 浮条（命中高亮、计数 n/m、环绕、替换/全部）
/// - **命令面板**：Ctrl+Shift+P（全部命令）/ Ctrl+P（快速打开文件）
/// - **问题检查**：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行（真检查，点了跳行）
/// - **脚本逻辑层**：`--enable-script` 时编译期嵌入的 `assets/logic.js` 生效
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
#include "st/text/highlight.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/command_palette.hpp"
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

namespace {

/// 编辑器字号档位默认值（相对正文 `font_base` 的倍数）。
///
/// 为什么用档位而不是绝对像素：主题是字号缩放的唯一真值源——
/// 绝对像素会与 `--ui-font-scale` 脱钩（实测踩到：UI 文字 15→22.5，编辑器恒为 13.5）。
/// 档位还让“设置面板列几档字号”变成一件自然的事。
///
/// `1.0` = **与正文同级**（与组件默认一致）。`--editor-font-scale` 可覆盖。
constexpr float kEditorFontScale{1.0f};

using namespace st::ui;
using namespace st::ui::dsl;

// ════════════════════════════════════════════════════════════════════════════
// 一、数据与工具（与界面无关：样例、语言注册、轻量检查、文件读取、目录扫描）
// ════════════════════════════════════════════════════════════════════════════

/// 内置样例文件（无 `--workspace` 时的自包含工作区）。
struct Sample {
  std::string name;
  std::string language;
  std::string code;
  std::string tree_key;  ///< 资源管理器里的路径（相对工作区根）
};

[[nodiscard]] auto samples() -> std::vector<Sample> {
  std::vector<Sample> list;
  list.push_back(Sample{
      "renderer.cpp", "cpp",
      R"(// 霜天光栅器：扫描线覆盖率抗锯齿
#include <vector>

namespace st::raster {

auto fill_path_aa(Surface& canvas, const Path& path, const Paint& paint) -> void {
  const auto polylines = path.flatten(0.25f);
  const Rect bounds = path.flattened_bounds(0.25f);
  // 覆盖率是**带符号**量（非零环绕）：画布侧取绝对值
  for (int y = first_y; y < last_y; ++y) {
    rasterize_row(canvas, polylines, y, paint);
  }
}

}  // namespace st::raster
)",
      "src/raster/renderer.cpp"});
  list.push_back(Sample{
      "deploy.py", "python",
      R"("""部署脚本：把构建产物推到目标机"""
import subprocess
from pathlib import Path

class Deployer:
    def __init__(self, host: str, port: int = 22):
        self.host = host
        self.port = port

    def push(self, artifact: Path) -> bool:
        # 只推可执行文件与资源清单
        cmd = f"scp -P {self.port} {artifact} {self.host}:/opt/app/"
        return subprocess.run(cmd, shell=True).returncode == 0

if __name__ == "__main__":
    ok = Deployer("10.0.0.7").push(Path("build/dev/bin/gallery"))
    print("部署成功" if ok else "部署失败")
)",
      "tools/deploy.py"});
  list.push_back(Sample{
      "control.rs", "rust",
      R"(// 控制通道：帧 = uint32 大端长度 + JSON
use std::io::Write;
use std::net::TcpStream;

pub struct Frame {
    pub id: u32,
    pub method: String,
}

impl Frame {
    pub fn encode(&self) -> Vec<u8> {
        let body = format!(r#"{{"id":{},"method":"{}"}}"#, self.id, self.method);
        let mut out = (body.len() as u32).to_be_bytes().to_vec();
        out.extend_from_slice(body.as_bytes());
        out
    }
}

fn main() -> std::io::Result<()> {
    let mut stream = TcpStream::connect("127.0.0.1:9000")?;
    stream.write_all(&Frame { id: 1, method: "tree".into() }.encode())?;
    Ok(())
}
)",
      "src/control/frame.rs"});
  list.push_back(Sample{
      "theme.json", "json",
      R"({
  "name": "霜天 · 暗色",
  "version": "0.1.0",
  "colors": {
    "background": "#0E1626",
    "keyword": "#C792EA",
    "string": "#A5E075",
    "numbers": [1, 2.5, -3e4],
    "opacity": 0.86
  },
  "enabled": true,
  "fallbacks": null
})",
      "assets/theme.json"});
  list.push_back(Sample{
      "pipeline.yaml", "yaml",
      R"(# CI 流水线：构建 → 测试 → 打包
name: shuangtian-ci
on:
  push:
    branches: [main, dev]
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: 自举工具链
        run: ./bootstrap.sh
      - name: 构建与测试
        run: |
          ./build/bin/st build gallery --profile dev
          ./build/bin/st test --san
      - name: 禁令扫描
        run: ./build/bin/st lint
)",
      ".github/pipeline.yaml"});
  list.push_back(Sample{
      "panel.html", "html",
      R"(<!-- 组件画廊：一个卡片 -->
<!DOCTYPE html>
<html lang="zh-CN">
  <head>
    <meta charset="utf-8" />
    <title>霜天 · 组件画廊</title>
    <style>
      .card { border-radius: 12px; padding: 16px; box-shadow: 0 2px 8px #0002; }
      .card__title { font-weight: 600; color: #2563EB; }
    </style>
  </head>
  <body>
    <div class="card" data-kind="stat">
      <span class="card__title">渲染后端</span>
      <b>软件光栅器</b>
    </div>
  </body>
</html>
)",
      "web/panel.html"});
  list.push_back(Sample{
      "query.sql", "sql",
      R"(-- 会话与工具调用统计
SELECT
    s.user_id,
    COUNT(DISTINCT s.id)       AS sessions,
    COUNT(t.id)                AS tool_calls,
    ROUND(AVG(t.duration_ms))  AS avg_ms
FROM sessions AS s
LEFT JOIN tool_calls AS t ON session_id = s.id
WHERE s.created_at >= '2026-01-01'
  AND t.status <> 'cancelled'
GROUP BY s.user_id
HAVING COUNT(t.id) > 10
ORDER BY tool_calls DESC
LIMIT 20;
)",
      "stats/query.sql"});
  list.push_back(Sample{
      "fix.patch", "diff",
      R"(--- a/src/raster/rasterizer.cpp
+++ b/src/raster/rasterizer.cpp
@@ -60,6 +60,12 @@ void rasterize_polylines(...) {
   for (const auto& polyline : polylines) {
     if (polyline.points.size() < 2) continue;
+    // 填充语义要求隐式闭合子路径
+    const bool needs_closing_edge = distance(first, last) > 1.0e-4f;
     for (std::size_t index = 1; index < polyline.points.size(); ++index) {
-      const math::Point to = polyline.points[index];
+      const math::Point to = index == size ? first : polyline.points[index];
 )",
      "patches/fix.patch"});
  return list;
}

/// 自定义语言：业务日志（演示"规则即数据"，与内置语言走同一台扫描器）。
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

constexpr std::string_view kStlogSample =
    "# 业务日志（自定义语言：运行时注册的规则）\n"
    "INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms\n"
    "DEBUG service=gateway trace_id=9f2c 命中缓存 true\n"
    "WARN  service=renderer component=font 字形缓存接近上限\n"
    "ERROR service=net trace_id=9f2c 连接失败 resp=null  # 需要重试\n"
    "FATAL service=core 磁盘写入失败，进程退出\n";

/// 一条轻量检查结果（**真检查**，不是占位数据）。
struct Problem {
  std::size_t line{0};   ///< 1 起行号
  bool warning{true};    ///< true = 警告，false = 错误
  std::string message{};
};

/// 便宜的逐行检查（不引入分析器）：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行。
///
/// 为什么值得做：状态栏的「错误 · 警告」计数与问题面板若没有真实来源，就只是摆设
/// ——改完代码什么都不会变，读者也无法验证计数联动是对的。
[[nodiscard]] auto lint_text(std::string_view text) -> std::vector<Problem> {
  std::vector<Problem> problems;
  std::size_t line_no = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::size_t end = eol == std::string_view::npos ? text.size() : eol;
    std::string_view line = text.substr(begin, end - begin);
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      problems.push_back(Problem{line_no, true, "行尾有多余空白"});
    }
    if (!line.empty() && line.front() == '\t') {
      problems.push_back(Problem{line_no, true, "用 Tab 缩进（本工程约定空格）"});
    }
    if (line.find("TODO") != std::string_view::npos ||
        line.find("FIXME") != std::string_view::npos) {
      problems.push_back(Problem{line_no, true, "待办标记（TODO/FIXME）"});
    }
    if (line.size() > 120) problems.push_back(Problem{line_no, true, "行超长（>120 列）"});
    if (eol == std::string_view::npos) break;
    begin = eol + 1;
  }
  return problems;
}

/// 一个打开的编辑器缓冲区。
struct OpenBuffer {
  std::string key;       ///< 业务身份（样例名或绝对路径）——标签按它对账
  std::string label;     ///< 标签名（文件名）
  std::string language;
  std::string text;      ///< 当前文本（切回来时恢复）
  bool dirty{false};     ///< 修改点
  std::string path{};    ///< 真实文件绝对路径（空 = 内置样例，保存走内存模拟）

  /// 相等：`State<T>::set` 用它判变化（整个 vector 的比较依赖它）。
  /// 文本比较成本可接受（只在写入时判一次）；真要省可就只比元数据，
  /// 但那样「内容变了却不重组」会让编辑器显示与数据脱节——宁可多比。
  auto operator==(const OpenBuffer& other) const -> bool = default;
};

/// 读文件进列表（已打开则刷新内容，保持同一标签）；返回该标签下标（失败 = size()）。
[[nodiscard]] auto read_into_buffers(std::vector<OpenBuffer>& buffers, const std::string& path)
    -> std::size_t {
  auto content = st::fs::read_text(path);
  if (!content) return buffers.size();
  for (std::size_t index = 0; index < buffers.size(); ++index) {
    if (buffers[index].path == path) {
      buffers[index].text = *content;
      buffers[index].dirty = false;
      return index;
    }
  }
  OpenBuffer buffer;
  buffer.key = path;
  buffer.path = path;
  const std::size_t slash = path.find_last_of("/\\");
  buffer.label = slash == std::string::npos ? path : path.substr(slash + 1);
  buffer.language = st::text::language_from_path(path).value_or("");
  buffer.text = std::move(*content);
  buffers.push_back(std::move(buffer));
  return buffers.size() - 1;
}

/// 目录树节点：`fs::list_dir` 一层（目录可再展开——懒加载）。
[[nodiscard]] auto scan_tree_nodes(const std::string& root) -> std::vector<TreeNodeData> {
  std::vector<TreeNodeData> nodes;
  auto entries = st::fs::list_dir(root);
  if (!entries) return nodes;
  std::vector<const st::fs::DirEntry*> dirs;
  std::vector<const st::fs::DirEntry*> files;
  for (const auto& entry : *entries) (entry.is_dir ? dirs : files).push_back(&entry);
  const auto by_name = [](const auto* a, const auto* b) { return a->name < b->name; };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  for (const auto* dir : dirs) {
    nodes.push_back(TreeNodeData{.key = "dir:" + dir->name, .label = dir->name, .is_dir = true});
  }
  for (const auto* file : files) {
    nodes.push_back(TreeNodeData{.key = root + "/" + file->name, .label = file->name});
  }
  return nodes;
}

// ════════════════════════════════════════════════════════════════════════════
// 二、页面：整个 IDE 是一个 Component
// ════════════════════════════════════════════════════════════════════════════
//
// 状态（≈ ArkTS @State 成员）：
//   buffers_ / active_     打开的编辑器与当前标签
//   sidebar_visible_/activity_  侧栏可见性与活动栏选中项
//   bottom_visible_ / menu_open_   终端面板与菜单栏的开合/选中项
//   palette_* / find_*     命令面板与查找条的开关、过滤词、计数文案
//   status_ / terminal_*   各处的文本
//
// `build()` 只描述「这些状态对应的界面形态」——所有「改完要点哪里」的同步都没了。

struct CodeEditorPage : Component {
 public:
  CodeEditorPage(std::vector<Sample> files, std::string workspace, std::string tool_root,
                 float editor_font_scale = kEditorFontScale,
                 float editor_line_spacing = CodeEditor::kDefaultLineSpacing)
      : editor_font_scale_(editor_font_scale), editor_line_spacing_(editor_line_spacing),
        tool_root_(std::move(tool_root)), files_(std::move(files)),
        workspace_(std::move(workspace)) {}

  CodeEditorPage(std::vector<Sample> files, std::string workspace,
                 float editor_font_scale = kEditorFontScale,
                 float editor_line_spacing = CodeEditor::kDefaultLineSpacing)
      : editor_font_scale_(editor_font_scale), editor_line_spacing_(editor_line_spacing),
        files_(std::move(files)), workspace_(std::move(workspace)) {}

  /// 窗口动作出口（由 `run_app` 注入 `Application`；为空时窗框**如实拒绝**动作，
  /// 但画面照旧——这正是 `ui::WindowControl` 端口要分离的那两件事）。
  WindowControl* window_control_{nullptr};
  /// 真值源读取口（`run_app` 注入为 `UiRoot::theme().mode()`）。
  ///
  /// **主题不在页面里存影子状态**：真值源是 `UiRoot::theme()`（由 App 持有）。
  /// 页面自存一个 `dark_` 会与它**静默分岔**——按钮文案与状态栏都跟着影子走，
  /// 而画面一点没变（实测故障：按了「暗色」、状态栏还报「视觉令牌已切换」）。
  /// 本页只负责「按当前真值算出下一档」，像个按钮该做的那样。
  std::function<ThemeMode()> theme_mode{};
  /// 主题切换出口（`run_app` 注入为 `Application::set_theme_mode`）。
  std::function<void(ThemeMode)> theme_setter{};
  /// 当前是否暗色——**从真值源读**，不缓存。
  [[nodiscard]] auto theme_is_dark() const -> bool {
    return theme_mode && theme_mode() == ThemeMode::Dark;
  }
  /// 外层窗框（由 `run_app` 装配后回填）：标题栏归它所有，本页只负责推标题。
  WindowFrame* frame_{nullptr};
  /// 菜单的**声明式宿主**（由 `run_app` 把本页 mount 进标题栏的 `leading` 槽）。
  ///
  /// 为什么不直接在 `build()` 里声明菜单：标题栏是**外壳**（`WindowFrame` 自持，
  /// 在声明树之外），而菜单要长在标题栏一行里——两者不在同一棵树上。
  /// `mount_into` 正好表达「声明式的一小块挂到既有元素的槽位」，于是整个 IDE
  /// 仍是一个 `Component`（只是它现在占两处树位），菜单与标题栏合并成一行。
  dsl::DeclarativeHost* menu_host_{nullptr};
  /// 菜单头部的**所有权**（`mount_into` 返回；`MenuHeader` 定义在本类之后，
  /// 此处只能前向声明）。
  std::unique_ptr<dsl::DeclarativeHost> menu_host_owned_{};

  // —— 状态 ——
  State<std::vector<OpenBuffer>> buffers_{std::vector<OpenBuffer>{}};
  State<std::size_t> active_{0};
  State<bool> sidebar_visible_{true};
  State<std::size_t> activity_{0};        ///< 活动栏选中项（0=资源管理器 … 4=扩展）
  State<bool> bottom_visible_{true};      ///< 终端面板开合（收起 = 分栏退化为单栏）
  State<std::string> status_{"就绪"};
  State<std::string> cursor_text_{"Ln 1, Col 1"};
  State<std::string> selection_text_{""};
  // 终端：会话/滚回/作业全在**框架组件** `st::ui::Terminal` 里，
  // 应用侧只留一个指针与"忙闲"镜像（后者用于面板按钮的实时性）。
  /// “关掉最后一个会话 ⇒ 收起面板”的待办（下一次 `pump` 在主循环栈上执行）。
  bool bottom_close_pending_{false};
  Terminal* terminal_ptr{nullptr};
  bool terminal_busy_{false};
  State<std::size_t> menu_open_{kNoMenu};
  State<bool> palette_open_{false};
  State<std::string> palette_query_{""};
  State<bool> palette_shows_files_{false};   ///< Ctrl+P（文件表）vs Ctrl+Shift+P（命令表）
  State<bool> find_open_{false};
  State<std::string> find_counter_{"0/0"};
  /// 转到行浮层（Ctrl+G / 选择菜单）。
  State<bool> goto_line_open_{false};
  State<std::string> goto_line_error_{""};
  /// 查找条选项（大小写 / 全词 / 正则）。
  State<bool> find_case_{false};
  State<bool> find_word_{false};
  State<bool> find_regex_{false};
  State<std::vector<std::string>> dir_expanded_{std::vector<std::string>{}};
  State<std::vector<std::string>> search_hits_{std::vector<std::string>{}};
  State<std::string> search_summary_{""};
  /// 当前编辑器的高亮语言（状态栏/扩展面板与编辑器共用一处真值源）。
  State<std::string> language_{"cpp"};
  /// 输出面板按来源分栏（`St build` / `St test` / `终端`…）。

  static constexpr std::size_t kNoMenu{static_cast<std::size_t>(-1)};

  // —— 非状态引用（逃生舱的强类型句柄；每次 build 重新取得）——
  CodeEditor* editor{nullptr};
  /// 编辑器是否已套用初始配置（字号档位/缩进宽）。只做一次：
  /// 这些值可以随后由属性面改（设置面板/控制通道），每帧重写就把它抹了。
  bool editor_configured_{false};
  MenuBar* menu_bar_ptr{nullptr};
  CommandPalette* palette_ptr{nullptr};
  Input* find_replace_input{nullptr};   ///< 替换文本不进状态（不参与重组）
  Input* find_needle_input{nullptr};    ///< 查找输入框（打开后把焦点交给它）
  bool focus_find_pending_{false};      ///< 「查找条开→下一帧聚焦输入框」的待办标记
  Input* goto_line_input{nullptr};      ///< 转到行的行号输入框
  bool goto_line_pending_focus_{false}; ///< 同上（转到行浮层）

  // —— 侧栏各视图的非状态数据（不进重组依赖：这些是“算一次用一帧”的快照）——
  std::vector<std::string> search_hit_paths_{};   ///< 与 `search_hits_` 同序的绝对路径
  std::vector<std::size_t> search_hit_lines_{};   ///< 同序的 1 起行号
  std::vector<std::size_t> search_hit_columns_{}; ///< 同序的列号（字节偏移）
  std::string last_query_{};                      ///< 上次搜索词（重跑用）
  std::string search_glob_{};                     ///< 文件过滤 glob
  State<bool> search_case_{false};
  State<bool> search_word_{false};
  State<bool> search_regex_{false};
  /// Git 状态快照（`refresh_git` 填充；渲染期读的是它而不是每次重查）。
  struct GitChange {
    std::string status{};   ///< 两字符状态码（`M `、`??`…，已去尾空格）
    std::string path{};
  };
  std::string git_branch_{};
  std::vector<GitChange> git_changes_{};
  std::string git_error_{};
  bool git_probed_{false};
  /// 状态栏问题计数“轮跳”到第几处（反复点同一个计数就依次往下跳）。
  std::size_t problem_cycle_{0};
  /// 已注册的语言清单（扩展视图用；懒加载一次）。
  std::vector<std::string> languages_{};
  /// 滚回视图（`on_scroll` 回调里要问它“到没到底”；每次重组重新取得）。
  ScrollView* terminal_scroll_view_{nullptr};
  /// 关闭脏标签的待确认动作（非空 = 弹了确认对话框）。
  std::function<void()> pending_close_{};
  /// 待确认关闭的标签名（对话框正文用）。
  std::string pending_close_label_{};
  /// 编辑器右键菜单开关 + 锚点（锚点跟随鼠标最后位置）。
  State<bool> context_open_{false};
  st::math::Point context_anchor_{};
  /// 快捷键一览浮层开关。
  State<bool> shortcuts_open_{false};
  /// 打开/另存对话框的待执行动作（选完路径后调）。
  std::function<void(const std::string&)> pending_file_action_{};

  // —— 动作：只写状态（同步由框架做）——

  /// 打开一份内置样例（已在标签里 = 激活；不在 = 新标签）。
  auto open_sample(const Sample& sample) -> void {
    auto list = buffers_.value();
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (list[index].key == sample.name) {
        activate(list, index);
        status_.set(sample.name + " · 已激活");
        return;
      }
    }
    stash_active_text(list);
    OpenBuffer buffer;
    buffer.key = sample.name;
    buffer.label = sample.name;
    buffer.language = sample.language;
    buffer.text = sample.code;
    list.push_back(std::move(buffer));
    buffers_.set(std::move(list));
    active_.set(buffers_.value().size() - 1);
    status_.set(sample.name + " · " + std::to_string(st::utf8_length(sample.code)) +
                " 字符 · 已打开");
  }

  /// 打开真实文件（资源树 / 搜索命中 / 快速打开共用）。
  auto open_path(const std::string& path) -> void {
    auto list = buffers_.value();
    stash_active_text(list);
    const std::size_t index = read_into_buffers(list, path);
    if (index >= list.size()) {
      status_.set("打开失败：" + path);
      return;
    }
    buffers_.set(std::move(list));
    active_.set(index);
    status_.set("已打开 " + path);
  }

  auto open_stlog() -> void {
    auto list = buffers_.value();
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (list[index].key == "stlog.log") {
        activate(list, index);
        return;
      }
    }
    stash_active_text(list);
    OpenBuffer buffer;
    buffer.key = "stlog.log";
    buffer.label = "stlog.log";
    buffer.language = "stlog";
    buffer.text = std::string(kStlogSample);
    list.push_back(std::move(buffer));
    buffers_.set(std::move(list));
    active_.set(buffers_.value().size() - 1);
    status_.set("自定义语言 stlog（运行时注册的规则）");
  }

  /// 关闭一个标签（按 key——索引会随增删漂移，key 才是业务身份）。
  auto close(std::string_view key) -> void {
    auto list = buffers_.value();
    stash_active_text(list);
    const std::size_t before = list.size();
    std::erase_if(list, [key](const OpenBuffer& buffer) { return buffer.key == key; });
    if (list.size() == before) return;
    std::size_t next = active_.value();
    if (next >= list.size() && !list.empty()) next = list.size() - 1;
    buffers_.set(std::move(list));
    active_.set(list.empty() ? 0 : next);
    if (editor != nullptr && !list.empty()) {
      editor->set_language(list[active_.value()].language);
      editor->set_text(list[active_.value()].text);
    }
    loaded_key_ = list.empty() ? std::string{} : list[active_.value()].key;
    refresh_problems();
    status_.set("已关闭 " + std::string(key));
  }

  /// 关闭当前标签（**走确认路径**：脏标签会先弹对话框）。
  ///
  /// 为何必须绕到 `request_close`：入口有四处关闭（Ctrl+W / 工具栏按钮 / 菜单 / 标签的 ×），
  /// 只有标签的 × 天然走 `request_close`——其余三处若直接调 `close()`，
  /// 就会出现“同一个动作，换个入口就静默丢修改”的不一致（实测踩到）。
  auto close_active() -> void {
    const auto list = buffers_.value();
    if (list.empty()) return;
    request_close(list[active_.value()].key);
  }

  /// 保存当前标签：真实文件走 `fs::write_text`，内置样例走内存模拟。
  auto save() -> void {
    auto list = buffers_.value();
    if (list.empty()) {
      status_.set("没有可保存的编辑器");
      return;
    }
    stash_active_text(list);
    OpenBuffer& buffer = list[active_.value()];
    if (!buffer.path.empty()) {
      status_.set(st::fs::write_text(buffer.path, buffer.text)
                      ? "已保存 " + buffer.path
                      : "保存失败（写盘被拒）：" + buffer.path);
    } else {
      status_.set("已保存 " + buffer.label + "（内存模拟，无磁盘写入）");
    }
    buffer.dirty = false;
        status_.set(std::format("已保存 {} · {} 字符", buffer.label,
                            st::utf8_length(buffer.text)));
    buffers_.set(std::move(list));
  }

  /// 切换亮/暗主题。
  ///
  /// 唯一的真值源是 `UiRoot::theme()`（App 持有）：这里**读**它算下一档、
  /// **写回**它（经 `theme_setter` → `Application::set_theme_mode`，后者会连带
  /// 处理好字号缩放与文本 gamma），然后只更新本页的文案。
  /// 不在这里维护自己的 `dark_`——那正是故障的来源（影子状态永不落地到主题）。
  auto toggle_theme() -> void {
    const bool next_dark = !theme_is_dark();
    if (theme_setter) theme_setter(next_dark ? ThemeMode::Dark : ThemeMode::Light);
    status_.set(next_dark ? "主题 dark · 视觉令牌已切换" : "主题 light · 视觉令牌已切换");
  }

  /// 编辑器被编辑：标脏 + 重跑轻量检查（问题面板与状态栏计数同源）。
  auto on_edit() -> void {
    auto list = buffers_.value();
    if (active_.value() < list.size()) {
      stash_active_text(list);
      if (!list[active_.value()].dirty) {
        list[active_.value()].dirty = true;
        buffers_.set(std::move(list));
      }
    }
    refresh_problems();
  }

  /// 光标/选区变化 → 状态栏文案。
  auto on_cursor_moved() -> void {
    if (editor == nullptr) return;
    cursor_text_.set(std::format("Ln {}, Col {}", editor->cursor_line() + 1,
                                 editor->cursor_column() + 1));
    const std::string picked = editor->selected_text();
    selection_text_.set(picked.empty() ? std::string{}
                                       : std::format("已选 {} 字符", st::utf8_length(picked)));
  }

  /// 重跑当前编辑器的轻量检查（问题列表与状态栏计数**同一份数据**）。
  auto refresh_problems() -> void {
    problems_.clear();
    const auto list = buffers_.value();
    if (active_.value() < list.size()) problems_ = lint_text(list[active_.value()].text);
  }

  [[nodiscard]] auto error_count() const -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        problems_.begin(), problems_.end(), [](const Problem& p) { return !p.warning; }));
  }
  [[nodiscard]] auto warning_count() const -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        problems_.begin(), problems_.end(), [](const Problem& p) { return p.warning; }));
  }
  [[nodiscard]] auto active_language() const -> std::string {
    const auto list = buffers_.value();
    return active_.value() < list.size() ? list[active_.value()].language : std::string("—");
  }
  /// 标题栏文案：脏时加一个圆点前缀（VSCode 同族的"未保存"提示）。
  [[nodiscard]] auto window_title() const -> std::string {
    const auto list = buffers_.value();
    // 有工作区时把它的名放进标题——「打开文件夹」之后这是“我现在在哪个项目里”的
    // 第一指示（VSCode 同此：标题栏显示文件夹名）。
    std::string suffix = "歌白代码";
    if (!workspace_.empty()) {
      const std::size_t slash = workspace_.find_last_of("/\\");
      const std::string name =
          slash == std::string::npos ? workspace_ : workspace_.substr(slash + 1);
      if (!name.empty()) suffix = name + " - 歌白代码";
    }
    if (active_.value() >= list.size()) return suffix;
    const OpenBuffer& buffer = list[active_.value()];
    return (buffer.dirty ? "● " : "") + buffer.label + " - " + suffix;
  }

  /// 打开命令面板（`files_table=true` 走 Ctrl+P 的快速打开文件表）。
  auto open_palette(bool files_table, std::string query = {}) -> void {
    palette_shows_files_.set(files_table);
    palette_query_.set(std::move(query));
    palette_open_.set(true);
  }

  auto open_find() -> void {
    find_open_.set(true);
    if (editor == nullptr) return;
    // 预填当前选中文本（VSCode 同族行为）
    const std::string picked = editor->selected_text();
    if (!picked.empty() && picked.find('\n') == std::string::npos) {
      editor->set_find(picked);
      editor->find_next(false);
    }
    update_find_counter();
    // 查找条出现后把焦点交给查找输入框：否则敲的字会落到底层编辑器里
    //（与命令面板 `grab_focus()` 同一回事；查找条的构建在下一帧，故走待办标记）。
    focus_find_pending_ = true;
  }

  /// 帧首处理“查找条已出现→把焦点交给它”（构建期拿不到输入框实例）。
  void apply_pending_focus() {
    if (!focus_find_pending_) return;
    if (find_needle_input == nullptr) return;
    focus_find_pending_ = false;
    if (!find_open_.value()) return;
    // 走宿主焦点契约（与 `CommandPalette::grab_focus` 同一路径）：
    // 直接把 `Input::activate()` 当“拿焦点”是错的——它的语义是“触发默认动作”
    // （对 Input 就是 `on_submit`，会当场多执行一次查找）。
    if (auto* host = find_needle_input->host(); host != nullptr) {
      (void)host->set_keyboard_focus(find_needle_input);
    }
  }

  // ══════════════════════════════════════════════════════════════════════════
  // build()：只描述形态
  // ══════════════════════════════════════════════════════════════════════════

  /// 标题栏**文案**：窗框（外壳）自持标题栏，本页只把“现在该显示什么”推过去
  /// （文件名 + 脏点，与 VSCode 把当前文件名写进标题栏同构）。
  ///
  /// 为什么保留这个“推”而不把标题也搬进 `MenuHeader`：标题属于**窗框**的状态
  /// （它决定窗口叫什么），菜单只是恰好与它同行；两者合并不等于所有权合并。
  void push_title() {
    if (frame_ == nullptr || frame_->title_bar() == nullptr) return;
    frame_->title_bar()->set_title(window_title());
  }

  /// 每帧推进的**构建前动作**（由入口在 `tick` 之前调）。
  ///
  /// 两件事：① 把工作线程攒下的终端输出搬进 `State`（本帧重组才能排进布局）；
  /// ② 执行待办的“贴底”。
  ///
  /// 为何贴底必须在这里而不是 `build()` 里：`build()` **只在状态变脏时才跑**，
  /// 而“刚追加了一行”与“几何已经更新”隔着一次布局——第二次只剩下重排（不重组），
  /// 写在 `build()` 里的“下帧再滚”永远等不到执行（实测：滚动条停在中间，尾部两行看不到）。
  /// 这里每帧都跑，正好在“上一帧布局已完成”的时刻调，`max_scroll()` 已是新值。
  void update_terminal() {
    // **先泵终端**（它可能受理“关闭会话”），**再**处理“收起面板”。
    //
    // 为何要分两步、而不在组件回调里直接 `set(false)`：那个回调是在
    // `Terminal::pump()` 的**栈上**发的，而 `set` 会**同步重组**、把 `Terminal`
    // 拆掉——回到组件内部时 `this` 已释放，直接段错误（实测 `0xC0000005`）。
    // 本函数由**入口主循环**调（3325 行），已完全在组件栈之外。
    pump_terminal();
    if (bottom_close_pending_) {
      bottom_close_pending_ = false;
      bottom_visible_.set(false);
      // ⚠ **必须同时清掉指针**：下一帧重组会把 `Terminal` 元素拆掉
      //（面板不再声明它），而 `terminal_ptr` 会变成悬垂——之后每帧的
      // `pump_terminal()` 都在解引用已释放内存（实测：界面整个卡死）。
      // 重开面板时 `build_bottom` 会重新赋值。
      terminal_ptr = nullptr;
    }
  }

  void build(Composer& c) override {
    const auto& buffers = buffers_.value();
    const std::size_t active = active_.value();
    const bool has_editor = !buffers.empty();

    // 编辑器实例可能在本次重组中新建/换绑：先置空，由 build_editor_area 重新取得
    editor = nullptr;
    terminal_scroll_view_ = nullptr;   // 每次重组重新取得（旧指针可能已被销毁）
    // **浮层里的输入框同理**：它们在上一次重组里被销毁，本帧还没重建。
    //
    // 不置空就会踩到已释放的 Input——实测（无头 E2E，转到行浮层）：
    // “聚焦输入框”的待办曾排在 `build_goto_line` **之前**，拿着上一帧的
    // `goto_line_input` 调 `host()->set_keyboard_focus(...)`，无头下随机
    // SIGSEGV（0xC0000005），带窗口时只表现为“焦点偶尔不对”。
    find_needle_input = nullptr;
    goto_line_input = nullptr;
    // `grow = true` 不可省：本页住在窗框的**内容槽**（列容器）里，不 grow 就只按内容的
    // 自然高度占位——实测 800px 窗口里页面只有 525px 高，底部剩 195px 空白，
    // 编辑器被压到 295px。留白与「代码区太矮」是同一个根因。
    column(c, {.gap = 0.0f, .grow = true, .id = "editor-page"}, [&] {
      build_workbench(c, buffers, active, has_editor);
      build_status_bar(c);
      build_find_bar(c);
    });
    push_title();   // 标题栏（外壳）的文案仍由本页推——见 `push_title` 的注释
    // 命令面板：**条件声明**（关掉 = 本帧不声明 → 框架 sweep 摘除）
    if (palette_open_.value()) build_palette(c);
    // 编辑器右键菜单 / 关闭确认对话框 / 快捷键一览：同一套条件声明。
    build_editor_context(c);
    build_close_confirm(c);
    build_shortcuts_card(c);
    build_open_dialog(c);
    build_goto_line(c);
    // 浮层内的“聚焦输入框”待办：**必须排在对应的构建之后**——
    // 排在前面拿到的是上一帧已销毁的指针（本函数开头已把两个指针置空，
    // 所以即使顺序被人改回去，也只是“这一次没聚焦”，不会再踩已释放对象）。
    apply_pending_focus();
    apply_pending_goto_focus();
    // 文本灌入：只在本帧的编辑器实例与「已装载的标签」不一致时写
    // （`set_text` 会清撤销栈并把光标归零；每次重组都写会让打字被重置——实测踩到）
    if (editor != nullptr && active < buffers.size() && buffers[active].key != loaded_key_) {
      editor->set_language(buffers[active].language);
      editor->set_text(buffers[active].text);
      loaded_key_ = buffers[active].key;
    }
  }

  // ————————————————————————————————————————————————————————————————————————
  // `MenuHeader`：长在标题栏 `leading` 槽里的那一小块（菜单栏 + 它的下拉面板）
  // ————————————————————————————————————————————————————————————————————————
  //
  // 挂载点：标题栏的 `leading` 槽（`[leading…][标题][trailing…][控制按钮]`）。
  // 为什么用 `leading`（而不是 `trailing`）：这一版的产品形态是**菜单在左、标题跟在其后**
  // （省下一整行 32px 给内容区，窗口顶部的拖动/双击语义仍归标题栏）。
  // 挂在 `leading` 后 `TitleBar::arrange` 会自动做三件事：标题**让到菜单右侧**、
  // 图标让位给槽（两者都画会叠在一起）、`drag_rect` 不再把这一行算成拖动区。
  //
  // 为什么单独成一个 `Component` 而不是让本页 `build()` 多画一块：标题栏是
  // **外壳**（`WindowFrame` 自持、在声明树之外），本页的 `build()` 只管内容槽。
  // `mount_into` 正好表达「声明式的一小块挂到既有元素的槽位」——于是整个 IDE
  // 仍是一个 `Component`（只是它现在占两处树位），菜单与标题栏合并成一行。
  //
  // 它只借宿主两样东西：`menu_open_`（打开的是哪一个，本页也要读它做快捷键）
  // 与 `on_menu`（选条目）。菜单数据与面板 overlay 都归它自己。

  struct MenuHeader : Component {
    CodeEditorPage* page{nullptr};
    MenuBar* bar{nullptr};

    void build(Composer& c) override {
      static const std::vector<MenuData> kMenus = {
          {"file", "文件",
           {{.id = "new", .label = "新建文件"},
            {.id = "open", .label = "打开文件…"},
            {.id = "open-folder", .label = "打开文件夹…（Ctrl+Shift+O）"},
            {.id = "save-as", .label = "另存为…"},
            {.separator = true},
            {.id = "save", .label = "保存（Ctrl+S）"},
            {.separator = true},
            {.id = "close-tab", .label = "关闭编辑器（Ctrl+W）"},
            {.id = "close-all", .label = "关闭全部编辑器"}}},
          {"edit", "编辑",
           {{.id = "undo", .label = "撤销"},
            {.id = "redo", .label = "重做"},
            {.separator = true},
            {.id = "comment", .label = "切换行注释（Ctrl+/）"},
            {.id = "select-all", .label = "全选"}}},
          {"selection", "选择",
           {{.id = "select-all", .label = "全选（Ctrl+A）"},
            {.id = "goto-line", .label = "转到行…"},
            {.id = "copy", .label = "复制"},
            {.id = "paste", .label = "粘贴"}}},
          {"view", "查看",
           {{.id = "command-palette", .label = "命令面板…（Ctrl+Shift+P）"},
            {.separator = true},
            {.id = "toggle-sidebar", .label = "切换侧栏可见性（Ctrl+B）"},
            {.id = "find", .label = "查找（Ctrl+F）"},
            {.id = "toggle-terminal", .label = "切换底部面板（Ctrl+J）"},
            {.id = "toggle-theme", .label = "切换亮/暗主题"}}},
          {"run", "运行",
           {{.id = "run-task", .label = "运行任务：构建 gallery"},
            {.id = "run-test", .label = "运行任务：st test"},
            {.id = "run-lint", .label = "运行任务：st lint"},
            {.separator = true},
            {.id = "toggle-terminal", .label = "切换终端（底部面板）"}}},
          {"help", "帮助",
           {{.id = "about", .label = "关于 歌白代码"},
            {.id = "shortcuts", .label = "键盘快捷键…"}}}};
      bar = dsl::menu_bar(
          c, kMenus,
          [this](const std::string& menu, const std::string& item) { page->on_menu(menu, item); },
          [this](std::size_t index) {
            page->menu_open_.set(page->menu_open_.value() == index ? kNoMenu : index);
          },
          {.id = "menubar"});
      // 打开状态 → 下一帧声明面板 overlay（不声明 = 自动消失，框架 sweep）
      const std::size_t open = page->menu_open_.value();
      if (open != kNoMenu && bar != nullptr) dsl::menu_panel_overlay(c, *bar, open);
    }
  };

  void on_menu(const std::string& menu, const std::string& item) {
    menu_open_.set(kNoMenu);   // 选完即关
    if (item == "toggle-terminal") {
      // 两个菜单（查看/运行）都指向它：开合终端面板
      bottom_visible_.set(!bottom_visible_.value());
      return;
    }
    if (menu == "file" && item == "save") {
      save();
    } else if (menu == "file" && item == "close-tab") {
      close_active();
    } else if (menu == "file" && item == "close-all") {
      close_all();
    } else if (menu == "file" && item == "save-as") {
      save_as();
    } else if (menu == "file" && (item == "new" || item == "open")) {
      if (item == "new") new_file_prompt();
      else open_file_dialog();
      } else if (menu == "file" && item == "open-folder") {
        open_folder_dialog();
    } else if (menu == "view" && item == "command-palette") {
      open_palette(false);
    } else if (menu == "view" && item == "toggle-sidebar") {
      sidebar_visible_.set(!sidebar_visible_.value());
    } else if (menu == "view" && item == "find") {
      open_find();
    } else if (menu == "view" && item == "toggle-theme") {
      toggle_theme();
    } else if (menu == "run" && (item == "run-task" || item == "run-test" || item == "run-lint")) {
      run_task(item == "run-task" ? "build" : (item == "run-test" ? "test" : "lint"));
    } else if (menu == "help" && item == "about") {
      status_.set("歌白代码 0.1.0 · 霜天框架示例 · 声明式组装");
    } else if (menu == "help" && item == "shortcuts") {
      shortcuts_open_.set(true);
    } else {
      apply_editor_menu(menu, item);
    }
  }

  /// 编辑/选择菜单：直接作用于当前编辑器（一次性命令，不涉及界面形态）。
  void apply_editor_menu(const std::string& menu, const std::string& item) {
    (void)menu;
    if (editor == nullptr) return;
    if (item == "undo") editor->undo();
    else if (item == "redo") editor->redo();
    else if (item == "comment") editor->toggle_comment();
    else if (item == "select-all") editor->select_all();
    else if (item == "copy") {
      // 真实复制：写编辑器剪贴板（无头环境没有系统剪贴板，见组件侧说明）
      const std::string picked = editor->selected_text();
      status_.set(picked.empty() ? "没有选中内容"
                                 : std::format("已复制 {} 字符", st::utf8_length(picked)));
      (void)editor->invoke_action("copy", {});
    } else if (item == "paste") {
      (void)editor->invoke_action("paste", {});
    } else if (item == "cut") {
      (void)editor->invoke_action("cut", {});
    } else if (item == "goto-line") {
      open_goto_line();
    }
  }

  // —— 转到行（Ctrl+G / 选择菜单）：一个小浮层 + 输入框，不靠“猜” ——
  //
  // 此前这个菜单项被实现成“跳到第一个问题行”，并与用户按下的 Ctrl+G 毫无关系——
  // 菜单项叫“转到行…”而行为是别的，属于可信度问题（用户试一次就不再信任何菜单项）。
  // 现在它是一条真链路：输入行号 → 回车 → 光标与视图都到那一行。
  void open_goto_line() {
    if (editor == nullptr) {
      status_.set("转到行：没有打开的编辑器");
      return;
    }
    goto_line_open_.set(true);
    goto_line_error_.set("");
    goto_line_pending_focus_ = true;
  }

  /// 提交行号（“3” 或 “3:8” 行:列）。
  ///
  /// 输入以**纯文本**传进来：浮层里输入框的 Enter 可能来自键盘、也可能来自
  /// 控制通道的 `invoke submit`——两条路都是 `Input::on_submit`，语义相同。
  void submit_goto_line(std::string_view text) {
    if (editor == nullptr) return;
    const std::string spec(st::trim(std::string(text)));
    if (spec.empty()) {
      goto_line_error_.set("请输入行号");
      return;
    }
    std::size_t line = 0;
    std::size_t column = 0;
    if (const std::size_t colon = spec.find(':'); colon != std::string::npos) {
      line = st::parse_u64(spec.substr(0, colon)).value_or(0);
      column = st::parse_u64(spec.substr(colon + 1)).value_or(0);
    } else {
      line = st::parse_u64(spec).value_or(0);
    }
    // 越界如实拒绝并报出**真实行数**：静默夹取会让用户以为“跳到末尾了”，
    // 而他打的是一串无效字符。
    if (line == 0) {
      goto_line_error_.set("行号必须是正整数");
      return;
    }
    const std::size_t total = editor->line_count();
    if (line > total) {
      goto_line_error_.set(std::format("超出范围（共 {} 行）", total));
      return;
    }
    goto_line_open_.set(false);
    goto_line_error_.set("");
    editor->goto_line(line);
    editor->scroll_to_line(line);
    if (column > 0) {
      const std::size_t start = editor->cursor_index() + column - 1;
      editor->set_selection(start, start);
    }
    on_cursor_moved();   // 状态栏的 Ln/Col 跟着走（不只依赖编辑器的回调）
    // 状态栏报**实际落点**（而不是用户输入的那串数字）：填了行:列时，列号会被
    // 行尾/换行夹取（空行上尤其明显），报“用户输入的”会与画面上的光标对不上。
    status_.set(std::format("已跳到第 {} 行 第 {} 列", editor->cursor_line() + 1,
                            editor->cursor_column() + 1));
  }

  /// 帧首处理“转到行浮层已出现→把焦点交给输入框”（构建期拿不到输入框实例）。
  /// 与查找条同一机制（见 `apply_pending_focus`）。
  void apply_pending_goto_focus() {
    if (!goto_line_pending_focus_) return;
    if (goto_line_input == nullptr) return;
    goto_line_pending_focus_ = false;
    if (!goto_line_open_.value()) return;
    if (auto* host = goto_line_input->host(); host != nullptr) {
      (void)host->set_keyboard_focus(goto_line_input);
    }
  }

  /// 转到行浮层（Ctrl+G / 选择菜单 → “转到行…”。）
  void build_goto_line(Composer& c) {
    if (!goto_line_open_.value()) return;
    (void)overlay(c, "goto-line", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 8.0f, .id = "goto-bar"}, [&] {
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)text(c, [] { return std::string("转到行"); });
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("goto-input");
            field.set_placeholder("行号（或 行:列）");
            field.style().width = 160.0f;
            goto_line_input = &field;
            field.on_submit = [this](std::string_view value) {
              // **按值**拷贝：提交会关掉浮层，下一次重组就销毁这个输入框，
              // 再靠引用/指针读它的缓冲就是悬垂访问（跨帧存裸指针的同一类坑）。
              submit_goto_line(std::string(value));
            };
          }, {.key = "goto-input"});
          (void)button(c, "跳转", [this] {
            submit_goto_line(goto_line_input != nullptr ? goto_line_input->value() : std::string{});
          }, {.id = "goto-go"});
          (void)button(c, "×", [this] {
            goto_line_open_.set(false);
            goto_line_error_.set("");
          }, {.id = "goto-close"});
        });
        // 出错信息就住在同一个浮层里（而不是把浮层关掉、把话丢到状态栏——
        // 那样用户得往窗口底部去找，还找不到是“哪一次输入”的错）。
        if (!goto_line_error_.value().empty()) {
          (void)text(c, [this] { return goto_line_error_.value(); }, {.id = "goto-error"});
        }
      });
    });
  }

  // —— 3. 主体三栏 ——
  void build_main(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                  bool has_editor) {
    row(c, {.grow = true, .id = "main-row"}, [&] {
      // ⚠ **活动栏不在这里**：它在 `build_workbench` 那一层（与底部面板并列、
      // 满高）。本函数只管“侧栏与编辑区”这一对横向分栏。
      //
      // **侧栏不常驻**（活动栏常驻，两者是独立的东西）：活动栏是“能去哪儿”的入口
      //（切视图的唯一途径），侧栏是“当前视图的内容”。曾经把两者绑在一个
      // `sidebar_visible_` 上，于是关掉侧栏连入口一起没了（实测：Ctrl+B 之后
      // 活动栏消失、无从点回去）。
      if (!sidebar_visible_.value()) {
        build_editor_area(c, buffers, active, has_editor);
        return;
      }
      // 侧栏与编辑区之间用 SplitView（宽度可拖）——**两个子面板就是它的两个子位**，
      // 声明式里用 `custom_container<SplitView>` 让两次子声明分别落到 first/second。
      (void)custom_container<SplitView>(
          c,
          [&] {
            (void)custom_container<Panel>(c, [&] { build_sidebar(c, buffers, active); },
                                          [](Panel& panel) { panel.set_id("sidebar"); },
                                          {.key = "sidebar"});
            (void)custom_container<Panel>(
                c, [&] { build_editor_area(c, buffers, active, has_editor); },
                [](Panel& panel) { panel.set_id("editor-column"); }, {.grow = true, .key = "editor"});
          },
          [](SplitView& split) {
            split.set_id("sidebar-split");
            split.set_min_ratio(0.12f);
            split.set_ratio(0.22f, false);
          },
          {.grow = true, .id = "sidebar-split"});
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
    const std::size_t current = activity_.value();
    // `surface = Alt`：与标题栏/状态栏**同色**——三者合起来是一圈“外壳”，
    // 色不一致会在拐角处露馅（见 `Element::Surface` 的说明）。
    column(c, {.gap = 2.0f, .padding_y = 4.0f, .width = 44.0f,
               .surface = Element::Surface::Alt, .id = "activity-bar"}, [&] {
      for (std::size_t index = 0; index < std::size(kActivities); ++index) {
        // 图标按钮：`custom<Button>`（Button 的 icon 是一等接口，属性面没有）
        const std::string id = std::string("activity-") + kActivities[index].id;
        // **选中态只取决于“当前是哪个视图”，与侧栏展开与否无关**。
        // 活动栏常驻之后，它就是“当前视图在哪”的唯一指示——收起侧栏时也必须有，
        // 否则用户看不出收起来的是哪一栏（VSCode 同此：收起侧栏后该项仍高亮）。
        const bool on = index == current;
        (void)custom<Button>(c, [&, index, on](Button& b) {
          b.set_id(id);
          b.set_icon(kActivities[index].icon);
          b.set_variant(on ? Button::Variant::Soft : Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this, index] {
            // 点当前视图 = 收起侧栏（IDEA/VSCode 通行行为）；点别处 = 切视图并展开。
            if (activity_.value() == index && sidebar_visible_.value()) {
              sidebar_visible_.set(false);
              return;
            }
            activity_.set(index);
            sidebar_visible_.set(true);
          };
        }, {.width = 40.0f, .height = 36.0f, .key = kActivities[index].id});
      }
      // —— 下组：终端 ——
      //
      // 用**弹性空隙**把它与上面五个**视图**分开（VSCode / Windows Terminal 的惯例：
      // 下组是“工具/面板”（开关类的），上组是“导航”（互斥切视图的））。
      // 注意空格子必须带 `grow`：裸 `spacer` 在列里只是固定高的小段，
      // 不会把后面的项顶到底部（实测：终端按钮被挤在资源树下面，看不见）。
      (void)column(c, {.grow = true, .id = "activity-spacer"}, [&] {});
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("activity-terminal");
        b.set_icon("terminal");
        // 面板开着 = 激活态（与上组一致：状态从真实可见性推，不是另存的标记）。
        b.set_variant(bottom_visible_.value() ? Button::Variant::Soft : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { toggle_terminal_panel(); };
      }, {.width = 40.0f, .height = 36.0f, .key = "terminal"});
    });
  }

  /// 开关底部终端面板（活动栏按钮 / `` Ctrl+` `` 同一条路径）。
  ///
  /// 打开时把键盘焦点交给终端输入行（`Terminal` 自己会在下一次事件里接住），
  /// 这是“点了终端按钮就能直接敲命令”的那一步。
  void toggle_terminal_panel() {
    const bool next = !bottom_visible_.value();
    bottom_visible_.set(next);
    status_.set(next ? "终端：已展开" : "终端：已收起");
  }

  /// 工作台各视图的标题（侧栏顶部一行：标题 + 该视图的动作按钮）。
  void build_sidebar_head(Composer& c, std::string_view title) {
    (void)row(c, {.gap = 4.0f, .height = 30.0f, .id = "sidebar-head"}, [&] {
      (void)text(c, [title] { return std::string(title); },
                 {.grow = true, .weight = FontWeight::Medium, .id = "sidebar-title"});
      build_sidebar_actions(c);
    });
  }

  /// 各视图的动作按钮（按当前视图给不同的一组）。
  void build_sidebar_actions(Composer& c) {
    const auto icon_button = [&](const char* id, const char* icon, const char* tip,
                                 std::function<void()> on_click) {
      (void)custom<Button>(c, [this, id, icon, tip, on_click = std::move(on_click)](Button& b) {
        b.set_id(id);
        b.set_icon(icon);
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = on_click;
      }, {.width = 24.0f, .height = 24.0f, .key = id});
      (void)tip;
    };
    switch (activity_.value()) {
      case 0:
        icon_button("explorer-refresh", "refresh", "刷新", [this] { refresh_tree(); });
        icon_button("explorer-new-file", "plus", "新建文件", [this] { new_file_prompt(); });
        icon_button("explorer-save", "download", "保存当前", [this] { save(); });
        break;
      case 1:
        icon_button("search-go", "search", "重新搜索", [this] { run_search(last_query_); });
        icon_button("search-clear", "close", "清空结果", [this] {
          search_hits_.set({});
          search_hit_paths_.clear();
          last_query_.clear();
        });
        break;
      case 2:
        icon_button("scm-refresh", "refresh", "刷新状态", [this] { refresh_git(); });
        break;
      case 3:
        icon_button("run-build", "play", "构建", [this] { run_task("build"); });
        icon_button("run-test", "check", "测试", [this] { run_task("test"); });
        break;
      default:
        icon_button("ext-refresh", "refresh", "重新枚举", [this] { refresh_languages(); });
        break;
    }
  }

  /// 侧栏内容（按活动栏选中项切换）。
  ///
  /// 五个视图各自独立成函数：视图多起来之后「一熊 if/else 里塞五段界面」会让每加一个
  /// 视图都要重读一整堆无关代码。
  void build_sidebar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    (void)buffers;
    (void)active;
    column(c, {.gap = 6.0f, .padding = 10.0f, .grow = true, .id = "sidebar-body"}, [&] {
      switch (activity_.value()) {
        case 0: build_explorer(c); break;
        case 1: build_search_view(c); break;
        case 2: build_scm_view(c); break;
        case 3: build_run_view(c); break;
        default: build_extensions_view(c); break;
      }
    });
  }

  // —— 视图 0：资源管理器（真工作区树 / 内置样例列表）——
  void build_explorer(Composer& c) {
    build_sidebar_head(c, workspace_.empty() ? "内置样例" : "资源管理器");
    if (workspace_.empty()) {
      // 无 `--workspace`：仍是自包含演示，但用 Tree 形态（与真实工作区同一套视觉）,
      // 且按扩展名给图标——否则“有工作区/无工作区”两份界面的语言不一致。
      std::vector<TreeNodeData> nodes;
      for (const auto& sample : files_) {
        nodes.push_back(TreeNodeData{.key = "sample:" + sample.name, .label = sample.tree_key});
      }
      nodes.push_back(TreeNodeData{.key = "sample:stlog.log", .label = "logs/app.stlog"});
      (void)tree(c, nodes, {},
                 [this](const std::string& key) {
                   const std::string name = key.substr(7);
                   if (name == "stlog.log") open_stlog();
                   else open_by_name(name);
                 },
                 {.grow = true, .id = "sample-tree"});
      return;
    }
    (void)custom_container<ScrollView>(
        c,
        [&] {
          (void)tree(c, workspace_nodes(),
                     [this](const std::string& key, bool expanded) {
                       on_tree_toggle(key, expanded);
                     },
                     [this](const std::string& key) {
                       if (key.rfind("dir:", 0) != 0) open_path(key);
                     },
                     {.id = "workspace-tree"});
        },
        [](ScrollView& scroll) { scroll.set_id("explorer-scroll"); },
        {.grow = true, .id = "explorer-host"});
    if (workspace_nodes().empty()) {
      (void)text(c, [this] { return std::string("目录为空或不可读：") + workspace_; });
    }
  }

  // —— 视图 1：搜索（工作区文本 + 按文件分组）——
  void build_search_view(Composer& c) {
    build_sidebar_head(c, "搜索");
    (void)custom<Input>(c, [this](Input& field) {
      field.set_id("search-input");
      field.set_placeholder("搜索（回车执行）");
      field.set_icon_prefix("search");
      field.on_submit = [this](std::string_view query) { apply_search(std::string(query)); };
    }, {.key = "search-input"});
    // 选项行：glob 过滤 + 大小写 / 全词 / 正则（与 VSCode 的查找条同族）
    (void)custom<Input>(c, [this](Input& field) {
      field.set_id("search-glob");
      field.set_placeholder("文件过滤（如 *.cpp, src/**）");
      field.on_submit = [this](std::string_view glob) {
        search_glob_ = std::string(glob);
        apply_search(last_query_);
      };
    }, {.key = "search-glob"});
    (void)row(c, {.gap = 4.0f}, [&] {
      toggle_chip(c, "search-case", "Aa", search_case_.value(), [this] {
        search_case_.set(!search_case_.value());
        apply_search(last_query_);
      });
      toggle_chip(c, "search-word", "ab", search_word_.value(), [this] {
        search_word_.set(!search_word_.value());
        apply_search(last_query_);
      });
      toggle_chip(c, "search-regex", ".*", search_regex_.value(), [this] {
        search_regex_.set(!search_regex_.value());
        apply_search(last_query_);
      });
      (void)spacer(c);
      (void)text(c, [this] { return search_summary_.value(); }, {.id = "search-summary"});
    });
    if (search_hits_.value().empty()) {
      (void)text(c, [this] {
        return search_summary_.value().empty()
                   ? std::string("输入关键词后回车，搜索整个工作区")
                   : std::string("没有匹配");
      }, {.id = "search-empty"});
      return;
    }
    // 结果按**文件分组**：条目 key 用 `路径:行号`（业务身份），前缀行不可点。
    std::vector<ListItemData> items;
    for (std::size_t index = 0; index < search_hits_.value().size(); ++index) {
      items.push_back(ListItemData{.key = std::format("hit:{}", index),
                                   .label = search_hits_.value()[index]});
    }
    (void)custom_container<ScrollView>(
        c,
        [&] {
          (void)list(c, items, [this](std::size_t index) { activate_search_hit(index); },
                     {.id = "search-results"});
        },
        [](ScrollView& scroll) { scroll.set_id("search-scroll"); },
        {.grow = true, .id = "search-host"});
  }

  /// 开关胶囊（「Aa / ab / .*」这类选项）：按下态由 `active` 决定。
  ///
  /// 为什么不用 `Checkbox`：查找选项是**密集的一行小开关**（三个挤在 200px 宽里），
  /// 带文字的勾选框会把行撑到放不下——胶囊用固定 26px 宽，观感与 VSCode 一致。
  void toggle_chip(Composer& c, const char* id, const char* label, bool active,
                   std::function<void()> on_click) {
    (void)custom<Button>(c, [id, label, active, on_click = std::move(on_click)](Button& b) {
      b.set_id(id);
      b.set_label(label);
      b.set_variant(active ? Button::Variant::Soft : Button::Variant::Ghost);
      b.set_size(Button::Size::Small);
      b.on_click = on_click;
    }, {.width = 30.0f, .height = 24.0f, .key = id});
  }

  // —— 视图 2：源代码管理（真 `git status`）——
  void build_scm_view(Composer& c) {
    build_sidebar_head(c, "源代码管理");
    if (!workspace_.empty() && git_branch_.empty() && !git_probed_) {
      refresh_git();
    }
    if (workspace_.empty()) {
      (void)text(c, [] { return std::string("内置样例模式：没有工作区可查 Git 状态"); });
      return;
    }
    (void)row(c, {.gap = 6.0f}, [&] {
      (void)icon(c, "git-branch", 14.0f);
      (void)text(c, [this] {
        return git_branch_.empty() ? std::string("（未探测）") : git_branch_;
      }, {.grow = true, .id = "git-branch-label"});
      (void)text(c, [this] { return std::format("{} 项变更", git_changes_.size()); },
                 {.id = "git-change-count"});
    });
    if (!git_error_.empty()) {
      (void)text(c, [this] { return git_error_; }, {.color = Tone::Danger, .id = "git-error"});
    }
    // 变更列表必须住在**滚动容器**里（与资源管理器/搜索同一个 `ScrollView` 契约）：
    // 侧栏是固定宽度的窄列，而 `Button` 的测量不理会父宽（按标签文本算），
    // 直接当列子元素会在长路径上**横向溢出到编辑区**（实测：变更条目叠在代码上方），
    // 而 `ScrollView` 会给子元素一个被夹到视口宽的约束。
    (void)custom_container<ScrollView>(
        c,
        [&] {
          for (const auto& change : git_changes_) {
            (void)custom<Button>(c, [this, change](Button& b) {
              b.set_label(std::format("{}  {}", change.status, change.path));
              b.set_variant(Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              b.set_icon("edit");
              b.on_click = [this, change] { show_git_diff(change); };
            }, {.key = "git:" + change.path});
          }
          if (git_changes_.empty() && git_error_.empty() && !git_branch_.empty()) {
            (void)text(c, [] { return std::string("工作区干净，没有未提交的变更"); });
          }
        },
        [](ScrollView& scroll) { scroll.set_id("scm-scroll"); },
        {.grow = true, .id = "scm-host"});
  }

  // —— 视图 3：运行与调试（真跑 st 子命令）——
  void build_run_view(Composer& c) {
    build_sidebar_head(c, "运行");
    struct Task {
      const char* id;
      const char* label;
      const char* detail;
    };
    static constexpr Task kTasks[] = {{"build", "构建当前目标", "st build gallery --profile dev"},
                                      {"test", "跑框架单测", "st test"},
                                      {"lint", "禁令扫描", "st lint"}};
    for (const Task& task : kTasks) {
      (void)row(c, {.gap = 6.0f}, [&] {
        (void)custom<Button>(c, [this, task](Button& b) {
          b.set_id(std::string("task-") + task.id);
          b.set_label(task.label);
          b.set_icon("play");
          b.set_variant(Button::Variant::Secondary);
          b.set_size(Button::Size::Small);
          b.on_click = [this, id = std::string(task.id)] { run_task(id); };
        }, {.height = 28.0f, .grow = true, .key = task.id});
      });
      (void)text(c, [task] { return std::string("  ") + task.detail; });
    }
    (void)text(c, [] { return std::string("任务输出落在底部终端（实时回流）"); });
    // “有没有活在干”问 **any_busy**：作业归属发起它的会话，用户此刻可能在别的标签上。
    if (terminal_ptr != nullptr && terminal_ptr->any_busy()) {
      (void)row(c, {.gap = 6.0f}, [&] {
        (void)custom<Spinner>(c, [](Spinner& s) { s.set_id("task-spinner"); },
                              {.width = 18.0f, .height = 18.0f, .key = "spinner"});
        (void)text(c, [this] {
          const std::string name = terminal_ptr != nullptr
                                       ? terminal_ptr->get_property("running_name")
                                             .value_or(std::string{})
                                       : std::string{};
          return name.empty() ? std::string("终端忙") : "终端忙：" + name;
        }, {.id = "task-running"});
      });
    }
  }

  // —— 视图 4：扩展（运行时注册的语言清单）——
  void build_extensions_view(Composer& c) {
    build_sidebar_head(c, "语言与扩展");
    if (languages_.empty()) refresh_languages();
    (void)text(c, [] { return std::string("已注册的语法（点一下切到该语言）"); });
    // 同上：语言清单很长，必须在滚动容器里（否则窄侧栏装不下会溢出）。
    (void)custom_container<ScrollView>(
        c,
        [&] {
          for (const auto& name : languages_) {
            (void)custom<Button>(c, [this, name](Button& b) {
              b.set_label(name);
              b.set_variant(language_.value() == name ? Button::Variant::Soft
                                                     : Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              b.on_click = [this, name] { set_language(name); };
            }, {.key = "lang:" + name});
          }
        },
        [](ScrollView& scroll) { scroll.set_id("ext-scroll"); },
        {.grow = true, .id = "ext-host"});
  }

  /// 切当前编辑器的高亮语言（也改写标签的语言字段，切回来时保持一致）。
  void set_language(const std::string& name) {
    language_.set(name);
    auto list = buffers_.value();
    if (active_.value() < list.size()) list[active_.value()].language = name;
    buffers_.set(std::move(list));
    if (editor != nullptr) editor->set_language(name);
    status_.set("语言已切为 " + name);
  }

  void build_editor_area(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                         bool has_editor) {
    column(c, {.gap = 0.0f, .grow = true, .id = "editor-area"}, [&] {
      std::vector<TabData> tabs;
      for (const auto& buffer : buffers) {
        tabs.push_back(TabData{.key = buffer.key, .label = buffer.label,
                               .modified = buffer.dirty, .closable = true});
      }
      // 标签条与它的动作区**同一行**：左侧标签（溢出自滚）、右侧固定宽的动作组。
      // 为什么不把动作挂进 Tabs 自己：它是自绘组件，不产生子元素，没有“尾部槽位”。
      (void)row(c, {.gap = 0.0f, .height = 34.0f, .id = "tabbar"}, [&] {
        (void)dsl::tabs(c, tabs, active, [this](std::size_t index) { switch_tab(index); },
                        [this](const std::string& key) { request_close(key); },
                        {.grow = true, .id = "editor-tabs"});
        (void)custom<Button>(c, [this](Button& b) {
          b.set_id("tab-close-all");
          b.set_icon("trash");
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this] { close_all(); };
        }, {.width = 30.0f, .height = 34.0f, .key = "close-all"});
      });
      if (!has_editor) {
        column(c, {.padding = 24.0f, .grow = true, .id = "empty-hint"}, [&] {
          (void)icon(c, "code", 40.0f);
          (void)text(c, [] {
            return std::string("从左侧资源管理器打开一个文件，或 Ctrl+Shift+P 打开命令面板");
          });
        });
        return;
      }
      // 编辑器头部**只保留标签栏**（用户 2026-10-08 要求“头部内容都去掉，只保留 tab 和内容”）。
      //
      // 去掉了两块（它们的定义保留在文件里，见 `build_editor_toolbar` /
      // `build_breadcrumb`——应用如有需要可随时接回）：
      // * `editor-toolbar`（保存/撤销/重做/注释/参考线/右键菜单入口，30px）；
      // * `breadcrumb`（路径面包屑，26px）。
      //
      // 两条理由：① 共 56px 垂直空间还给代码（编辑器是主角）；
      // ② 那些动作**本来就有**键键与菜单两条入口（Ctrl+S/Ctrl+Z/…、菜单栏、
      // 右键菜单），零鼠标可达性不靠这条工具栏——与它当初存在的理由
      //（“鼠标用户没有第三条腿”）相比，空间收益更实在。
      //
      // 编辑器：custom<T> 逃生舱（CodeEditor 的一等接口属性面覆盖不到）。
      //
      // **只写“持久配置”**（字体/行宽），且每项都自带相等早退——它们在语义上是
      // 宿主配置、不归属性面管。**语言与只读态不在这里写**：它们由“当前标签”决定
      // （见 `build()` 末尾的装载逻辑与 `#editor` 的动作面），每次重组都重设会把
      // 控制通道 `set language=...` / `set read_only=true` 当场抹掉（实测：切一次
      // 底部面板就全回默认值）。
      (void)custom<CodeEditor>(c, [this](CodeEditor& ed) {
        ed.set_id("editor");   // 控制通道钩子：tools/*.py 依赖
    // 字号档位只在**首次**装上时设一次（见 `configure_editor`）。
        // 每帧无条件写会把控制通道 / 设置项的 `set font_scale=…` 当场抹掉——
        // 与 `read_only` 同一类缺陷（实测：把档位改成 1.0，下一帧就变回 1.15）。
        if (!editor_configured_) {
          ed.set_font_scale(editor_font_scale_);
          ed.set_line_spacing(editor_line_spacing_);
          ed.set_tab_width(4);
          editor_configured_ = true;
        }
        ed.style().grow = true;
        ed.style().padding = st::math::Insets{8.0f, 4.0f, 8.0f, 4.0f};
        ed.on_change = [this](std::string_view) { on_edit(); };
        ed.on_cursor_change = [this] { on_cursor_moved(); };
        // 右键菜单走组件的一等回调（不是 `set_event_handler`——那个永远轮不到：
        // `CodeEditor::on_event` 对任何按钮的按下都返回 true）。
        ed.on_context_menu = [this](st::math::Point at) { open_context_menu(at); };
        editor = &ed;
      }, {.grow = true, .id = "editor-host"});
    });
  }

  /// 编辑器工具栏（在标签栏下方）：保存 / 撤销 / 重做 / 注释 / 参考线 / 右键菜单入口。
  ///
  /// 为什么要有它（VSCode 其实没有这条工具栏）：霜天的控制通道与键盘快捷键是两条腿，
  /// 而**鼠标用户没有第三条腿**——“保存/撤销”这些最常用的动作在纯鼠标下无处可点。
  /// 工具栏把它们变成可发现、可点击、且有稳定 id 的元素（自动化也能直接 `invoke`）。
  void build_editor_toolbar(Composer& c, const std::vector<OpenBuffer>& buffers,
                            std::size_t active) {
    (void)row(c, {.gap = 2.0f, .padding_x = 8.0f, .height = 30.0f, .id = "editor-toolbar"}, [&] {
      const auto tool = [&](const char* id, const char* icon, const char* label,
                            std::function<void()> on_click) {
        (void)custom<Button>(c, [id, icon, label, on_click = std::move(on_click)](Button& b) {
          b.set_id(id);
          b.set_icon(icon);
          b.set_label(label);
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = on_click;
        }, {.height = 26.0f, .key = id});
      };
      tool("tool-save", "download", "保存", [this] { save(); });
      tool("tool-undo", "arrow-left", "撤销", [this] {
        if (editor != nullptr) (void)editor->undo();
      });
      tool("tool-redo", "arrow-right", "重做", [this] {
        if (editor != nullptr) (void)editor->redo();
      });
      tool("tool-comment", "code", "注释", [this] {
        if (editor != nullptr) (void)editor->toggle_comment();
      });
      (void)spacer(c);
      const bool dirty = active < buffers.size() && buffers[active].dirty;
      (void)text(c, [dirty] { return dirty ? std::string("● 未保存") : std::string("已保存"); },
                 {.id = "editor-dirty-hint"});
      tool("tool-find", "search", "查找", [this] { open_find(); });
      tool("tool-close", "close", "关闭", [this] { close_active(); });
    });
  }

  /// 面包屑：当前文件路径按 `/` 拆段（段可点——点目录段切到资源管理器并展开）。
  void build_breadcrumb(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    if (active >= buffers.size()) return;
    const OpenBuffer& buffer = buffers[active];
    // 显示路径：真实文件取相对工作区的路径；内置样例用 `tree_key`。
    std::string shown = buffer.path.empty()
                            ? buffer.key
                            : (workspace_.empty() ? buffer.path
                                                  : st::fs::relative_to(buffer.path, workspace_));
    const auto segments = st::split(shown, '/');
    (void)row(c, {.gap = 2.0f, .padding_x = 10.0f, .height = 26.0f, .id = "breadcrumb"}, [&] {
      for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
          (void)icon(c, "chevron-right", 11.0f, {.id = std::format("crumb-sep-{}", index)});
        }
        const bool last = index + 1 == segments.size();
        const std::string segment(segments[index]);
        (void)custom<Button>(c, [this, segment, last](Button& b) {
          b.set_label(segment);
          b.set_id(std::format("crumb-{}", segment));
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this, segment, last] {
            // 点目录段：切到资源管理器（树里高亮/展开由树自身管理）；点文件名段=无动作。
            activity_.set(0);
            sidebar_visible_.set(true);
            status_.set(last ? "当前文件：" + segment : "已切到资源管理器：" + segment);
          };
        }, {.height = 22.0f, .key = "crumb:" + segment});
      }
    });
  }

  /// 打开编辑器右键菜单（由 `CodeEditor::on_context_menu` 回调调用）。
  void open_context_menu(st::math::Point at) {
    context_anchor_ = at;
    context_open_.set(true);
  }

  // —— 4. 上半 + 终端：一个**上下可分栏**的两段（拖动改高度，见 build）——
  //
  // 为什么中间要一个 SplitView 而不是两个固定高的兄弟：终端的高度是**用户偏好**
  // （看输出时想高、看代码时想矮），固定值只能靠改代码。分栏手柄还顺带把
  // 「上半 / 下半」的边界画成一条可抓的线（与侧栏同一种交互语言）。
  //
  // 底部面板**只有终端一个视图**（问题/输出已删，见 `Problem` 处的说明）——
  // 于是这里不再需要标签栏，只留一行“终端标题 + 状态 + 动作”。
  void build_bottom(Composer& c) {
    column(c, {.gap = 0.0f, .grow = true, .id = "bottom-panel"}, [&] {
      // 面板头：身份 + 动作 + 收起。**会话标签在组件内部**（它才有会话状态），
      // 所以这一行只留应用自己的东西。
      (void)row(c, {.gap = 6.0f, .padding_x = 8.0f, .height = 32.0f, .id = "bottom-head"}, [&] {
        (void)icon(c, "terminal", 14.0f);
        (void)text(c, [this] {
          const std::string title =
              terminal_ptr != nullptr
                  ? terminal_ptr->get_property("session_title").value_or("终端")
                  : std::string("终端");
          return "终端 · " + title;
        }, {.id = "bottom-title"});
        (void)spacer(c);
        build_terminal_actions(c);
        (void)custom<Button>(c, [this](Button& b) {
          b.set_id("bottom-close");
          b.set_icon("close");
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this] { bottom_visible_.set(false); };
        }, {.width = 26.0f, .height = 26.0f, .key = "bottom-close"});
      });
      (void)custom<Terminal>(c, [this](Terminal& view) {
        view.set_id("terminal");
        // 默认目录跟着工作区（`switch_workspace` 会同步它）。
        view.set_working_directory(workspace_);
        // **真终端**：起一条真 shell（伪终端 + ANSI 屏幕 + 字节级输入）。
        // 组件自己去重：已有一条就不重复开。
        view.open_shell();
        // 忙闲变化 → 状态栏与中止按钮的可用性。
        view.on_busy_change = [this](bool busy) {
          terminal_busy_ = busy;
          status_.set(busy ? "终端：运行中" : "终端：已退出");
        };
        view.on_error = [this](const std::string& text) { status_.set("终端：" + text); };
        // shell 自己设的标题（`OSC 0/2`）→ 标签跟着变（比固定“终端 1”有用）。
        view.on_title_change = [this](std::size_t index, const std::string& title) {
          if (terminal_ptr != nullptr) terminal_ptr->set_session_title(index, title);
        };
        // 换标签 → 面板标题跟着变。
        view.on_session_change = [this](std::size_t) { status_.set("终端：已切换会话"); };
        // 关掉**最后一个**会话 = 收起面板（布局决策在应用这一层）。
        //
        // ⚠ **不能当场 `set(false)`**：本回调是在 `Terminal::pump()` 的栈上发的，
        // 而重组会把 `Terminal` 拆掉——回到组件时 `this` 已释放，直接段错误
        //（实测 `0xC0000005`）。改成记下一个“待办”，让**入口主循环下一帧**
        //（完全在组件栈之外）再改状态。
        view.on_close_last_session = [this](bool last) {
          if (last) bottom_close_pending_ = true;
        };
        terminal_ptr = &view;
      }, {.grow = true, .key = "terminal"});
    });
  }

  /// 终端动作区（清屏 / 中止 / 重跑 / 新建会话）。
  ///
  /// 为何“中止”必须要有（2026-10-06）：命令跑在**工作线程**上，界面上没有停止入口的话，
  /// 一条 `st test`（几十秒）就把终端锁死了；只能等它跑完或重启应用。
  void build_terminal_actions(Composer& c) {
    const auto action = [&](const char* id, const char* icon, std::function<void()> on_click) {
      (void)custom<Button>(c, [id, icon, on_click = std::move(on_click)](Button& b) {
        b.set_id(id);
        b.set_icon(icon);
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = on_click;
      }, {.width = 26.0f, .height = 26.0f, .key = id});
    };
    // “有没有活在干”问 **busy()**（当前会话）；中止也只中止当前会话。
    if (terminal_ptr != nullptr && terminal_ptr->busy()) {
      action("terminal-stop", "square", [this] { request_terminal_stop(); });
    }
    // “重跑上一条”不再需要：真终端里 `↑` + Enter 就是它（shell 自己的历史）。
    action("terminal-clear", "trash", [this] {
      if (terminal_ptr != nullptr) terminal_ptr->clear();
      status_.set("终端已清屏");
    });
    action("terminal-new", "plus", [this] {
      if (terminal_ptr != nullptr) terminal_ptr->add_session();
    });
  }

  // 两个共用小工具（终端/搜索/面包屑都用）：直接在这里定义——
  // 只写声明的话，后面若有任何分支未走到它们的定义，编译器会报
  // “used but never defined”（`static` 成员函数在文件作用域里就是这个语义）。
  /// 把用户输入的文件名解析为绝对路径（相对终端当前目录 → 工作区）。
  [[nodiscard]] auto resolve_path(std::string_view raw) const -> std::string {
    std::string path(st::trim(raw));
    if (st::fs::is_absolute(path)) return st::fs::normalize(path);
    const std::string base = terminal_exec_dir();
    if (base.empty()) return path;
    return st::fs::normalize(st::fs::join(base, path));
  }

  /// 文本的行数（空串算 0 行）。
  [[nodiscard]] static auto line_count_of(std::string_view text) -> std::size_t {
    if (text.empty()) return 0;
    std::size_t count = 1;
    for (const char c : text) {
      if (c == '\n') ++count;
    }
    return count;
  }

  /// 终端的工作目录（相对路径按它解析；未显式设置过就用工作区根）。
  [[nodiscard]] auto terminal_exec_dir() const -> std::string {
    if (terminal_ptr != nullptr) {
      const std::string cwd = terminal_ptr->get_property("working_directory").value_or("");
      if (!cwd.empty()) return cwd;
    }
    return workspace_;
  }

  /// 上半（资源管理器/编辑区）与下半（问题/输出/终端）的**分栏宿主**。
  ///
  /// 结构：`[活动栏] [上/下分栏]`——活动栏提到这一层，**与底部面板并列**。
  ///
  /// 为什么把活动栏从 `main-row` 里提出来：它是工作台的**全局导航**，不是
  /// “上半区的一部分”。放在 `main-row` 里时，它只到上分区底（y=40..439），
  /// 底部终端面板一展开，活动栏就在终端左边变成一根矮条——与标题栏/状态栏
  /// 这两根**通栏**在视觉上对不齐（用户要求“占满标题栏到状态栏之间的全部高度”）。
  ///
  /// 与 VSCode 同构：`[activity][sidebar|editor]` 上、`[panel]` 下，两者共享
  /// 同一条水平分栏线；活动栏横跨**整个**下半区左侧。
  void build_workbench(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                       bool has_editor) {
    row(c, {.gap = 0.0f, .grow = true, .id = "workbench-row"}, [&] {
      // 活动栏：**常驻**且**满高**（见上）。侧栏依旧不常驻（两者是独立的东西）。
      build_activity_bar(c);
      (void)custom_container<SplitView>(
          c,
          [&] {
            (void)custom_container<Panel>(
                c, [&] { build_main(c, buffers, active, has_editor); },
                [](Panel& panel) { panel.set_id("editor-upper"); }, {.grow = true, .key = "upper"});
            if (!bottom_visible_.value()) return;   // 收起时下半不声明 → 分栏退化为单栏
            (void)custom_container<Panel>(
                c, [&] { build_bottom(c); },
                [](Panel& panel) { panel.set_id("editor-lower"); }, {.grow = true, .key = "lower"});
          },
          [this](SplitView& split) {
            split.set_id("bottom-split");
            split.set_orientation(SplitView::Orientation::Vertical);
            split.set_min_ratio(0.05f);
            split.set_ratio(bottom_ratio_, false);
            // 拖拽回调只写一个**非响应式**成员：比例由组件自己持有并重排，
            // 写 State 会每拖动一像素触发一次整页重组（浪费且在拖拽中重建子元素）。
            split.on_change = [this](float ratio) { bottom_ratio_ = ratio; };
          },
          {.grow = true, .id = "bottom-split"});
    });
  }

  // 终端面板的组装归**框架组件** `st::ui::Terminal`——应用侧那份自绘的
  // 滚回 + 输入行 + 滚动跟随已经删掉（见 `build_bottom`）。

    // —— 5. 状态栏（兼容钩子 id 全保留：`status` / `btn-theme` / 计数 / 光标 / 语言）——
  //
  // 每一项尽量做成**可点**的：状态栏是 IDE 里“看一眼”的地方，而看一眼之后往往
  // 想知道更多（分支 → 源代码管理面板、问题数 → 问题面板、语言 → 语言列表……）。
  void build_status_bar(Composer& c) {
    row(c, {.gap = 10.0f, .padding_x = 10.0f, .height = 26.0f,
            .surface = Element::Surface::Alt, .id = "statusbar"}, [&] {
      status_item(c, "status-branch", "git-branch", [this] {
        return git_branch_.empty() ? std::string("main") : git_branch_;
      }, [this] {
        activity_.set(2);
        sidebar_visible_.set(true);
        refresh_git();
      });
      status_item(c, "status-errors", "error",
                  [this] { return std::to_string(error_count()); }, [this] { show_problems(false); });
      status_item(c, "status-warnings", "warning",
                  [this] { return std::to_string(warning_count()); },
                  [this] { show_problems(true); });
      (void)spacer(c);   // 弹性空隙：右侧信息组贴右缘（同标题栏）
      (void)text(c, [this] { return cursor_text_.value(); }, {.id = "cursor-label"});
      (void)text(c, [this] { return selection_text_.value(); }, {.id = "selection-label"});
      // 缩进 / 编码：点一下就地切换（缩进 2↔4↔8；编码在演示里是只读事实）。
      status_item(c, "status-indent", "", [this] { return std::format("空格: {}", tab_width_); },
                  [this] { cycle_tab_width(); });
      (void)text(c, [] { return std::string("UTF-8"); }, {.id = "status-encoding"});
      // 语言：点一下切到「扩展」视图（那里列出全部可选语言）
      status_item(c, "language-label", "", [this] { return active_language(); }, [this] {
        activity_.set(4);
        sidebar_visible_.set(true);
      });
      (void)text(c, [this] { return status_.value(); }, {.id = "status"});
      (void)button(c, theme_is_dark() ? "亮色" : "暗色", [this] { toggle_theme(); },
                   {.id = "btn-theme"});
    });
  }

  /// 一个可点的状态栏项（图标 + 文本，点击即触发动作）。
  void status_item(Composer& c, const char* id, const char* icon,
                   std::function<std::string()> content, std::function<void()> on_click) {
    (void)custom<Button>(c, [id, icon, content = std::move(content),
                             on_click = std::move(on_click)](Button& b) {
      b.set_id(id);
      b.set_icon(icon);
      b.set_label(content());
      b.set_variant(Button::Variant::Ghost);
      b.set_size(Button::Size::Small);
      b.on_click = on_click;
    }, {.height = 22.0f, .key = id});
  }

  /// 点状态栏的问题计数：**在编辑器里定位第一处问题**。
  ///
  /// 为何不再“切到问题面板”：面板已删（见 `Problem` 的说明），而状态栏那个计数
  /// 用户真正想做的事是“带我去看看”——所以直接把光标送到那一行并高亮它。
  /// 反复点同一类计数就依次跳到下一处（比开一个列表更少一步）。
  void show_problems(bool warnings_only) {
    refresh_problems();
    std::vector<std::size_t> targets;
    for (std::size_t index = 0; index < problems_.size(); ++index) {
      if (problems_[index].warning == warnings_only) targets.push_back(index);
    }
    if (targets.empty()) {
      status_.set(warnings_only ? "没有警告" : "没有错误");
      return;
    }
    problem_cycle_ = (problem_cycle_ + 1) % targets.size();
    jump_to_problem(targets[problem_cycle_]);
    status_.set(std::format("{} {}/{}：第 {} 行 {}", warnings_only ? "警告" : "错误",
                            problem_cycle_ + 1, targets.size(), problems_[targets[problem_cycle_]].line,
                            problems_[targets[problem_cycle_]].message));
  }

  /// 缩进宽度循环 2 → 4 → 8（点状态栏的缩进格）。
  void cycle_tab_width() {
    tab_width_ = tab_width_ == 2 ? 4 : (tab_width_ == 4 ? 8 : 2);
    if (editor != nullptr) editor->set_tab_width(tab_width_);
    status_.set(std::format("缩进宽度已设为 {}", tab_width_));
  }

  // —— 6. 查找替换条（Ctrl+F / Ctrl+H；可见性由 find_open_ 驱动）——
  void build_find_bar(Composer& c) {
    if (!find_open_.value()) return;
    (void)overlay(c, "find", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 8.0f, .id = "find-bar"}, [&] {
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("find-needle");
            field.set_placeholder("查找");
            field.style().width = 170.0f;
            find_needle_input = &field;
            field.on_change = [this](std::string_view value) {
              if (editor == nullptr) return;
              // 选项（大小写/全词）由组件自己管——**不再改写查找词**：
              // 旧写法给全词塞 `\b词\b`，而组件做的是字面量匹配，于是“勾了全词
              // 就一处也搜不到”，却什么错也不报（用户只能看到“搜不到”）。
              editor->set_find(std::string(value), find_options());
              editor->find_next(false);
              update_find_counter();
            };
            field.on_submit = [this](std::string_view) {
              if (editor == nullptr) return;
              editor->find_next(false);
              update_find_counter();
            };
          }, {.key = "find-needle"});
          (void)text(c, [this] { return find_counter_.value(); }, {.id = "find-counter"});
          (void)button(c, "↑", [this] { step_find(true); }, {.id = "find-prev"});
          (void)button(c, "↓", [this] { step_find(false); }, {.id = "find-next"});
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("find-replace");
            field.set_placeholder("替换为");
            field.style().width = 150.0f;
            find_replace_input = &field;   // 替换文本不进状态（不参与重组）
          }, {.key = "find-replace"});
          (void)button(c, "替换", [this] { replace_one(); }, {.id = "find-replace-one"});
          (void)button(c, "全部", [this] { replace_all(); }, {.id = "find-replace-all"});
          // 查找选项三开关：与搜索视图同一套胶囊（大小写 / 全词 / 正则）。
          // 改选项后**重跑查找**：结果集变了，不重跑就会“勾了正则但命中数还是旧的”。
          toggle_chip(c, "find-case", "Aa", find_case_.value(), [this] {
            find_case_.set(!find_case_.value());
            refind();
          });
          toggle_chip(c, "find-word", "ab", find_word_.value(), [this] {
            find_word_.set(!find_word_.value());
            refind();
          });
          // 正则胶囊：**如实置灰**。编辑器不做正则，而“按了没反应”与“不支持”
          // 是两件事——按钮文案直接写出为什么（与组件"动作面如实报能力"同一姿态）。
          toggle_chip(c, "find-regex", ".*", find_regex_.value(), [this] {
            status_.set("本编辑器的查找不做正则（只支持大小写与全词）");
          });
          (void)button(c, "×", [this] { close_find(); }, {.id = "find-close"});
        });
      });
    });
  }

  /// 用当前选项重跑查找（选项变化时调）。
  ///
  /// 大小写与全词都是**组件的一等选项**（`CodeEditor::FindOptions`）：直接交给
  /// 查找器、在“命中那一刻”判——而不是把查找词改写成 `\b词\b` 这种
  /// “看着支持、实际给出错结果”的做法（本编辑器不做正则，`\b` 只会被当成
  /// 两个字面字符，一处也匹配不到，而且什么错也不报）。
  void refind() {
    if (editor == nullptr) return;
    const std::string needle = find_needle_input != nullptr ? find_needle_input->value()
                                                            : std::string{};
    editor->set_find(needle, find_options());
    editor->find_next(false);
    update_find_counter();
  }

  /// 把页面的两个查找开关翻译成组件的查找选项（**唯一转换点**）。
  [[nodiscard]] auto find_options() const -> CodeEditor::FindOptions {
    return CodeEditor::FindOptions{.case_sensitive = find_case_.value(),
                                   .whole_word = find_word_.value()};
  }

  // —— 编辑器右键菜单（`ContextMenu` overlay；锚点 = 最后一次鼠标位置）——
  void build_editor_context(Composer& c) {
    if (!context_open_.value()) return;
    (void)overlay(c, "editor-context", {}, [&] {
      // `ContextMenu` 的条目只能在构造后写入（声明式要求无参构造）：
      // 走 `custom<T>` 逃生舱 + `MenuPanel::set_items`，与 `ContextMenu::make` 同一套接线。
      (void)custom<ContextMenu>(c, [this](ContextMenu& menu) {
        menu.anchor_ = context_anchor_;
        if (menu.panel() != nullptr) {
          menu.panel()->set_items(context_items());
          menu.panel()->on_activate = [this](std::size_t index) {
            context_open_.set(false);
            run_context_action(index);
          };
          menu.panel()->on_close = [this] { context_open_.set(false); };
        }
        menu.on_close = [this] { context_open_.set(false); };
      }, {.id = "editor-context-menu"});
    });
  }

  /// 右键菜单条目（序号 = `kContextItems` 的下标）。
  void run_context_action(std::size_t index) {
    const auto click = [this](const char* action) {
      if (editor != nullptr) (void)editor->invoke_action(action, {});
    };
    switch (index) {
      case 0: click("undo"); status_.set("已撤销"); break;
      case 1: click("redo"); status_.set("已重做"); break;
      case 2: click("cut"); status_.set("已剪切"); break;
      case 3: click("copy"); status_.set("已复制"); break;
      case 4: click("paste"); status_.set("已粘贴"); break;
      case 5: click("select_all"); status_.set("已全选"); break;
      case 6:
        if (editor != nullptr) status_.set(editor->toggle_comment() ? "已切换行注释"
                                                                   : "本语言无行注释标记");
        break;
      case 7: open_find(); break;
      case 8: open_palette(false); break;
      default: break;
    }
  }

  static auto context_items() -> std::vector<MenuItem> {
    return {{.id = "undo", .label = "撤销"},
            {.id = "redo", .label = "重做"},
            {.separator = true},
            {.id = "cut", .label = "剪切"},
            {.id = "copy", .label = "复制"},
            {.id = "paste", .label = "粘贴"},
            {.id = "select_all", .label = "全选"},
            {.separator = true},
            {.id = "comment", .label = "切换行注释"},
            {.separator = true},
            {.id = "find", .label = "查找…"},
            {.id = "palette", .label = "命令面板…"}};
  }

  /// 关闭脏标签确认对话框（`Dialog` overlay；三个按钮与 VSCode 同序）。
  void build_close_confirm(Composer& c) {
    if (!pending_close_) return;
    (void)overlay(c, "close-confirm", {}, [&] {
      (void)custom<Dialog>(c, [this](Dialog& dialog) {
        dialog.set_id("close-confirm-dialog");
        dialog.set_title("未保存的修改");
        dialog.set_body(std::format("“{}”有未保存的修改。要保存吗？", pending_close_label_));
        dialog.set_actions({"取消", "不保存", "保存"});
        dialog.on_action = [this](std::size_t index) {
          if (index == 0) confirm_close_cancel();
          else if (index == 1) confirm_close_discard();
          else confirm_close_save();
        };
        dialog.on_dismiss = [this] { confirm_close_cancel(); };
      }, {.id = "close-confirm"});
    });
  }

  /// 键盘快捷键一览（`Ctrl+K` 或帮助菜单；内容与代码里注册的表同源，不手写第二份）。
  void build_shortcuts_card(Composer& c) {
    if (!shortcuts_open_.value()) return;
    (void)overlay(c, "shortcuts", {}, [&] {
      (void)custom<Dialog>(c, [this](Dialog& dialog) {
        dialog.set_id("shortcuts-dialog");
        dialog.set_title("键盘快捷键");
        dialog.set_body(
            "文件\n"
            "  Ctrl+S          保存            Ctrl+W   关闭当前编辑器\n"
            "  Ctrl+Shift+S    另存为          Ctrl+O   打开文件\n"
            "  Ctrl+N          新建文件\n"
            "\n"
            "编辑\n"
            "  Ctrl+Z/Y        撤销 / 重做     Ctrl+/   切换行注释\n"
            "  Ctrl+A          全选            Ctrl+D   选中下一处同词\n"
            "  Alt+↑ / Alt+↓   上/下移当前行\n"
            "\n"
            "导航\n"
            "  Ctrl+F/Ctrl+H   查找 / 替换     Ctrl+G   转到行…\n"
            "  F8              下一处问题\n"
            "  Ctrl+P          快速打开文件    Ctrl+Shift+P  命令面板\n"
            "  Ctrl+Shift+E/F/G 资源管理器 / 搜索 / 源代码管理\n"
            "  Ctrl+Tab        循环切换标签    Ctrl+B   切换侧栏\n"
            "  Ctrl+J          切换底部面板    Ctrl+K   本帮助\n"
            "  Esc             关闭浮层\n");
        dialog.set_actions({"知道了"});
        dialog.on_action = [this](std::size_t) { shortcuts_open_.set(false); };
        dialog.on_dismiss = [this] { shortcuts_open_.set(false); };
      }, {.id = "shortcuts"});
    });
  }

  // —— 打开 / 新建 / 另存为：真实 `FileDialog`（框架组件，支持程序化选路）——

  void open_file_dialog() {
    pending_open_mode_ = 0;
    new_file_open_.set(true);
  }
  void save_as() {
    pending_open_mode_ = 1;
    new_file_open_.set(true);
  }
  /// 打开**文件夹**（换工作区）：同一条对话框链路，用 `FileDialog` 的目录模式。
  void open_folder_dialog() {
    pending_open_mode_ = 3;
    new_file_open_.set(true);
  }

  /// 文件对话框（打开 / 另存为 / 新建共用；`pending_open_mode_` 区分）。
  ///
  /// 为什么用真 `FileDialog` 而不是自绘输入框：目录浏览/上级/双击进入这些行为
  /// 组件已经写好且**在无头下可程序化选路**（`set_pending_path`），自绘会全部重写一遍。
  void build_open_dialog(Composer& c) {
    if (!new_file_open_.value()) return;
    const bool saving = pending_open_mode_ == 1;
    const bool picking_folder = pending_open_mode_ == 3;
    (void)overlay(c, "file-dialog", {}, [&] {
      (void)custom<FileDialog>(c, [this, saving, picking_folder](FileDialog& dialog) {
        dialog.set_id("file-dialog");
        // 目录模式：没有文件名行、按钮是「选择此文件夹」、确认返回**当前目录**。
        if (picking_folder) dialog.set_mode(FileDialog::Mode::Directory);
        dialog.set_directory(workspace_.empty() ? std::string(".") : workspace_);
        if (saving && editor != nullptr) {
          const auto list = buffers_.value();
          if (active_.value() < list.size()) dialog.set_filename(list[active_.value()].label);
        }
        dialog.on_confirm = [this, saving](const std::string& path) {
          new_file_open_.set(false);
          if (pending_open_mode_ == 3) {
            switch_workspace(path);
            return;
          }
          if (pending_open_mode_ == 2) {
            // 新建：写一个空文件并打开它
            if (st::fs::write_text(path, "")) {
              refresh_tree();
              open_path(path);
            } else {
              status_.set("新建失败：" + path);
            }
            return;
          }
          if (saving) {
            auto list = buffers_.value();
            if (active_.value() < list.size()) {
              list[active_.value()].path = path;
              list[active_.value()].key = path;
              const std::size_t slash = path.find_last_of("/\\");
              list[active_.value()].label =
                  slash == std::string::npos ? path : path.substr(slash + 1);
              list[active_.value()].language =
                  st::text::language_from_path(path).value_or(list[active_.value()].language);
              buffers_.set(std::move(list));
              save();
            }
            return;
          }
          open_path(path);
        };
        dialog.on_cancel = [this] { new_file_open_.set(false); };
      }, {.id = "file-dialog"});
    });
  }

  // —— 命令面板（overlay；命令表 / 文件表两种）——
  void build_palette(Composer& c) {
    (void)overlay(c, "palette", {}, [&] {
      (void)custom<CommandPalette>(c, [this](CommandPalette& palette) {
        palette.set_id("command-palette");
        palette.set_commands(palette_shows_files_.value() ? file_commands() : command_table());
        if (palette.query() != palette_query_.value()) palette.set_query(palette_query_.value());
        palette.set_visible(true);
        palette.on_command = [this](std::string_view id) {
          palette_open_.set(false);
          run_command(std::string(id));
        };
        palette.on_close = [this] { palette_open_.set(false); };
        palette_ptr = &palette;
        // 面板显示后必须把焦点交给过滤框——不调的话敲的字会跑进底层编辑器里（实测踩到）。
        // 这一步要在**挂树之后**调：`Element::add_child` 会把宿主契约一路继承下去，
        // 此时 `grab_focus()` 才能经 `UiRoot::set_focus` 真把键盘焦点转过来。
        palette.grab_focus();
      }, {.id = "command-palette"});
    });
  }

  private:
 // —— 内部工具 ——

 /// 把当前编辑器文本存回列表（切标签/关标签/保存前都要先做）。
  void stash_active_text(std::vector<OpenBuffer>& list) {
    if (editor != nullptr && active_.value() < list.size()) {
      list[active_.value()].text = editor->text();
    }
  }

  /// 切到某个标签（先存回旧文本，再灌新文本）。
  void activate(std::vector<OpenBuffer>& list, std::size_t index) {
    stash_active_text(list);
    buffers_.set(std::move(list));
    active_.set(index);
    load_active_text();
  }

  /// 把当前标签的文本灌进编辑器（切标签/打开文件的唯一入口）。
  void load_active_text() {
    const auto list = buffers_.value();
    if (active_.value() >= list.size()) return;
    loaded_key_ = list[active_.value()].key;   // 标记「已装载」——build() 末尾据此跳过
    if (editor != nullptr) {
      editor->set_language(list[active_.value()].language);
      editor->set_text(list[active_.value()].text);
    }
    refresh_problems();
  }

  void open_by_name(const std::string& name) {
    for (const auto& sample : files_) {
      if (sample.name == name) {
        open_sample(sample);
        return;
      }
    }
    if (name == "stlog.log") open_stlog();
  }

  void switch_tab(std::size_t index) {
    const auto list = buffers_.value();
    if (index >= list.size()) return;
    auto copy = list;
    activate(copy, index);
    status_.set("已切到 " + list[index].label);
  }

  void jump_to_problem(std::size_t index) {
    if (index >= problems_.size() || editor == nullptr) return;
    editor->goto_line(problems_[index].line);
    editor->scroll_to_line(problems_[index].line);
    status_.set(std::format("已跳到第 {} 行", problems_[index].line));
  }

  // —— 标签关闭策略：脏标签先确认（VSCode 的保存/不保存/取消三选）——

  /// 关闭入口（快捷键/×/菜单都走这里）：脏标签弹确认，干净标签直接关。
  void request_close(std::string_view key) {
    const auto list = buffers_.value();
    for (const auto& buffer : list) {
      if (buffer.key != key) continue;
      if (buffer.dirty) {
        pending_close_ = [this, key = std::string(key)] { close(key); };
        pending_close_label_ = buffer.label;
        status_.set("未保存的修改：" + buffer.label + "（确认关闭？）");
        return;
      }
      break;
    }
    close(key);
  }

  /// 确认对话框的第一个按钮：保存并关闭。
  void confirm_close_save() {
    save();
    finalize_pending_close();
  }
  /// 第二个按钮：不保存直接关。
  void confirm_close_discard() {
    finalize_pending_close();
  }
  /// 第三个按钮（也走 Esc / 点遮罩）：取消。
  void confirm_close_cancel() {
    pending_close_ = {};
    pending_close_label_.clear();
    status_.set("已取消关闭");
  }

  void finalize_pending_close() {
    auto action = pending_close_;
    pending_close_ = {};
    pending_close_label_.clear();
    if (action) action();
  }

  /// 关闭全部标签（脏的先确认——这里只处理当前活动那个，其余交回逐次关闭）。
  void close_all() {
    const auto list = buffers_.value();
    for (const auto& buffer : list) {
      if (buffer.dirty) {
        status_.set("有未保存的修改：" + buffer.label + "——请先保存或逐个关闭");
        switch_tab(index_of_key(buffer.key));
        request_close(buffer.key);
        return;
      }
    }
    buffers_.set({});
    active_.set(0);
    loaded_key_.clear();
    refresh_problems();
    status_.set("已关闭全部编辑器");
  }

  /// 关闭当前标签**右侧**的全部（脏的按同样规则拦截）。
  void close_to_right() {
    auto list = buffers_.value();
    const std::size_t from = active_.value();
    for (std::size_t index = from + 1; index < list.size(); ++index) {
      if (list[index].dirty) {
        status_.set("右侧有未保存的修改：" + list[index].label);
        return;
      }
    }
    if (from + 1 >= list.size()) return;
    stash_active_text(list);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(from + 1), list.end());
    buffers_.set(std::move(list));
    status_.set("已关闭右侧标签");
  }

  [[nodiscard]] auto index_of_key(std::string_view key) const -> std::size_t {
    const auto& list = buffers_.value();
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (list[index].key == key) return index;
    }
    return 0;
  }

  void update_find_counter() {
    if (editor == nullptr) return;
    const std::size_t total = editor->find_match_count();
    const std::size_t current = editor->find_active_index();
    find_counter_.set(total == 0 ? std::string("0/0")
                                 : std::format("{}/{}",
                                               current == CodeEditor::kNoFindMatch ? 0
                                                                                   : current + 1,
                                               total));
  }

  void step_find(bool backward) {
    if (editor == nullptr) return;
    editor->find_next(backward);
    update_find_counter();
  }

  [[nodiscard]] auto replacement_text() const -> std::string {
    return find_replace_input != nullptr ? find_replace_input->value() : std::string{};
  }

  void replace_one() {
    if (editor == nullptr) return;
    editor->replace_current(replacement_text());
    update_find_counter();
  }

  void replace_all() {
    if (editor == nullptr) return;
    status_.set(std::format("已替换 {} 处", editor->replace_all(replacement_text())));
    update_find_counter();
  }

  void close_find() {
    if (editor != nullptr) editor->clear_find();
    find_open_.set(false);
  }

  public:
 /// 启动信息进**状态栏**（入口在 `app.start()` 之后才能拿到后端/端口信息）。
 ///
 /// 为什么不写进终端：真终端里那条流**归 shell**——应用不是写者。
 /// 往别人的流里插一行会打乱屏幕（shell 下次重绘就把它抹掉，
 /// 或更糟：把提示符位置弄乱）。应用自己的通知属于状态栏。
 auto note_startup(const std::string& text) -> void { status_.set(text); }
   /// 每帧推动终端作业（入口主循环调；见 `update_terminal` 的说明）。
  auto pump() -> void {
    // ⚠ **顺序**：先让终端泵（它可能受理“关闭会话”），**再**处理“收起面板”。
    // 不能在组件回调里当场收起面板——那会在组件栈上重组，把组件拆掉后
    // 回到它内部就是 use-after-free（实测 `0xC0000005`）。这里在**主循环**的
    // 栈上改状态，完全在组件之外。
    pump_terminal();
    if (bottom_close_pending_) {
      bottom_close_pending_ = false;
      bottom_visible_.set(false);
    }
  }
  /// 退出前收尾：停掉在跑的作业并 join 读线程（`jthread` 把它露在对象生命周期
  /// 之外会很危险：它可能正阻塞在 `read_line` 上，而对象已经在析构）。
  auto shutdown() -> void { shutdown_terminal(); }
  /// 关闭查找条（入口的 Esc 快捷键用；与面板上的「×」同一条路径）。
 auto dismiss_find() -> void { close_find(); }
 /// 新建文件（入口快捷键用）。
 auto new_file_request() -> void { new_file_prompt(); }
 /// 刷新 Git 状态（入口快捷键用）。
 auto refresh_git_request() -> void { refresh_git(); }
 /// 跳到第 index 处问题（入口快捷键用）。
 auto jump_to_problem_request(std::size_t index) -> void { jump_to_problem(index); }

 private:

  /// 工作区树：根 + 已展开目录的子项（展开状态由 `dir_expanded_` 表达）。
  [[nodiscard]] auto workspace_nodes() const -> std::vector<TreeNodeData> {
    std::vector<TreeNodeData> nodes = scan_tree_nodes(workspace_);
    const auto expanded = dir_expanded_.value();
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      if (nodes[index].key.rfind("dir:", 0) != 0) continue;
      const std::string name = nodes[index].key.substr(4);
      if (std::find(expanded.begin(), expanded.end(), name) == expanded.end()) continue;
      nodes[index].expanded = true;
      for (auto& child : scan_tree_nodes(workspace_ + "/" + name)) {
        child.depth += 1;
        if (child.key.rfind("dir:", 0) == 0) child.key = "dir:" + name + "/" + child.key.substr(4);
        nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(++index), std::move(child));
      }
    }
    return nodes;
  }

  void on_tree_toggle(const std::string& key, bool expanded) {
    if (workspace_.empty() || key.rfind("dir:", 0) != 0) return;
    const std::string name = key.substr(4);
    auto list = dir_expanded_.value();
    if (expanded) {
      if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
    } else {
      std::erase(list, name);
    }
    dir_expanded_.set(std::move(list));
  }

  /// 递归搜工作区（无工作区时搜内置样例）；结果写进 `search_hits_`。
  ///
  /// 改自原先的“只找一个词”：新增 glob 过滤与大小写/全词/正则三个开关，
  /// 并把命中**按文件分组**（同一个文件的命中排在一起，前缀行给文件名）——
  /// 平铺的结果在几十条之后就完全不可用了（看不出哪几行属于同一个文件）。
  void run_search(const std::string& query) {
    apply_search(query);
  }

  /// 真正执行搜索（UI 里的入口与终端 `find` 共用）。
  void apply_search(const std::string& query) {
    last_query_ = query;
    search_hit_paths_.clear();
    search_hit_lines_.clear();
    search_hit_columns_.clear();
    std::vector<std::string> hits;
    constexpr std::size_t kMaxHits = 300;
    constexpr std::size_t kMaxFileBytes = 1U << 20U;
    if (!search_hits_.value().empty()) {
      search_summary_.set(std::format("{} 处命中", search_hits_.value().size()));
    }
    if (query.empty()) {
      search_summary_.set("");
      search_hits_.set({});
      return;
    }
    // 匹配器：大小写敏感 / 全词 / 正则——先定好参数，逐行复用。
    //
    // **不用 `std::regex`**（本仓不引 regex：构造昂贵、异常语义差、且跨平台行为
    // 有差异）。这里实现一个小而**诚实的**子集匹配器：
    // - `.*`：任意字符序列（可跨列）；
    // - 普通字符：字面量；
    // - 支持 `^` / `$` 锚定与 `\d` / `\w` / `\s` 三个字符类。
    // 不支持集合/分组/量词——这类表达式在搜索里几乎用不到，而“假装支持”会静默给出错结果。
    const std::string needle = search_case_.value() ? query : to_lower(query);
    const auto locate = [&](std::string_view raw) -> std::optional<std::size_t> {
      const std::string haystack = search_case_.value() ? std::string(raw) : to_lower(raw);
      if (search_regex_.value()) return match_regex(needle, haystack);
      std::size_t pos = haystack.find(needle);
      while (pos != std::string::npos) {
        if (!search_word_.value()) return pos;
        // 全词：两侧不得为标识符字符（字母/数字/下划线），与多数编辑器的口径一致。
        const bool left_ok = pos == 0 || !is_word_char(haystack[pos - 1]);
        const std::size_t end = pos + needle.size();
        const bool right_ok = end >= haystack.size() || !is_word_char(haystack[end]);
        if (left_ok && right_ok) return pos;
        pos = haystack.find(needle, pos + 1);
      }
      return std::nullopt;
    };

    const auto add_hit = [&](const std::string& path, const std::string& name, std::size_t line,
                             std::size_t column, std::string_view line_text) {
      hits.push_back(std::format("{}:{}  {}", name, line, st::trim(line_text).substr(0, 70)));
      search_hit_paths_.push_back(path);
      search_hit_lines_.push_back(line);
      search_hit_columns_.push_back(column);
    };

    const auto scan_text = [&](const std::string& path, const std::string& name,
                               const std::string& content) {
      std::size_t line_no = 0;
      std::size_t begin = 0;
      while (begin <= content.size() && hits.size() < kMaxHits) {
        const std::size_t eol = content.find('\n', begin);
        const std::size_t end = eol == std::string::npos ? content.size() : eol;
        std::string_view line(content.data() + begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ++line_no;
        if (const auto column = locate(line); column.has_value()) {
          add_hit(path, name, line_no, *column, line);
        }
        if (eol == std::string::npos) break;
        begin = eol + 1;
      }
    };

    if (!workspace_.empty()) {
      std::vector<std::pair<std::string, int>> stack;
      stack.reserve(64);
      stack.emplace_back(workspace_, 0);
      while (!stack.empty() && hits.size() < kMaxHits) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        if (depth > 6) continue;
        auto listing = st::fs::list_dir(dir);
        if (!listing) continue;
        for (const auto& item : *listing) {
          if (hits.size() >= kMaxHits) break;
          const std::string full = st::fs::join(dir, item.name);
          if (item.is_dir) {
            if (!item.name.empty() && item.name.front() == '.') continue;   // 隐藏目录
            if (item.name == "build" || item.name == "node_modules") continue;  // 产物目录
            stack.emplace_back(full, depth + 1);
            continue;
          }
          if (item.size > kMaxFileBytes) continue;
          // glob 过滤：按**相对工作区**的路径匹配（写 `*.cpp` 时人想的是相对路径）。
          if (!search_glob_.empty() &&
              !st::fs::match_glob(search_glob_, st::fs::relative_to(full, workspace_))) {
            continue;
          }
          auto content = st::fs::read_text(full);
          if (!content) continue;   // 非 UTF-8 / 读不了：跳过而不是硬猜
          scan_text(full, item.name, *content);
        }
      }
    } else {
      // 内置样例回退（无 --workspace 时保持自包含）
      for (const auto& sample : files_) scan_text(sample.name, sample.name, sample.code);
      scan_text("stlog.log", "stlog.log", std::string(kStlogSample));
    }

    if (hits.empty()) {
      search_summary_.set("0 处命中");
      status_.set("搜索命中 0 处");
      search_hits_.set({});
      return;
    }
    const bool truncated = hits.size() >= kMaxHits;
    search_summary_.set(std::format("{} 处命中{}", hits.size(), truncated ? "（已截断）" : ""));
    status_.set(std::format("搜索命中 {} 处", hits.size()));
    search_hits_.set(std::move(hits));
  }

  [[nodiscard]] static auto to_lower(std::string_view text) -> std::string {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  }
  [[nodiscard]] static auto is_word_char(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  }

  /// 小型正则匹配器（支持子集见 `apply_search` 的说明）；返回首个匹配的**起始列**。
  ///
  /// 匹配策略：逐起点尝试 + 回溯（模式长度有限、行长度在几千字节量级），
  /// 搜索场景完全够用——不会像完整引擎那样再引一个依赖。
  [[nodiscard]] static auto match_regex(std::string_view pattern, std::string_view text)
      -> std::optional<std::size_t> {
    bool anchored_start = !pattern.empty() && pattern.front() == '^';
    const bool anchored_end = !pattern.empty() && pattern.back() == '$';
    if (anchored_start) pattern.remove_prefix(1);
    if (anchored_end && !pattern.empty()) pattern.remove_suffix(1);
    const std::size_t last_start = anchored_start ? 0 : text.size();
    for (std::size_t start = 0; start <= last_start; ++start) {
      std::size_t cursor = start;
      std::size_t pattern_at = 0;
      bool ok = true;
      while (pattern_at < pattern.size()) {
        const char token = pattern[pattern_at];
        if (token == '\\' && pattern_at + 1 < pattern.size()) {
          const char klass = pattern[pattern_at + 1];
          if (cursor >= text.size()) {
            ok = false;
            break;
          }
          const char c = text[cursor];
          const bool hit = (klass == 'd' && c >= '0' && c <= '9') ||
                           (klass == 'w' && is_word_char(c)) ||
                           (klass == 's' && (c == ' ' || c == '\t'));
          if (!hit) {
            ok = false;
            break;
          }
          ++cursor;
          pattern_at += 2;
          continue;
        }
        // `.*`：贪心跳到**能让剩余模式匹配上**的第一个位置（无剩余的剩余==0）。
        if (token == '.' && pattern_at + 1 < pattern.size() && pattern[pattern_at + 1] == '*') {
          const std::string_view rest = pattern.substr(pattern_at + 2);
          std::size_t probe = cursor;
          bool matched = false;
          for (;;) {
            if (match_regex(rest, text.substr(probe)).has_value()) {
              matched = true;
              break;
            }
            if (probe >= text.size()) break;
            ++probe;
          }
          if (!matched) {
            ok = false;
            break;
          }
          pattern_at = pattern.size();
          cursor = text.size();
          break;
        }
        if (cursor >= text.size() || text[cursor] != token) {
          ok = false;
          break;
        }
        ++cursor;
        ++pattern_at;
      }
      if (ok && pattern_at >= pattern.size() && (!anchored_end || cursor == text.size())) {
        return start;
      }
    }
    return std::nullopt;
  }

  // —— Git：真调 `git` 子进程（只读子命令）——
  //
  // 为什么值得接：SCM 面板若只是写死一个 “main” 分支名与 “0 变更”，它就不是信息
  // 而是装饰。真读 `git status --porcelain=v1 -b` 之后，改一个文件、刷新，面板会跟着变。
  void refresh_git() {
    git_probed_ = true;
    git_error_.clear();
    git_changes_.clear();
    git_branch_.clear();
    if (workspace_.empty()) {
      git_error_ = "没有工作区";
      return;
    }
    st::process::Options options;
    options.cwd = workspace_;
    const auto result = st::process::run("git", {"status", "--porcelain=v1", "-b"}, options);
    if (!result) {
      git_error_ = "git 不可用：" + result.error().to_string();
      return;
    }
    if (result->exit_code != 0) {
      git_error_ = st::trim(result->stderr_text.empty() ? result->stdout_text
                                                        : result->stderr_text);
      if (git_error_.empty()) git_error_ = "不是 Git 仓库（或 git 拒绝执行）";
      return;
    }
    std::size_t begin = 0;
    const std::string& out = result->stdout_text;
    while (begin <= out.size()) {
      const std::size_t eol = out.find('\n', begin);
      const std::size_t end = eol == std::string::npos ? out.size() : eol;
      std::string_view line(out.data() + begin, end - begin);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.starts_with("## ")) {
        // `## main...origin/main [ahead 1]` → 取第一段
        line.remove_prefix(3);
        const std::size_t stop = line.find_first_of(" .");
        git_branch_ = std::string(stop == std::string_view::npos ? line : line.substr(0, stop));
      } else if (line.size() > 3) {
        git_changes_.push_back(GitChange{.status = std::string(st::trim(line.substr(0, 2))),
                                         .path = std::string(st::trim(line.substr(3)))});
      }
      if (eol == std::string::npos) break;
      begin = eol + 1;
    }
    if (git_branch_.empty()) git_branch_ = "（游离 HEAD）";
    status_.set(std::format("Git：{} · {} 项变更", git_branch_, git_changes_.size()));
  }

  /// 展开一个变更文件：与 HEAD 的差异**跑进终端**（只读 `git diff`）。
  ///
  /// 为何不再开一个“输出面板”：差异是**命令的输出**，而命令的输出去处只有一个。
  /// 有两处就会产生“同一个 `git diff` 一会儿在这里、一会在那里”的困惑。
  void show_git_diff(const GitChange& change) {
    bottom_visible_.set(true);
    // 真终端里应用只能**代敲**（自己不是那条流的写者）：把命令当输入送给 shell。
    if (terminal_ptr != nullptr) {
      terminal_ptr->run(std::format("git diff --no-color -- {}", change.path));
    }
    status_.set("差异已打开：" + change.path);
  }

  // —— 运行任务：真跑 `st`（走终端的作业通道，输出实时回流）——
  //
  // 为何搬到终端后面：以前这里是同步 `st::process::run` + 写 `output_`，
  // 于是跑 `st test` 的几十秒里界面假死、看不到任何进展。现在它只是
  // “把命令交给终端”——输出逐行出现，中止按钮真的能杀进程。
  void run_task(const std::string& id) {
    if (id == "build") {
      // 真终端里应用只能**代敲**（自己不是那条流的写者）：把命令当输入送给 shell。
      if (terminal_ptr != nullptr) terminal_ptr->run("st build gallery --profile dev");
    } else if (id == "test") {
      if (terminal_ptr != nullptr) terminal_ptr->run("st test");
    } else {
      if (terminal_ptr != nullptr) terminal_ptr->run("st lint");
    }
  }

  /// 枚举运行时已注册的语言（扩展视图）。
  void refresh_languages() {
    languages_ = CodeEditor::available_languages();
    status_.set(std::format("已注册 {} 种语言", languages_.size()));
  }

  /// 重新扫描工作区树（资源管理器的刷新按钮）。
  void refresh_tree() {
    dir_expanded_.set(dir_expanded_.value());   // 触发一次重组，树数据本就每帧重扫
    status_.set(workspace_.empty() ? "内置样例模式" : "已刷新工作区树");
  }

  /// **换工作区**（「打开文件夹…」的落地动作）。
  ///
  /// 语义（与 VSCode 一致的三条）：
  /// * **已打开的文件标签保留**——换工作区是换“资源树的根”，不是关项目；
  ///   用户常要“在新目录里继续看刚编辑的那个文件”，标签全关掉是丢工作。
  /// * 资源树换成新目录并**重置展开状态**（旧展开路径对新根无意义）。
  /// * 标题栏/状态栏跟着换，让用户确认换成功了。
  ///
  /// 校验：目标必须是**存在的目录**（写到状态栏而不是弹窗——与 `close_all` 的姿态一致，
  /// 不打断手头的事）。
  void switch_workspace(const std::string& path) {
    if (path.empty()) return;
    if (!st::fs::exists(path) || !st::fs::is_directory(path)) {
      status_.set("不是目录：" + path);
      return;
    }
    workspace_ = path;
    dir_expanded_.set(std::vector<std::string>{});   // 新根：展开状态重来
    refresh_tree();
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    status_.set("工作区：" + (name.empty() ? path : name));
    push_title();   // 标题栏跟着换（窗口标题是“当前在哪”的第一指示）
  }


  /// 新建文件：有工作区时开文件对话框选路径；无工作区时开一份内存缓冲（`untitled-N`）。
  ///
  /// 为何两档都要：内置样例模式的演示价值就在于“不用开文件对话框也能跑起来”，
  /// 而如果无工作区时只回一句“需要 --workspace”，那“新建文件”这个入口在演示里就是死的。
  void new_file_prompt() {
    if (workspace_.empty()) {
      auto list = buffers_.value();
      stash_active_text(list);
      const std::size_t index = untitled_count_++;
      OpenBuffer buffer;
      buffer.key = std::format("untitled-{}", index);
      buffer.label = std::format("untitled-{}.txt", index);
      buffer.language = "text";
      buffer.text = "";
      list.push_back(std::move(buffer));
      buffers_.set(std::move(list));
      active_.set(buffers_.value().size() - 1);
      load_active_text();
      status_.set("已新建内存缓冲（保存时可用另存为选路径）");
      return;
    }
    pending_open_mode_ = 2;
    new_file_open_.set(true);
  }

  // —— 搜索命中的跳转（打开文件 + 选中命中）——

  /// 点搜索命中 → 打开文件并跳到那一行 + 选中命中词。
  void activate_search_hit(std::size_t index) {
    if (index >= search_hit_paths_.size() || search_hit_paths_[index].empty()) return;
    const std::size_t line = index < search_hit_lines_.size() ? search_hit_lines_[index] : 1;
    const std::size_t column = index < search_hit_columns_.size() ? search_hit_columns_[index] : 0;
    const std::string path = search_hit_paths_[index];
    // 内置样例模式：path 是样例名，不是真文件。
    if (!st::fs::is_absolute(path) && workspace_.empty()) {
      open_by_name(path);
    } else {
      open_path(path);
    }
    pending_jump_ = {line, column};
    if (editor != nullptr) {
      apply_jump(line, column);
      pending_jump_ = {0, 0};
    }
  }

  void apply_jump(std::size_t line, std::size_t column) {
    if (editor == nullptr) return;
    editor->goto_line(line);
    const std::size_t start = editor->cursor_index() + column;
    editor->set_selection(start, start + last_query_.size());
    editor->scroll_to_line(line);
    status_.set(std::format("已跳到第 {} 行", line));
  }

  // ════════════════════════════════════════════════════════════════════════
  // 终端：内置命令 + 白名单外部命令（长命令在工作线程上跑，输出逐块回流）
  // ════════════════════════════════════════════════════════════════════════

  /// 每帧泵一次终端（应用主循环调；见 `pump()`）。
  void pump_terminal() {
    if (terminal_ptr != nullptr) terminal_ptr->pump();
  }

  /// 退出前收尾：中止还在跑的会话（否则子进程会变成孤儿）。
  void shutdown_terminal() {
    if (terminal_ptr != nullptr && terminal_ptr->any_busy()) terminal_ptr->send_stop();
  }

  /// 请求中止当前会话（面板按钮）。
  void request_terminal_stop() {
    if (terminal_ptr != nullptr) terminal_ptr->send_stop();
  }

  // **应用不再解释命令**：真终端里输入的一切都送给 shell，由它执行
  //（`cd`、`ls`、`git` 都是 shell 自己的命令）。原先那套"内置命令 + 白名单"
  // 是假终端时代的产物——那时没有 shell 可托付，只能自己解析一遍。
  //
  // 于是随之移除的还有：命令替换（`$last`/`$root`/`$cwd`）、只读 git 白名单、
  // 语法高亮式的 `cat`/`wc` 内建、以及"未知命令"提示——它们全都建立在
  // "应用是命令的解释者"这个前提上，而那个前提已经不成立了。

  /// 命令面板执行一条命令（id 是稳定的业务身份）。
  void run_command(const std::string& id) {
    // ⚠ 顺序敏感：`"file.open"` 必须排在 `"file.open."` 前缀判断**之前**。
    // 两个 id 共享前缀，`rfind("file.open.", 0)` 对 `"file.open"` 为假（少了那个点），
    // 但把精确匹配放前面才不会在将来加 `file.open-xxx` 时踩到同类前缀问题。
    if (id == "file.open") {
      open_file_dialog();
    } else if (id == "file.open-folder") {
      open_folder_dialog();
    } else if (id.rfind("file.open.", 0) == 0) {
      open_by_name(id.substr(10));
    } else if (id.rfind("lang.", 0) == 0) {
      open_stlog();
    } else if (id == "file.save") {
      save();
    } else if (id == "file.close-tab") {
      close_active();
    } else if (id == "view.toggle-sidebar") {
      sidebar_visible_.set(!sidebar_visible_.value());
    } else if (id == "view.toggle-theme") {
      toggle_theme();
    } else if (id == "view.toggle-indent-guides") {
      if (editor != nullptr) {
        editor->set_indent_guides(!editor->indent_guides());
        status_.set(editor->indent_guides() ? "缩进参考线：开" : "缩进参考线：关");
      }
    } else if (id == "help.about") {
      status_.set("歌白代码 0.1.0 · 霜天框架示例 · 声明式组装");
    } else if (!id.empty()) {
      const std::size_t slash = id.find_last_of("/\\");
      const std::string leaf(slash == std::string::npos ? id : id.substr(slash + 1));
      status_.set(id.find('/') != std::string::npos ? "已打开 " + leaf : "已执行：" + leaf);
    }
  }

  /// 全部命令表（Ctrl+Shift+P）。
  [[nodiscard]] auto command_table() -> std::vector<CommandPalette::Command> {
    std::vector<CommandPalette::Command> table;
    const auto add = [&table](std::string id, std::string title, std::string detail) {
      table.push_back(CommandPalette::Command{.id = std::move(id), .title = std::move(title),
                                              .detail = std::move(detail), .handler = {}});
    };
    add("file.save", "文件: 保存", "把当前编辑器标记为已保存");
    add("file.close-tab", "文件: 关闭当前编辑器", "Ctrl+W");
    // 打开的两个入口（文件 / 文件夹）都进命令表——菜单里能点，命令面板里能搜，
    // 否则“打开文件夹”只有记住快捷键的人找得到。
    add("file.open", "文件: 打开文件…", "Ctrl+O");
    add("file.open-folder", "文件: 打开文件夹…", "换工作区（Ctrl+Shift+O）");
    add("view.toggle-sidebar", "查看: 切换侧栏可见性", "显示/隐藏侧栏（活动栏常驻，Ctrl+B）");
    add("view.toggle-theme", "查看: 切换亮/暗主题", "主题令牌整体切换");
    add("view.toggle-indent-guides", "查看: 切换缩进参考线", "每级缩进一条竖线");
    add("help.about", "帮助: 关于", "歌白代码 · 霜天示例");
    for (const auto& sample : files_) {
      add("file.open." + sample.name, "文件: 打开 " + sample.name, "语言 " + sample.language);
    }
    add("lang.stlog", "语言: 打开 stlog 样例", "自定义语言（运行时注册）");
    return table;
  }

  /// 快速打开的文件表（Ctrl+P）：有工作区时列真实文件（递归、限量）。
  [[nodiscard]] auto file_commands() const -> std::vector<CommandPalette::Command> {
    std::vector<CommandPalette::Command> table;
    if (!workspace_.empty()) {
      std::vector<std::pair<std::string, int>> stack{{workspace_, 0}};
      while (!stack.empty() && table.size() < 300) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        if (depth > 5) continue;
        auto listing = st::fs::list_dir(dir);
        if (!listing) continue;
        for (const auto& item : *listing) {
          const std::string full = dir + "/" + item.name;
          if (item.is_dir) {
            if (!item.name.empty() && item.name.front() == '.') continue;
            stack.emplace_back(full, depth + 1);
            continue;
          }
          table.push_back(CommandPalette::Command{
              .id = full, .title = item.name,
              .detail = full.substr(workspace_.size() + 1), .handler = {}});
        }
      }
      return table;
    }
    for (const auto& sample : files_) {
      table.push_back(CommandPalette::Command{.id = sample.name, .title = sample.name,
                                              .detail = "语言 " + sample.language, .handler = {}});
    }
    return table;
  }

 public:
  // —— 供入口与主循环使用 ——
  /// 编辑器字号档位（构造时定，`--editor-font-scale` 可覆盖）。
  /// 声明在 `files_` 之前：初始化顺序按**声明序**，否则 -Werror=reorder 直接编译失败。
  float editor_font_scale_{kEditorFontScale};
  /// 编辑器行距倍数（构造时定，`--editor-line-spacing` 可覆盖）。
  float editor_line_spacing_{CodeEditor::kDefaultLineSpacing};
  /// 霜天仓库根（`--tool-root`）：PATH 上没有 `st` 时到它下面 `build/bin/st[.exe]` 找。
  std::string tool_root_{};
  std::vector<Sample> files_{};
  std::string workspace_{};
  std::vector<Problem> problems_{};
  std::pair<std::size_t, std::size_t> pending_jump_{0, 0};
  /// 新建/打开/另存对话框开关（三者共用一个 `FileDialog`）。
  State<bool> new_file_open_{false};
  /// 缩进宽度（状态栏可点切换；配到编辑器上一次）。
  int tab_width_{4};
  /// 0 = 打开，1 = 另存为，2 = 新建。
  /// 3 = **打开文件夹**（换工作区）：同一条对话框链路，只是用 `Mode::Directory`。
  int pending_open_mode_{0};
  /// 内存缓冲的递增序号（`untitled-N`）。
  std::size_t untitled_count_{1};

 private:
  std::string loaded_key_{};   ///< 编辑器实例当前装载的是哪个标签（防重复 set_text）
  /// 底部面板当前高度比例（上下分栏的 `ratio`）。
  ///
  /// **刻意不是 `State`**：分栏组件自己持有比例并据此重排，拖动时它逐像素回调本值——
  /// 若这是个 State，每拖一像素就会标脏整页重组一次（拖拽中重建子元素，手感与开销都差）。
  /// 这里只做“跨重组记住用户拖到哪了”的存档。
  ///
  /// ⚠ 这是 **`SplitView::ratio` 的口径 = 第一面板（编辑器）的占比**，不是终端的份额。
  /// 实测踩到过反直觉的错误：把“想让终端占多”写成把比值改大（0.72 → 0.60），
  /// 结果编辑器反而吃掉了六成高度。
  ///
  /// 把 0.72 → **0.55**：原先终端只拿到 28%（约 6 行）——真终端 6 行不可用
  ///（`dir`/`git log` 一屏就满，连提示符都要滚）。现在终端约 295px ≈ 14 行。
  float bottom_ratio_{0.55f};
};

// ════════════════════════════════════════════════════════════════════════════
// 三、入口：命令行、快捷键、主循环
// ════════════════════════════════════════════════════════════════════════════

struct Options {
  bool headless{false};
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
  /// 工作区目录（真实文件模式）：资源管理器/打开/保存全部走 `st::fs` 真实读写；
  /// 缺省回退内置样例工作区（内存模拟）。
  std::string workspace{};
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
      "  --workspace DIR       工作区目录（真实文件；缺省用内置样例）\n"
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
    if (raw == "--help" || raw == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (raw == "--headless") options.headless = true;
    else if (raw == "--enable-script") options.enable_script = true;
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(value("1")));
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
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(value("0")));
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--shots") options.shots = value({});
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(value("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(value("0"));
    else if (raw == "--workspace") options.workspace = value(".");
    else if (raw == "--tool-root") options.tool_root = value("");
    else if (raw == "--decorations") options.decorations = true;
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
  app_options.backend = options.headless ? "headless" : std::string{};
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

  st::app::Application app("gbcode", "0.1.0", app_options);

  // 声明式页面（整个 IDE 是一个 Component：内容槽里的一切由 `build()` 描述）。
  float editor_font_scale = kEditorFontScale;
  if (options.editor_font_scale != "auto") {
    editor_font_scale = static_cast<float>(std::stod(options.editor_font_scale));
  }
  float editor_line_spacing = CodeEditor::kDefaultLineSpacing;
  if (options.editor_line_spacing != "auto") {
    editor_line_spacing = static_cast<float>(std::stod(options.editor_line_spacing));
  }
  auto page = std::make_shared<CodeEditorPage>(samples(), options.workspace, options.tool_root,
                                               editor_font_scale, editor_line_spacing);
  // 窗框的窗口动作出口：`Application` 实现了 `ui::WindowControl`（转发给后端）。
  // 在这一处"装"进去，页面内的组件就不需要知道应用/后端的存在（依赖方向单向）。
  page->window_control_ = &app;
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
    frame->title_bar()->set_icon("code");
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
  // 初始文件（--language 指定；都没有则第一个）
  const Sample* initial = page->files_.empty() ? nullptr : &page->files_.front();
  for (const auto& sample : page->files_) {
    if (sample.language == options.language) {
      initial = &sample;
      break;
    }
  }
  if (initial != nullptr) page->open_sample(*initial);
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
  // **除 Esc 外的一律先让路给浮层**：浮层是当前交互的“前台”（对话框、查找条、
  // 命令面板、转到行、菜单面板），用户在浮层里的按键不应被编辑区快捷键抢走。
  // 具体的坑：浮层带输入框时，Ctrl+S/Ctrl+D 这类“编辑器动作”会在输入框收到
  // 字符之前就被根级处理器吃掉——用户以为自己只是在浮层里打字。
  {
    UiRoot* root = &app.root();
    const auto overlay_open = [root]() -> bool { return root->overlay_count() > 0; };
    const auto bind = [root, overlay_open](const char* key, bool shift,
                                          std::function<void()> action) {
      UiRoot::Shortcut mods{};
      mods.key = key;
      mods.ctrl = true;
      mods.shift = shift;
      (void)root->register_shortcut(key, mods, [action = std::move(action), overlay_open]() {
        // 浮层开着 = 这次按键属于浮层：**不消费**，让它继续下沉到浮层里的输入框。
        if (overlay_open()) return false;
        action();
        return true;   // 全局命令：总是消费，不再下沉
      });
    };
    bind("s", false, [page] { page->save(); });
    bind("s", true, [page] { page->save_as(); });
    bind("w", false, [page] { page->close_active(); });
    bind("o", false, [page] { page->open_file_dialog(); });
  // Ctrl+Shift+O：打开文件夹（换工作区）。与 VSCode 的 Ctrl+K Ctrl+O 同义，
  // 用单键是因为本框架没有和弦键位机制——单键的可发现性反而更好。
  bind("o", true, [page] { page->open_folder_dialog(); });
    bind("n", false, [page] { page->new_file_request(); });
    bind("b", false, [page] { page->sidebar_visible_.set(!page->sidebar_visible_.value()); });
    bind("j", false, [page] { page->toggle_terminal_panel(); });   // Ctrl+J：切换终端面板
    // Ctrl+`（VSCode 的终端默认键位）：与活动栏那个按钮同一条路径。
    // 注：`` ` `` 在不同键盘布局上键名不一（`\`` / `Backquote`），两种都收。
    bind("`", false, [page] { page->toggle_terminal_panel(); });
    bind("Backquote", false, [page] { page->toggle_terminal_panel(); });
    bind("k", false, [page] { page->shortcuts_open_.set(!page->shortcuts_open_.value()); });
    // Ctrl+G：真的“转到行…”浮层（与选择菜单那一条同一条链路）。
    bind("g", false, [page] { page->open_goto_line(); });
    // Ctrl+D：选中下一处同词；Alt+↑/↓：上/下移当前行。
    //
    // 为何要**重复**注册（组件 `handle_key` 里已经处理过一遍）：这两组键在
    // 编辑器未聚焦时（刚点过侧栏/终端）也应当生效，而全局表先于焦点链派发——
    // 焦点在编辑器时先被根吃下，不会出现“一次按键搬两行”。
    bind("d", false, [page] {
      if (page->editor == nullptr) return;
      // 状态栏如实报“选了哪个词 / 找不到下一处”——两者都不是错误，但必须能区分。
      if (page->editor->select_next_occurrence()) {
        page->status_.set("已选中「" + page->editor->selected_text() + "」");
      } else {
        page->status_.set("没有下一处同词");
      }
    });
    {
      UiRoot::Shortcut alt_up{};
      alt_up.key = "ArrowUp";
      alt_up.alt = true;
      (void)root->register_shortcut("alt-up", alt_up, [page, overlay_open]() {
        if (overlay_open()) return false;
        if (page->editor != nullptr && page->editor->move_lines(-1)) page->status_.set("已上移当前行");
        return true;
      });
      UiRoot::Shortcut alt_down{};
      alt_down.key = "ArrowDown";
      alt_down.alt = true;
      (void)root->register_shortcut("alt-down", alt_down, [page, overlay_open]() {
        if (overlay_open()) return false;
        if (page->editor != nullptr && page->editor->move_lines(1)) page->status_.set("已下移当前行");
        return true;
      });
    }
    // Ctrl+Shift+E / F / G：切到三个主视图（与歌白文件工作台同一套键位）
    bind("e", true, [page] {
      page->activity_.set(0);
      page->sidebar_visible_.set(true);
    });
    bind("f", true, [page] {
      page->activity_.set(1);
      page->sidebar_visible_.set(true);
    });
    bind("g", true, [page] {
      page->activity_.set(2);
      page->sidebar_visible_.set(true);
      page->refresh_git_request();
    });
    bind("p", true, [page] { page->open_palette(false); });          // Ctrl+Shift+P：命令表
    bind("p", false, [page] { page->open_palette(true); });          // Ctrl+P：快速打开文件
    bind("f", false, [page] { page->open_find(); });                 // Ctrl+F：查找
    bind("h", false, [page] { page->open_find(); });                 // Ctrl+H：查找替换
    bind("tab", false, [page] {                                      // Ctrl+Tab：循环切标签
      const auto count = page->buffers_.value().size();
      if (count > 1) page->active_.set((page->active_.value() + 1) % count);
    });
    // F8：跳到当前文件的第一处问题（与 VSCode 的“下一处问题”同语义）
    UiRoot::Shortcut f8{};
    f8.key = "f8";
    (void)root->register_shortcut("f8", f8, [page]() {
      page->refresh_problems();
      if (page->problems_.empty()) {
        page->status_.set("没有问题可跳转");
        return true;
      }
      page->jump_to_problem_request(0);
      return true;
    });
    // Esc 关闭查找条与转到行浮层（`Input` 不认 Esc，用根级快捷键；**只在自己可见时
    // 接管**，免得吃掉别的场景的 Esc——比如命令面板自己要用的那个）。
    //
    // 为何两条要排在一起：它们是同一类“小浮层”，Esc 是它们共同的出口。
    // 只给查找条接 Esc 会让另一个变成“关不掉的浮层”（实测：转到行浮层开着时
    // 连 Ctrl+F 都打不开——快捷键被“浮层优先”让路，而它自己又不认 Esc）。
    UiRoot::Shortcut plain{};
    plain.key = "escape";
    (void)root->register_shortcut("escape", plain, [page]() {
      if (page->goto_line_open_.value()) {
        page->goto_line_open_.set(false);
        page->goto_line_error_.set("");
        return true;
      }
      if (!page->find_open_.value()) return false;
      page->dismiss_find();
      return true;
    });
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

  // 脚本逻辑层（仅 `--enable-script` 时可用）：**组件控制逻辑用 JS 写**。
  // 这份 JS 是编译期嵌入的真实资源（`assets/logic.js`）——与 C++ 里那堆 raw string 不同，
  // 它在编辑器里有高亮、不需要转义，改完重新构建即生效。
  if (auto* script = app.script(); script != nullptr) {
    const auto logic = b::embed<"examples/gbcode/assets/logic.js">();
    const std::string_view source(logic.data(), logic.length());
    if (auto loaded = script->eval(source, "assets/logic.js"); !loaded) {
      st::print("示例 JS 逻辑载入失败: {}\n", loaded.error().message);
    } else {
      st::print("示例 JS 逻辑已载入（{} 字节）\n", logic.length());
    }
  }

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

}  // namespace

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
