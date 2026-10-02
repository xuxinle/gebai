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
    else if (raw == "--headless") options.headless = true;
    else if (raw == "--enable-script") options.enable_script = true;
  }
  return options;
}

// 打开的编辑器：一个标签 = 一份文本 + 一个 CodeEditor 实例

struct OpenBuffer {
  std::string key;       // 业务身份（样例名）
  std::string label;     // 标签名（文件名）
  std::string language;
  std::string text;      // 当前文本（切换回来时恢复）
  bool dirty{false};     // 修改点
  CodeEditor* editor{};  // 非拥有：挂在编辑区容器下的实例（随容器存活）
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
};

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
          // 同步 Tabs 修改点
          std::vector<st::ui::Tabs::Tab> tabs;
          for (const auto& open : wb.buffers) {
            tabs.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
          }
          wb.tabs->sync_tabs(tabs);
        }
      }
      wb.cursor_label->set_content("编辑中…");
      root.mark_dirty_all();
    };
    editor->on_cursor_change = [&wb, &root]() {
      if (wb.active >= wb.buffers.size()) return;
      CodeEditor* active_editor = wb.buffers[wb.active].editor;
      if (active_editor == nullptr) return;
      wb.cursor_label->set_content(std::format("Ln {}, Col {}", active_editor->cursor_line() + 1,
                                               active_editor->cursor_column() + 1));
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
  wb.title_text->set_content(sample.name + " - codeeditor");
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
  const OpenBuffer& buffer = wb.buffers[wb.active];
  wb.title_text->set_content(buffer.label + " - codeeditor");
  wb.status->set_content("已关闭标签 · 当前 " + buffer.label);
  root.set_focus(buffer.editor);
  root.mark_dirty_all();
}

