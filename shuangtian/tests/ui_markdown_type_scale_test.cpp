/// Markdown **各格式字号**必须同源于正文基准（不是只有正文/标题跟着走）。
///
/// ## 缺陷现场（用户：“markdown 各种格式的文本渲染字体大小差距过大”）
///
/// 旧实现是**两套口径**：
///
/// | 谁 | 字号来自 | 随调用方的正文基准变化？ |
/// |---|---|---|
/// | 正文 / 标题 | `base × 阶梯` | ✅ |
/// | 行内码 / 代码块 | 主题绝对档 `font_sm = 14` | ❌ |
/// | 表格 | 主题绝对档 `font_xs = 12` | ❌ |
/// | 语言标签 | 字面量 `11` | ❌ |
///
/// 于是同一份文档在不同宿主里**层级关系相反**（`md_type_scale_probe` 实测）：
///
/// | 正文基准 | 行内码/代码 | 表格 | 语言标签 |
/// |---|---|---|---|
/// | 10px（工具卡） | **1.40×（比正文还大）** | 1.20× | 1.10× |
/// | 15px（界面正文） | 0.93× | 0.80× | 0.73× |
/// | 26px（阅读模式） | 0.54×（**只有一半**） | 0.46× | 0.42× |
///
/// 另一处同源缺陷：**行内码取全局常量**，所以标题里的 `code` 掉回正文字号——
/// 26px 正文下 H1 内码 / H1 = **0.40**（标题里嵌了一行小字）。
///
/// ## 本文件钉什么
///
/// 用 `RecordingTextPort` 记录**每段文字实际绘制时的字号**（不是布局高度、不是属性面，
/// 就是绘制调用里的那个数——这正是用户眼睛看到的东西），断言：
///
/// 1. **比值恒定**：每个格式 / 正文 的比值跨 10~26px 不漂（≤ 0.02）；
/// 2. **随基准单调**：每个格式的字号都随基准严格增大（钉死“固定绝对档”）；
/// 3. **标题内码**跟随所在标题（≥ 0.82×）——不许大字里嵌小字；
/// 4. **字不高于行盒**：任何一次绘制都不允许字号 > 所在行盒高（挤行的直接原因）；
/// 5. **`base == 14` 时观感不变**：行内码/代码/表格 = 13.3、语言标签 = 11.2（锚点保护）。
///
/// 3 与 4 是本次改动**顺带修掉**的同类缺陷，一起钉住——它们与 1 是同一个根因
/// （“字号只有一半路径跟着派生”），分开修必然再漂。
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/test/test.hpp"

#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::text::FontRole;
using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;
using st::ui::TextPort;
using st::test::ProportionalTextPort;

constexpr float kWidth = 900.0f;

/// 每类结构带一个唯一标记词（认领绘制字号用）。
constexpr std::string_view kSample = R"MD(# H1标题

###### H6标题

正文样本

行内样本`行内码`结束

- 无序项

> 引用项

```cpp
代码行
```

| 表头列 | 第二列 |
| --- | --- |
| 单元列 | 3 |
)MD";

/// 标题里含行内码（两个标题各用一个标记词，否则认领只拿得到第一处）。
constexpr std::string_view kHeadingInlineSample = R"MD(# 大标题`码甲`

###### 末级标题`码乙`
)MD";

/// 一次 `draw` 的记录：文字、字号、绘制原点 y。
struct DrawCall {
  std::string text{};
  float size{0.0f};
  float y{0.0f};
};

/// 记录型端口：度量转发给 `ProportionalTextPort`（真几何），`draw` 只留记录。
class ProbePort final : public TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   FontRole role = FontRole::Proportional) const
      -> float override {
    return inner_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner_.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, FontRole role = FontRole::Proportional, float embolden = 0.0f,
            bool bold = false) const override {
    calls.push_back(DrawCall{std::string(utf8), size, origin.y});
    inner_.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return inner_.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return inner_.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    return inner_.wrap_limited(utf8, size, max_width, max_lines);
  }

  mutable std::vector<DrawCall> calls{};

 private:
  ProportionalTextPort inner_{};
};

/// 一个基准下量到的整套字号。
struct Sizes {
  float body{0.0f};
  float h1{0.0f};
  float h6{0.0f};
  float inline_code{0.0f};
  float code{0.0f};
  float table{0.0f};
  float language{0.0f};
  float heading_inline_h1{0.0f};
  float heading_inline_h6{0.0f};
  std::size_t size_over_row_height{0};
};

