/// 行尾不许凭空出现省略号：**排版算的行宽必须与绘制真实宽度同一口径**。
///
/// ## 缺陷现场（用户：「怎么缩略了」，附截图里那段末尾多了一个 `…`）
///
/// `draw_spans` 有一条注释写着“量宽与绘制**必须同一角色**”，但排版期
/// （`push_spans`）违反了它三处，而这三处会叠加成同一个可见症状——**行尾被截断**：
///
/// | 漏算的东西 | 量级（实测，13.3px） |
/// |---|---|
/// | 行内码按**比例**字体量、却按**等宽**字体画 | `inline_code` 11 字符差 **+8.12px** |
/// | 行内码芯片的左右内边距（绘制时向外撑 2×pad） | 每片段 +**7.6px** |
/// | 芯片内边距没算进“剩余可用宽” | 同量 |
///
/// 三者叠加后，一行会**先被判成装得下、再被画成越界**，于是 `ellipsize` 收尾加 `…`——
/// 内容本来还有，只是被那个省略号吃掉了。
///
/// ## 本文件钉什么
///
/// 用 `RecordingPort` 记录**每一次绘制**的 `(文本, x, 角色, 字号)`，再用同一端口
/// 反算它的真实宽度，断言：
///
/// 1. **任何绘制都不越过所在行盒的右缘**（这就是“会不会被截断”的直接判据）；
/// 2. **一次省略号截断都不许发生**（含 `…` 的绘制调用数 = 0）；
/// 3. 行内码确实以**等宽**角色绘制（否则说明之前那半截又退回去了）。
///
/// ## 量尺自检（第一版就踩到）
///
/// 行盒（`row_at_probe`）是**内容坐标**，而 `draw` 的 `origin` 是**绝对坐标**。
/// 两者混算会凭空差一个 padding——第一版把“+2.35px 余量”读成了“−17.65px 溢出”，
/// 差点据此又改错一轮。本测试**把 padding 归零**消除这条缝，并断言“量到的绘制条数 ≥ 5”
/// （否则“一条都没量到”会被当成“全都没溢出”）。
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

constexpr float kWidth = 760.0f;

/// 用户截图里那段（逐字复刻）：行内码 + 加粗 + 斜体 + 链接 + 长句折行。
constexpr std::string_view kSample =
    "正文里混着 `行内码 inline_code`、**加粗**、*斜体*、[链接](https://example.com) 与中文标点，"
    "还有一行足够长的句子用来触发自动折行，方便看行距与折行后的节奏是否统一。\n";

struct DrawCall {
  std::string text{};
  float x{0.0f};
  float y{0.0f};
  float size{0.0f};
  FontRole role{FontRole::Proportional};
};

/// 记录型端口：度量转发给 `ProportionalTextPort` 并**保留角色差**。
///
/// ⚠ 桩必须复现“等宽比比例宽”这条真实行为——否则“排版按比例量、绘制按等宽画”
/// 这个缺陷在桩上会互相抵消，测试恒绿（本仓 §7.2 的“桩把差异抹平了”）。
class ProbePort final : public TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return inner_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   FontRole role = FontRole::Proportional) const
      -> float override {
    const float base = inner_.measure_width(utf8, size, FontRole::Proportional);
    // 等宽 = 比例 × 1.12（真实字体实测 74.69 / 66.57 ≈ 1.12）
    return role == FontRole::Monospace ? base * 1.12f : base;
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return inner_.line_height(size);
  }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color, FontRole role = FontRole::Proportional, float embolden = 0.0f,
            bool bold = false) const override {
    calls.push_back(DrawCall{std::string(utf8), origin.x, origin.y, size, role});
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

/// 行盒（内容坐标）：y / 高 / 右缘。
struct Box {
  float y{0.0f};
  float height{0.0f};
  float right{0.0f};
};

[[nodiscard]] auto parse_boxes(const std::string& text) -> std::vector<Box> {
  std::vector<Box> boxes;
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
        boxes.push_back(Box{values[1], values[3], values[0] + values[2]});
      }
    }
    start = end + 1;
  }
  return boxes;
}

/// 一次测量 + 绘制的完整记录。
struct Run {
  std::vector<DrawCall> calls{};
  std::vector<Box> boxes{};
  float worst_slack{0.0f};
  std::size_t measured{0};
  std::size_t truncated{0};
  std::size_t mono_draws{0};
};

