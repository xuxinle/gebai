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
/// 已在本次整合中被它替换；`examples/codeeditor-dsl`（声明式重写版）同时并入，不再有分身。
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
/// `#terminal-input` / `#terminal-output`（终端）、`#command-palette`。
///
/// 坐标系：全部逻辑像素；文本索引为 UTF-8 字节偏移且落在码点边界。

#include "battery/embed.hpp"  // 编译期资源嵌入（示例 JS 逻辑层）

#include <algorithm>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/app.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/text/highlight.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/icon.hpp"

namespace {

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
//   bottom_ / menu_open_   底部面板与菜单栏的选中项
//   palette_* / find_*     命令面板与查找条的开关、过滤词、计数文案
//   status_/output_/terminal_*  各处的文本
//
// `build()` 只描述「这些状态对应的界面形态」——所有「改完要点哪里」的同步都没了。

struct CodeEditorPage : Component {
 public:
  CodeEditorPage(std::vector<Sample> files, std::string workspace)
      : files_(std::move(files)), workspace_(std::move(workspace)) {}

  // —— 状态 ——
  State<std::vector<OpenBuffer>> buffers_{std::vector<OpenBuffer>{}};
  State<std::size_t> active_{0};
  State<bool> sidebar_visible_{true};
  State<std::size_t> activity_{0};        ///< 活动栏选中项（0=资源管理器 … 4=扩展）
  State<std::size_t> bottom_{2};          ///< 底部面板（默认终端：VSCode 用户最常用落点）
  State<std::string> status_{"就绪"};
  State<std::string> cursor_text_{"Ln 1, Col 1"};
  State<std::string> selection_text_{""};
  State<std::string> output_{"[启动] codeeditor · 声明式构建"};
  State<std::vector<std::string>> terminal_log_{
      std::vector<std::string>{"shuangtian dev terminal", "> 输入 help 查看可用命令，Enter 提交"}};
  State<bool> dark_{false};
  State<std::size_t> menu_open_{kNoMenu};
  State<bool> palette_open_{false};
  State<std::string> palette_query_{""};
  State<bool> palette_shows_files_{false};   ///< Ctrl+P（文件表）vs Ctrl+Shift+P（命令表）
  State<bool> find_open_{false};
  State<std::string> find_counter_{"0/0"};
  State<std::vector<std::string>> dir_expanded_{std::vector<std::string>{}};
  State<std::vector<std::string>> search_hits_{std::vector<std::string>{}};

  static constexpr std::size_t kNoMenu{static_cast<std::size_t>(-1)};

  // —— 非状态引用（逃生舱的强类型句柄；每次 build 重新取得）——
  CodeEditor* editor{nullptr};
  MenuBar* menu_bar_ptr{nullptr};
  CommandPalette* palette_ptr{nullptr};
  Input* find_replace_input{nullptr};   ///< 替换文本不进状态（不参与重组）
  Input* terminal_input{nullptr};

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

  auto close_active() -> void {
    const auto list = buffers_.value();
    if (list.empty()) return;
    close(list[active_.value()].key);
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
    output_.set(output_.value() + "\n[保存] " + buffer.label + " · " +
                std::to_string(st::utf8_length(buffer.text)) + " 字符");
    buffers_.set(std::move(list));
  }

  auto toggle_theme() -> void {
    const bool next = !dark_.value();
    dark_.set(next);
    status_.set(next ? "主题 dark · 视觉令牌已切换" : "主题 light · 视觉令牌已切换");
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
    if (active_.value() >= list.size()) return "codeeditor";
    const OpenBuffer& buffer = list[active_.value()];
    return (buffer.dirty ? "● " : "") + buffer.label + " - codeeditor";
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
  }

  // ══════════════════════════════════════════════════════════════════════════
  // build()：只描述形态
  // ══════════════════════════════════════════════════════════════════════════

