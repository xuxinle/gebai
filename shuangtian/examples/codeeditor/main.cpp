/// 代码编辑器演示：按 VSCode 的信息架构组装的 IDE 形态界面。
///
/// 布局五层（自上而下）：
/// - 标题栏：文件名 + 应用名 + 装饰性窗口控制（— □ ×）
/// - 菜单栏：文件 / 编辑 / 选择 / 查看 / 运行 / 帮助（`MenuBar` + 下拉面板）
/// - 主体三栏：活动栏（资源管理器·搜索·源代码管理·运行·扩展）+ 侧栏（按活动切换
///   面板）+ 编辑区（`Tabs` 多标签 + `CodeEditor`；隐藏标签零布局零绘制、状态不丢）
/// - 底部面板：问题 / 输出 / 终端（`Tabs` 切换；终端为可输入的模拟列表）
/// - 状态栏：分支 · 错误/警告计数 · 光标 Ln,Col · 缩进 · 编码 · 语言 · 主题切换
///
/// 演示点：
/// - **全部内置组件组装**，零自绘定制控件（对比早期 mdeditor 的四个自绘组件——
///   组件库覆盖度演进的实证，见 DESIGN.md §8.1.1）
/// - **多标签编辑模型**：打开/切换/关闭/脏标记（`Tabs::sync_tabs` 按 key 对齐）
/// - **命令面板**：Ctrl+Shift+P（overlay + 前缀过滤列表）
/// - **全局快捷键**：Ctrl+S 保存 / Ctrl+W 关标签 / Ctrl+Tab 切标签 / Ctrl+B 折叠侧栏
///   （`UiRoot::register_shortcut`——先于焦点链，文本组件吞键也拦得住）
/// - **自定义语言**：运行时注册 `stlog`（业务日志），与内置语言走同一台扫描器
/// - **脚本逻辑层**：`--enable-script` 时编译期嵌入的 `assets/logic.js` 生效
///
/// 控制通道兼容钩子（`tools/st_visual_check.py` 与子代理依赖）：
/// `#btn-theme`（主题切换按钮）、`#editor`（活动编辑器）、`#status`（状态栏文案）。
///
/// 坐标系：全部逻辑像素；文本索引为 UTF-8 字节偏移且落在码点边界。

#include <algorithm>
#include <map>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "battery/embed.hpp"  // 编译期资源嵌入（示例 JS 逻辑层）
#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/raster/paint.hpp"
#include "st/text/highlight.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::math::Insets;
using st::ui::Align;
using st::ui::Button;
using st::ui::CodeEditor;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::FontWeight;
using st::ui::IconView;
using st::ui::Input;
using st::ui::SplitView;
using st::ui::List;
using st::ui::MenuBar;
using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::Tabs;
using st::ui::Text;
using st::ui::Tone;
using st::ui::Tree;
// ————————————————————————————————————————————————————————————
// 数据：样例文件（一个"工作区"）
// ————————————————————————————————————————————————————————————

struct Sample {
  std::string name;
  std::string language;
  std::string code;
  std::string tree_key;  // 资源管理器里的路径（相对工作区根）
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
  constexpr float kEpsilon = 1.0f / 512.0f;
  (void)kEpsilon;
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

constexpr const char* kStlogSample =
    R"(# 业务日志（自定义语言：运行时注册的规则）
INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms
DEBUG service=gateway trace_id=9f2c 命中缓存 true
WARN  service=renderer component=font 字形缓存接近上限
ERROR service=net trace_id=9f2c 连接失败 resp=null  # 需要重试
FATAL service=core 磁盘写入失败，进程退出
)";

struct Options {
  std::string shots{};
  std::string control_file{};
  std::uint16_t control_port{0};
  float scale{0.0f};
  std::uint32_t frames{0};
  int max_ms{0};
  bool headless{false};
  bool enable_script{false};
  std::string theme{"light"};
  std::string language{"cpp"};
  /// 文字抗锯齿：auto（有窗口 → LCD 亚像素；无头 → 灰度）/ on / off。
  std::string text_lcd{"auto"};
  /// 字形网格拟合：auto/off/light/normal（见 AppOptions::text_fit）。
  std::string text_fit{"auto"};
  /// 工作区目录（真实文件模式）：给定时资源管理器/打开/保存全部走 `st::fs` 真实读写；
  /// 缺省回退内置样例工作区（内存模拟）。
  std::string workspace{};
};

[[nodiscard]] auto parse_options(int argc, char** argv) -> Options {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view raw = argv[index];
    const auto value = [&](std::string fallback) {
      return index + 1 < argc ? std::string(argv[++index]) : std::move(fallback);
    };
    if (raw == "--shots") options.shots = value({});
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(value("0")));
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(value("1")));
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(value("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(value("0"));
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--text-lcd") options.text_lcd = value("auto");
    else if (raw == "--text-fit") options.text_fit = value("auto");
    else if (raw == "--language") options.language = value("cpp");
    else if (raw == "--workspace") options.workspace = value(".");
    else if (raw == "--headless") options.headless = true;
    else if (raw == "--enable-script") options.enable_script = true;
  }
  return options;
}

// 打开的编辑器：一个标签 = 一份文本 + 一个 CodeEditor 实例

struct OpenBuffer {
  std::string key;       // 业务身份（样例名或绝对路径）
  std::string label;     // 标签名（文件名）
  std::string language;
  std::string text;      // 当前文本（切换回来时恢复）
  bool dirty{false};     // 修改点
  CodeEditor* editor{};  // 非拥有：挂在编辑区容器下的实例（随容器存活）
  std::string path{};    // 真实文件绝对路径（空 = 内置样例，保存走内存模拟）
};

/// 打开真实文件：`fs::read_text` 读入 → OpenBuffer（path 非空）。
/// 语言按扩展名推断（`CodeEditor::set_language_from_path` 同源逻辑）。
[[nodiscard]] auto open_file_buffer(std::vector<OpenBuffer>& buffers, const std::string& path)
    -> OpenBuffer* {
  auto content = st::fs::read_text(path);
  if (!content) return nullptr;
  // 已打开？刷新内容（保持同一标签）
  for (auto& buffer : buffers) {
    if (buffer.path == path) {
      buffer.text = *content;
      buffer.dirty = false;
      return &buffer;
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
  return &buffers.back();
}

/// 真实文件保存（脏标记清除；返回是否写盘成功）。
[[nodiscard]] auto save_buffer_to_disk(OpenBuffer& buffer) -> bool {
  if (buffer.path.empty()) return false;
  if (buffer.editor != nullptr) buffer.text = buffer.editor->text();
  if (auto status = st::fs::write_text(buffer.path, buffer.text); !status) return false;
  buffer.dirty = false;
  return true;
}

/// 目录树展开：`fs::list_dir` 一层（目录可再展开；文件点击打开）。
[[nodiscard]] auto scan_tree_nodes(const std::string& root) -> std::vector<st::ui::TreeNode> {
  std::vector<st::ui::TreeNode> nodes;
  auto entries = st::fs::list_dir(root);
  if (!entries) return nodes;
  // 目录在前、文件在后，各自字典序
  std::vector<const st::fs::DirEntry*> dirs;
  std::vector<const st::fs::DirEntry*> files;
  for (const auto& entry : *entries) {
    (entry.is_dir ? dirs : files).push_back(&entry);
  }
  const auto by_name = [](const auto* a, const auto* b) { return a->name < b->name; };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);
  for (const auto* dir : dirs) {
    nodes.push_back(st::ui::TreeNode{"dir:" + dir->name, dir->name, false, true, 0});
  }
  for (const auto* file : files) {
    nodes.push_back(st::ui::TreeNode{root + "/" + file->name, file->name, false, false, 0});
  }
  return nodes;
}

/// 查找替换浮层（VSCode Ctrl+F/H 形态的顶部右侧窄条）。
/// 组装在调用点（示例级）；与编辑器的交互全部走 `CodeEditor` 的 find/replace API。
struct FindBar {
  st::ui::Panel* card{nullptr};
  st::ui::Input* needle{nullptr};
  st::ui::Input* replacement{nullptr};
  st::ui::Text* counter{nullptr};
  st::ui::Button* next{nullptr};
  st::ui::Button* prev{nullptr};
  st::ui::Button* replace_one{nullptr};
  st::ui::Button* replace_all{nullptr};
  st::ui::Button* close{nullptr};
};

/// 工作台状态：全部原始指针非拥有（元素生命周期归 Panel 树）。
struct Workbench {
  // 标题
  Text* title_text{nullptr};
  // 活动栏 + 侧栏
  std::vector<Button*> activity_buttons{};
  std::vector<Panel*> side_panels{};
  // 编辑区
  Tabs* tabs{nullptr};
  Panel* editor_host{nullptr};  // 编辑器实例的直接父容器
  std::vector<OpenBuffer> buffers{};
  std::size_t active{0};
  // 状态栏
  Text* status{nullptr};
  Text* language_label{nullptr};
  Text* cursor_label{nullptr};
  Text* branch_label{nullptr};
  Button* theme_button{nullptr};
  // 终端
  Text* terminal_output{nullptr};
  Input* terminal_input{nullptr};
  // 输出面板
  Text* output_text{nullptr};
  // 空态提示（无标签时才显示）
  Element* empty_hint{nullptr};
  // 查找替换浮层
  FindBar find{};
  // 真实文件工作区（空 = 内置样例模式）
  std::string workspace_root{};
  // 目录展开状态（dir key → 展开？；驱动 scan_tree_nodes 的增量刷新）
  std::map<std::string, bool> dir_expanded{};
  Tree* workspace_tree{nullptr};
  // 问题面板（真实轻量检查的报告与计数）
  List* problems_list{nullptr};
  Text* problem_error_label{nullptr};
  Text* problem_warning_label{nullptr};
  // 状态栏附加信息（选区/脏标记）
  Text* selection_label{nullptr};
  // 状态栏错误/警告计数（与问题面板同源——两边都来自 `lint_text`）
  Text* status_error_count{nullptr};
  Text* status_warning_count{nullptr};
};

/// 一条轻量检查结果（**真检查**，不是占位数据）。
struct Problem {
  std::size_t line{0};       ///< 1 起行号
  bool warning{true};        ///< true = 警告，false = 错误
  std::string message{};
};

/// 便宜的逐行检查（不引入分析器）：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行。
///
/// 为什么值得做：状态栏的「错误 · 警告」计数与问题面板若没有真实来源，
/// 就只是摆设——改完代码什么都不会变，读者也无法验证计数联动是对的。
/// 这组规则几行就能跑完，且对真实的 C++/Python 文件都有意义。
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
    // Tab 缩进：与前导内容无关，只要行首是 Tab 就提醒（编辑器默认插空格）
    if (!line.empty() && line.front() == '\t') {
      problems.push_back(Problem{line_no, true, "用 Tab 缩进（本工程约定空格）"});
    }
    const std::size_t todo = line.find("TODO");
    const std::size_t fixme = line.find("FIXME");
    if (todo != std::string_view::npos || fixme != std::string_view::npos) {
      problems.push_back(Problem{line_no, true, "待办标记（TODO/FIXME）"});
    }
    if (line.size() > 120) {
      problems.push_back(Problem{line_no, true, "行超长（>120 列）"});
    }
    if (eol == std::string_view::npos) break;
    begin = eol + 1;
  }
  return problems;
}