[[nodiscard]] auto size_of(const std::vector<DrawCall>& calls, std::string_view needle)
    -> std::optional<float> {
  for (const DrawCall& call : calls) {
    if (call.text.find(needle) != std::string::npos) return call.size;
  }
  return std::nullopt;
}

[[nodiscard]] auto constraints() -> Constraints {
  Constraints c;
  c.max_width = kWidth;
  c.max_height = 4000.0f;
  c.available_width = kWidth;
  c.available_height = 4000.0f;
  return c;
}

/// 排一次 + 画一次，把所有绘制记录攒下来。
[[nodiscard]] auto paint_sample(float base, std::string_view source) -> std::vector<DrawCall> {
  const Theme theme = Theme::light();
  ProbePort port;
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(source));
  view.apply_theme(theme);
  const RenderContext context{theme, &port, 0.0};
  view.measure(context, constraints());
  view.arrange(context, st::math::Rect{0.0f, 0.0f, kWidth, 4000.0f});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(900, 4000, 1.0f);
  view.paint(context, canvas);
  return port.calls;
}

/// 行盒高度清单（内容坐标 y + 高），从组件自报的 `row_at_probe` 解析。
///
/// 为何读属性面而不另算：`row_at_probe` 是组件自报的排版结果，
/// 另算一套就变成“测试与实现两套事实”（本仓踩过：探针与应用不共享同一份实现）。
[[nodiscard]] auto parse_row_boxes(const MarkdownView& view) -> std::vector<std::pair<float, float>> {
  std::vector<std::pair<float, float>> boxes;
  const std::string text = view.get_property("row_at_probe").value_or(std::string{});
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string_view line{text.data() + start, end - start};
    const std::size_t at = line.find("box=(");
    if (at != std::string_view::npos) {
      const std::size_t close = line.find(')', at);
      if (close != std::string_view::npos) {
        const std::string nums{line.substr(at + 5, close - at - 5)};
        std::array<float, 4> values{};
        std::size_t field = 0;
        std::size_t index = 0;
        while (field < 4U) {
          const std::size_t comma = nums.find(',', index);
          const std::size_t stop = comma == std::string::npos ? nums.size() : comma;
          values[field] = std::stof(nums.substr(index, stop - index));
          ++field;
          index = stop + 1;
        }
        boxes.emplace_back(values[1], values[3]);
      }
    }
    start = end + 1;
  }
  return boxes;
}

[[nodiscard]] auto measure_at(float base) -> Sizes {
  const std::vector<DrawCall> calls = paint_sample(base, kSample);
  Sizes sizes;
  sizes.body = size_of(calls, "正文样本").value_or(0.0f);
  sizes.h1 = size_of(calls, "H1标题").value_or(0.0f);
  sizes.h6 = size_of(calls, "H6标题").value_or(0.0f);
  sizes.inline_code = size_of(calls, "行内码").value_or(0.0f);
  sizes.code = size_of(calls, "代码行").value_or(0.0f);
  sizes.table = size_of(calls, "表头列").value_or(0.0f);
  sizes.language = size_of(calls, "cpp").value_or(0.0f);
  const std::vector<DrawCall> heading_calls = paint_sample(base, kHeadingInlineSample);
  sizes.heading_inline_h1 = size_of(heading_calls, "码甲").value_or(0.0f);
  sizes.heading_inline_h6 = size_of(heading_calls, "码乙").value_or(0.0f);

  // 字号 > 所在行盒高 ⇒ 字形溢出行带（挤行）。
  const Theme theme = Theme::light();
  ProbePort port;
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(kSample));
  view.apply_theme(theme);
  const RenderContext context{theme, &port, 0.0};
  view.measure(context, constraints());
  view.arrange(context, st::math::Rect{0.0f, 0.0f, kWidth, 4000.0f});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(900, 4000, 1.0f);
  view.paint(context, canvas);
  const auto boxes = parse_row_boxes(view);
  for (const DrawCall& call : port.calls) {
    for (const auto& [y, height] : boxes) {
      if (call.y >= y - 0.5f && call.y < y + height + 0.5f) {
        if (height > 0.0f && call.size > height) ++sizes.size_over_row_height;
        break;
      }
    }
  }
  return sizes;
}

