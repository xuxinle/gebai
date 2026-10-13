#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "tests/support/text_port_fixtures.hpp"

// 测试在匿名命名空间内，`st::test::X` 得写全；用具名别名让用例读起来干净。
using st::test::ProportionalTextPort;

namespace {


[[nodiscard]] auto stub_port() -> const ProportionalTextPort& {
  static const ProportionalTextPort port;
  return port;
}

/// 画布尺寸（逻辑像素 = 物理像素，DPI 1.0）。
constexpr int kCanvasWidth{600};
constexpr int kCanvasHeight{400};

/// 含标题 / 段落 / 行内码 / 列表 / 任务项 / 引用 / 代码块 / 表格 / 分隔线的样本文档。
constexpr std::string_view kSample = R"MD(# 霜天 Markdown 视图

段落文本，含 **加粗**、`code`、[链接](https://example.com) 与中文标点，以及换行折行的长句子用来触发折行。

- 无序项一
- 无序项二
  - 嵌套项

1. 有序一
2. 有序二

- [x] 已完成任务
- [ ] 未完成任务

> 引用段落

```python
def f(x):
    s = "hi"  # 注释
    return 42
```

| 名称 | 数量 |
| --- | --- |
| 苹果 | 3 |

---

尾段。
)MD";

/// 同一段代码，一种标注语言（python）、一种不标注：用于证明高亮确实生效。
constexpr std::string_view kHighlightDoc = R"MD(```python
def f(x):
    s = "hi"  # 注释
    return 42
```
)MD";
constexpr std::string_view kPlainDoc = R"MD(```
def f(x):
    s = "hi"  # 注释
    return 42
```
)MD";

[[nodiscard]] auto constraints_for(int width, int height) -> st::ui::Constraints {
  st::ui::Constraints constraints;
  constraints.max_width = static_cast<float>(width);
  constraints.max_height = static_cast<float>(height);
  constraints.available_width = constraints.max_width;
  constraints.available_height = constraints.max_height;
  return constraints;
}

/// 主题 → 排版 → 绘制。`background` 为不透明底时先铺底（导出 PPM 用），
/// 否则保持透明（像素断言针对绘制内容本身）。
[[nodiscard]] auto render(st::ui::MarkdownView& view, const st::ui::Theme& theme, int width,
                          int height) -> st::raster::Canvas {
  view.apply_theme(theme);
  const st::ui::RenderContext context{theme, &stub_port(), 0.0};
  view.measure(context, constraints_for(width, height));
  view.arrange(context,
               st::math::Rect{0.0f, 0.0f, view.measured_size().width, view.measured_size().height});
  st::raster::Canvas canvas(width, height);
  view.paint(context, canvas);
  return canvas;
}

[[nodiscard]] auto render_over_background(st::ui::MarkdownView& view, const st::ui::Theme& theme,
                                          int width, int height) -> st::raster::Canvas {
  view.apply_theme(theme);
  const st::ui::RenderContext context{theme, &stub_port(), 0.0};
  view.measure(context, constraints_for(width, height));
  view.arrange(context,
               st::math::Rect{0.0f, 0.0f, view.measured_size().width, view.measured_size().height});
  st::raster::Canvas canvas(width, height);
  canvas.clear(theme.colors().bg);
  view.paint(context, canvas);
  return canvas;
}

/// 颜色近似（容差内且基本不透明）。
[[nodiscard]] auto near_color(st::math::Color actual, st::math::Color expected,
                              int tolerance = 8) -> bool {
  if (actual.a < 200U) return false;
  const auto close = [tolerance](std::uint8_t left, std::uint8_t right) {
    return std::abs(static_cast<int>(left) - static_cast<int>(right)) <= tolerance;
  };
  return close(actual.r, expected.r) && close(actual.g, expected.g) &&
         close(actual.b, expected.b);
}

[[nodiscard]] auto count_color(const st::raster::Canvas& canvas, st::math::Color target,
                               int tolerance = 8) -> std::size_t {
  std::size_t count = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      if (near_color(canvas.pixel_at(x, y), target, tolerance)) ++count;
    }
  }
  return count;
}

/// PPM（P6）导出：20 行内的最小实现，便于人工查看渲染结果。
auto write_ppm(std::string_view path, const st::raster::Canvas& canvas) -> bool {
  const std::vector<std::uint8_t> rgba = canvas.to_rgba8();
  const int width = canvas.physical_width();
  const int height = canvas.physical_height();
  if (width <= 0 || height <= 0 || rgba.size() < 4U * static_cast<std::size_t>(width) *
                                                   static_cast<std::size_t>(height)) {
    return false;
  }
  std::ofstream out(std::string(path), std::ios::binary);
  if (!out) return false;
  out << std::format("P6\n{} {}\n255\n", width, height);
  std::string row;
  row.reserve(static_cast<std::size_t>(width) * 3U);
  for (int y = 0; y < height; ++y) {
    row.clear();
    for (int x = 0; x < width; ++x) {
      const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(x)) * 4U;
      row.push_back(static_cast<char>(rgba[index]));
      row.push_back(static_cast<char>(rgba[index + 1U]));
      row.push_back(static_cast<char>(rgba[index + 2U]));
    }
    out.write(row.data(), static_cast<std::streamsize>(row.size()));
  }
  return out.good();
}

[[nodiscard]] auto ppm_path(std::string_view name) -> std::string {
  return std::format("{}/{}", st::fs::temp_dir(), name);
}

[[nodiscard]] auto equals(std::string_view left, std::string_view right) -> bool {
  return left == right;
}

}  // namespace

// ① 一次性 set_markdown：块数 > 0，语义文本含关键内容
ST_TEST(md_view_set_markdown_blocks_and_semantics) {
  st::ui::MarkdownView view{std::string(kSample)};
  ST_CHECK(view.block_count() > 0U);
  ST_CHECK(view.role() == st::ui::Role::Markdown);
  ST_CHECK(equals(view.type(), "MarkdownView"));
  ST_CHECK_EQ(view.markdown(), std::string(kSample));

  const std::string text = view.semantics_text();
  ST_CHECK(text.find("霜天 Markdown 视图") != std::string::npos);   // 标题
  ST_CHECK(text.find("加粗") != std::string::npos);                 // 强调（去标记）
  ST_CHECK(text.find("code") != std::string::npos);                 // 行内码
  ST_CHECK(text.find("嵌套项") != std::string::npos);               // 嵌套列表
  ST_CHECK(text.find("已完成任务") != std::string::npos);           // 任务项
  ST_CHECK(text.find("引用段落") != std::string::npos);             // 引用
  ST_CHECK(text.find("def f(x):") != std::string::npos);            // 代码块
  ST_CHECK(text.find("苹果") != std::string::npos);                 // 表格单元格
  ST_CHECK(text.find("尾段") != std::string::npos);                 // 尾段
  ST_CHECK(text.find("**") == std::string::npos);                   // 标记已去除

  // 控制通道属性面
  ST_REQUIRE(view.get_property("block_count").has_value());
  ST_CHECK_EQ(*view.get_property("block_count"), std::format("{}", view.block_count()));
  ST_CHECK(view.set_property("selectable", "true"));
  ST_CHECK_EQ(*view.get_property("selectable"), std::string("true"));
  ST_CHECK(view.set_property("base_font_size", "15.5"));
  ST_CHECK(!view.set_property("mystery", "1"));
  ST_CHECK(view.get_property("plain_text").has_value() == false);
  ST_CHECK(!view.property_names().empty());
}

// ② 流式切片（1/3/7 字节）与一次性解析等价；块数单调不减
ST_TEST(md_view_stream_append_matches_oneshot) {
  st::ui::MarkdownView once{std::string(kSample)};
  const std::string expected_text = once.semantics_text();
  const std::size_t expected_blocks = once.block_count();
  ST_REQUIRE(expected_blocks > 0U);

  for (const std::size_t step : {std::size_t{1}, std::size_t{3}, std::size_t{7}}) {
    st::ui::MarkdownView streamed;
    std::size_t previous_blocks = 0;
    bool monotonic = true;
    std::string accumulated;
    for (std::size_t position = 0; position < kSample.size(); position += step) {
      const std::size_t count = std::min(step, kSample.size() - position);
      const std::string_view chunk = kSample.substr(position, count);
      streamed.append_chunk(chunk);
      accumulated.append(chunk);
      if (streamed.block_count() < previous_blocks) monotonic = false;
      previous_blocks = streamed.block_count();
    }
    ST_CHECK(monotonic);                                     // 块数单调不减 ⇒ 已渲染内容不跳变
    ST_CHECK_EQ(streamed.block_count(), expected_blocks);     // 最终块数与一次性一致
    ST_CHECK_EQ(streamed.markdown(), std::string(kSample));   // 原文逐字节一致
    ST_CHECK_EQ(accumulated, std::string(kSample));
    ST_CHECK_EQ(streamed.semantics_text(), expected_text);
    ST_CHECK(streamed.get_property("pending").has_value());
  }

  // 追加过程中的部分前缀也要能排版（未闭合围栏：最后一块是 code_block）
  st::ui::MarkdownView partial;
  partial.append_chunk("# 标题\n\n```cpp\nint x = 1;\n");
  ST_CHECK(partial.block_count() >= 2U);
  ST_CHECK(equals(*partial.get_property("pending"), "true"));
  ST_CHECK(partial.semantics_text().find("int x = 1;") != std::string::npos);
}