  void build(Composer& c) override {
    const auto& buffers = buffers_.value();
    const std::size_t active = active_.value();
    const bool has_editor = !buffers.empty();

    // 编辑器实例可能在本次重组中新建/换绑：先置空，由 build_editor_area 重新取得
    editor = nullptr;
    column(c, {.gap = 0.0f, .id = "editor-page"}, [&] {
      build_title_bar(c);
      build_menu(c);
      build_main(c, buffers, active, has_editor);
      build_bottom(c);
      build_status_bar(c);
      build_find_bar(c);
    });
    // 命令面板：**条件声明**（关掉 = 本帧不声明 → 框架 sweep 摘除）
    if (palette_open_.value()) build_palette(c);
    // 文本灌入：只在本帧的编辑器实例与「已装载的标签」不一致时写
    // （`set_text` 会清撤销栈并把光标归零；每次重组都写会让打字被重置——实测踩到）
    if (editor != nullptr && active < buffers.size() && buffers[active].key != loaded_key_) {
      editor->set_language(buffers[active].language);
      editor->set_text(buffers[active].text);
      loaded_key_ = buffers[active].key;
    }
  }

  // —— 1. 标题栏 ——
  void build_title_bar(Composer& c) {
    row(c, {.gap = 8.0f, .padding = 12.0f, .height = 36.0f, .id = "titlebar"}, [&] {
      (void)icon(c, "code", 16.0f);
      (void)text(c, [this] { return window_title(); }, {.id = "title-text"});
      // 弹性空隙：把窗口控制推到**右侧**（`spacer()` 默认就是弹性的）。
      // 注：早先这里写成 `spacer(c, 0.0f)` 且当时它=固定 0 宽 → 右对齐静默失效
      // （三个图标跟在标题后面，实测 x=209 而非 1268）。现在 `size<=0` = `grow=true`。
      (void)spacer(c);
      for (const char* glyph : {"minus", "square", "close"}) (void)icon(c, glyph, 14.0f);
    });
  }

