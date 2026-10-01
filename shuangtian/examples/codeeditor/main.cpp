/// 代码编辑器演示（视觉验证 + 能力展示）：`st build codeeditor` 后无头启动并截图核验。
///
/// 展示点：
/// - **内置语言**：同一份"文件列表"用不同语言打开（C++/Python/Rust/JSON/YAML/HTML/SQL/Diff…）
/// - **自定义语言**：注册一个业务日志语言（`stlog`），验证"规则即数据"
/// - **只读查看器**：`set_read_only(true)` 的代码浏览模式
/// - **控制通道**：属性面与动作（`set text/language/cursor`、`invoke goto_line/comment`…）

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
#include "st/ui/components/overlay.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::math::Color;
using st::math::Insets;
using st::ui::Align;
using st::ui::Button;
using st::ui::Card;
using st::ui::CodeEditor;
using st::ui::FlexDirection;
using st::ui::FontWeight;
using st::ui::IconView;
using st::ui::Panel;
using st::ui::Text;
using st::ui::Tone;

struct Sample {
  std::string name;
  std::string language;
  std::string code;
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
)"});
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
    ok = Deployer("10.0.0.7").push(Path("build/dev/bin/mdeditor"))
    print("部署成功" if ok else "部署失败")
)"});
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
})"});
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
})"});
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
          ./build/bin/st build mdeditor --profile dev
          ./build/bin/st test --san
      - name: 禁令扫描
        run: ./build/bin/st lint
)"});
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
)"});
  list.push_back(Sample{
      "query.sql", "sql",
      R"(-- 会话与工具调用统计
SELECT
    s.user_id,
    COUNT(DISTINCT s.id)       AS sessions,
    COUNT(t.id)                AS tool_calls,
    ROUND(AVG(t.duration_ms))  AS avg_ms
FROM sessions AS s
LEFT JOIN tool_calls AS t ON t.session_id = s.id
WHERE s.created_at >= '2026-01-01'
  AND t.status <> 'cancelled'
GROUP BY s.user_id
HAVING COUNT(t.id) > 10
ORDER BY tool_calls DESC
LIMIT 20;
)"});
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
)"});
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

}  // namespace