// ③ 渲染到 600×400 画布：内容包围盒非空、内容高度 > 视口（可滚动）
ST_TEST(md_view_render_pixels_and_content_bounds) {
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::MarkdownView view{std::string(kSample)};
  const st::raster::Canvas canvas = render(view, theme, kCanvasWidth, kCanvasHeight);

  const st::math::IntRect bounds = canvas.content_bounds();
  std::cout << std::format(
                   "  [md] content_bounds=({},{},{}x{}) content_height={:.1f} viewport_height={}\n",
                   bounds.x, bounds.y, bounds.width, bounds.height, view.content_height(),
                   view.get_property("viewport_height").value_or("?"))
            << std::flush;
  ST_CHECK(!bounds.is_empty());
  ST_CHECK(bounds.width > 100);
  ST_CHECK(bounds.height > 100);
  ST_CHECK(view.content_height() > static_cast<float>(kCanvasHeight));
  ST_CHECK_EQ(view.scroll_offset(), 0.0f);

  // 导出 PPM（亮/暗各一张，铺主题底色便于查看）
  st::ui::MarkdownView light_view{std::string(kSample)};
  const st::raster::Canvas light = render_over_background(light_view, theme, kCanvasWidth, kCanvasHeight);
  const std::string light_path = ppm_path("st_md_view_light.ppm");
  ST_CHECK(write_ppm(light_path, light));

  st::ui::MarkdownView dark_view{std::string(kSample)};
  const st::raster::Canvas dark =
      render_over_background(dark_view, st::ui::Theme::dark(), kCanvasWidth, kCanvasHeight);
  const std::string dark_path = ppm_path("st_md_view_dark.ppm");
  ST_CHECK(write_ppm(dark_path, dark));
  std::cout << std::format("  [md] ppm: {} , {}\n", light_path, dark_path) << std::flush;
}