/// 真实调用方用到的正文基准（`apps/xiaobai` 10/11/13、`apps/gbcode` 15、阅读 20）。
constexpr std::array<float, 6> kBases{10.0f, 11.0f, 12.0f, 13.0f, 20.0f, 26.0f};

}  // namespace

ST_TEST(md_type_sizes_keep_constant_ratio_to_body) {
  // **核心回归**：每个格式与正文的比值跨字号不漂。
  //
  // 缺陷实现下（行内码/代码/表格/语言标签取主题绝对档）同一列会从 1.40 漂到 0.42，
  // 这条立刻变红。
  for (const float base : kBases) {
    const Sizes sizes = measure_at(base);
    ST_REQUIRE(sizes.body > 0.0f);
    ST_CHECK(sizes.h1 / sizes.body > 1.30f && sizes.h1 / sizes.body < 1.40f);
    ST_CHECK(sizes.h6 / sizes.body > 0.98f && sizes.h6 / sizes.body < 1.02f);
    ST_CHECK(sizes.inline_code / sizes.body > 0.93f && sizes.inline_code / sizes.body < 0.97f);
    ST_CHECK(sizes.code / sizes.body > 0.93f && sizes.code / sizes.body < 0.97f);
    ST_CHECK(sizes.table / sizes.body > 0.93f && sizes.table / sizes.body < 0.97f);
    ST_CHECK(sizes.language / sizes.body > 0.78f && sizes.language / sizes.body < 0.82f);
  }
}

ST_TEST(md_type_sizes_scale_with_base) {
  // **每个格式都必须随基准增大**（钉死“固定绝对字号”这一形态）。
  const Sizes small = measure_at(11.0f);
  const Sizes large = measure_at(22.0f);
  const auto grew = [](float a, float b) { return b > a + 0.4f; };
  ST_CHECK(grew(small.body, large.body));
  ST_CHECK(grew(small.h1, large.h1));
  ST_CHECK(grew(small.inline_code, large.inline_code));
  ST_CHECK(grew(small.code, large.code));
  ST_CHECK(grew(small.table, large.table));
  ST_CHECK(grew(small.language, large.language));
}

ST_TEST(md_heading_inline_code_follows_heading_size) {
  // **标题里的行内码按标题派生**（缺陷实现下它恒等于正文字号：
  // 26px 正文时 H1 内码/H1 = 0.40，标题里嵌了一行小字）。
  for (const float base : kBases) {
    const Sizes sizes = measure_at(base);
    ST_REQUIRE(sizes.h1 > 0.0f);
    ST_CHECK(sizes.heading_inline_h1 / sizes.h1 > 0.82f);
    ST_CHECK(sizes.heading_inline_h6 / sizes.h6 > 0.82f);
    // 同一份基准下两处的比值必须一致（同一口径，不是一个按标题一个按正文）。
    ST_CHECK(sizes.heading_inline_h1 / sizes.h1 - sizes.heading_inline_h6 / sizes.h6 < 0.02f);
  }
}

ST_TEST(md_type_sizes_never_exceed_row_height) {
  // **字不许比行盒高**：纵向度量（行高/内边距/项目符号）必须跟着字号一起派生。
  //
  // 缺陷形态：字号放大而行高固定 ⇒ 字形溢出行带、相邻行粘连。
  for (const float base : {10.0f, 13.0f, 20.0f, 26.0f}) {
    const Sizes sizes = measure_at(base);
    ST_CHECK_EQ(sizes.size_over_row_height, std::size_t{0});
  }
}

ST_TEST(md_type_sizes_are_unchanged_at_anchor) {
  // **锚点保护**：`base == 14` 时派生值与旧观感一致（行内码/代码/表格 13.3、
  // 语言标签 11.2）。防的是“统一派生时顺手改了 14px 这一档的观感”。
  const Sizes sizes = measure_at(14.0f);
  ST_REQUIRE(sizes.body > 0.0f);
  ST_CHECK(sizes.body > 13.99f && sizes.body < 14.01f);
  ST_CHECK(sizes.code > 13.29f && sizes.code < 13.31f);          // 14 × 0.95
  ST_CHECK(sizes.table > 13.29f && sizes.table < 13.31f);
  ST_CHECK(sizes.language > 11.19f && sizes.language < 11.21f);  // 14 × 0.80
}