[[nodiscard]] auto run_at(float base) -> Run {
  const Theme theme = Theme::light();
  ProbePort port;
  MarkdownView view;
  view.set_shrink_to_fit(false);
  view.set_base_font_size(base);
  view.set_markdown(std::string(kSample));
  view.apply_theme(theme);
  // **内边距归零**：行盒是内容坐标、绘制 origin 是绝对坐标，混算会凭空差一个 padding
  //（探针第一版把 +2.35px 余量读成了 −17.65px 溢出）。
  view.style().padding = st::math::Insets::all(0.0f);
  const RenderContext context{theme, &port, 0.0};
  Constraints constraints;
  constraints.max_width = kWidth;
  constraints.available_width = kWidth;
  constraints.max_height = 4000.0f;
  constraints.available_height = 4000.0f;
  view.measure(context, constraints);
  view.arrange(context, st::math::Rect{0.0f, 0.0f, kWidth, 4000.0f});
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(760, 4000, 1.0f);
  view.paint(context, canvas);

  Run run;
  run.calls = port.calls;
  run.boxes = parse_boxes(view.get_property("row_at_probe").value_or(""));
  run.worst_slack = 1e9f;
  for (const DrawCall& call : run.calls) {
    const float right = call.x + port.measure_width(call.text, call.size, call.role);
    if (call.role == FontRole::Monospace) ++run.mono_draws;
    if (call.text.find("…") != std::string::npos) ++run.truncated;
    const Box* owner = nullptr;
    for (const Box& box : run.boxes) {
      if (call.y >= box.y - 0.5f && call.y < box.y + box.height + 0.5f) {
        owner = &box;
        break;
      }
    }
    if (owner == nullptr) continue;
    run.worst_slack = std::min(run.worst_slack, owner->right - right);
    ++run.measured;
  }
  return run;
}

}  // namespace

ST_TEST(md_inline_code_never_truncates_line_tail) {
  // **核心回归**：行尾不许被截成省略号。
  //
  // 缺陷实现下这一行会溢出 ~15px 并被 `ellipsize` 收尾（用户截图里那个 `…`），
  // 三条断言同时变红。
  for (const float base : {11.0f, 13.0f, 16.0f, 20.0f}) {
    const Run run = run_at(base);
    // 量尺自检：一条都没量到就不能报“没有溢出”。
    ST_REQUIRE(run.measured >= 5U);
    ST_CHECK_EQ(run.truncated, std::size_t{0});
    ST_CHECK(run.worst_slack >= -0.5f);
  }
}

ST_TEST(md_inline_code_is_measured_in_monospace_role) {
  // 行内码必须**以等宽角色绘制**——它的宽度也必须按同一角色计入折行。
  //
  // 这条钉住“排版与绘制同角色”的另一半：绘制侧本来就对，但排版侧曾经按比例量，
  // 于是“画得比算得宽”必然溢出。
  const Run run = run_at(13.0f);
  ST_CHECK(run.mono_draws > 0U);
  // 行内码那段文字必须出现在 mono 绘制里（不是被当成正文画掉的）。
  bool saw_inline_code_as_mono = false;
  for (const DrawCall& call : run.calls) {
    if (call.role == FontRole::Monospace && call.text.find("inline_code") != std::string::npos) {
      saw_inline_code_as_mono = true;
    }
  }
  ST_CHECK(saw_inline_code_as_mono);
}

ST_TEST(md_line_fits_with_chip_padding_accounted) {
  // **芯片内边距**也要算进折行：行内码绘制时两侧各撑出一截底色，
  // 排版不扣这份余量就会“以为刚好、实际越界”。
  //
  // 判据取“余量不为负”而不是“余量恰好为某个值”：不同字体的字宽不同，
  // 能表达共同性质的只有符号（不许越界 = 不许为负）。
  const Run run = run_at(13.0f);
  ST_REQUIRE(run.measured > 0U);
  ST_CHECK(run.worst_slack >= -0.5f);
  // 而且余量不该大到离谱（那说明折行提前发生了，行被切得过碎）。
  ST_CHECK(run.worst_slack < 40.0f);
}