// ④ 滚轮滚动：偏移变化、可夹取、内容高度不变
ST_TEST(md_view_wheel_scrolls) {
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::MarkdownView view{std::string(kSample)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);
  const float content_height = view.content_height();
  ST_REQUIRE(content_height > static_cast<float>(kCanvasHeight));
  ST_CHECK_EQ(view.scroll_offset(), 0.0f);

  const st::ui::RenderContext context{theme, &stub_port(), 0.0};
  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{300.0f, 200.0f};
  wheel.wheel_delta = -120.0f;  // 向下滚
  ST_CHECK(view.on_event(context, wheel));
  ST_CHECK(wheel.handled);
  const float after_down = view.scroll_offset();
  ST_CHECK(after_down > 0.0f);
  ST_CHECK(after_down <= view.content_height());

  wheel.wheel_delta = 120.0f;  // 向上滚
  ST_CHECK(view.on_event(context, wheel));
  ST_CHECK_EQ(view.scroll_offset(), 0.0f);

  view.scroll_to(1.0e6f);  // 夹取到最大偏移
  ST_CHECK(view.scroll_offset() > 0.0f);
  ST_CHECK(view.scroll_offset() <= view.content_height());
  view.scroll_to(-50.0f);
  ST_CHECK_EQ(view.scroll_offset(), 0.0f);

  st::ui::Event key;
  key.kind = st::ui::EventKind::KeyDown;
  key.key = "PageDown";
  ST_CHECK(view.on_event(context, key));
  ST_CHECK(view.scroll_offset() > 0.0f);
  key.key = "Home";
  ST_CHECK(view.on_event(context, key));
  ST_CHECK_EQ(view.scroll_offset(), 0.0f);

  // 滚动只影响绘制偏移，不改变内容高度
  ST_CHECK_NEAR(view.content_height(), content_height, 0.001);
}