  // —— 2. 菜单栏（下拉面板经 overlay：`menu_panel_overlay` 是声明式入口）——
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
          {.id = "toggle-theme", .label = "切换亮/暗主题"}}},
        {"run", "运行",
         {{.id = "run-task", .label = "运行任务：构建 gallery"},
          {.id = "run-test", .label = "运行任务：st test"},
          {.separator = true},
          {.id = "toggle-terminal", .label = "切换终端（底部面板）"}}},
        {"help", "帮助", {{.id = "about", .label = "关于 codeeditor"}}}};
    menu_bar_ptr = dsl::menu_bar(
        c, kMenus,
        [this](const std::string& menu, const std::string& item) { on_menu(menu, item); },
        [this](std::size_t index) {
          menu_open_.set(menu_open_.value() == index ? kNoMenu : index);
        },
        {.id = "menubar"});
    // 打开状态 → 下一帧声明面板 overlay（不声明 = 自动消失，框架 sweep）
    const std::size_t open = menu_open_.value();
    if (open != kNoMenu && menu_bar_ptr != nullptr) dsl::menu_panel_overlay(c, *menu_bar_ptr, open);
  }

  void on_menu(const std::string& menu, const std::string& item) {
    menu_open_.set(kNoMenu);   // 选完即关
    if (menu == "file" && item == "save") {
      save();
    } else if (menu == "file" && item == "close-tab") {
      close_active();
    } else if (menu == "file" && (item == "new" || item == "open")) {
      open_palette(true);
    } else if (menu == "view" && item == "command-palette") {
      open_palette(false);
    } else if (menu == "view" && item == "toggle-sidebar") {
      sidebar_visible_.set(!sidebar_visible_.value());
    } else if (menu == "view" && item == "find") {
      open_find();
    } else if (menu == "view" && item == "toggle-theme") {
      toggle_theme();
    } else if (menu == "run" && item == "toggle-terminal") {
      bottom_.set(2);
    } else if (menu == "help" && item == "about") {
      status_.set("codeeditor 0.1.0 · 霜天框架示例 · 声明式组装");
    } else {
      apply_editor_menu(menu, item);
    }
  }

  /// 编辑/选择/运行菜单：直接作用于当前编辑器（一次性命令，不涉及界面形态）。
  void apply_editor_menu(const std::string& menu, const std::string& item) {
    if (menu == "run") {
      output_.set(output_.value() +
                  std::format("\n[任务] {}（模拟输出）",
                              item == "run-task" ? "st build gallery --profile dev" : "st test"));
      status_.set("任务已提交（模拟）");
      return;
    }
    if (editor == nullptr) return;
    if (menu == "edit" && item == "undo") editor->undo();
    else if (menu == "edit" && item == "redo") editor->redo();
    else if (menu == "edit" && item == "comment") editor->toggle_comment();
    else if (item == "select-all") editor->select_all();
    else if (item == "copy") (void)editor->selected_text();
    else if (item == "paste") editor->insert_text("（剪贴板内容）");
    else if (item == "goto-line") editor->goto_line(1);
  }

  // —— 3. 主体三栏 ——
  void build_main(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                  bool has_editor) {
    row(c, {.grow = true, .id = "main-row"}, [&] {
      if (!sidebar_visible_.value()) {
        build_editor_area(c, buffers, active, has_editor);
        return;
      }
      build_activity_bar(c);
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
    column(c, {.gap = 2.0f, .width = 44.0f, .id = "activity-bar"}, [&] {
      for (std::size_t index = 0; index < std::size(kActivities); ++index) {
        // 图标按钮：`custom<Button>`（Button 的 icon 是一等接口，属性面没有）
        const std::string id = std::string("activity-") + kActivities[index].id;
        const bool on = index == current;
        (void)custom<Button>(c, [&, index, on](Button& b) {
          b.set_id(id);
          b.set_icon(kActivities[index].icon);
          b.set_variant(on ? Button::Variant::Soft : Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this, index] { activity_.set(index); };
        }, {.width = 40.0f, .height = 40.0f, .key = kActivities[index].id});
      }
    });
  }

  /// 侧栏内容（按活动栏选中项切换）。
  void build_sidebar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    (void)buffers;
    (void)active;
    column(c, {.gap = 8.0f, .padding = 12.0f, .grow = true, .id = "sidebar-body"}, [&] {
      const std::size_t which = activity_.value();
      if (which == 0) {
        (void)text(c, [] { return std::string("资源管理器"); });
        if (!workspace_.empty()) {
          // 真实工作区：树（懒展开——点目录才扫它的子层）
          (void)tree(c, workspace_nodes(),
                     [this](const std::string& key, bool expanded) {
                       on_tree_toggle(key, expanded);
                     },
                     [this](const std::string& key) {
                       if (key.rfind("dir:", 0) != 0) open_path(key);
                     },
                     {.grow = true, .id = "workspace-tree"});
        } else {
          for (const auto& sample : files_) {
            (void)custom<Button>(c, [this, &sample](Button& b) {
              b.set_label(sample.tree_key);
              b.set_variant(Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              b.on_click = [this, name = sample.name] { open_by_name(name); };
            }, {.key = sample.tree_key});
          }
        }
      } else if (which == 1) {
        (void)text(c, [] { return std::string("搜索"); });
        (void)custom<Input>(c, [this](Input& field) {
          field.set_id("search-input");
          field.set_placeholder("搜索（工作区文本）");
          field.set_icon_prefix("search");
          field.on_change = [this](std::string_view query) { run_search(std::string(query)); };
        }, {.key = "search-input"});
        std::vector<ListItemData> hit_items;
        for (std::size_t index = 0; index < search_hits_.value().size(); ++index) {
          hit_items.push_back(ListItemData{.key = std::format("hit:{}", index),
                                           .label = search_hits_.value()[index]});
        }
        (void)list(c, hit_items, [this](std::size_t index) { activate_search_hit(index); },
                   {.grow = true, .id = "search-results"});
      } else if (which == 2) {
        (void)text(c, [] { return std::string("源代码管理\n分支 main\n变更 0 · 暂存 0"); });
      } else if (which == 3) {
        (void)text(c, [] {
          return std::string("运行和调试\n· 构建 gallery\n· st test\n· st lint");
        });
      } else {
        (void)text(c, [] { return std::string("扩展\n· stlog 高亮（内置规则）\n· 无其他扩展"); });
      }
    });
  }

  void build_editor_area(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                         bool has_editor) {
    column(c, {.gap = 0.0f, .grow = true, .id = "editor-area"}, [&] {
      std::vector<TabData> tabs;
      for (const auto& buffer : buffers) {
        tabs.push_back(TabData{.key = buffer.key, .label = buffer.label,
                               .modified = buffer.dirty, .closable = true});
      }
      (void)dsl::tabs(c, tabs, active, [this](std::size_t index) { switch_tab(index); },
                      [this](const std::string& key) { close(key); }, {.id = "editor-tabs"});

      if (!has_editor) {
        column(c, {.padding = 24.0f, .grow = true, .id = "empty-hint"}, [&] {
          (void)icon(c, "code", 40.0f);
          (void)text(c, [] {
            return std::string("从左侧资源管理器打开一个文件，或 Ctrl+Shift+P 打开命令面板");
          });
        });
        return;
      }
      const std::size_t index = active < buffers.size() ? active : 0;
      const auto& buffer = buffers[index];
      // 编辑器：custom<T> 逃生舱（CodeEditor 的一等接口属性面覆盖不到）。
      // 只写元数据（字体/行宽/只读）——**文本由 build() 末尾按「装载的标签」灌入**：
      // `set_text` 会清撤销栈并把光标归零，每次重组都写会让打字被重置（实测踩到）。
      (void)custom<CodeEditor>(c, [this, &buffer](CodeEditor& ed) {
        ed.set_id("editor");   // 控制通道钩子：tools/*.py 依赖
        ed.set_font_size(13.5f);
        ed.set_tab_width(4);
        ed.set_language(buffer.language);
        ed.set_read_only(false);
        ed.style().grow = true;
        ed.style().padding = st::math::Insets{8.0f, 4.0f, 8.0f, 4.0f};
        ed.on_change = [this](std::string_view) { on_edit(); };
        ed.on_cursor_change = [this] { on_cursor_moved(); };
        editor = &ed;
      }, {.grow = true, .id = "editor-host"});
    });
  }

  // —— 4. 底部面板（问题 / 输出 / 终端）——
  void build_bottom(Composer& c) {
    column(c, {.height = 170.0f, .id = "bottom-panel"}, [&] {
      const std::vector<TabData> labels{
          {.key = "problems", .label = "问题", .closable = false},
          {.key = "output", .label = "输出", .closable = false},
          {.key = "terminal", .label = "终端", .closable = false}};
      (void)dsl::tabs(c, labels, bottom_.value(),
                      [this](std::size_t index) { bottom_.set(index); }, {}, {.id = "bottom-tabs"});
      switch (bottom_.value()) {
        case 0: build_problems_panel(c); break;
        case 1: build_output_panel(c); break;
        default: build_terminal_panel(c); break;
      }
    });
  }

  /// 问题面板：真实轻量检查的报告（点条目跳到那一行）。
  void build_problems_panel(Composer& c) {
    column(c, {.gap = 6.0f, .padding = 8.0f, .grow = true, .id = "panel-problems"}, [&] {
      (void)row(c, {.gap = 16.0f}, [&] {
        (void)icon(c, "error", 14.0f);
        (void)text(c, [this] { return std::to_string(error_count()) + " 个错误"; });
        (void)icon(c, "warning", 14.0f);
        (void)text(c, [this] { return std::to_string(warning_count()) + " 个警告"; });
      });
      std::vector<ListItemData> items;
      for (const auto& problem : problems_) {
        items.push_back(ListItemData{
            .key = std::format("problem:{}", problem.line),
            .label = std::format("{}  第 {} 行  {}", problem.warning ? "警告" : "错误",
                                 problem.line, problem.message)});
      }
      if (items.empty()) {
        items.push_back(ListItemData{.key = "no-problems", .label = "工作区干净，没有发现问题"});
      }
      (void)list(c, items, [this](std::size_t index) { jump_to_problem(index); },
                 {.grow = true, .id = "problems-list"});
    });
  }

  void build_output_panel(Composer& c) {
    (void)custom_container<ScrollView>(
        c, [&] { (void)text(c, [this] { return output_.value(); },
                            {.padding = 8.0f, .grow = true, .id = "output-text"}); },
        [](ScrollView& scroll) { scroll.set_id("output-scroll"); },
        {.grow = true, .id = "panel-output"});
  }

  /// 终端面板：输出行 + 输入行（Enter 提交 → 回显）。
  void build_terminal_panel(Composer& c) {
    column(c, {.gap = 6.0f, .padding = 8.0f, .grow = true, .id = "panel-terminal"}, [&] {
      std::string joined;
      for (const auto& line : terminal_log_.value()) joined += line + "\n";
      (void)custom_container<ScrollView>(
          c, [&] { (void)text(c, [joined] { return joined; }, {.id = "terminal-output"}); },
          [](ScrollView& scroll) { scroll.set_id("terminal-scroll"); }, {.grow = true});
      (void)row(c, {.gap = 6.0f}, [&] {
        (void)text(c, [] { return std::string("❯"); });
        (void)custom<Input>(c, [this](Input& field) {
          field.set_id("terminal-input");
          field.set_placeholder("help");
          field.on_submit = [this](std::string_view command) {
            run_terminal(std::string(command));
          };
          terminal_input = &field;
        }, {.key = "terminal-input"});
      });
    });
  }

  // —— 5. 状态栏（兼容钩子 id 全保留：`status` / `btn-theme` / 计数 / 光标 / 语言）——
  void build_status_bar(Composer& c) {
    row(c, {.gap = 12.0f, .padding = 10.0f, .height = 26.0f, .id = "statusbar"}, [&] {
      (void)icon(c, "git-branch", 12.0f);
      (void)text(c, [] { return std::string("main*"); }, {.id = "branch-label"});
      (void)icon(c, "error", 12.0f);
      (void)text(c, [this] { return std::to_string(error_count()); }, {.id = "status-errors"});
      (void)icon(c, "warning", 12.0f);
      (void)text(c, [this] { return std::to_string(warning_count()); }, {.id = "status-warnings"});
      (void)spacer(c);   // 弹性空隙：右侧信息组贴右缘（同标题栏）
      (void)text(c, [this] { return cursor_text_.value(); }, {.id = "cursor-label"});
      (void)text(c, [this] { return selection_text_.value(); }, {.id = "selection-label"});
      (void)text(c, [] { return std::string("·  空格: 4  ·  UTF-8"); });
      (void)text(c, [this] { return active_language(); }, {.id = "language-label"});
      (void)text(c, [this] { return status_.value(); }, {.id = "status"});
      (void)button(c, dark_.value() ? "亮色" : "暗色", [this] { toggle_theme(); },
                   {.id = "btn-theme"});
    });
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
            field.on_change = [this](std::string_view value) {
              if (editor == nullptr) return;
              editor->set_find(std::string(value));
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
          (void)button(c, "×", [this] { close_find(); }, {.id = "find-close"});
        });
      });
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
        // 面板显示后必须把焦点交给过滤框——不调的话敲的字会跑进底层编辑器里（实测踩到）
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
  /// 关闭查找条（入口的 Esc 快捷键用；与面板上的「×」同一条路径）。
  auto dismiss_find() -> void { close_find(); }

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
  void run_search(const std::string& query) {
    search_paths_.clear();
    search_lines_.clear();
    search_columns_.clear();
    std::vector<std::string> hits;
    constexpr std::size_t kMaxHits = 200;
    constexpr std::size_t kMaxFileBytes = 1U << 20U;

    const auto add_hit = [&](const std::string& path, const std::string& label,
                             std::size_t line, std::size_t column) {
      hits.push_back(label);
      search_paths_.push_back(path);
      search_lines_.push_back(line);
      search_columns_.push_back(column);
    };

    if (!query.empty() && !workspace_.empty()) {
      std::vector<std::pair<std::string, int>> stack{{workspace_, 0}};
      while (!stack.empty() && hits.size() < kMaxHits) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        if (depth > 6) continue;
        auto listing = st::fs::list_dir(dir);
        if (!listing) continue;
        for (const auto& item : *listing) {
          if (hits.size() >= kMaxHits) break;
          const std::string full = dir + "/" + item.name;
          if (item.is_dir) {
            if (!item.name.empty() && item.name.front() == '.') continue;   // 隐藏目录
            stack.emplace_back(full, depth + 1);
            continue;
          }
          if (item.size > kMaxFileBytes) continue;
          auto content = st::fs::read_text(full);
          if (!content) continue;   // 非 UTF-8 / 读不了：跳过而不是硬猜
          std::size_t line_no = 0;
          std::size_t line_begin = 0;
          while (line_begin <= content->size() && hits.size() < kMaxHits) {
            const std::size_t eol = content->find('\n', line_begin);
            const std::size_t line_end = eol == std::string::npos ? content->size() : eol;
            const std::string_view line(content->data() + line_begin, line_end - line_begin);
            ++line_no;
            const std::size_t column = line.find(query);
            if (column != std::string_view::npos) {
              add_hit(full, std::format("{}:{}  {}", item.name, line_no,
                                        st::trim(line).substr(0, 60)),
                      line_no, column);
            }
            if (eol == std::string::npos) break;
            line_begin = eol + 1;
          }
        }
      }
    } else if (!query.empty()) {
      // 内置样例回退（无 --workspace 时保持自包含）
      for (const auto& sample : files_) {
        std::size_t line_no = 0;
        std::size_t cursor = 0;
        while (cursor <= sample.code.size() && hits.size() < kMaxHits) {
          const std::size_t eol = sample.code.find('\n', cursor);
          const std::string_view line =
              std::string_view(sample.code)
                  .substr(cursor, eol == std::string::npos ? std::string_view::npos : eol - cursor);
          ++line_no;
          const std::size_t column = line.find(query);
          if (column != std::string_view::npos) {
            add_hit(sample.name,
                    std::format("{}:{}  {}", sample.name, line_no, st::trim(line).substr(0, 60)),
                    line_no, column);
          }
          if (eol == std::string::npos) break;
          cursor = eol + 1;
        }
      }
    }

    if (query.empty()) {
      status_.set("搜索：输入关键词");
    } else if (hits.empty()) {
      hits.push_back("没有匹配：" + query);
      search_paths_.push_back({});
      search_lines_.push_back(0);
      search_columns_.push_back(0);
      status_.set("搜索命中 0 处");
    } else {
      status_.set(std::format("搜索命中 {} 处", hits.size()));
    }
    search_hits_.set(std::move(hits));
  }

  /// 点搜索命中 → 打开文件并跳到那一行 + 选中命中词。
  void activate_search_hit(std::size_t index) {
    if (index >= search_paths_.size() || search_paths_[index].empty()) return;
    open_path(search_paths_[index]);
    if (editor == nullptr) {
      // 本帧的编辑器实例要等下一次重组才拿得到：把跳转记下，build() 末尾补做
      pending_jump_ = {search_lines_[index], search_columns_[index]};
      return;
    }
    apply_jump(search_lines_[index], search_columns_[index]);
  }

  void apply_jump(std::size_t line, std::size_t column) {
    if (editor == nullptr) return;
    editor->goto_line(line);
    const std::size_t start = editor->cursor_index() + column;
    editor->set_selection(start, start + 1);
    editor->scroll_to_line(line);
    status_.set(std::format("已跳到第 {} 行", line));
  }

  void run_terminal(const std::string& command) {
    std::string reply;
    if (command == "help") {
      reply =
          "可用命令：\n"
          "  help           本帮助\n"
          "  langs          已注册语言列表\n"
          "  ls             工作区根目录（真实文件模式）\n"
          "  find <词>      在工作区里搜（打到搜索面板）\n"
          "  goto <行>      当前编辑器跳到该行\n"
          "  stats          当前编辑器统计\n"
          "  save / theme / clear / open <文件>";
    } else if (command == "langs") {
      for (const auto& name : CodeEditor::available_languages()) reply += name + " ";
      reply = "已注册语言：" + reply;
    } else if (command == "ls") {
      if (workspace_.empty()) {
        reply = "当前为内置样例模式（启动时加 --workspace <目录> 可用真实文件）";
      } else if (auto listing = st::fs::list_dir(workspace_); listing.has_value()) {
        reply = "工作区 " + workspace_ + "：";
        for (const auto& item : *listing) {
          reply += "\n  " + item.name + (item.is_dir ? "/" : std::format("  {} B", item.size));
        }
      } else {
        reply = "目录读不了：" + workspace_;
      }
    } else if (command.starts_with("goto ")) {
      try {
        const auto line = static_cast<std::size_t>(std::stoull(std::string(command.substr(5))));
        if (editor != nullptr) {
          editor->goto_line(line);
          editor->scroll_to_line(line);
          reply = std::format("已跳到第 {} 行", line);
        } else {
          reply = "没有打开的编辑器";
        }
      } catch (...) {
        reply = "用法：goto <行号>";
      }
    } else if (command.starts_with("find ")) {
      const std::string needle(command.substr(5));
      activity_.set(1);   // 切到搜索面板让结果可见
      run_search(needle);
      reply = std::format("已搜索「{}」，结果见搜索面板", needle);
    } else if (command == "stats") {
      const auto list = buffers_.value();
      if (editor != nullptr && active_.value() < list.size()) {
        reply = std::format("{} · {} 行 · {} 字符", list[active_.value()].label,
                            editor->line_count(), st::utf8_length(editor->text()));
      } else {
        reply = "没有打开的编辑器";
      }
    } else if (command == "save") {
      save();
      reply = "已执行保存（见输出面板）";
    } else if (command == "theme") {
      toggle_theme();
      reply = "已切换主题";
    } else if (command == "clear") {
      terminal_log_.set({"shuangtian dev terminal"});
      if (terminal_input != nullptr) terminal_input->set_text("");
      return;
    } else if (command.starts_with("open ")) {
      const std::string want(command.substr(5));
      open_by_name(want);
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
    if (terminal_input != nullptr) terminal_input->set_text("");
  }

  /// 命令面板执行一条命令（id 是稳定的业务身份）。
  void run_command(const std::string& id) {
    if (id.rfind("file.open.", 0) == 0) {
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
      status_.set("codeeditor 0.1.0 · 霜天框架示例 · 声明式组装");
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
    add("view.toggle-sidebar", "查看: 切换侧栏可见性", "显示/隐藏活动栏与侧栏（Ctrl+B）");
    add("view.toggle-theme", "查看: 切换亮/暗主题", "主题令牌整体切换");
    add("view.toggle-indent-guides", "查看: 切换缩进参考线", "每级缩进一条竖线");
    add("help.about", "帮助: 关于", "codeeditor · 霜天示例");
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
  std::vector<Sample> files_{};
  std::string workspace_{};
  std::vector<Problem> problems_{};
  std::vector<ListItemData> search_hits_cache_{};
  std::vector<std::string> search_paths_{};
  std::vector<std::size_t> search_lines_{};
  std::vector<std::size_t> search_columns_{};
  std::pair<std::size_t, std::size_t> pending_jump_{0, 0};

 private:
  std::string loaded_key_{};   ///< 编辑器实例当前装载的是哪个标签（防重复 set_text）
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
  std::uint16_t control_port{0};
  std::string control_file{};
  std::string shots{};
  std::uint32_t frames{0};
  int max_ms{0};
  bool enable_script{false};
  /// 工作区目录（真实文件模式）：资源管理器/打开/保存全部走 `st::fs` 真实读写；
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
    if (raw == "--headless") options.headless = true;
    else if (raw == "--enable-script") options.enable_script = true;
    else if (raw == "--scale") options.scale = static_cast<float>(std::stod(value("1")));
    else if (raw == "--theme") options.theme = value("light");
    else if (raw == "--language") options.language = value("cpp");
    else if (raw == "--text-lcd") options.text_lcd = value("auto");
    else if (raw == "--text-fit") options.text_fit = value("auto");
    else if (raw == "--control-port") options.control_port = static_cast<std::uint16_t>(std::stoi(value("0")));
    else if (raw == "--control-file") options.control_file = value({});
    else if (raw == "--shots") options.shots = value({});
    else if (raw == "--frames") options.frames = static_cast<std::uint32_t>(std::stoi(value("0")));
    else if (raw == "--ms") options.max_ms = std::stoi(value("0"));
    else if (raw == "--workspace") options.workspace = value(".");
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
  app_options.theme = options.theme == "dark" ? ThemeMode::Dark : ThemeMode::Light;

  st::app::Application app("codeeditor", "0.1.0", app_options);

  // 声明式挂载：整个 IDE 是一个 Component（`mount` 的单根语义正合适——
  // 这个应用的全部界面都由声明式描述，没有手搭外壳）。
  auto page = std::make_shared<CodeEditorPage>(samples(), options.workspace);
  auto host = dsl::mount(app.root(), page);
  if (host == nullptr) {
    std::fprintf(stderr, "声明式挂载失败\n");
    return 1;
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
  page->dark_.set(options.theme == "dark");
  // 首帧重组 + 聚焦编辑器：**必须在 `start()` 之前完成**。
  // `start()` 内会开控制通道，而客户端（脚本）一看到控制文件就连——
  // 那时如果界面还没建好（或焦点还没设），它拿到的是半成品状态
  // （实测踩到：`st_editor_smoke.py` 断言“启动即聚焦”，偶发失败）。
  (void)host->tick();
  app.root().set_focus(page->editor);
  app.root().mark_dirty_all();

  // —— 全局快捷键（注册到 UiRoot：先于焦点链派发，文本组件吞键也拦得住）——
  {
    UiRoot* root = &app.root();
    const auto bind = [root](const char* key, bool shift, std::function<void()> action) {
      UiRoot::Shortcut mods{};
      mods.key = key;
      mods.ctrl = true;
      mods.shift = shift;
      (void)root->register_shortcut(key, mods, [action = std::move(action)]() {
        action();
        return true;   // 全局命令：总是消费，不再下沉
      });
    };
    bind("s", false, [page] { page->save(); });
    bind("w", false, [page] { page->close_active(); });
    bind("b", false, [page] { page->sidebar_visible_.set(!page->sidebar_visible_.value()); });
    bind("p", true, [page] { page->open_palette(false); });          // Ctrl+Shift+P：命令表
    bind("p", false, [page] { page->open_palette(true); });          // Ctrl+P：快速打开文件
    bind("f", false, [page] { page->open_find(); });                 // Ctrl+F：查找
    bind("h", false, [page] { page->open_find(); });                 // Ctrl+H：查找替换
    bind("tab", false, [page] {                                      // Ctrl+Tab：循环切标签
      const auto count = page->buffers_.value().size();
      if (count > 1) page->active_.set((page->active_.value() + 1) % count);
    });
    // Esc 关闭查找条（Input 不认 Esc，用根级快捷键；只在自己可见时接管，
    // 免得吃掉别的场景的 Esc——比如命令面板自己要用的那个）
    UiRoot::Shortcut plain{};
    plain.key = "escape";
    (void)root->register_shortcut("escape", plain, [page]() {
      if (!page->find_open_.value()) return false;
      page->dismiss_find();
      return true;
    });
  }

  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败: %s\n", started.error().to_string().c_str());
    return 1;
  }
  page->output_.set(std::format("[启动] codeeditor 0.1.0 · 后端 {} · headless={} · DPI {:.1f} · 控制通道 127.0.0.1:{}",
                                app.backend_name(), app.headless(),
                                static_cast<double>(app.device_scale()), app.control_port()));
  (void)host->tick();
  app.render_frame();

  // 脚本逻辑层（仅 `--enable-script` 时可用）：**组件控制逻辑用 JS 写**。
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

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    // 帧首推进：先在编辑器上补做「上一帧记下的跳转」（切标签/打开文件后
    // 编辑器实例要等重组才拿到，所以跳转请求排队到这一帧落地）。
    if (host->dirty()) (void)host->tick();
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    app.pace_loop(frame_start_ms);
  }
  st::print("codeeditor 退出：{} 帧，标签 {} 个，可用语言 {} 种\n", frames,
            page->buffers_.value().size(), CodeEditor::available_languages().size());
  return 0;
}

}  // namespace

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