/// 保存当前标签（模拟：清修改点 + 状态栏反馈 + 输出面板记一行）。
void save_active(Workbench& wb, st::ui::UiRoot& root) {
  if (wb.active >= wb.buffers.size()) {
    wb.status->set_content("没有可保存的编辑器");
    root.mark_dirty_all();
    return;
  }
  OpenBuffer& buffer = wb.buffers[wb.active];
  buffer.dirty = false;
  buffer.text = buffer.editor != nullptr ? buffer.editor->text() : buffer.text;
  std::vector<st::ui::Tabs::Tab> tabs;
  for (const auto& open : wb.buffers) {
    tabs.push_back(st::ui::Tabs::Tab{open.key, open.label, open.dirty, true});
  }
  wb.tabs->sync_tabs(tabs);
  wb.status->set_content("已保存 " + buffer.label + "（内存模拟，无磁盘写入）");
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

/// 命令（命令面板里的一个条目）。
struct Command {
  std::string title;
  std::string detail;
  std::function<void()> run;
};
// 命令面板（overlay：FillViewport + 顶部居中卡片 + 过滤列表）

class CommandPalette : public st::ui::Element {
 public:
  CommandPalette(std::string query, std::vector<Command> commands,
                 std::function<void()> on_close)
      : commands_(std::move(commands)), close_(std::move(on_close)) {
    set_id("command-palette");
    style().background = st::math::Color{0, 0, 0, 110};
    query_ = query.empty() ? "" : query;
    auto box = std::make_unique<Panel>(FlexDirection::Column);
    box->set_id("palette-card");
    box->style().width = 520.0f;
    box->style().background = st::math::Color::rgba(26, 34, 48, 255);  // 深底（亮暗主题下都是"浮层"）
    box->style().radius = 10.0f;
    box->style().border_color = st::math::Color::rgba(255, 255, 255, 26);
    box->style().border_width = 1.0f;
    box->style().shadow = st::ui::Shadow{};
    box->style().clip_children = true;
    auto input_row = std::make_unique<Panel>(FlexDirection::Row);
    input_row->style().padding = Insets{12.0f, 10.0f, 12.0f, 10.0f};
    input_row->style().gap = 8.0f;
    input_row->style().align_items = Align::Center;
    input_row->add_child(std::make_unique<st::ui::IconView>("chevron-right", 16.0f));
    input_ = static_cast<Input*>(input_row->add_child(std::make_unique<Input>()));
    box->add_child(std::move(input_row));
    list_ = static_cast<st::ui::List*>(box->add_child(std::make_unique<st::ui::List>()));
    list_->style().max_height = 300.0f;
    card_ = box.get();
    add_child(std::move(box));
    rebuild("");
    input_->set_placeholder("输入命令名过滤，Enter 执行，Esc 关闭");
    input_->on_change = [this](std::string_view value) {
      rebuild(std::string(value));
    };
    input_->on_submit = [this](std::string_view) { activate_highlighted(); };
  }

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CommandPalette"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::Dialog; }
  [[nodiscard]] auto intercepts_input() const noexcept -> bool override { return true; }

  void arrange(const st::ui::RenderContext& context, st::math::Rect rect) override {
    // 卡片顶部居中（VSCode 命令面板形态）：FillViewport 给的是全视口矩形，
    // 子卡片的位置由本组件重排——直接用 layout_children 把内容放到目标矩形。
    const st::math::Size card_size = card_->measured_size();
    const float x = rect.x + (rect.width - card_size.width) * 0.5f;
    const float y = rect.y + 48.0f;
    card_rect_ = st::math::Rect{x, y, card_size.width, card_size.height};
    card_->measure(context, st::ui::Constraints{});
    card_->arrange(context, card_rect_);
    bounds_ = rect;
  }

  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override {
    if (event.kind == st::ui::EventKind::KeyDown) {
      if (event.key == "Escape") {
        if (close_) close_();
        return true;
      }
      if (event.key == "ArrowDown" || event.key == "ArrowUp") {
        move_highlight(event.key == "ArrowDown" ? 1 : -1);
        return true;
      }
      if (event.key == "Enter") {
        activate_highlighted();
        return true;
      }
    }
    // 面板外点击 → 关闭
    if (event.kind == st::ui::EventKind::MouseDown && !card_rect_.contains(event.position)) {
      if (close_) close_();
      return true;
    }
    return Element::on_event(context, event);
  }

 private:
  void rebuild(const std::string& query) {
    std::vector<st::ui::List::Entry> entries;
    for (const Command& command : commands_) {
      if (!query.empty() && command.title.find(query) == std::string::npos &&
          command.detail.find(query) == std::string::npos) {
        continue;
      }
      st::ui::List::Entry entry;
      entry.key = command.title;
      entry.label = command.title;
      Command captured = command;
      entry.on_activate = [this, captured]() {
        if (close_) close_();
        captured.run();
      };
      entries.push_back(std::move(entry));
    }
    list_->sync_items(entries);
    highlight_ = entries.empty() ? st::ui::kNoSelection : 0;
    if (highlight_ != st::ui::kNoSelection) list_->select(highlight_, false);
  }

  void move_highlight(int delta) {
    if (list_->item_count() == 0) return;
    std::size_t current = list_->selected_index();
    if (current == st::ui::kNoSelection) current = 0;
    if (delta > 0) {
      current = current + 1 >= list_->item_count() ? 0 : current + 1;
    } else {
      current = current == 0 ? list_->item_count() - 1 : current - 1;
    }
    list_->select(current, false);
    highlight_ = current;
  }

  void activate_highlighted() {
    if (highlight_ == st::ui::kNoSelection || highlight_ >= list_->item_count()) {
      if (close_) close_();
      return;
    }
    if (auto* item = list_->item(highlight_); item != nullptr) {
      item->activate();
    }
  }

  std::vector<Command> commands_{};
  std::function<void()> close_{};
  std::string query_{};
  Input* input_{nullptr};
  st::ui::List* list_{nullptr};
  Panel* card_{nullptr};
  st::math::Rect card_rect_{};
  std::size_t highlight_{st::ui::kNoSelection};
};
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
  {
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

  // 问题面板
  auto problems_panel = std::make_unique<Panel>(FlexDirection::Row);
  problems_panel->set_id("panel-problems");
  problems_panel->style().grow = true;
  problems_panel->style().padding = Insets{8.0f, 6.0f, 8.0f, 6.0f};
  problems_panel->style().gap = 16.0f;
  const auto make_problem = [&problems_panel](const char* icon, st::ui::Tone tone,
                                              const std::string& text) {
    auto row = std::make_unique<Panel>(FlexDirection::Row);
    row->style().gap = 6.0f;
    row->style().align_items = Align::Center;
    auto mark = std::make_unique<st::ui::IconView>(icon, 14.0f);
    mark->set_tone(tone);
    row->add_child(std::move(mark));
    auto label = std::make_unique<Text>(text);
    label->set_font_size(12.0f);
    row->add_child(std::move(label));
    problems_panel->add_child(std::move(row));
  };
  make_problem("error", st::ui::Tone::Danger, "0 个错误");
  make_problem("warning", st::ui::Tone::Warning, "0 个警告");
  auto problems_gap = std::make_unique<Panel>(FlexDirection::Row);
  problems_gap->style().grow = true;
  problems_panel->add_child(std::move(problems_gap));
  auto problems_note = std::make_unique<Text>("工作区干净，没有发现问题");
  problems_note->set_tone(Tone::Faint);
  problems_note->set_font_size(12.0f);
  problems_panel->add_child(std::move(problems_note));
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
  error_count->set_font_size(11.5f);
  status_bar->add_child(std::move(error_count));
  auto warn_icon = std::make_unique<st::ui::IconView>("warning", 12.0f);
  warn_icon->set_tone(Tone::Warning);
  status_bar->add_child(std::move(warn_icon));
  auto warn_count = std::make_unique<Text>("0");
  warn_count->set_font_size(11.5f);
  status_bar->add_child(std::move(warn_count));

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

  // ════════════════ 行为接线 ════════════════

  // Tabs：切换标签 = 换绑编辑器内容；× = 关闭
  wb.tabs->on_change = [&wb, root](std::size_t index) {
    if (index >= wb.buffers.size()) return;
    // 存回旧文本
    if (wb.active < wb.buffers.size() && wb.buffers[wb.active].editor != nullptr) {
      wb.buffers[wb.active].text = wb.buffers[wb.active].editor->text();
    }
    wb.active = index;
    bind_editor(wb, *root);
    wb.title_text->set_content(wb.buffers[index].label + " - codeeditor");
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
  // 目录展开/收起：示例树是静态的，toggle 只维持视觉状态
  tree_ptr->on_toggle = [root](std::string_view, bool) { root->mark_dirty_all(); };

  // 搜索：在样例文本里逐行找，结果进列表
  search_input_ptr->on_change = [search_list_ptr, &files, &wb, root](std::string_view query) {
    std::vector<List::Entry> entries;
    if (!query.empty()) {
      for (const auto& sample : files) {
        std::size_t line_no = 0;
        std::size_t cursor = 0;
        while (cursor <= sample.code.size()) {
          const std::size_t eol = sample.code.find('\n', cursor);
          const std::string_view line = std::string_view(sample.code).substr(
              cursor, eol == std::string::npos ? std::string_view::npos : eol - cursor);
          ++line_no;
          if (line.find(query) != std::string_view::npos) {
            List::Entry entry;
            entry.key = sample.name + ":" + std::to_string(line_no);
            entry.label = sample.name + ":" + std::to_string(line_no) + "  " + std::string(line);
            entry.on_activate = [&files, &wb, root, name = sample.name]() {
              for (const auto& candidate : files) {
                if (candidate.name == name) {
                  open_sample(wb, candidate, *root);
                  return;
                }
              }
            };
            entries.push_back(std::move(entry));
          }
          if (eol == std::string::npos) break;
          cursor = eol + 1;
        }
      }
    }
    search_list_ptr->sync_items(entries);
    root->mark_dirty_all();
  };
  // 终端：Enter 提交 → 回显命令与模拟输出
  wb.terminal_input->on_submit = [&wb, &files, root](std::string_view command) {
    std::string reply;
    if (command == "help") {
      reply = "可用命令：help · langs · open <file> · save · theme · clear";
    } else if (command == "langs") {
      std::string names;
      for (const auto& name : CodeEditor::available_languages()) {
        names += name + " ";
      }
      reply = "已注册语言：" + names;
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

  // 命令面板条目
  std::vector<Command> commands;
  commands.push_back(Command{"文件: 全部保存", "把所有打开的编辑器标记为已保存", [&wb, root]() {
                               save_active(wb, *root);
                             }});
  commands.push_back(Command{"查看: 切换侧栏可见性", "显示/隐藏活动栏与侧栏（Ctrl+B）", []() {}});
  commands.push_back(Command{"查看: 切换亮/暗主题", "主题令牌整体切换", [&wb]() {
                               wb.theme_button->activate();
                             }});
  commands.push_back(Command{"帮助: 关于", "codeeditor · 霜天示例", [&wb, root]() {
                               wb.status->set_content("codeeditor 0.1.0 · 霜天框架示例 · 全内置组件组装");
                               root->mark_dirty_all();
                             }});
  for (const auto& sample : files) {
    Command command{"文件: 打开 " + sample.name, "语言 " + sample.language,
                    [&wb, &sample, root]() { open_sample(wb, sample, *root); }};
    commands.push_back(std::move(command));
  }
  commands.push_back(Command{"语言: 打开 stlog 样例", "自定义语言（运行时注册）", [&wb, root]() {
                               Sample stlog_sample{"stlog.log", "stlog", kStlogSample, "logs/stlog.log"};
                               open_sample(wb, stlog_sample, *root);
                             }});

  const auto open_palette = [&app, &commands, root](std::string query = {}) {
    auto* existing = root->find("command-palette");
    if (existing != nullptr) root->remove_overlay(existing);
    auto palette = std::make_unique<CommandPalette>(
        std::move(query), commands, [root]() {
          if (auto* panel = root->find("command-palette"); panel != nullptr) {
            root->remove_overlay(panel);
          }
          root->mark_dirty_all();
        });
    root->add_overlay(std::move(palette), st::ui::UiRoot::OverlayLayout::FillViewport);
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
    if (auto panel = menu_bar_ptr->make_panel(index); panel != nullptr) {
      root->add_overlay(std::move(panel), st::ui::UiRoot::OverlayLayout::FillViewport);
      root->mark_dirty_all();
    }
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
    wb.title_text->set_content(wb.buffers[index].label + " - codeeditor");
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
