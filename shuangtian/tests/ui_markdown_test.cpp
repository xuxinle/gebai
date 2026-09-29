#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 测试用文本端口：不做字形光栅化，把每次 `draw` 落成**同色实心块**（宽度按码点估算）。
/// 目的：像素断言与系统字体无关、完全确定 —— 断言验证的是**组件向文本端口提交的颜色与布局**，
/// 而不是字形覆盖率（字形的正确性由 text 层单测覆盖）。
/// 折行由组件自研（不依赖端口 `wrap`），故此处 `wrap` 仅按显式换行切分。
class StubTextPort final : public st::ui::TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return st::math::Size{measure_width(utf8, size), line_height(size)};
  }

  [[nodiscard]] auto measure_width(std::string_view utf8, float size) const -> float override {
    float width = 0.0f;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const st::Codepoint codepoint = st::decode_utf8(utf8, index);
      width += codepoint.value < 0x80U ? size * 0.55f : size;
    }
    return width;
  }

  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45f; }

  void draw(st::raster::Canvas& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color) const override {
    if (utf8.empty()) return;
    // 落成**整像素**矩形：端口的输出与坐标的像素对齐方式无关，断言只关心颜色与位置
    const float x = std::round(origin.x);
    const float y = std::round(origin.y + size * 0.25f);
    const float width = std::max(std::round(measure_width(utf8, size)), 1.0f);
    const float height = std::max(std::round(size * 0.7f), 1.0f);
    canvas.fill_rect(st::math::Rect{x, y, width, height}, st::raster::Paint::solid(color));
  }

  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    if (measure_width(utf8, size) <= max_width) return std::string(utf8);
    const float ellipsis_width = size;  // 「…」按一个全角宽度计
    std::string out;
    float width = 0.0f;
    std::size_t index = 0;
    while (index < utf8.size()) {
      const std::size_t start = index;
      const st::Codepoint codepoint = st::decode_utf8(utf8, index);
      const float char_width = codepoint.value < 0x80U ? size * 0.55f : size;
      if (width + char_width + ellipsis_width > max_width) break;
      out.append(utf8.substr(start, index - start));
      width += char_width;
    }
    out.append("…");
    return out;
  }

  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin <= utf8.size()) {
      std::size_t end = utf8.find('\n', begin);
      if (end == std::string_view::npos) end = utf8.size();
      lines.push_back(utf8.substr(begin, end - begin));
      if (end >= utf8.size()) break;
      begin = end + 1;
    }
    return lines;
  }

  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override {
    std::vector<std::string> out;
    for (const std::string_view line : wrap(utf8, size, max_width)) {
      if (out.size() >= max_lines) break;
      out.emplace_back(line);
    }
    if (out.empty()) out.emplace_back();
    return out;
  }
};

[[nodiscard]] auto stub_port() -> const StubTextPort& {
  static const StubTextPort port;
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