// ④b 经 UiRoot 的事件分发（集成路径）
ST_TEST(md_view_uiroot_dispatch_scrolls) {
  st::ui::UiRoot root;
  root.set_text_port(&stub_port());
  root.set_viewport(st::math::Size{static_cast<float>(kCanvasWidth),
                                   static_cast<float>(kCanvasHeight)});
  root.set_content(std::make_unique<st::ui::MarkdownView>(std::string(kSample)));
  root.layout(true);

  auto* view = dynamic_cast<st::ui::MarkdownView*>(root.content());
  ST_REQUIRE(view != nullptr);
  ST_CHECK(!view->semantics_text().empty());
  ST_REQUIRE(view->block_count() > 0U);

  const st::ui::SemanticsNode tree = root.semantics(4);
  ST_REQUIRE(!tree.children.empty());
  ST_CHECK(tree.children[0].role == st::ui::Role::Markdown);
  ST_CHECK(tree.children[0].text.find("霜天 Markdown 视图") != std::string::npos);

  st::raster::Canvas canvas(kCanvasWidth, kCanvasHeight);
  root.paint(canvas);
  ST_CHECK(!canvas.content_bounds().is_empty());

  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{300.0f, 200.0f};
  wheel.wheel_delta = -120.0f;
  ST_CHECK(root.dispatch(wheel));
  ST_CHECK(view->scroll_offset() > 0.0f);
}

// ⑤ 代码高亮：标注 python 时画布出现关键字/字符串/数字色；不标注语言则全是正文色
ST_TEST(md_view_code_highlight_colors) {
  const st::ui::Theme theme = st::ui::Theme::light();

  st::ui::MarkdownView highlighted{std::string(kHighlightDoc)};
  const st::raster::Canvas lit = render(highlighted, theme, kCanvasWidth, kCanvasHeight);
  const std::size_t keywords = count_color(lit, theme.colors().primary);
  const std::size_t strings = count_color(lit, theme.colors().success);
  const std::size_t numbers = count_color(lit, theme.colors().accent);
  std::cout << std::format("  [md] python 代码块像素：primary(关键字)={} success(字符串)={} accent(数字)={}\n",
                           keywords, strings, numbers)
            << std::flush;
  ST_CHECK(keywords > 0U);   // def / return → 关键字 = primary
  ST_CHECK(strings > 0U);    // "hi" → 字符串 = success
  ST_CHECK(numbers > 0U);    // 42 → 数字 = accent

  st::ui::MarkdownView plain{std::string(kPlainDoc)};
  const st::raster::Canvas flat = render(plain, theme, kCanvasWidth, kCanvasHeight);
  ST_CHECK(count_color(flat, theme.colors().primary) == 0U);
  ST_CHECK(count_color(flat, theme.colors().success) == 0U);
  ST_CHECK(count_color(flat, theme.colors().accent) == 0U);
  // 未标注语言仍以正文色绘制（代码内容没有被丢掉）
  ST_CHECK(count_color(flat, theme.colors().text) > 0U);

  // 暗色主题下高亮同样生效（配色随主题派生，不是硬编码色值）
  st::ui::MarkdownView dark_view{std::string(kHighlightDoc)};
  const st::raster::Canvas dark = render(dark_view, st::ui::Theme::dark(), kCanvasWidth, kCanvasHeight);
  ST_CHECK(count_color(dark, st::ui::Theme::dark().colors().primary) > 0U);
}