/// 重跑当前编辑器的检查 → 刷新问题列表与状态栏计数（编辑与切标签后都调）。
void refresh_problems(Workbench& wb, st::ui::UiRoot& root) {
  if (wb.problems_list == nullptr) return;
  std::vector<Problem> problems;
  if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
    problems = lint_text(wb.buffers[wb.active].editor->text());
  }
  std::size_t errors = 0;
  std::size_t warnings = 0;
  std::vector<st::ui::List::Entry> entries;
  for (const auto& problem : problems) {
    (problem.warning ? warnings : errors) += 1;
    st::ui::List::Entry entry;
    entry.key = std::format("problem:{}", problem.line);
    entry.label = std::format("{}  第 {} 行  {}", problem.warning ? "警告" : "错误",
                              problem.line, problem.message);
    // 点问题 → 跳到那一行（与搜索命中同一条跳转语义）
    entry.on_activate = [&wb, root = &root, line = problem.line]() {
      if (wb.active >= wb.buffers.size() || wb.buffers[wb.active].editor == nullptr) return;
      wb.buffers[wb.active].editor->goto_line(line);
      wb.buffers[wb.active].editor->scroll_to_line(line);
      root->set_focus(wb.buffers[wb.active].editor);
      root->mark_dirty_all();
    };
    entries.push_back(std::move(entry));
  }
  if (entries.empty()) {
    st::ui::List::Entry clean;
    clean.key = "no-problems";
    clean.label = "工作区干净，没有发现问题";
    entries.push_back(std::move(clean));
  }
  wb.problems_list->sync_items(entries);
  if (wb.problem_error_label != nullptr) {
    wb.problem_error_label->set_content(std::format("{} 个错误", errors));
  }
  if (wb.problem_warning_label != nullptr) {
    wb.problem_warning_label->set_content(std::format("{} 个警告", warnings));
  }
  // 状态栏计数与问题面板**同源**（同一份 lint 结果）——
  // 两边不同源的话，会出现“状态栏 0 警告、面板列着两条”这类自相矛盾。
  if (wb.status_error_count != nullptr) {
    wb.status_error_count->set_content(std::to_string(errors));
  }
  if (wb.status_warning_count != nullptr) {
    wb.status_warning_count->set_content(std::to_string(warnings));
  }
}

/// 标题栏文案：脏时加一个圆点前缀（VSCode 同族的"未保存"提示）。
///
/// 抽成一个函数是必须的——标题栏在 7 处被赋值，散着写必然有几处漏掉脏标记。
[[nodiscard]] auto window_title(const Workbench& wb) -> std::string {
  if (wb.active >= wb.buffers.size()) return "codeeditor";
  const OpenBuffer& buffer = wb.buffers[wb.active];
  return std::string(buffer.dirty ? "● " : "") + buffer.label + " - codeeditor";
}

/// 把编辑器实例挂到编辑区（首个标签时创建，此后复用同一个实例）。
void bind_editor(Workbench& wb, st::ui::UiRoot& root) {
  if (wb.editor_host->child_count() == 0) {
    auto editor = std::make_unique<CodeEditor>();
    editor->set_id("editor");
    editor->set_font_size(13.5f);
    editor->set_tab_width(4);
    editor->style().grow = true;
    editor->on_change = [&wb, &root](std::string_view) {
      if (wb.active < wb.buffers.size()) {
        auto& buffer = wb.buffers[wb.active];
        if (!buffer.dirty) {
          buffer.dirty = true;
          // 同步 Tabs 修改点 + 标题栏脏点（两者都是“这份内容还没存”的可见信号）
          std::vector<st::ui::Tabs::Tab> tabs;
          for (const auto& open : wb.buffers) {
            tabs.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
          }
          wb.tabs->sync_tabs(tabs);
          if (wb.title_text != nullptr) wb.title_text->set_content(window_title(wb));
        }
      }
      // 每次编辑后重跑轻量检查（几百行是亚毫秒；大文件本来也不在示例的工作量里）
      refresh_problems(wb, root);
      root.mark_dirty_all();
    };
    editor->on_cursor_change = [&wb, &root]() {
      if (wb.active >= wb.buffers.size()) return;
      CodeEditor* active_editor = wb.buffers[wb.active].editor;
      if (active_editor == nullptr) return;
      wb.cursor_label->set_content(std::format("Ln {}, Col {}", active_editor->cursor_line() + 1,
                                               active_editor->cursor_column() + 1));
      // 选区计数：有选项时看得到「选了多少」（VSCode 同族状态栏项）
      if (wb.selection_label != nullptr) {
        if (const std::string picked = active_editor->selected_text(); !picked.empty()) {
          wb.selection_label->set_content(
              std::format("已选 {} 字符", st::utf8_length(picked)));
        } else {
          wb.selection_label->set_content({});
        }
      }
      root.mark_dirty_all();
    };
    wb.editor_host->add_child(std::move(editor));
  }
  if (wb.active < wb.buffers.size()) {
    OpenBuffer& buffer = wb.buffers[wb.active];
    buffer.editor = static_cast<CodeEditor*>(wb.editor_host->children().back().get());
    buffer.editor->set_language(buffer.language);
    buffer.editor->set_text(buffer.text);
    buffer.editor->set_read_only(false);
    wb.language_label->set_content(buffer.language);
    // VSCode 口径：行列 1 起始（打开后光标在文首）
    wb.cursor_label->set_content("Ln 1, Col 1");
  }
}

/// 打开一个样例（已在标签里 = 激活；不在 = 新标签 + 重建编辑器内容）。
void open_sample(Workbench& wb, const Sample& sample, st::ui::UiRoot& root) {
  std::size_t found = wb.buffers.size();
  for (std::size_t index = 0; index < wb.buffers.size(); ++index) {
    if (wb.buffers[index].key == sample.name) {
      found = index;
      break;
    }
  }
  if (found == wb.buffers.size()) {
    OpenBuffer buffer;
    buffer.key = sample.name;
    buffer.label = sample.name;
    buffer.language = sample.language;
    buffer.text = sample.code;
    wb.buffers.push_back(std::move(buffer));
  }
  wb.active = found;
  // 同步标签条（key 对齐，活动态跟随 key）
  std::vector<st::ui::Tabs::Tab> tabs;
  for (const auto& open : wb.buffers) {
    tabs.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
  }
  wb.tabs->sync_tabs(tabs);
  wb.tabs->set_active(found);
  bind_editor(wb, root);
  refresh_problems(wb, root);
  wb.title_text->set_content(window_title(wb));
  wb.status->set_content(sample.name + " · " +
                         std::to_string(st::utf8_length(sample.code)) + " 字符 · 已打开");
  root.set_focus(wb.buffers[found].editor);
  root.mark_dirty_all();
}

/// 关闭当前标签（内容在内存里，编辑器实例复用）。
void close_tab(Workbench& wb, st::ui::UiRoot& root) {
  if (wb.buffers.empty()) return;
  // 先存回当前文本（编辑器实例的 text 是最新值）
  if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
    wb.buffers[wb.active].text = wb.buffers[wb.active].editor->text();
  }
  wb.buffers.erase(wb.buffers.begin() + static_cast<std::ptrdiff_t>(wb.active));
  if (wb.active >= wb.buffers.size() && !wb.buffers.empty()) {
    wb.active = wb.buffers.size() - 1;
  }
  std::vector<st::ui::Tabs::Tab> tabs;
  for (const auto& open : wb.buffers) {
    tabs.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
  }
  wb.tabs->sync_tabs(tabs);
  if (wb.buffers.empty()) {
    wb.tabs->clear_tabs();
    wb.title_text->set_content("codeeditor");
    wb.status->set_content("无打开的编辑器（Ctrl+Shift+P 打开命令面板）");
    if (wb.empty_hint != nullptr) wb.empty_hint->set_visible(true);
    if (wb.editor_host != nullptr) wb.editor_host->set_visible(false);
    root.set_focus(nullptr);
    root.mark_dirty_all();
    return;
  }
  if (wb.empty_hint != nullptr) wb.empty_hint->set_visible(false);
  if (wb.editor_host != nullptr) wb.editor_host->set_visible(true);
  wb.tabs->set_active(wb.active);
  bind_editor(wb, root);
  refresh_problems(wb, root);
  const OpenBuffer& buffer = wb.buffers[wb.active];
  wb.title_text->set_content(window_title(wb));
  wb.status->set_content("已关闭标签 · 当前 " + buffer.label);
  root.set_focus(buffer.editor);
  root.mark_dirty_all();
}

/// 保存当前标签：真实文件走 `fs::write_text`，内置样例走内存模拟。
void save_active(Workbench& wb, st::ui::UiRoot& root) {
  if (wb.active >= wb.buffers.size()) {
    wb.status->set_content("没有可保存的编辑器");
    root.mark_dirty_all();
    return;
  }
  OpenBuffer& buffer = wb.buffers[wb.active];
  if (!buffer.path.empty()) {
    if (save_buffer_to_disk(buffer)) {
      wb.status->set_content("已保存 " + buffer.path);
    } else {
      wb.status->set_content("保存失败（写盘被拒）：" + buffer.path);
    }
  } else {
    wb.status->set_content("已保存 " + buffer.label + "（内存模拟，无磁盘写入）");
  }
  buffer.text = buffer.editor != nullptr ? buffer.editor->text() : buffer.text;
  buffer.dirty = false;
  std::vector<st::ui::Tabs::Tab> tabs;
  for (const auto& open : wb.buffers) {
    tabs.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
  }
  wb.tabs->sync_tabs(tabs);
  wb.output_text->set_content(wb.output_text->content() + "\n[保存] " + buffer.label + " · " +
                              std::to_string(st::utf8_length(buffer.text)) + " 字符");
  root.mark_dirty_all();
}

/// 切换侧栏面板（活动栏语义：互斥高亮 + 面板可见性）。
void select_activity(Workbench& wb, std::size_t index) {
  for (std::size_t i = 0; i < wb.activity_buttons.size(); ++i) {
    wb.activity_buttons[i]->set_variant(i == index ? Button::Variant::Soft
                                                   : Button::Variant::Ghost);
    wb.side_panels[i]->set_visible(i == index);
  }
}