auto run_app(int argc, char** argv) -> int {
  const Options options = parse_options(argc, argv);
  register_custom_language();
  const std::vector<Sample> files = samples();

  st::app::AppOptions app_options;
  app_options.width = 1180;
  app_options.height = 720;
  app_options.scale = options.scale;
  app_options.title = "霜天 · 代码编辑器";
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

  const auto find_sample = [&files](std::string_view language) -> const Sample* {
    for (const auto& sample : files) {
      if (sample.language == language) return &sample;
    }
    return &files.front();
  };

  auto page = std::make_unique<Panel>(FlexDirection::Column);
  page->set_id("editor-page");

  // —— 顶部：语言标签栏 ——
  auto top = std::make_unique<Panel>(FlexDirection::Row);
  top->set_id("topbar");
  top->style().height = 54.0f;
  top->style().padding = Insets{0.0f, 14.0f, 16.0f, 0.0f};
  top->style().gap = 8.0f;
  top->style().align_items = Align::Center;
  auto brand_icon = std::make_unique<IconView>("code", 20.0f);
  brand_icon->set_tone(Tone::Primary);
  top->add_child(std::move(brand_icon));
  auto brand = std::make_unique<Text>("霜天 · 文件编辑器");
  brand->set_weight(FontWeight::SemiBold);
  brand->set_id("brand");
  top->add_child(std::move(brand));

  auto editor = std::make_unique<CodeEditor>();
  editor->set_id("editor");
  editor->set_font_size(13.5f);
  editor->set_tab_width(4);
  editor->style().grow = true;  // 在卡片内撑满可用高度（编辑器应占据全部剩余空间）
  auto* editor_ptr = editor.get();

  auto viewer = std::make_unique<CodeEditor>();
  viewer->set_id("viewer");
  viewer->set_font_size(12.5f);
  viewer->set_read_only(true);
  viewer->set_show_line_numbers(true);
  viewer->style().grow = true;
  auto* viewer_ptr = viewer.get();

  auto status = std::make_unique<Text>("就绪");
  status->set_id("status");
  status->set_tone(Tone::Faint);
  status->set_font_size(12.0f);
  auto* status_ptr = status.get();

  auto language_label = std::make_unique<Text>("cpp");
  language_label->set_id("language-label");
  language_label->set_tone(Tone::Muted);
  language_label->set_font_size(12.0f);
  auto* language_ptr = language_label.get();

  const auto load = [editor_ptr, status_ptr, language_ptr, &files](const Sample& sample) {
    editor_ptr->set_language(sample.language);
    editor_ptr->set_text(sample.code);
    language_ptr->set_content(sample.language);
    status_ptr->set_content(sample.name + " · " +
                            std::to_string(st::utf8_length(sample.code)) + " 字符");
  };

  auto tab_gap = std::make_unique<Panel>(FlexDirection::Row);
  tab_gap->style().grow = true;
  top->add_child(std::move(tab_gap));

  // 语言切换按钮（内置语言 + 自定义语言）
  for (const auto& sample : files) {
    auto button = std::make_unique<Button>(sample.language, Button::Variant::Ghost, Button::Size::Small);
    button->set_id("tab-" + sample.language);
    const Sample* captured = &sample;
    button->on_click = [load, captured, status_ptr]() {
      load(*captured);
    };
    top->add_child(std::move(button));
  }
  auto custom_button = std::make_unique<Button>("stlog", Button::Variant::Secondary, Button::Size::Small);
  custom_button->set_id("tab-stlog");
  custom_button->on_click = [editor_ptr, status_ptr, language_ptr, root]() {
    editor_ptr->set_language("stlog");
    editor_ptr->set_text(
        R"(# 业务日志（自定义语言：运行时注册的规则）
INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms
DEBUG service=gateway trace_id=9f2c 命中缓存 true
WARN  service=renderer component=font 字形缓存接近上限
ERROR service=net trace_id=9f2c 连接失败 resp=null  # 需要重试
FATAL service=core 磁盘写入失败，进程退出
)");
    language_ptr->set_content("stlog");
    status_ptr->set_content("自定义语言 stlog（注册自应用启动时）");
    root->mark_dirty_all();
  };
  top->add_child(std::move(custom_button));

  auto dark_button = std::make_unique<Button>("暗色", Button::Variant::Ghost, Button::Size::Small);
  dark_button->set_id("btn-theme");
  dark_button->set_icon("moon");
  auto* dark_ptr = dark_button.get();
  dark_button->on_click = [&app, dark_ptr, root]() {
    const bool dark = app.root().theme().mode() == st::ui::ThemeMode::Light;
    app.set_theme_mode(dark ? st::ui::ThemeMode::Dark : st::ui::ThemeMode::Light);
    dark_ptr->set_label(dark ? "亮色" : "暗色");
    dark_ptr->set_icon(dark ? "sun" : "moon");
    root->mark_dirty_all();
  };
  top->add_child(std::move(dark_button));
  page->add_child(std::move(top));

  // —— 主体：左编辑器 + 右只读查看器 ——
  auto body = std::make_unique<Panel>(FlexDirection::Row);
  body->set_id("body");
  body->style().grow = true;
  body->style().padding = Insets{0.0f, 14.0f, 14.0f, 0.0f};
  body->style().gap = 12.0f;

  auto main_card = std::make_unique<Card>(0.0f);
  main_card->set_id("card-editor");
  main_card->style().grow = true;
  main_card->style().direction = FlexDirection::Column;
  auto main_head = std::make_unique<Panel>(FlexDirection::Row);
  main_head->style().padding = Insets{10.0f, 8.0f, 10.0f, 8.0f};
  main_head->style().gap = 8.0f;
  main_head->style().align_items = Align::Center;
  auto main_title = std::make_unique<Text>("编辑（可写 · 高亮 · 行号 · 撤销）");
  main_title->set_tone(Tone::Faint);
  main_title->set_font_size(11.0f);
  main_head->add_child(std::move(main_title));
  main_card->add_child(std::move(main_head));
  main_card->add_child(std::move(editor));
  main_card->style().clip_children = true;
  body->add_child(std::move(main_card));

  auto view_card = std::make_unique<Card>(0.0f);
  view_card->set_id("card-viewer");
  view_card->style().width = 460.0f;
  view_card->style().direction = FlexDirection::Column;
  auto view_head = std::make_unique<Panel>(FlexDirection::Row);
  view_head->style().padding = Insets{10.0f, 8.0f, 10.0f, 8.0f};
  view_head->style().gap = 8.0f;
  view_head->style().align_items = Align::Center;
  auto view_title = std::make_unique<Text>("只读查看（set_read_only）");
  view_title->set_tone(Tone::Faint);
  view_title->set_font_size(11.0f);
  view_head->add_child(std::move(view_title));
  view_card->add_child(std::move(view_head));
  view_card->add_child(std::move(viewer));
  view_card->style().clip_children = true;
  body->add_child(std::move(view_card));
  page->add_child(std::move(body));

  // —— 底部状态栏 ——
  auto bar = std::make_unique<Panel>(FlexDirection::Row);
  bar->set_id("statusbar");
  bar->style().height = 34.0f;
  bar->style().padding = Insets{14.0f, 0.0f, 14.0f, 0.0f};
  bar->style().gap = 10.0f;
  bar->style().align_items = Align::Center;
  auto dot = std::make_unique<IconView>("dot", 8.0f);
  dot->set_tone(Tone::Success);
  bar->add_child(std::move(dot));
  bar->add_child(std::move(language_label));
  auto separator = std::make_unique<Text>("·");
  separator->set_tone(Tone::Faint);
  bar->add_child(std::move(separator));
  bar->add_child(std::move(status));
  auto gap = std::make_unique<Panel>(FlexDirection::Row);
  gap->style().grow = true;
  bar->add_child(std::move(gap));
  auto hint = std::make_unique<Text>("Ctrl+/ 注释 · Tab 缩进 · Ctrl+Z 撤销 · 拖拽选择 · 双击选词");
  hint->set_tone(Tone::Faint);
  hint->set_font_size(11.0f);
  bar->add_child(std::move(hint));
  page->add_child(std::move(bar));

  // 初始内容
  load(*find_sample(options.language));
  viewer_ptr->set_language("stlog");
  viewer_ptr->set_text(
      R"(# 只读查看器：同一组件开启 read_only
INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms
DEBUG service=gateway 命中缓存 true
WARN  service=renderer 字形缓存接近上限
ERROR service=net 连接失败 resp=null
)");

  app.set_content(std::move(page));
  if (auto started = app.start(); !started) {
    std::fprintf(stderr, "启动失败: %s\n", started.error().to_string().c_str());
    return 1;
  }
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
  root->set_focus(editor_ptr);
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
  st::print("codeeditor 退出：{} 帧，语言 {}，可用语言 {} 种\n", frames, editor_ptr->language(),
            CodeEditor::available_languages().size());
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