// ⑥ 空内容 / 超长单词 / 逐字节喂入：不崩、不越界
ST_TEST(md_view_empty_and_long_word_robust) {
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::MarkdownView empty;
  ST_CHECK_EQ(empty.block_count(), 0U);
  ST_CHECK(empty.semantics_text().empty());
  const st::raster::Canvas empty_canvas = render(empty, theme, kCanvasWidth, kCanvasHeight);
  ST_CHECK(empty.content_height() > 0.0f);  // 占位行有高度
  ST_CHECK(!empty_canvas.content_bounds().is_empty());  // 占位文本（（空））被绘制

  std::string long_word(320U, 'w');
  std::string document;
  for (int index = 0; index < 6; ++index) document += long_word + "\n\n";   // 超长单词重复到超过一屏
  document += "没有任何空格的中文长句子用来验证折行不会越界\n\n";
  document += std::format("```\n{}\n```\n", std::string(200U, 'x'));
  st::ui::MarkdownView view{document};
  const st::raster::Canvas canvas = render(view, theme, kCanvasWidth, kCanvasHeight);
  ST_REQUIRE(view.block_count() > 0U);
  const st::math::IntRect bounds = canvas.content_bounds();
  ST_CHECK(!bounds.is_empty());
  ST_CHECK(bounds.x >= 0);
  ST_CHECK(bounds.right() <= kCanvasWidth);   // 超长内容被裁剪在元素内
  ST_CHECK(bounds.bottom() <= kCanvasHeight);
  ST_CHECK(view.content_height() > static_cast<float>(kCanvasHeight));

  // 逐字节喂入含多字节 UTF-8 的文档（切在字符中间也不崩）
  const std::string cjk = "# 中文标题\n\n汉字段落。\n\n```py\nprint('中文')\n```\n";
  st::ui::MarkdownView streamed;
  for (const char raw : cjk) {
    streamed.append_chunk(std::string_view(&raw, 1));
  }
  ST_CHECK_EQ(streamed.markdown(), cjk);
  ST_CHECK(streamed.semantics_text().find("汉字段落") != std::string::npos);
  (void)render(streamed, theme, kCanvasWidth, kCanvasHeight);

  // 空画布（0×0）与空内容：绘制直接返回，不越界
  st::ui::MarkdownView narrow{std::string(kSample)};
  narrow.apply_theme(theme);
  const st::ui::RenderContext context{theme, &stub_port(), 0.0};
  narrow.measure(context, constraints_for(1, 1));
  narrow.arrange(context, st::math::Rect{0.0f, 0.0f, 0.0f, 0.0f});
  st::raster::Canvas tiny(1, 1);
  narrow.paint(context, tiny);
  ST_CHECK(tiny.content_bounds().is_empty());

  // NullTextPort（无字体环境）：布局与绘制都不崩
  st::ui::MarkdownView headless{std::string(kSample)};
  headless.apply_theme(theme);
  const st::ui::RenderContext null_context{theme, nullptr, 0.0};
  headless.measure(null_context, constraints_for(kCanvasWidth, kCanvasHeight));
  headless.arrange(null_context, st::math::Rect{0.0f, 0.0f, kCanvasWidth, kCanvasHeight});
  st::raster::Canvas null_canvas(kCanvasWidth, kCanvasHeight);
  headless.paint(null_context, null_canvas);
  ST_CHECK(headless.content_height() > 0.0f);
  ST_CHECK(!null_canvas.content_bounds().is_empty());  // 文本不画，底色与分隔线照画
}

// ————————————————— 选择与复制（2026-10-13）—————————————————