// 主装配

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);
  register_custom_language();
  const std::vector<Sample> files = samples();

  st::app::AppOptions app_options;
  app_options.width = 1280;
  app_options.height = 800;
  app_options.scale = options.scale;
  app_options.title = "codeeditor · 霜天";
  app_options.headless = options.headless;
  app_options.backend = options.headless ? "headless" : std::string{};
  // 脚本能力显式开启：默认关闭，控制通道的 `script` 方法仅在开启后可用
  app_options.enable_script = options.enable_script;
  app_options.text_lcd = options.text_lcd;
  app_options.text_fit = options.text_fit;
  app_options.control_port = options.control_port;
  app_options.control_file = options.control_file;
  app_options.screenshot_dir = options.shots;
  app_options.theme = options.theme == "dark" ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light;

  st::app::Application app("codeeditor", "0.1.0", app_options);
  st::ui::UiRoot* root = &app.root();
  Workbench wb;

  // ════════════════ 页面骨架：五层纵向 ════════════════
  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id("editor-page");

  // —— 1. 标题栏（VSCode：居中文件标题 + 右侧窗口控制）——
  auto title_bar = std::make_unique<Panel>(FlexDirection::Row);
  title_bar->set_id("titlebar");
  title_bar->style().height = 36.0f;
  title_bar->style().padding = Insets{12.0f, 0.0f, 12.0f, 0.0f};
  title_bar->style().align_items = Align::Center;
  title_bar->style().gap = 8.0f;
  auto brand_icon = std::make_unique<st::ui::IconView>("code", 16.0f);
  title_bar->add_child(std::move(brand_icon));
  auto title_text = std::make_unique<Text>("codeeditor");
  title_text->set_id("title-text");
  title_text->set_font_size(12.5f);
  title_text->set_tone(Tone::Muted);
  wb.title_text = static_cast<Text*>(title_bar->add_child(std::move(title_text)));
  auto title_gap = std::make_unique<Panel>(FlexDirection::Row);
  title_gap->style().grow = true;
  title_bar->add_child(std::move(title_gap));
  // 装饰性窗口控制（— □ ×）：示例界面无窗口管理职责，仅呈现形态
  for (const auto& glyph : std::vector<std::string>{"minus", "square", "close"}) {
    auto control = std::make_unique<st::ui::IconView>(glyph, 14.0f);
    control->set_tone(Tone::Faint);
    auto holder = std::make_unique<Panel>(FlexDirection::Row);
    holder->style().padding = Insets{6.0f, 4.0f, 6.0f, 4.0f};
    holder->add_child(std::move(control));
    title_bar->add_child(std::move(holder));
  }
  page->add_child(std::move(title_bar));

  // —— 2. 菜单栏（MenuBar + 下拉面板 overlay）——
  auto menu_bar = std::make_unique<MenuBar>();
  menu_bar->set_id("menubar");
  std::vector<st::ui::Menu> menus;
  menus.push_back(st::ui::Menu{"file", "文件", {
      st::ui::MenuItem{.id = "new", .label = "新建文件"},
      st::ui::MenuItem{.id = "open", .label = "打开文件…"},
      st::ui::MenuItem{.separator = true},
      st::ui::MenuItem{.id = "save", .label = "保存（Ctrl+S）"},
      st::ui::MenuItem{.id = "close-tab", .label = "关闭编辑器（Ctrl+W）"},
  }});
  menus.push_back(st::ui::Menu{"edit", "编辑", {
      st::ui::MenuItem{.id = "undo", .label = "撤销"},
      st::ui::MenuItem{.id = "redo", .label = "重做"},
      st::ui::MenuItem{.separator = true},
      st::ui::MenuItem{.id = "comment", .label = "切换行注释（Ctrl+/）"},
      st::ui::MenuItem{.id = "select-all", .label = "全选"},
  }});
  menus.push_back(st::ui::Menu{"selection", "选择", {
      st::ui::MenuItem{.id = "select-all", .label = "全选（Ctrl+A）"},
      st::ui::MenuItem{.id = "goto-line", .label = "转到行…"},
      st::ui::MenuItem{.id = "copy", .label = "复制"},
      st::ui::MenuItem{.id = "paste", .label = "粘贴"},
  }});
  menus.push_back(st::ui::Menu{"view", "查看", {
      st::ui::MenuItem{.id = "command-palette", .label = "命令面板…（Ctrl+Shift+P）"},
      st::ui::MenuItem{.separator = true},
      st::ui::MenuItem{.id = "toggle-sidebar", .label = "切换侧栏可见性（Ctrl+B）"},
      st::ui::MenuItem{.id = "toggle-theme", .label = "切换亮/暗主题"},
  }});
  menus.push_back(st::ui::Menu{"run", "运行", {
      st::ui::MenuItem{.id = "run-task", .label = "运行任务：构建 gallery"},
      st::ui::MenuItem{.id = "run-test", .label = "运行任务：st test"},
      st::ui::MenuItem{.separator = true},
      st::ui::MenuItem{.id = "toggle-terminal", .label = "切换终端（底部面板）"},
  }});
  menus.push_back(st::ui::Menu{"help", "帮助", {
      st::ui::MenuItem{.id = "about", .label = "关于 codeeditor"},
  }});
  menu_bar->set_menus(menus);
  auto* menu_bar_ptr = menu_bar.get();
  page->add_child(std::move(menu_bar));

  // —— 3. 主体：活动栏 + 侧栏 + 编辑区 ——
  auto main_row = std::make_unique<Panel>(FlexDirection::Row);
  main_row->set_id("main-row");
  main_row->style().grow = true;

  // 活动栏（48px：图标按钮纵排）
  struct ActivitySpec {
    const char* icon;
    const char* label;
  };
  const std::vector<ActivitySpec> activities = {
      {"folder", "资源管理器"}, {"search", "搜索"}, {"git-branch", "源代码管理"},
      {"play", "运行和调试"}, {"package", "扩展"},
  };

  auto activity_bar = std::make_unique<Panel>(FlexDirection::Column);
  activity_bar->set_id("activitybar");
  activity_bar->style().width = 48.0f;
  activity_bar->style().padding = Insets{0.0f, 6.0f, 0.0f, 6.0f};
  activity_bar->style().gap = 2.0f;
  activity_bar->style().align_items = Align::Center;

  auto sidebar = std::make_unique<Panel>(FlexDirection::Column);
  sidebar->set_id("sidebar");
  // 宽度不再硬编码：由 `sidebar-split` 的比例决定（可拖拽）。

  // 侧栏面板 ×5（同一时刻只显示一个）
  // —— 3a. 资源管理器：工作区树（目录 + 文件，点击打开标签）——
  auto explorer_panel = std::make_unique<Panel>(FlexDirection::Column);
  explorer_panel->set_id("panel-explorer");
  explorer_panel->style().grow = true;
  auto explorer_head = std::make_unique<Panel>(FlexDirection::Row);
  explorer_head->style().padding = Insets{10.0f, 8.0f, 10.0f, 8.0f};
  explorer_head->style().align_items = Align::Center;
  explorer_head->style().gap = 6.0f;
  auto explorer_title = std::make_unique<Text>("资源管理器");
  explorer_title->set_font_size(11.5f);
  explorer_title->set_weight(FontWeight::SemiBold);
  explorer_title->set_tone(Tone::Muted);
  explorer_head->add_child(std::move(explorer_title));
  explorer_panel->add_child(std::move(explorer_head));
  auto explorer_scroll = std::make_unique<ScrollView>();
  explorer_scroll->set_id("explorer-scroll");
  explorer_scroll->style().grow = true;
  auto tree = std::make_unique<Tree>();
  tree->set_id("workspace-tree");
  auto* tree_ptr = tree.get();
  wb.workspace_tree = tree_ptr;
  wb.workspace_root = options.workspace;
  if (!options.workspace.empty()) {
    // 真实文件模式：`fs::list_dir` 扫工作区根（目录可展开，见 on_toggle 的增量刷新）
    tree->sync_nodes(scan_tree_nodes(options.workspace));
  } else {
    std::vector<st::ui::TreeNode> nodes;
    // 目录（按 tree_key 的第一段聚合；未展开目录的子项不列——展开状态由数据表达，
    // 示例只对 src/tools 预展开并给出子项，其余目录收起（与展开箭头一致，避免"假展开"）
    nodes.push_back(st::ui::TreeNode{"dir-src", "src", true, true, 0});
    nodes.push_back(st::ui::TreeNode{"src/raster/renderer.cpp", "  renderer.cpp", false, false, 1});
    nodes.push_back(st::ui::TreeNode{"dir-tools", "tools", true, true, 0});
    nodes.push_back(st::ui::TreeNode{"tools/deploy.py", "  deploy.py", false, false, 1});
    nodes.push_back(st::ui::TreeNode{"dir-assets", "assets", false, true, 0});
    nodes.push_back(st::ui::TreeNode{"dir-github", ".github", false, true, 0});
    nodes.push_back(st::ui::TreeNode{"dir-web", "web", false, true, 0});
    nodes.push_back(st::ui::TreeNode{"dir-stats", "stats", false, true, 0});
    nodes.push_back(st::ui::TreeNode{"dir-patches", "patches", false, true, 0});
    for (const auto& sample : files) {
      // 已挂在目录下的文件不再顶层重复
      if (sample.tree_key.rfind("src/", 0) == 0 || sample.tree_key.rfind("tools/", 0) == 0) {
        continue;
      }
      st::ui::TreeNode node{sample.name, sample.name, false, false, 0};
      nodes.push_back(std::move(node));
    }
    nodes.push_back(st::ui::TreeNode{"stlog.log", "stlog.log", false, false, 0});
    tree->sync_nodes(nodes);
  }
  explorer_scroll->add_child(std::move(tree));
  explorer_panel->add_child(std::move(explorer_scroll));

  // —— 3b. 搜索面板：输入框 + 结果列表 ——
  auto search_panel = std::make_unique<Panel>(FlexDirection::Column);
  search_panel->set_id("panel-search");
  search_panel->style().grow = true;
  auto search_input = std::make_unique<Input>();
  search_input->set_id("search-input");
  search_input->set_placeholder("搜索（在工作区样例文本里找）");
  search_input->set_icon_prefix("search");
  auto* search_input_ptr = static_cast<Input*>(search_panel->add_child(std::move(search_input)));
  auto search_scroll = std::make_unique<ScrollView>();
  search_scroll->set_id("search-scroll");
  search_scroll->style().grow = true;
  auto search_list = std::make_unique<List>();
  search_list->set_id("search-results");
  auto* search_list_ptr = static_cast<List*>(search_scroll->add_child(std::move(search_list)));
  search_panel->add_child(std::move(search_scroll));

  // —— 3c/3d/3e. 源代码管理 / 运行 / 扩展（占位卡片）——
  const auto make_stub_panel = [](const char* id, const char* icon, const char* title,
                                  const char* body) -> std::unique_ptr<Panel> {
    auto panel = std::make_unique<Panel>(FlexDirection::Column);
    panel->set_id(id);
    panel->style().grow = true;
    panel->style().padding = Insets{12.0f, 10.0f, 12.0f, 10.0f};
    panel->style().gap = 8.0f;
    auto head = std::make_unique<Panel>(FlexDirection::Row);
    head->style().gap = 6.0f;
    head->style().align_items = Align::Center;
    head->add_child(std::make_unique<st::ui::IconView>(icon, 15.0f));
    auto head_title = std::make_unique<Text>(title);
    head_title->set_font_size(11.5f);
    head_title->set_weight(FontWeight::SemiBold);
    head_title->set_tone(Tone::Muted);
    head->add_child(std::move(head_title));
    panel->add_child(std::move(head));
    auto body_text = std::make_unique<Text>(body);
    body_text->set_font_size(12.0f);
    body_text->set_tone(Tone::Faint);
    panel->add_child(std::move(body_text));
    return panel;
  };
  auto scm_panel = make_stub_panel("panel-scm", "git-branch", "源代码管理",
                                   "分支 main\n变更 0 · 暂存 0\n（示例占位：无真实 git）");
  auto run_panel = make_stub_panel("panel-run", "play", "运行和调试",
                                   "运行任务：\n· 构建 gallery\n· st test\n· st lint");
  auto extensions_panel = make_stub_panel("panel-extensions", "package", "扩展",
                                          "已安装：\n· stlog 高亮（内置规则）\n· 无其他扩展");

  wb.side_panels = {
      static_cast<Panel*>(sidebar->add_child(std::move(explorer_panel))),
      static_cast<Panel*>(sidebar->add_child(std::move(search_panel))),
      static_cast<Panel*>(sidebar->add_child(std::move(scm_panel))),
      static_cast<Panel*>(sidebar->add_child(std::move(run_panel))),
      static_cast<Panel*>(sidebar->add_child(std::move(extensions_panel))),
  };
  // 活动栏按钮（Soft = 活动态；互斥切换）
  for (std::size_t index = 0; index < activities.size(); ++index) {
    auto button = std::make_unique<Button>("", Button::Variant::Ghost, Button::Size::Small);
    button->set_id(std::string("activity-") + activities[index].icon);
    button->set_icon(activities[index].icon);
    button->style().width = 40.0f;
    button->style().height = 40.0f;
    button->on_click = [&wb, index]() { select_activity(wb, index); };
    wb.activity_buttons.push_back(
        static_cast<Button*>(activity_bar->add_child(std::move(button))));
  }
  auto activity_gap = std::make_unique<Panel>(FlexDirection::Column);
  activity_gap->style().grow = true;
  activity_bar->add_child(std::move(activity_gap));
  auto settings_button = std::make_unique<Button>("", Button::Variant::Ghost, Button::Size::Small);
  settings_button->set_id("activity-settings");
  settings_button->set_icon("settings");
  settings_button->style().width = 40.0f;
  settings_button->style().height = 40.0f;
  activity_bar->add_child(std::move(settings_button));
  main_row->add_child(std::move(activity_bar));
  // 侧栏与编辑区之间用 **SplitView**（侧栏宽度可拖拽）——
  // 此前是硬编码 240px 不可调（「SplitView 内置化」正是由此反推的框架缺口）。
  // 活动栏不进分栏（VSCode 里它固定 48px），分栏只包侧栏 + 编辑区。
  auto sidebar_split = std::make_unique<SplitView>();
  sidebar_split->set_id("sidebar-split");
  sidebar_split->style().grow = true;
  sidebar_split->set_min_ratio(0.12f);
  sidebar_split->set_ratio(0.22f, false);   // ≈240px / (1360-48) 【与旧硬编码宽度同量级】
  sidebar_split->set_first(std::move(sidebar));

  // —— 编辑区：Tabs + 编辑器 ——
  auto editor_column = std::make_unique<Panel>(FlexDirection::Column);
  editor_column->set_id("editor-column");
  editor_column->style().grow = true;

  auto tabs = std::make_unique<Tabs>();
  tabs->set_id("editor-tabs");
  wb.tabs = static_cast<Tabs*>(editor_column->add_child(std::move(tabs)));

  auto editor_host = std::make_unique<Panel>(FlexDirection::Column);
  editor_host->set_id("editor-host");
  editor_host->style().grow = true;
  editor_host->style().padding = Insets{8.0f, 4.0f, 8.0f, 4.0f};
  wb.editor_host = static_cast<Panel*>(editor_column->add_child(std::move(editor_host)));

  // 空态提示（无标签时显示；有编辑器后被盖住/编辑器隐藏）
  auto empty_hint = std::make_unique<Panel>(FlexDirection::Column);
  empty_hint->set_id("empty-hint");
  empty_hint->style().grow = true;
  empty_hint->style().align_items = Align::Center;
  empty_hint->style().justify = st::ui::Justify::Center;
  empty_hint->style().gap = 6.0f;
  auto hint_icon = std::make_unique<st::ui::IconView>("code", 40.0f);
  hint_icon->set_tone(Tone::Faint);
  empty_hint->add_child(std::move(hint_icon));
  auto hint_text = std::make_unique<Text>("从左侧资源管理器打开一个文件，或 Ctrl+Shift+P 打开命令面板");
  hint_text->set_tone(Tone::Faint);
  hint_text->set_font_size(12.0f);
  empty_hint->add_child(std::move(hint_text));
  auto* empty_hint_ptr = static_cast<Panel*>(editor_column->add_child(std::move(empty_hint)));
  wb.empty_hint = empty_hint_ptr;
  wb.editor_host->style().grow = true;

  sidebar_split->set_second(std::move(editor_column));
  main_row->add_child(std::move(sidebar_split));
  page->add_child(std::move(main_row));

  // —— 4. 底部面板：问题 / 输出 / 终端 ——
  auto bottom_panel = std::make_unique<Panel>(FlexDirection::Column);
  bottom_panel->set_id("bottom-panel");
  bottom_panel->style().height = 170.0f;

  auto bottom_tabs = std::make_unique<Tabs>();
  bottom_tabs->set_id("bottom-tabs");
  std::vector<std::string> bottom_labels = {"问题", "输出", "终端"};
  bottom_tabs->set_tabs(bottom_labels);
  auto* bottom_tabs_ptr = bottom_tabs.get();
  bottom_panel->add_child(std::move(bottom_tabs));

  // —— 问题面板：**真实轻量检查**报告 ——
  //
  // 旧版是“0 个错误 / 0 个警告”的假数据（写着 0 却什么都没查），既无信息量也
  // 无法验证状态栏计数联动。现在跑一组便宜的逐行检查（行尾空白 / Tab 缩进 /
  // TODO·FIXME / 超长行），报告行号与原因；列表项可点，点了**跳到那一行**。
  auto problems_panel = std::make_unique<Panel>(FlexDirection::Column);
  problems_panel->set_id("panel-problems");
  problems_panel->style().grow = true;
  problems_panel->style().padding = Insets{8.0f, 6.0f, 8.0f, 6.0f};
  problems_panel->style().gap = 6.0f;
  auto problems_head = std::make_unique<Panel>(FlexDirection::Row);
  problems_head->style().gap = 16.0f;
  problems_head->style().align_items = Align::Center;
  const auto make_problem_count = [&problems_head](const char* icon, st::ui::Tone tone) {
    auto row = std::make_unique<Panel>(FlexDirection::Row);
    row->style().gap = 6.0f;
    row->style().align_items = Align::Center;
    auto mark = std::make_unique<st::ui::IconView>(icon, 14.0f);
    mark->set_tone(tone);
    row->add_child(std::move(mark));
    auto label = std::make_unique<Text>("0");
    label->set_font_size(12.0f);
    Text* label_ptr = static_cast<Text*>(row->add_child(std::move(label)));
    problems_head->add_child(std::move(row));
    return label_ptr;
  };
  wb.problem_error_label = make_problem_count("error", st::ui::Tone::Danger);
  wb.problem_warning_label = make_problem_count("warning", st::ui::Tone::Warning);
  auto problems_title = std::make_unique<Text>("当前编辑器");
  problems_title->set_tone(Tone::Faint);
  problems_title->set_font_size(11.5f);
  problems_head->add_child(std::move(problems_title));
  auto problems_head_gap = std::make_unique<Panel>(FlexDirection::Row);
  problems_head_gap->style().grow = true;
  problems_head->add_child(std::move(problems_head_gap));
  problems_panel->add_child(std::move(problems_head));
  auto problems_scroll = std::make_unique<ScrollView>();
  problems_scroll->set_id("problems-scroll");
  problems_scroll->style().grow = true;
  auto problems_list = std::make_unique<List>();
  problems_list->set_id("problems-list");
  wb.problems_list = static_cast<List*>(problems_scroll->add_child(std::move(problems_list)));
  problems_panel->add_child(std::move(problems_scroll));
  auto* problems_ptr = bottom_panel->add_child(std::move(problems_panel));

  // 输出面板
  auto output_scroll = std::make_unique<ScrollView>();
  output_scroll->set_id("output-scroll");
  output_scroll->style().grow = true;
  auto output_text = std::make_unique<Text>(
      "[启动] codeeditor 0.1.0 · 后端待启动后回填\n[提示] Ctrl+S 保存 · Ctrl+W 关标签 · Ctrl+Shift+P 命令面板");
  output_text->set_id("output-text");
  output_text->set_font_size(12.0f);
  output_text->set_tone(Tone::Muted);
  output_text->style().monospace = true;
  wb.output_text = static_cast<Text*>(output_scroll->add_child(std::move(output_text)));
  Element* output_ptr = bottom_panel->add_child(std::move(output_scroll));
  output_ptr->set_id("panel-output");

  // 终端面板（输出行 + 输入行）
  auto terminal_panel = std::make_unique<Panel>(FlexDirection::Column);
  terminal_panel->set_id("panel-terminal");
  terminal_panel->style().grow = true;
  terminal_panel->style().padding = Insets{8.0f, 6.0f, 8.0f, 6.0f};
  terminal_panel->style().gap = 6.0f;
  auto terminal_scroll = std::make_unique<ScrollView>();
  terminal_scroll->set_id("terminal-scroll");
  terminal_scroll->style().grow = true;
  terminal_scroll->style().padding = Insets{2.0f, 0.0f, 2.0f, 0.0f};
  auto terminal_output = std::make_unique<Text>(
      "shuangtian dev terminal\n> 输入 help 查看可用命令，Enter 提交");
  terminal_output->set_id("terminal-output");
  terminal_output->set_font_size(12.0f);
  terminal_output->style().monospace = true;
  wb.terminal_output = static_cast<Text*>(terminal_scroll->add_child(std::move(terminal_output)));
  terminal_panel->add_child(std::move(terminal_scroll));
  auto terminal_input_row = std::make_unique<Panel>(FlexDirection::Row);
  terminal_input_row->style().gap = 6.0f;
  terminal_input_row->style().align_items = Align::Center;
  auto terminal_prompt = std::make_unique<Text>("❯");
  terminal_prompt->set_tone(Tone::Success);
  terminal_prompt->style().monospace = true;
  terminal_input_row->add_child(std::move(terminal_prompt));
  auto terminal_input = std::make_unique<Input>();
  terminal_input->set_id("terminal-input");
  terminal_input->set_placeholder("help");
  wb.terminal_input = static_cast<Input*>(terminal_input_row->add_child(std::move(terminal_input)));
  terminal_panel->add_child(std::move(terminal_input_row));
  auto* terminal_ptr = bottom_panel->add_child(std::move(terminal_panel));

  // 底部三个面板的可见性由 bottom_tabs 驱动
  const std::vector<Element*> bottom_pages{problems_ptr, output_ptr, terminal_ptr};
  const auto show_bottom = [bottom_tabs_ptr, &bottom_pages, root](std::size_t index) {
    for (std::size_t i = 0; i < bottom_pages.size(); ++i) {
      bottom_pages[i]->set_visible(i == index);
    }
    bottom_tabs_ptr->set_active(index);
    root->mark_dirty_all();
  };
  bottom_tabs_ptr->on_change = [show_bottom](std::size_t index) { show_bottom(index); };
  show_bottom(2);  // 默认显示终端（VSCode 用户最常用的落点）

  page->add_child(std::move(bottom_panel));

  // —— 5. 状态栏 ——
  auto status_bar = std::make_unique<Panel>(FlexDirection::Row);
  status_bar->set_id("statusbar");
  status_bar->style().height = 26.0f;
  status_bar->style().padding = Insets{10.0f, 0.0f, 10.0f, 0.0f};
  status_bar->style().gap = 12.0f;
  status_bar->style().align_items = Align::Center;

  auto branch_icon = std::make_unique<st::ui::IconView>("git-branch", 12.0f);
  branch_icon->set_tone(Tone::Primary);
  status_bar->add_child(std::move(branch_icon));
  auto branch_label = std::make_unique<Text>("main*");
  branch_label->set_id("branch-label");
  branch_label->set_font_size(11.5f);
  wb.branch_label = static_cast<Text*>(status_bar->add_child(std::move(branch_label)));

  auto error_icon = std::make_unique<st::ui::IconView>("error", 12.0f);
  error_icon->set_tone(Tone::Danger);
  status_bar->add_child(std::move(error_icon));
  auto error_count = std::make_unique<Text>("0");
  error_count->set_id("status-errors");
  error_count->set_font_size(11.5f);
  wb.status_error_count = static_cast<Text*>(status_bar->add_child(std::move(error_count)));
  auto warn_icon = std::make_unique<st::ui::IconView>("warning", 12.0f);
  warn_icon->set_tone(Tone::Warning);
  status_bar->add_child(std::move(warn_icon));
  auto warn_count = std::make_unique<Text>("0");
  warn_count->set_id("status-warnings");
  warn_count->set_font_size(11.5f);
  wb.status_warning_count = static_cast<Text*>(status_bar->add_child(std::move(warn_count)));

  auto status_gap = std::make_unique<Panel>(FlexDirection::Row);
  status_gap->style().grow = true;
  status_bar->add_child(std::move(status_gap));

  auto cursor_label = std::make_unique<Text>("Ln 1, Col 1");
  cursor_label->set_id("cursor-label");
  cursor_label->set_font_size(11.5f);
  wb.cursor_label = static_cast<Text*>(status_bar->add_child(std::move(cursor_label)));
  auto sep1 = std::make_unique<Text>("·");
  sep1->set_tone(Tone::Faint);
  status_bar->add_child(std::move(sep1));
  auto spaces_label = std::make_unique<Text>("空格: 4");
  spaces_label->set_font_size(11.5f);
  status_bar->add_child(std::move(spaces_label));
  auto sep2 = std::make_unique<Text>("·");
  sep2->set_tone(Tone::Faint);
  status_bar->add_child(std::move(sep2));
  auto encoding_label = std::make_unique<Text>("UTF-8");
  encoding_label->set_font_size(11.5f);
  status_bar->add_child(std::move(encoding_label));
  auto sep3 = std::make_unique<Text>("·");
  sep3->set_tone(Tone::Faint);
  status_bar->add_child(std::move(sep3));
  auto language_label = std::make_unique<Text>(options.language);
  language_label->set_id("language-label");
  language_label->set_font_size(11.5f);
  wb.language_label = static_cast<Text*>(status_bar->add_child(std::move(language_label)));

  // 选区计数（有选中时才显示文字；空串即“不占视觉位置”）
  auto selection_label = std::make_unique<Text>("");
  selection_label->set_id("selection-label");
  selection_label->set_tone(Tone::Primary);
  selection_label->set_font_size(11.5f);
  wb.selection_label = static_cast<Text*>(status_bar->add_child(std::move(selection_label)));

  // 状态文案（左半区）：兼容钩子——id 沿用旧版 `status`
  auto status = std::make_unique<Text>("就绪");
  status->set_id("status");
  status->set_tone(Tone::Faint);
  status->set_font_size(11.5f);
  wb.status = static_cast<Text*>(status_bar->add_child(std::move(status)));

  // 主题切换按钮：兼容钩子——id 沿用 `btn-theme`（st_visual_check.py 依赖）
  auto theme_button = std::make_unique<Button>("暗色", Button::Variant::Ghost, Button::Size::Small);
  theme_button->set_id("btn-theme");
  theme_button->set_icon("moon");
  wb.theme_button = theme_button.get();
  auto* app_ptr = &app;
  auto* root_ptr = root;
  wb.theme_button->on_click = [app_ptr, root_ptr, wb_theme = wb.theme_button]() {
    const bool dark = app_ptr->root().theme().mode() == st::ui::ThemeMode::Light;
    app_ptr->set_theme_mode(dark ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light);
    wb_theme->set_label(dark ? "亮色" : "暗色");
    wb_theme->set_icon(dark ? "sun" : "moon");
    root_ptr->mark_dirty_all();
  };
  status_bar->add_child(std::move(theme_button));
  page->add_child(std::move(status_bar));

  // ════════════════ 查找替换浮层（Ctrl+F / Ctrl+H）════════════════
  {
    auto card = std::make_unique<Panel>(FlexDirection::Row);
    card->set_id("find-bar");
    card->style().padding = Insets{8.0f, 8.0f, 8.0f, 8.0f};
    card->style().gap = 6.0f;
    card->style().align_items = Align::Center;
    card->style().background = st::math::Color::rgb(32, 40, 54);
    card->style().border_color = st::math::Color::rgba(255, 255, 255, 30);
    card->style().border_width = 1.0f;
    card->style().radius = 8.0f;
    auto needle = std::make_unique<Input>();
    needle->set_id("find-needle");
    needle->set_placeholder("查找");
    needle->style().width = 170.0f;
    wb.find.needle = static_cast<Input*>(card->add_child(std::move(needle)));

    auto counter = std::make_unique<Text>("0/0");
    counter->set_id("find-counter");
    counter->set_font_size(11.5f);
    counter->set_tone(Tone::Muted);
    counter->style().width = 46.0f;
    wb.find.counter = static_cast<Text*>(card->add_child(std::move(counter)));

    auto prev = std::make_unique<Button>("↑", Button::Variant::Ghost, Button::Size::Small);
    prev->set_id("find-prev");
    wb.find.prev = static_cast<Button*>(card->add_child(std::move(prev)));
    auto next = std::make_unique<Button>("↓", Button::Variant::Ghost, Button::Size::Small);
    next->set_id("find-next");
    wb.find.next = static_cast<Button*>(card->add_child(std::move(next)));

    auto replacement = std::make_unique<Input>();
    replacement->set_id("find-replace");
    replacement->set_placeholder("替换为");
    replacement->style().width = 150.0f;
    wb.find.replacement = static_cast<Input*>(card->add_child(std::move(replacement)));

    auto replace_one = std::make_unique<Button>("替换", Button::Variant::Secondary, Button::Size::Small);
    replace_one->set_id("find-replace-one");
    wb.find.replace_one = static_cast<Button*>(card->add_child(std::move(replace_one)));
    auto replace_all = std::make_unique<Button>("全部", Button::Variant::Secondary, Button::Size::Small);
    replace_all->set_id("find-replace-all");
    wb.find.replace_all = static_cast<Button*>(card->add_child(std::move(replace_all)));

    auto close = std::make_unique<Button>("×", Button::Variant::Ghost, Button::Size::Small);
    close->set_id("find-close");
    wb.find.close = static_cast<Button*>(card->add_child(std::move(close)));
    wb.find.card = card.get();
    root->add_overlay(std::move(card), st::ui::UiRoot::OverlayLayout::Stack);
    wb.find.card->set_visible(false);  // Ctrl+F 唤出
  }

  // ════════════════ 行为接线 ════════════════

  // —— 查找替换浮层 ——
  const auto active_editor = [&wb]() -> CodeEditor* {
    return wb.active < wb.buffers.size() ? wb.buffers[wb.active].editor : nullptr;
  };
  const auto refresh_find_counter = [&wb, &active_editor]() {
    if (active_editor() == nullptr) return;
    const std::size_t total = active_editor()->find_match_count();
    const std::size_t active = active_editor()->find_active_index();
    wb.find.counter->set_content(
        total == 0 ? std::string("0/0")
                   : std::format("{}/{}", active == CodeEditor::kNoFindMatch ? 0 : active + 1,
                                 total));
  };
  wb.find.needle->on_change = [&wb, &active_editor, &refresh_find_counter, root](
                                  std::string_view value) {
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      editor->set_find(std::string(value));
      editor->find_next(false);
      refresh_find_counter();
    }
    root->mark_dirty_all();
  };
  wb.find.needle->on_submit = [&wb, &active_editor, &refresh_find_counter, root](
                                  std::string_view) {
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      editor->find_next(false);
      refresh_find_counter();
      root->set_focus(wb.find.needle);
    }
    root->mark_dirty_all();
  };
  wb.find.next->on_click = [&wb, &active_editor, &refresh_find_counter, root]() {
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      editor->find_next(false);
      refresh_find_counter();
      root->mark_dirty_all();
    }
  };
  wb.find.prev->on_click = [&wb, &active_editor, &refresh_find_counter, root]() {
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      editor->find_next(true);
      refresh_find_counter();
      root->mark_dirty_all();
    }
  };
  wb.find.replace_one->on_click = [&wb, &active_editor, &refresh_find_counter, root]() {
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      editor->replace_current(wb.find.replacement->value());
      refresh_find_counter();
      root->mark_dirty_all();
    }
  };
  wb.find.replace_all->on_click = [&wb, &active_editor, &refresh_find_counter, root]() {
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      const std::size_t count = editor->replace_all(wb.find.replacement->value());
      wb.status->set_content(std::format("已替换 {} 处", count));
      refresh_find_counter();
      root->mark_dirty_all();
    }
  };
  // 关闭查找条（× 按钮与 Esc 共用一条路径——此前 Esc 关不掉：浮层本身不认 Esc）
  const auto close_find_bar = [&wb, &active_editor, root]() {
    if (CodeEditor* editor = active_editor(); editor != nullptr) editor->clear_find();
    wb.find.card->set_visible(false);
    if (CodeEditor* editor = active_editor(); editor != nullptr) root->set_focus(editor);
    root->mark_dirty_all();
  };
  wb.find.close->on_click = [&close_find_bar]() { close_find_bar(); };
  // 打开查找条（focus 输入框；预填当前选中文本——VSCode 同族行为）
  const auto open_find_bar = [&wb, &active_editor, &refresh_find_counter, root]() {
    wb.find.card->set_visible(true);
    if (CodeEditor* editor = active_editor(); editor != nullptr) {
      const std::string picked = editor->selected_text();
      if (!picked.empty() && picked.find('\n') == std::string::npos) {
        wb.find.needle->set_text(picked);
        editor->set_find(picked);
        editor->find_next(false);
      }
      refresh_find_counter();
    }
    root->set_focus(wb.find.needle);
    root->mark_dirty_all();
  };

  // Esc 关闭查找条：注册在**根**上的全局快捷键（先于焦点链），
  // 因 `Input` 不认 Esc。只在白可见时接管，以免吃掉其他场景的 Esc。
  st::ui::UiRoot::Shortcut plain_key{};
  (void)root->register_shortcut(
      "escape", plain_key, [&wb, &close_find_bar]() {
        if (!wb.find.card->visible()) return false;
        close_find_bar();
        return true;
      });

  // Tabs：切换标签 = 换绑编辑器内容；× = 关闭
  wb.tabs->on_change = [&wb, root](std::size_t index) {
    if (index >= wb.buffers.size()) return;
    // 存回旧文本
    if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
      wb.buffers[wb.active].text = wb.buffers[wb.active].editor->text();
    }
    wb.active = index;
    bind_editor(wb, *root);
    refresh_problems(wb, *root);
    wb.title_text->set_content(window_title(wb));
    root->set_focus(wb.buffers[index].editor);
    root->mark_dirty_all();
  };
  wb.tabs->on_close = [&wb, root](std::size_t index) {
    if (index >= wb.buffers.size()) return;
    // 存回文本再走统一关闭路径
    if (wb.buffers[index].editor != nullptr) {
      wb.buffers[index].text = wb.buffers[index].editor->text();
    }
    wb.active = index;
    close_tab(wb, *root);
  };

  // 资源管理器：点文件 = 打开标签；stlog.log = 自定义语言样例
  // （`tree_ptr` 在创建期已取裸指针——`root->find` 需 set_content 之后才可用）
  tree_ptr->on_select = [&wb, &files, root](std::string_view key) {
    // 真实文件模式：key 即绝对路径（scan_tree_nodes 生成）
    if (!wb.workspace_root.empty() && key.rfind("dir:", 0) != 0) {
      if (auto* buffer = open_file_buffer(wb.buffers, std::string(key)); buffer != nullptr) {
        // 找到它在 buffers 的下标 → 走统一激活路径
        std::size_t index = 0;
        for (std::size_t i = 0; i < wb.buffers.size(); ++i) {
          if (&wb.buffers[i] == buffer) index = i;
        }
        std::vector<st::ui::Tabs::Tab> tab_bar;
        for (const auto& open : wb.buffers) {
          tab_bar.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
        }
        wb.tabs->sync_tabs(tab_bar);
        wb.tabs->set_active(index);
        wb.active = index;
        bind_editor(wb, *root);
        refresh_problems(wb, *root);
        wb.title_text->set_content(window_title(wb));
        wb.status->set_content("已打开 " + buffer->path);
        root->set_focus(buffer->editor);
        root->mark_dirty_all();
        return;
      }
      wb.status->set_content("打开失败：" + std::string(key));
      root->mark_dirty_all();
      return;
    }
    if (key == "stlog.log") {
      // 自定义语言样例：作为标签打开
      Sample stlog_sample{"stlog.log", "stlog", kStlogSample, "logs/stlog.log"};
      open_sample(wb, stlog_sample, *root);
      wb.status->set_content("自定义语言 stlog（运行时注册的规则）");
      root->mark_dirty_all();
      return;
    }
    for (const auto& sample : files) {
      if (sample.name == key) {
        open_sample(wb, sample, *root);
        return;
      }
    }
  };
  // 目录展开/收起：真实工作区按需增量扫描（展开目录 → 子节点挂到该目录下）
  tree_ptr->on_toggle = [&wb, root](std::string_view key, bool expanded) {
    if (wb.workspace_root.empty() || key.rfind("dir:", 0) != 0) {
      root->mark_dirty_all();
      return;
    }
    // dir: 的 key 只有目录名——从现有节点反查它的完整路径：父级展开链在节点 key 里
    // （scan_tree_nodes 用「root + "/" + name」做文件 key、dir: 用短名；这里简单化：
    // 展开一级目录 = 工作区根下的目录）
    const std::string dir_name(key.substr(4));
    const std::string full = wb.workspace_root + "/" + dir_name;
    wb.dir_expanded[full] = expanded;
    // 重建树：根 + 每个已展开目录的子项（缩进 +1）
    std::vector<st::ui::TreeNode> nodes;
    auto base = scan_tree_nodes(wb.workspace_root);
    for (auto& node : base) {
      nodes.push_back(node);
      if (node.key.rfind("dir:", 0) == 0) {
        const std::string name = std::string(node.key.substr(4));
        const std::string full_child = wb.workspace_root + "/" + name;
        const auto state = wb.dir_expanded.find(full_child);
        if (state != wb.dir_expanded.end() && state->second) {
          nodes.back().expanded = true;
          for (auto& child : scan_tree_nodes(full_child)) {
            child.depth += 1;
            // 子目录 key 加父前缀保持唯一
            if (child.key.rfind("dir:", 0) == 0) {
              child.key = "dir:" + name + "/" + std::string(child.key.substr(4));
            }
            nodes.push_back(child);
          }
        }
      }
    }
    wb.workspace_tree->sync_nodes(nodes);
    root->mark_dirty_all();
  };

  // —— 搜索：**真实递归**搜工作区文件（无工作区时回退到内置样例文本）——
  //
  // 旧实现只搜内嵌的几份样例字符串，与资源管理器“真实文件模式”自相矛盾；
  // 也无法回答“这个词到底在不在这份工作区里”。现在递归 `fs::list_dir` 读文件、
  // 逐行匹配，结果给「文件:行号 + 片段」，点一下**跳到该行并选中命中**。
  search_input_ptr->on_change = [search_list_ptr, &files, &wb, root](std::string_view query) {
    std::vector<List::Entry> entries;
    if (!query.empty()) {
      const auto add_hit = [&](const std::string& path, const std::string& label,
                               std::size_t line_no, std::size_t column,
                               std::size_t length) {
        List::Entry entry;
        entry.key = label;
        entry.label = label;
        // 命中即可跳转（真实文件走 fs 打开；样例按名字找）
        entry.on_activate = [&wb, root, path, line_no, column, length]() {
          OpenBuffer* buffer = nullptr;
          if (!wb.workspace_root.empty()) {
            buffer = open_file_buffer(wb.buffers, path);
          }
          st::ui::Tabs::Tab unused{};
          (void)unused;
          if (buffer != nullptr) {
            std::size_t index = 0;
            for (std::size_t i = 0; i < wb.buffers.size(); ++i) {
              if (&wb.buffers[i] == buffer) index = i;
            }
            std::vector<st::ui::Tabs::Tab> tab_bar;
            for (const auto& open : wb.buffers) {
              tab_bar.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
            }
            wb.tabs->sync_tabs(tab_bar);
            wb.tabs->set_active(index);
            wb.active = index;
            bind_editor(wb, *root);
            refresh_problems(wb, *root);
          }
          if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
            CodeEditor* editor = wb.buffers[wb.active].editor;
            // 跳行 → 选中命中词（两者都是命中后“我到底要看哪里”的一部分）
            editor->goto_line(line_no);
            const std::size_t start =
                editor->cursor_index() + column;
            editor->set_selection(start, start + length);
            editor->scroll_to_line(line_no);
            root->set_focus(editor);
            wb.status->set_content(
                std::format("已跳到 {}:{}", path, line_no));
          }
          root->mark_dirty_all();
        };
        entries.push_back(std::move(entry));
      };

      if (!wb.workspace_root.empty()) {
        // 递归扫描（跳过二进制/隐藏目录/超大文件）；限量避免大工作区把示例拖卡
        constexpr std::size_t kMaxHits = 200;
        constexpr std::size_t kMaxFileBytes = 1U << 20U;
        std::vector<std::pair<std::string, int>> stack{{wb.workspace_root, 0}};
        while (!stack.empty() && entries.size() < kMaxHits) {
          const auto [dir, depth] = stack.back();
          stack.pop_back();
          if (depth > 6) continue;
          auto listing = st::fs::list_dir(dir);
          if (!listing) continue;
          for (const auto& item : *listing) {
            if (entries.size() >= kMaxHits) break;
            const std::string full = dir + "/" + item.name;
            if (item.is_dir) {
              if (!item.name.empty() && item.name.front() == '.') continue;  // 隐藏目录
              stack.emplace_back(full, depth + 1);
              continue;
            }
            if (item.size > kMaxFileBytes) continue;  // 二进制/大文件不搜
            auto content = st::fs::read_text(full);
            if (!content) continue;                    // 非 UTF-8/读不了：跳过而不是硬猜
            std::size_t line_no = 0;
            std::size_t line_begin = 0;
            while (line_begin <= content->size() && entries.size() < kMaxHits) {
              const std::size_t eol = content->find('\n', line_begin);
              const std::size_t line_end =
                  eol == std::string::npos ? content->size() : eol;
              const std::string_view line(content->data() + line_begin, line_end - line_begin);
              ++line_no;
              const std::size_t column = line.find(query);
              if (column != std::string_view::npos) {
                const std::string leaf = item.name;
                add_hit(full,
                        std::format("{}:{}  {}", leaf, line_no,
                                    st::trim(line).substr(0, 60)),
                        line_no, column, query.size());
              }
              if (eol == std::string::npos) break;
              line_begin = eol + 1;
            }
          }
        }
        if (entries.empty()) {
          List::Entry no_match;
          no_match.key = "no-match";
          no_match.label = std::string("没有匹配：") + std::string(query);
          entries.push_back(std::move(no_match));
        }
      } else {
        // 内置样例回退（无 --workspace 时的原行为，保持示例自包含）
        for (const auto& sample : files) {
          std::size_t line_no = 0;
          std::size_t cursor = 0;
          while (cursor <= sample.code.size() && entries.size() < 200) {
            const std::size_t eol = sample.code.find('\n', cursor);
            const std::string_view line = std::string_view(sample.code).substr(
                cursor, eol == std::string::npos ? std::string_view::npos : eol - cursor);
            ++line_no;
            const std::size_t column = line.find(query);
            if (column != std::string_view::npos) {
              const std::string name = sample.name;
              List::Entry entry;
              entry.key = name + ":" + std::to_string(line_no);
              entry.label = std::format("{}:{}  {}", name, line_no,
                                        st::trim(line).substr(0, 60));
              entry.on_activate = [&files, &wb, root, name, line_no, column, query_len = query.size()]() {
                for (const auto& candidate : files) {
                  if (candidate.name != name) continue;
                  open_sample(wb, candidate, *root);
                  if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
                    CodeEditor* editor = wb.buffers[wb.active].editor;
                    editor->goto_line(line_no);
                    const std::size_t start = editor->cursor_index() + column;
                    editor->set_selection(start, start + query_len);
                    editor->scroll_to_line(line_no);
                    root->set_focus(editor);
                  }
                  wb.status->set_content(std::format("已跳到 {}:{}", name, line_no));
                  root->mark_dirty_all();
                  return;
                }
              };
              entries.push_back(std::move(entry));
            }
            if (eol == std::string::npos) break;
            cursor = eol + 1;
          }
        }
      }
    }
    search_list_ptr->sync_items(entries);
    wb.status->set_content(entries.empty()
                               ? std::string("搜索：输入关键词（递归搜工作区）")
                               : std::format("搜索命中 {} 处", entries.size()));
    root->mark_dirty_all();
  };
  // 终端：Enter 提交 → 回显命令与模拟输出
  wb.terminal_input->on_submit = [&wb, &files, root](std::string_view command) {
    std::string reply;
    if (command == "help") {
      reply =
          "可用命令：\n"
          "  help           本帮助\n"
          "  langs          已注册语言列表\n"
          "  ls             工作区根目录（真实文件模式）\n"
          "  find <词>      在工作区里搜（打到搜索面板 + 报告命中数）\n"
          "  goto <行>      当前编辑器跳到该行\n"
          "  stats          当前编辑器统计（行/字符/匹配数）\n"
          "  save / theme / clear / open <文件>";
    } else if (command == "langs") {
      std::string names;
      for (const auto& name : CodeEditor::available_languages()) {
        names += name + " ";
      }
      reply = "已注册语言：" + names;
    } else if (command == "ls") {
      if (wb.workspace_root.empty()) {
        reply = "当前为内置样例模式（启动时加 --workspace <目录> 可用真实文件）";
      } else if (auto listing = st::fs::list_dir(wb.workspace_root); listing.has_value()) {
        reply = "工作区 " + wb.workspace_root + "：";
        for (const auto& item : *listing) {
          reply += "\n  " + item.name + (item.is_dir ? "/" : std::format("  {} B", item.size));
        }
      } else {
        reply = "目录读不了：" + wb.workspace_root;
      }
    } else if (command.starts_with("goto ")) {
      try {
        const auto line = static_cast<std::size_t>(std::stoull(std::string(command.substr(5))));
        if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
          wb.buffers[wb.active].editor->goto_line(line);
          wb.buffers[wb.active].editor->scroll_to_line(line);
          reply = std::format("已跳到第 {} 行", line);
        } else {
          reply = "没有打开的编辑器";
        }
      } catch (...) {
        reply = "用法：goto <行号>";
      }
    } else if (command.starts_with("find ")) {
      const std::string needle(command.substr(5));
      // 复用搜索面板的真实路径：填入关键词 → on_change 跑递归搜索
      if (auto* search = root->find("search-input"); search != nullptr) {
        (void)search->set_property("value", needle);
        reply = std::format("已在工作区搜索「{}」，结果见搜索面板", needle);
        select_activity(wb, 1);  // 切到搜索面板让结果可见
      } else {
        reply = "搜索面板不可用";
      }
    } else if (command == "stats") {
      if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
        CodeEditor* editor = wb.buffers[wb.active].editor;
        reply = std::format("{} · {} 行 · {} 字符", wb.buffers[wb.active].label,
                            editor->line_count(), st::utf8_length(editor->text()));
      } else {
        reply = "没有打开的编辑器";
      }
    } else if (command == "save") {
      save_active(wb, *root);
      reply = "已执行保存（见输出面板）";
    } else if (command == "theme") {
      wb.theme_button->activate();
      reply = "已切换主题";
    } else if (command == "clear") {
      wb.terminal_output->set_content("shuangtian dev terminal");
      wb.terminal_input->set_text("");
      root->mark_dirty_all();
      return;
    } else if (command.starts_with("open ")) {
      const std::string want(command.substr(5));
      bool hit = false;
      for (const auto& sample : files) {
        if (sample.name == want) {
          open_sample(wb, sample, *root);
          reply = "已打开 " + want;
          hit = true;
          break;
        }
      }
      if (!hit) reply = "没有找到文件：" + want;
    } else if (!command.empty()) {
      reply = "未知命令：" + std::string(command) + "（help 查看）";
    }
    if (!command.empty()) {
      wb.terminal_output->set_content(wb.terminal_output->content() + "\n❯ " +
                                      std::string(command) + "\n" + reply);
    }
    wb.terminal_input->set_text("");
    root->mark_dirty_all();
  };

  // —— 命令面板条目 ——
  //
  // 迁移到**框架内置** `ui::CommandPalette`（DESIGN §8.1.1 反推的框架缺口已落地）：
  // 此前示例自带一份私有实现（遮罩 + 卡片 + 过滤列表 + 高亮导航共 130 行），
  // 与框架组件能力重叠——示例的职责是展示组件库，不是复制它。
  // 受益面：大小写不敏感过滤、属性/动作面（`query`/`select`）都可被控制通道驱动。
  std::vector<st::ui::CommandPalette::Command> commands;
  const auto add_command = [&commands](std::string id, std::string title, std::string detail,
                                       std::function<void()> run) {
    st::ui::CommandPalette::Command item;
    item.id = std::move(id);
    item.title = std::move(title);
    item.detail = std::move(detail);
    item.handler = std::move(run);
    commands.push_back(std::move(item));
  };
  add_command("file.save-all", "文件: 全部保存", "把所有打开的编辑器标记为已保存",
              [&wb, root]() { save_active(wb, *root); });
  add_command("file.close-tab", "文件: 关闭当前编辑器", "Ctrl+W",
              [&wb, root]() { close_tab(wb, *root); });
  add_command("view.toggle-sidebar", "查看: 切换侧栏可见性", "Ctrl+B", []() {});
  add_command("view.toggle-theme", "查看: 切换亮/暗主题", "主题令牌整体切换",
              [&wb]() { wb.theme_button->activate(); });
  add_command("view.toggle-indent-guides", "查看: 切换缩进参考线",
              "每级缩进一条竖线（对齐大段代码的层次）", [&wb, root]() {
                if (wb.active >= wb.buffers.size() || wb.buffers[wb.active].editor == nullptr) return;
                CodeEditor* editor = wb.buffers[wb.active].editor;
                editor->set_indent_guides(!editor->indent_guides());
                wb.status->set_content(editor->indent_guides() ? "缩进参考线：开" : "缩进参考线：关");
                root->mark_dirty_all();
              });
  add_command("help.about", "帮助: 关于", "codeeditor · 霜天示例", [&wb, root]() {
    wb.status->set_content("codeeditor 0.1.0 · 霜天框架示例 · 全内置组件组装");
    root->mark_dirty_all();
  });
  for (const auto& sample : files) {
    add_command("file.open." + sample.name, "文件: 打开 " + sample.name, "语言 " + sample.language,
                [&wb, &sample, root]() { open_sample(wb, sample, *root); });
  }
  add_command("lang.stlog", "语言: 打开 stlog 样例", "自定义语言（运行时注册）", [&wb, root]() {
    Sample stlog_sample{"stlog.log", "stlog", kStlogSample, "logs/stlog.log"};
    open_sample(wb, stlog_sample, *root);
  });

  // 全量命令表快照：Ctrl+P 会把面板表换成文件表，Ctrl+Shift+P / 菜单再换回来
  const std::vector<st::ui::CommandPalette::Command> all_commands = commands;

  // 浮层只造一次，反复开关只翻可见性——避免每次「关→摘除→再建」
  // （旧实现每次重建整棵子树，控制通道拿到的元素 id 会跟着漂）。
  auto palette = std::make_unique<st::ui::CommandPalette>(commands);
  auto* palette_ptr = palette.get();
  palette_ptr->set_id("command-palette");
  palette_ptr->on_command = [&wb, root](std::string_view id) {
    if (auto* panel = root->find("command-palette"); panel != nullptr) {
      panel->set_visible(false);
    }
    // 状态文案只做“兜底”与收尾：命令 handler 自己写过的更具体的信息（如“已打开 X”）
    // 不要被盖掉——否则用户看到的是内部命令 id（对读者毫无意义）。
    const std::string marker = "已执行命令：";
    const bool handler_wrote = wb.status->content().rfind(marker, 0) != 0 &&
                               !wb.status->content().empty();
    if (!handler_wrote) {
      // 打开文件类命令的 id 就是绝对路径：展示文件名比展示 id 有意义
      const std::size_t slash = id.find_last_of("/\\");
      const std::string leaf(slash == std::string_view::npos ? id : id.substr(slash + 1));
      wb.status->set_content(
          id.find('/') != std::string_view::npos ? "已打开 " + leaf : "已执行：" + leaf);
    }
    if (auto* editor = wb.active < wb.buffers.size() ? wb.buffers[wb.active].editor : nullptr;
        editor != nullptr) {
      root->set_focus(editor);
    }
    root->mark_dirty_all();
  };
  palette_ptr->on_close = [root]() {
    if (auto* panel = root->find("command-palette"); panel != nullptr) panel->set_visible(false);
    root->mark_dirty_all();
  };
  root->add_overlay(std::move(palette), st::ui::UiRoot::OverlayLayout::FillViewport);
  palette_ptr->set_visible(false);

  // 打开命令面板：先把命令表恢复为**全量命令**（Ctrl+P 可能刚把它换成了文件表），
  // 再预填过滤词（如菜单「新建/打开」走 `文件: 打开 `）。
  const auto open_palette = [palette_ptr, root, &all_commands](std::string query = {}) {
    palette_ptr->set_commands(all_commands);
    palette_ptr->set_visible(true);
    palette_ptr->set_query(std::move(query));
    // 键盘直达过滤框。不调这句的话焦点还在底层编辑器上：
    // 面板开着、字却打进了右边的代码里（实测踩到的“面板打不了字”）。
    palette_ptr->grab_focus();
    root->mark_dirty_all();
  };

  // 菜单动作
  menu_bar_ptr->on_action = [&wb, &open_palette, root, &app](const std::string& menu,
                                                             const std::string& item) {
    if (menu == "file" && item == "save") save_active(wb, *root);
    else if (menu == "file" && item == "close-tab") close_tab(wb, *root);
    else if (menu == "file" && (item == "new" || item == "open")) open_palette("文件: 打开 ");
    else if (menu == "view" && item == "command-palette") open_palette();
    else if (menu == "view" && item == "toggle-theme") wb.theme_button->activate();
    else if (menu == "help" && item == "about") {
      wb.status->set_content("codeeditor 0.1.0 · 霜天框架示例 · 全内置组件组装");
    }
    // 编辑/选择/运行菜单直接作用在当前编辑器
    else if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
      CodeEditor* editor = wb.buffers[wb.active].editor;
      if (menu == "edit" && item == "undo") editor->undo();
      else if (menu == "edit" && item == "redo") editor->redo();
      else if (menu == "edit" && item == "comment") editor->toggle_comment();
      else if (menu == "edit" || menu == "selection") {
        if (item == "select-all") editor->select_all();
        else if (item == "copy") (void)editor->selected_text();
        else if (item == "paste") editor->insert_text("（剪贴板内容）");
        else if (item == "goto-line") editor->goto_line(1);
      } else if (menu == "run" && item == "run-task") {
        wb.output_text->set_content(wb.output_text->content() + "\n[任务] st build gallery --profile dev（模拟输出）");
        wb.status->set_content("任务已提交（模拟）");
      } else if (menu == "run" && item == "run-test") {
        wb.output_text->set_content(wb.output_text->content() + "\n[任务] st test（模拟输出）");
        wb.status->set_content("任务已提交（模拟）");
      }
    }
    root->mark_dirty_all();
  };
  menu_bar_ptr->on_open_menu = [menu_bar_ptr, root](std::size_t index) {
    // 先尽掉旧面板再挂新的——悬停切换标题 / 键盘 ←→ 都走这里；
    // 不清的话每切一次就多叠一张面板（实测：菜单选完后浮层不消失/叠成两层）。
    for (std::size_t i = root->overlay_count(); i > 0; --i) {
      st::ui::Element* overlay = root->overlay_at(i - 1);
      if (overlay != nullptr && overlay->type() == "MenuPanel") root->remove_overlay(overlay);
    }
    if (auto panel = menu_bar_ptr->make_panel(index); panel != nullptr) {
      root->add_overlay(std::move(panel), st::ui::UiRoot::OverlayLayout::FillViewport);
    }
    root->mark_dirty_all();
  };
  menu_bar_ptr->on_menu_close = [root]() {
    // 面板内部已把 open_index 清干净；这里只需要把 overlay 摘掉（否则留在屏上）。
    for (std::size_t i = root->overlay_count(); i > 0; --i) {
      st::ui::Element* overlay = root->overlay_at(i - 1);
      if (overlay != nullptr && overlay->type() == "MenuPanel") root->remove_overlay(overlay);
    }
    root->mark_dirty_all();
  };
  (void)empty_hint_ptr;
  empty_hint_ptr->set_visible(false);  // 空态提示默认隐藏（有初始文件；无标签时显示）

  // 全局快捷键（先于焦点链——文本编辑器吞键也拦得住）
  st::ui::UiRoot::Shortcut ctrl{};
  ctrl.ctrl = true;
  (void)root->register_shortcut("s", ctrl, [&wb, root]() {
    save_active(wb, *root);
    return true;
  });
  (void)root->register_shortcut("w", ctrl, [&wb, root]() {
    close_tab(wb, *root);
    return true;
  });
  // Ctrl+Tab 切标签：直接调切换逻辑（与 on_change 同路径）
  const auto switch_tab = [&wb, root](std::size_t index) {
    if (index >= wb.buffers.size()) return;
    if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
      wb.buffers[wb.active].text = wb.buffers[wb.active].editor->text();
    }
    wb.active = index;
    wb.tabs->set_active(index);
    bind_editor(wb, *root);
    refresh_problems(wb, *root);
    wb.title_text->set_content(window_title(wb));
    root->set_focus(wb.buffers[index].editor);
    root->mark_dirty_all();
  };
  (void)root->register_shortcut("tab", ctrl, [&wb, &switch_tab]() {
    if (wb.buffers.empty()) return false;
    switch_tab((wb.active + 1) % wb.buffers.size());
    return true;
  });
  st::ui::UiRoot::Shortcut ctrl_shift_p{}; ctrl_shift_p.ctrl = true; ctrl_shift_p.shift = true;
  (void)root->register_shortcut("p", ctrl_shift_p,
                          [&open_palette]() {
                            open_palette();
                            return true;
                          });
  // Ctrl+P 快速打开文件（VSCode 同款）：把面板的命令表临时换成**文件表**——
  // 与 Ctrl+Shift+P 有实质区别（后者是全部命令），也顺带验证了
  // `CommandPalette::set_commands` 的整表替换能力。
  auto files_for_quick_open = std::vector<st::ui::CommandPalette::Command>{};
  // 无工作区时快速打开列内置样例；有工作区时列真实文件（递归、限量）。
  if (!wb.workspace_root.empty()) {
    std::vector<std::pair<std::string, int>> stack{{wb.workspace_root, 0}};
    while (!stack.empty() && files_for_quick_open.size() < 300) {
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
        st::ui::CommandPalette::Command entry;
        entry.id = full;
        entry.title = item.name;
        // detail 参与过滤：打路径片段也能搜到（如 “ui/components”）
        entry.detail = full.substr(wb.workspace_root.size() + 1);
        entry.handler = [&wb, root, full]() {
          if (auto* buffer = open_file_buffer(wb.buffers, full); buffer != nullptr) {
            std::size_t index = 0;
            for (std::size_t i = 0; i < wb.buffers.size(); ++i) {
              if (&wb.buffers[i] == buffer) index = i;
            }
            std::vector<st::ui::Tabs::Tab> tab_bar;
            for (const auto& open : wb.buffers) {
              tab_bar.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
            }
            wb.tabs->sync_tabs(tab_bar);
            wb.tabs->set_active(index);
            wb.active = index;
            bind_editor(wb, *root);
            refresh_problems(wb, *root);
            wb.title_text->set_content(window_title(wb));
            wb.status->set_content("已打开 " + buffer->path);
            root->set_focus(buffer->editor);
          } else {
            wb.status->set_content("打开失败：" + full);
          }
          root->mark_dirty_all();
        };
        files_for_quick_open.push_back(std::move(entry));
      }
    }
  } else {
    for (const auto& sample : files) {
      st::ui::CommandPalette::Command entry;
      entry.id = sample.name;
      entry.title = sample.name;
      entry.detail = "语言 " + sample.language;
      entry.handler = [&wb, &sample, root]() { open_sample(wb, sample, *root); };
      files_for_quick_open.push_back(std::move(entry));
    }
  }

  (void)root->register_shortcut("p", ctrl,
                                [palette_ptr, root, &all_commands, &files_for_quick_open]() {
                                  palette_ptr->set_commands(files_for_quick_open);
                                  palette_ptr->set_visible(true);
                                  palette_ptr->set_query({});
                                  palette_ptr->grab_focus();  // 同命令面板：直接开始打字过滤
                                  root->mark_dirty_all();
                                  return true;
                                });
  (void)all_commands;
  (void)root->register_shortcut("f", ctrl, [root, &open_find_bar]() {
    open_find_bar();
    return true;
  });
  st::ui::UiRoot::Shortcut ctrl_h{}; ctrl_h.ctrl = true;
  (void)root->register_shortcut("h", ctrl_h, [&open_find_bar]() {
    open_find_bar();
    return true;
  });
  (void)root->register_shortcut("b", ctrl, [root]() {
    // 折叠/展开侧栏：侧栏与活动栏一起切（VSCode 的 Ctrl+B 只藏侧栏，这里联动活动栏演示）
    Element* sidebar_element = root->find("sidebar");
    if (sidebar_element == nullptr) return false;
    const bool show = !sidebar_element->visible();
    sidebar_element->set_visible(show);
    if (auto* activity = root->find("activitybar"); activity != nullptr) {
      activity->set_visible(show);
    }
    root->mark_dirty_all();
    return true;
  });
  // ════════════════ 启动 ════════════════
  app.set_content(std::move(page));
  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败: %s\n", started.error().to_string().c_str());
    return 1;
  }

  // 回填运行时信息
  wb.output_text->set_content(std::format(
      "[启动] codeeditor 0.1.0 · 后端 {} · headless={} · DPI {:.1f} · 控制通道 127.0.0.1:{}",
      app.backend_name(), app.headless(), static_cast<double>(app.device_scale()),
      app.control_port()));

  // 初始状态：活动栏选中"资源管理器"，打开初始文件（--language 指定，默认 cpp）
  select_activity(wb, 0);
  const Sample* initial = &files.front();
  for (const auto& sample : files) {
    if (sample.language == options.language) {
      initial = &sample;
      break;
    }
  }
  open_sample(wb, *initial, *root);

  // 脚本逻辑层（仅在 `--enable-script` 时可用）：**组件控制逻辑用 JS 写**。
  // 这份 JS 是编译期嵌入的真实资源（`assets/logic.js`）——与 C++ 里那堆 raw string 不同，
  // 它在编辑器里有高亮、不需要转义，改完重新构建即生效。
  if (auto* script = app.script(); script != nullptr) {
    const auto logic = b::embed<"examples/codeeditor/assets/logic.js">();
    const std::string_view source(logic.data(), logic.length());
    if (auto loaded = script->eval(source, "assets/logic.js"); !loaded) {
      st::print("示例 JS 逻辑载入失败: {}\n", loaded.error().message);
    } else {
      st::print("示例 JS 逻辑已载入（{} 字节）\n", logic.length());
    }
  }

  root->set_focus(wb.buffers.empty() ? nullptr : wb.buffers[wb.active].editor);
  root->mark_dirty_all();
  app.render_frame();

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    // 节拍交给框架：有活 → 帧预算；空闲 → 4ms（控制通道响应节拍）。
    // 旧实现固定 `sleep_for(16ms)`：命令延迟被拉到 16~31ms（实测 ping p50=31ms）。
    app.pace_loop(frame_start_ms);
  }
  st::print("codeeditor 退出：{} 帧，标签 {} 个，可用语言 {} 种\n", frames, wb.buffers.size(),
            CodeEditor::available_languages().size());
  return 0;
}

}  // namespace

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
