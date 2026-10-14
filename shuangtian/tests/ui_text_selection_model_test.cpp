/// **文本选中与复制**（`st::ui::TextSelectionModel`）的回归测试。
///
/// 背景（2026-10-14）：需求是「框架里**每个**文字渲染组件都要能选中与复制」，
/// 而此前 `CodeEditor` / `MarkdownView` / `Terminal` / `Input` / `Text` 各写一套；
/// 框架里渲染文字的组件有二十多个，每接一个就要把同一批坑重踩一遍。
///
/// **实测代价**：`MarkdownView` 那套把「高亮带的垂直范围」算错两轮
///（先用行高 → 高亮臃肿；后差一个 `padding.top` → 高亮跑到字上方），
/// 而正确几何在 `line_layout.hpp` 里早就有了（`glyph_top`/`glyph_height`）。
///
/// 本组用例钉住**与组件形态无关**的契约（纯函数层见
/// `ui_text_selection_test.cpp`，那里钉端点序/词界/最近码点/切片）。
/// 这里专钉**行为层**：状态机、全选、复制切片、**高亮带几何**。
///
/// ⚠ 桩的 `below` 必须非零：`below == 0` 会把某些公式的差异抹平
///（`ui_text_vertical_center_test.cpp` 记过这个假绿）。
#include "st/test/test.hpp"

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/text/text.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/text_selection.hpp"

namespace {

using st::ui::Caret;
using st::ui::TextSelectionModel;

/// 固定度量的桩；行高 26 **故意远大于**墨迹高 15（above 12 + below 3），
/// 以复现「行距大」的场景（Markdown 正文 1.6 倍行距就是这种）。
class FixedPort final : public st::ui::TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view, float) const -> st::math::Size override {
    return st::math::Size{10.0F, 26.0F};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float,
                                   st::text::FontRole) const -> float override {
    return static_cast<float>(utf8.size()) * 8.0F;
  }
  [[nodiscard]] auto line_height(float) const -> float override { return 20.0F; }
  [[nodiscard]] auto ascent(float) const -> float override { return 16.0F; }
  [[nodiscard]] auto shaped_ascent(std::string_view, float, st::text::FontRole) const
      -> float override {
    return 16.0F;
  }
  [[nodiscard]] auto ink_metrics(std::string_view, float,
                                 st::text::FontRole) const
      -> std::optional<st::ui::TextPort::InkMetrics> override {
    return st::ui::TextPort::InkMetrics{12.0F, 3.0F};   // above + below = 15
  }
  void draw(st::raster::Surface&, std::string_view, st::math::Point, float, st::math::Color,
            st::text::FontRole, float, bool) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const -> std::string override {
    return std::string(utf8);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float,
                                  std::size_t) const -> std::vector<std::string> override {
    return {std::string(utf8)};
  }
};

/// 假组件：3 行文本，行高 26（控件内的行盒高）。
struct Fixture {
  std::vector<std::string> lines{"abc def", "中文行内容", "third"};
  FixedPort port{};
  TextSelectionModel selection;

  Fixture() {
    st::ui::TextSelectionHost hook{};
    hook.port = &port;
    hook.line_count = [this] { return lines.size(); };
    hook.line_text = [this](std::size_t row) -> std::string_view {
      return row < lines.size() ? std::string_view(lines[row]) : std::string_view{};
    };
    hook.hit_test = [this](st::math::Point point) -> Caret {
      const int row = point.y < 26.0F ? 0 : (point.y < 52.0F ? 1 : 2);
      const auto index = static_cast<std::size_t>(row < 0 ? 0 : row);
      const std::size_t limit = index < lines.size() ? lines[index].size() : 0U;
      std::size_t offset = static_cast<std::size_t>((point.x > 0.0F ? point.x : 0.0F) / 8.0F);
      return Caret{row, std::min(offset, limit)};
    };
    hook.span_x = [](std::size_t, std::size_t from, std::size_t to) {
      return std::pair{static_cast<float>(from) * 8.0F, static_cast<float>(to) * 8.0F};
    };
    hook.line_box = [](std::size_t row) {
      return std::pair{static_cast<float>(row) * 26.0F, 26.0F};
    };
    hook.line_font = [](std::size_t) {
      return std::pair{15.0F, st::text::FontRole::Proportional};
    };
    selection.set_host(std::move(hook));
    selection.set_enabled(true);
  }
};

constexpr float kTolerance = 0.01F;

}  // namespace

ST_TEST(text_selection_model_select_all_survives_line_end_sentinel) {
  // ① 全选必须拿到**全文**。
  //
  // 为何单列一条：`select_all` 不能为了拿行尾去折行（`selected_text()` 是 const
  // 无上下文接口），于是末偏移用一个**行尾哨兵**。若把哨兵当普通偏移去
  // `min(offset, size)`，全选拷出来就是**空串**——实测在 `Text` 上踩过。
  Fixture f;
  f.selection.select_all();
  ST_CHECK(f.selection.has_selection());
  ST_CHECK_EQ(f.selection.selected_text(), std::string("abc def\n中文行内容\nthird"));
}