ST_TEST(md_view_selection_and_copy) {
  // 用户报「markdown 渲染后的文本要能复制」：行级拖选 + 选中文本可读 +
  // copy 动作拷选中（无选择时拷全文）。这里断言**状态与动作**，
  // 像素高亮在下面一条（md_view_selection_highlight_pixels）。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::MarkdownView view{std::string(kSample)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);

  // 默认 selectable=true（渲染视图开箱可复制）。
  ST_CHECK(view.get_property("selectable").value_or("false") == "true");
  // 无选择：selection 为空。
  ST_CHECK_EQ(view.selected_text(), std::string{});

  // Ctrl+A 全选：selection = 去标记纯文本（长度 ≈ 语义文本）。
  const st::ui::RenderContext context{theme, &stub_port(), 0.0};
  st::ui::Event key;
  key.kind = st::ui::EventKind::KeyDown;
  key.key = "a";
  key.ctrl = true;
  ST_CHECK(view.on_event(context, key));
  const std::string all = view.selected_text();
  ST_REQUIRE(!all.empty());
  ST_CHECK(all.find("霜天 Markdown 视图") != std::string::npos);
  // 控制通道属性面可读回（自动化的判据）。
  ST_CHECK(view.get_property("selection")
               .value_or("") .find("霜天") != std::string::npos);

  // 点击（起点==终点）= 清选择。
  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.button = 1;
  down.position = st::math::Point{40.0f, 30.0f};
  ST_CHECK(view.on_event(context, down));
  st::ui::Event up = down;
  up.kind = st::ui::EventKind::MouseUp;
  ST_CHECK(view.on_event(context, up));
  ST_CHECK_EQ(view.selected_text(), std::string{});

  // 拖选两行：MouseDown → MouseMove → MouseUp → selection 非空且含首行文字。
  ST_CHECK(view.on_event(context, down));
  st::ui::Event move = down;
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{40.0f, 90.0f};
  ST_CHECK(view.on_event(context, move));
  ST_CHECK(view.on_event(context, up));
  const std::string picked = view.selected_text();
  ST_CHECK(!picked.empty());
  ST_CHECK(picked.find("霜天") != std::string::npos);
}

ST_TEST(md_view_selection_highlight_pixels) {
  // 选中行必须**画出来**（像素断言）：同内容、同主题，仅有无选择之别。
  // 两张图必须不同，且差异像素落在行区域内部（不是别处误绘）。
  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::MarkdownView plain_view{std::string(kSample)};
  st::raster::Canvas plain = render(plain_view, theme, kCanvasWidth, kCanvasHeight);

  st::ui::MarkdownView picked_view{std::string(kSample)};
  (void)render(picked_view, theme, kCanvasWidth, kCanvasHeight);
  const st::ui::RenderContext context{theme, &stub_port(), 0.0};
  st::ui::Event key;
  key.kind = st::ui::EventKind::KeyDown;
  key.key = "a";
  key.ctrl = true;
  ST_REQUIRE(picked_view.on_event(context, key));
  st::raster::Canvas picked = render(picked_view, theme, kCanvasWidth, kCanvasHeight);

  std::size_t diff = 0;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (!(plain.pixel_at(x, y) == picked.pixel_at(x, y))) ++diff;
    }
  }
  ST_CHECK(diff > 500);   // 全选高亮是成片的色块，差异不可能是个位数
}

ST_TEST(md_view_link_hit_and_click_callback) {
  // 链接是一等交互（2026-10-13）：命中测试与绘制**同源**（共用 `rows_` 与
  // `Span::link`）；点击回调把 URL 交给宿主（打开浏览器是宿主的事）。
  const st::ui::Theme theme = st::ui::Theme::light();
  constexpr std::string_view kDoc = "看 [霜天文档](https://example.com/docs) 了解。\n";
  st::ui::MarkdownView view{std::string(kDoc)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);
  const st::ui::RenderContext context{theme, &stub_port(), 0.0};

  // 命中测试：链接文字在首行，x 向右扫直到命中（不硬编码像素，避开字体差异）。
  // 用公开面给出的控件原点：内容首行 ≈ `bounds.y`（内边距很小）。
  const float top_y = view.bounds().y + 6.0f;
  std::string hit;
  for (float x = 0.0f; x < 400.0f && hit.empty(); x += 2.0f) {
    hit = view.link_at(st::math::Point{x, top_y});
  }
  ST_CHECK_EQ(hit, std::string("https://example.com/docs"));
  // 同一行里**非链接区域**不应误报（命中测试要真的看 x 区间）。
  bool false_positive = false;
  for (float x = 380.0f; x < 600.0f; x += 4.0f) {
    if (!view.link_at(st::math::Point{x, top_y}).empty()) {
      false_positive = true;
    }
  }
  ST_CHECK(!false_positive);

  // 点击回调：真实鼠标事件路径（MouseDown）应上报 URL。
  std::string clicked;
  view.on_link_click = [&clicked](std::string_view url) { clicked = std::string(url); };
  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.button = 1;
  float hit_x = 0.0f;
  for (float x = 0.0f; x < 400.0f; x += 2.0f) {
    if (!view.link_at(st::math::Point{x, top_y}).empty()) {
      hit_x = x;
      break;
    }
  }
  down.position = st::math::Point{view.bounds().x + hit_x, top_y};
  ST_CHECK(view.on_event(context, down));
  ST_CHECK_EQ(clicked, std::string("https://example.com/docs"));
}

ST_TEST(md_view_code_block_hit) {
  // 代码块命中（用于宿主挂复制按钮）：返回该块的首行下标，非代码区域返回 -1。
  const st::ui::Theme theme = st::ui::Theme::light();
  constexpr std::string_view kDoc = R"MD(普通段落。

```cpp
int main() { return 0; }
int other() { return 1; }
```

后续段落。
)MD";
  st::ui::MarkdownView view{std::string(kDoc)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);

  // 扫描整列 y 找代码块命中：至少命中一次，且命中的是**同一个首行下标**
  //（同一块内的不同行走同一条归并结果）。
  std::set<int> hits;
  for (float y = view.bounds().y; y < view.bounds().y + view.content_height(); y += 3.0f) {
    const int first = view.code_block_at(st::math::Point{20.0f, y});
    if (first >= 0) hits.insert(first);
  }
  ST_CHECK(!hits.empty());
  ST_CHECK_EQ(hits.size(), std::size_t{1});   // 同一个块 → 同一个首行
}

ST_TEST(md_view_table_cells_wrap_instead_of_ellipsize) {
  // 表格硬伤的回归（2026-10-13）：旧实现把列**等比缩放**再 ellipsize，
  // 于是长表格每格都以 `...` 结尾（用户：“截断是灾难级的”）。
  //
  // 新口径分三层，各自都是缺陷的指纹：
  // ① 列宽按“内容权重 + 最长不可断单元保底”分配；
  // ② 单元格内**折行**（内容不丢）；
  // ③ 断行**不在词内**（`batterycenter/embed` 不被硬断）。
  //
  // 反向验证：把 `port.wrap` 换回单行 `ellipsize`，本用例的完整性断言变红。
  const st::ui::Theme theme = st::ui::Theme::light();
  // 造一张**宽窄差异极大**的表（正是本仓 README 的形态）：
  // 左列短关键词（含一个长包名）、右列长描述。
  constexpr std::string_view kDoc =
      "| 依赖 | 用途 | 性质 |\n"
      "|---|---|---|\n"
      "| batterycenter/embed 1.2.19 | 编译期资源嵌入（生成期由 stpm 原生实现，不引入 CMake） | 可选 |\n"
      "| Windows | 自绘一切，不依赖系统控件；DPI 变化时自动重建字形缓存与布局 | 必需 |\n";
  st::ui::MarkdownView view{std::string(kDoc)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);

  // 从渲染后的排版里取出表格单元格文字（语义通道不含 cell 细节，
  // 用 `selection` 全选来拿渲染的纯文本——它按行拼接）。
  // ⚠ 单元格折行后，同一格的多行在 `selected_text()` 里以**空格**相连；
  // 而字可能恰好断在 token 中间——所以先把空白全部去掉再查，
  // 这样既能验“内容没被砍”，也不依赖折行位置。
  ST_CHECK(view.invoke_action("select_all", {}));
  const std::string rendered = view.selected_text();
  std::string squashed;
  for (const char raw : rendered) {
    if (raw != ' ' && raw != '\n' && raw != '\t') squashed += raw;
  }
  for (const std::string token : {"batterycenter/embed1.2.19",
                                  "编译期资源嵌入（生成期由stpm原生实现，不引入CMake）",
                                  "DPI变化时自动重建字形缓存与布局"}) {
    ST_CHECK(squashed.find(token) != std::string::npos);
  }
  // 不应出现省略号（表格单元格的省略是旧缺陷的指纹）。
  ST_CHECK(rendered.find("...") == std::string::npos);
}