ST_TEST(text_selection_model_joins_lines_with_newline) {
  // ② 跨行复制：行间补 `\n`，行内只取选中区间（不多不少）；反向拖选同结果。
  Fixture f;
  f.selection.set_range(Caret{0, 4U}, Caret{1, 6U});   // "def" … "中文"（2 汉字 = 6 字节）
  ST_CHECK_EQ(f.selection.selected_text(), std::string("def\n中文"));
  f.selection.set_range(Caret{1, 6U}, Caret{0, 4U});   // 反向
  ST_CHECK_EQ(f.selection.selected_text(), std::string("def\n中文"));
}

ST_TEST(text_selection_model_band_uses_ink_not_line_box) {
  // ③ **高亮带走墨迹带，不走行盒**——用户两次报「高亮对不上」就是这条。
  //
  // 行盒 26、墨迹 15。用行盒的实现高亮会是 26（臃肿/错位）；
  // 正确是 15，且在该行内**居中**（上下留白相等 5.5）。
  Fixture f;
  f.selection.set_range(Caret{0, 0U}, Caret{0, 3U});
  const auto bands = f.selection.bands();
  ST_REQUIRE(bands.size() == 1U);
  ST_CHECK_NEAR(bands[0].height, 15.0F, kTolerance);
  ST_CHECK_NEAR(bands[0].top, 5.5F, kTolerance);
  ST_CHECK(bands[0].height < 26.0F - kTolerance);   // 绝不是行盒高
  // x 区间来自组件的 span_x（这里 0..3 字节 ⇒ 0..24px）。
  ST_CHECK_NEAR(bands[0].x0, 0.0F, kTolerance);
  ST_CHECK_NEAR(bands[0].x1, 24.0F, kTolerance);
}

ST_TEST(text_selection_model_empty_line_is_visible) {
  // ④ 跨行选中中间的空行时至少画一个可见窄块——
  // 否则“选了几行却有一行什么都没高亮”，看着像漏画。
  Fixture f;
  f.lines = {"a", "", "b"};
  f.selection.set_range(Caret{0, 0U}, Caret{2, 1U});
  const auto bands = f.selection.bands();
  ST_REQUIRE(bands.size() == 3U);
  ST_CHECK(bands[1].x1 > bands[1].x0);
  ST_CHECK(bands[1].height > 0.0F);
}

ST_TEST(text_selection_model_drag_state_machine) {
  // ⑤ 拖选状态机：按下开选、移动扩选、抬起结束；未启用时一律不理会。
  Fixture f;
  ST_CHECK(f.selection.on_press(st::math::Point{8.0F, 10.0F}, false));
  ST_CHECK(f.selection.selecting());
  ST_CHECK(!f.selection.has_selection());            // 只按下还没拖 ⇒ 无选区
  ST_CHECK(f.selection.on_drag(st::math::Point{48.0F, 10.0F}));
  ST_CHECK(f.selection.has_selection());
  // x=8 → 偏移 1（"b" 前）、x=48 → 偏移 6：区间是 "bc de"（**字符级**，不是整词）。
  ST_CHECK_EQ(f.selection.selected_text(), std::string("bc de"));
  ST_CHECK(f.selection.on_release());
  ST_CHECK(!f.selection.selecting());
  ST_CHECK(!f.selection.on_drag(st::math::Point{0.0F, 40.0F}));   // 抬起后不再扩选

  Fixture off;
  off.selection.set_enabled(false);
  ST_CHECK(!off.selection.on_press(st::math::Point{8.0F, 10.0F}, false));
  off.selection.select_all();
  ST_CHECK(!off.selection.has_selection());          // 未启用 ⇒ 全选也不生效
}

ST_TEST(text_selection_model_word_and_line_and_keys) {
  // ⑥ 双击选词 / 三击整行 / `Ctrl+A` / `Esc` 的契约。
  Fixture f;
  // 双击落在 "abc" 内 ⇒ 选中整个词（"def" 是另一个词）。
  ST_CHECK(f.selection.select_word_at(st::math::Point{8.0F, 10.0F}));
  ST_CHECK_EQ(f.selection.selected_text(), std::string("abc"));
  // 三击 ⇒ 整行（末偏移哨兵解析为行尾）。
  ST_CHECK(f.selection.select_line_at(st::math::Point{8.0F, 10.0F}));
  ST_CHECK_EQ(f.selection.selected_text(), std::string("abc def"));
  // `Ctrl+A` ⇒ 全选；`Esc` ⇒ 清选；无选区时 `Esc` 放行（不吞键）。
  ST_CHECK(f.selection.on_key("a", true));
  ST_CHECK(f.selection.has_selection());
  ST_CHECK(f.selection.on_key("Escape", false));
  ST_CHECK(!f.selection.has_selection());
  ST_CHECK(!f.selection.on_key("Escape", false));
}