ST_TEST(md_view_table_keeps_long_words_intact) {
  // **词内不断**的回归（2026-10-13）：旧实现把“前一个字符是 CJK”的任意位置
  // 都当成可断点，且列宽保底用过小的常数——实测 `batterycenter/embed` 被
  // 硬断成 `em|bed`、`nlohmann/jso|n`（截图取证）。
  //
  // 判据用**渲染后的表格行**：把 `selection` 里同一格的多行拼回去后，
  // 包名必须完整。这一条与“列宽保底用最长不可断单元”是同一个事实。
  const st::ui::Theme theme = st::ui::Theme::light();
  constexpr std::string_view kDoc =
      "| 依赖 | 说明 |\n"
      "|---|---|\n"
      "| batterycenter/embed 1.2.19 | 短说明 |\n"
      "| nlohmann/json 3.12.0 | 短说明 |\n";
  st::ui::MarkdownView view{std::string(kDoc)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);
  ST_CHECK(view.invoke_action("select_all", {}));
  const std::string rendered = view.selected_text();
  std::string squashed;
  for (const char raw : rendered) {
    if (raw != ' ' && raw != '\n' && raw != '\t') squashed += raw;
  }
  // 包名完整（不被断在词中 / 不被截断）。
  ST_CHECK(squashed.find("batterycenter/embed") != std::string::npos);
  ST_CHECK(squashed.find("nlohmann/json") != std::string::npos);
}

ST_TEST(md_view_table_wrap_keeps_all_cells_content) {
  // 单元格折行的**完整性**回归：旧实现把列等比缩放再 `ellipsize`，
  // 于是长表格每格都以 `...` 结尾（用户：“截断是灾难级的”）。
  // 现在内容一字不丢（折行承担溢出）。
  //
  // 反向验证：把 `port.wrap` 换回单行 `ellipsize`，本用例变红（已验）。
  const st::ui::Theme theme = st::ui::Theme::light();
  // 造一张**宽窄差异极大**的表（正是本仓 README 的形态）：长描述列 + 短关键词列。
  constexpr std::string_view kDoc =
      "| 依赖 | 用途 |\n"
      "|---|---|\n"
      "| batterycenter/embed 1.2.19 | 编译期资源嵌入（生成期由 stpm 原生实现，不引入 CMake） |\n";
  st::ui::MarkdownView view{std::string(kDoc)};
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);
    ST_CHECK(view.invoke_action("select_all", {}));
  const std::string rendered = view.selected_text();
  std::string squashed;
  for (const char raw : rendered) {
    if (raw != ' ' && raw != '\n' && raw != '\t') squashed += raw;
  }
  ST_CHECK(squashed.find("batterycenter/embed1.2.19") != std::string::npos);
}

ST_TEST(md_view_copy_action_prefers_selection) {
  // copy 动作：无选择 → 拷**原文**（源码）；有选择 → 拷**选中纯文本**。
  // 用 HostFocus 的剪贴板桩接住写入值（“谁把什么传给了剪贴板”）。
  struct ClipboardHost : st::ui::Element::HostFocus {
    auto set_keyboard_focus(st::ui::Element*) -> bool override { return false; }
    auto host_clipboard_text() -> std::string override { return {}; }
    auto host_set_clipboard_text(std::string_view text) -> bool override {
      if (written.empty()) written = std::string(text);
      return true;
    }
    std::string written{};
  };

  const st::ui::Theme theme = st::ui::Theme::light();
  st::ui::MarkdownView view{std::string(kSample)};
  ClipboardHost host;
  view.set_host(&host);
  (void)render(view, theme, kCanvasWidth, kCanvasHeight);

  // ① 无选择：拷原文（含 Markdown 标记，如 `# `）。
  ST_CHECK(view.invoke_action("copy", {}));
  ST_REQUIRE(!host.written.empty());
  ST_CHECK(host.written == view.markdown());
  ST_CHECK(host.written.find("#") != std::string::npos);

  // ② 有选择：拷选中纯文本（无 Markdown 标记的正文行）。
  host.written.clear();
  ST_CHECK(view.invoke_action("select_all", {}));
  ST_CHECK(view.invoke_action("copy", {}));
  ST_REQUIRE(!host.written.empty());
  ST_CHECK(host.written == view.selected_text());
  ST_CHECK(host.written.find("霜天 Markdown 视图") != std::string::npos);
}
