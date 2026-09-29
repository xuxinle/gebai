#include "st/test/test.hpp"

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 等宽假文本端口（8px/码点，行高 1.45×字号）：度量可复现，且实现真实的省略语义。
class TableTestTextPort final : public st::ui::TextPort {
 public:
  static constexpr float kAdvance{8.0f};

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return st::math::Size{measure_width(utf8, size), line_height(size)};
  }

  [[nodiscard]] auto measure_width(std::string_view utf8, float size) const -> float override {
    (void)size;
    return kAdvance * static_cast<float>(st::utf8_length(utf8));
  }

  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45f; }

  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float size,
            st::math::Color color) const override {
    (void)canvas;
    (void)origin;
    (void)size;
    (void)color;
    drawn_.emplace_back(utf8);
  }

  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    if (max_width <= 0.0f) return {};
    if (measure_width(utf8, size) <= max_width) return std::string(utf8);
    const auto capacity = static_cast<std::size_t>(max_width / kAdvance);
    if (capacity <= 1U) return std::string("…");
    return std::string(st::utf8_slice(utf8, 0, capacity - 1U)) + "…";
  }

  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    (void)size;
    (void)max_width;
    return {utf8};
  }

  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    (void)size;
    (void)max_width;
    (void)max_lines;
    return {std::string(utf8)};
  }

  [[nodiscard]] auto drawn() const -> const std::vector<std::string>& { return drawn_; }

 private:
  mutable std::vector<std::string> drawn_{};
};

[[nodiscard]] auto contains(const std::vector<std::string>& items, std::string_view needle) -> bool {
  for (const std::string& item : items) {
    if (item == needle) return true;
  }
  return false;
}

[[nodiscard]] auto contains_ellipsis(const std::vector<std::string>& items) -> bool {
  for (const std::string& item : items) {
    if (item.find("…") != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// —— ① 行列布局（bounds 逐个断言）+ 斑马纹像素 + 超长省略 ——

ST_TEST(ui_table_layout_zebra_and_ellipsis) {
  st::ui::Theme theme = st::ui::Theme::light();
  TableTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Table table;
  table.set_columns({{"名称", 120.0f, st::ui::TextAlign::Start},
                     {"说明", 0.0f, st::ui::TextAlign::End}});
  const std::string long_text(40, 'x');
  table.add_row({long_text, "ok"});
  table.add_row({"beta", "2"});
  table.set_zebra(true);
  table.set_row_height(32.0f);
  table.apply_theme(theme);

  ST_CHECK_EQ(table.row_count(), 2U);
  ST_CHECK_EQ(table.column_count(), 2U);
  ST_CHECK_EQ(table.zebra(), true);
  ST_CHECK_NEAR(table.row_height(), 32.0f, 0.01f);
  ST_CHECK_EQ(table.cell(1U, 0U), std::string_view{"beta"});
  ST_CHECK_EQ(std::string{st::ui::to_string(table.role())}, std::string{"table"});
  ST_CHECK_EQ(table.semantics_value(), std::string{"2x2"});

  table.arrange(context, st::math::Rect{0.0f, 0.0f, 400.0f, 100.0f});

  // 列宽：固定 120 + 自适应 280（恰好填满容器）
  ST_CHECK_NEAR(table.column_width(0U), 120.0f, 0.01f);
  ST_CHECK_NEAR(table.column_width(1U), 280.0f, 0.01f);
  ST_CHECK_NEAR(table.total_width(), 400.0f, 0.01f);
  ST_CHECK_NEAR(table.max_scroll(), 0.0f, 0.01f);

  // 表头带（36px）与行带（32px）逐条断言
  const st::math::Rect header = table.header_rect();
  ST_CHECK_NEAR(header.x, 0.0f, 0.01f);
  ST_CHECK_NEAR(header.y, 0.0f, 0.01f);
  ST_CHECK_NEAR(header.width, 400.0f, 0.01f);
  ST_CHECK_NEAR(header.height, 36.0f, 0.01f);
  const st::math::Rect row0 = table.row_rect(0U);
  ST_CHECK_NEAR(row0.y, 36.0f, 0.01f);
  ST_CHECK_NEAR(row0.height, 32.0f, 0.01f);
  const st::math::Rect row1 = table.row_rect(1U);
  ST_CHECK_NEAR(row1.y, 68.0f, 0.01f);
  ST_CHECK_NEAR(row1.height, 32.0f, 0.01f);

  // 单元格 x = 前序列宽之和
  const st::math::Rect c00 = table.cell_rect(0U, 0U);
  ST_CHECK_NEAR(c00.x, 0.0f, 0.01f);
  ST_CHECK_NEAR(c00.y, 36.0f, 0.01f);
  ST_CHECK_NEAR(c00.width, 120.0f, 0.01f);
  ST_CHECK_NEAR(c00.height, 32.0f, 0.01f);
  const st::math::Rect c01 = table.cell_rect(0U, 1U);
  ST_CHECK_NEAR(c01.x, 120.0f, 0.01f);
  ST_CHECK_NEAR(c01.width, 280.0f, 0.01f);
  const st::math::Rect c10 = table.cell_rect(1U, 0U);
  ST_CHECK_NEAR(c10.x, 0.0f, 0.01f);
  ST_CHECK_NEAR(c10.y, 68.0f, 0.01f);
  const st::math::Rect hc1 = table.header_cell_rect(1U);
  ST_CHECK_NEAR(hc1.x, 120.0f, 0.01f);
  ST_CHECK_NEAR(hc1.y, 0.0f, 0.01f);
  ST_CHECK_NEAR(hc1.height, 36.0f, 0.01f);

  // 像素取色：表头 / 无斑马行 / 斑马行
  st::raster::Canvas canvas(400, 100);
  canvas.clear(theme.colors().bg);
  table.paint(context, canvas);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{200.0f, 18.0f}) == theme.colors().surface_alt);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{200.0f, 52.0f}) == theme.colors().bg);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{200.0f, 84.0f}) == theme.colors().surface_alt);
  ST_CHECK(!(theme.colors().surface_alt == theme.colors().bg));

  // 超长文本被省略：原文未绘制，且出现省略号
  ST_CHECK(!contains(port.drawn(), long_text));
  ST_CHECK(contains_ellipsis(port.drawn()));
  ST_CHECK(contains(port.drawn(), "名称"));
  ST_CHECK(contains(port.drawn(), "beta"));

  // 属性面
  ST_CHECK_EQ(table.get_property("columns").value_or(""), std::string{"名称:120:start,说明:0:end"});
  ST_CHECK_EQ(table.get_property("zebra").value_or(""), std::string{"true"});
  ST_CHECK(table.set_property("row_height", "48"));
  ST_CHECK_NEAR(table.row_height(), 48.0f, 0.01f);
  ST_CHECK(table.set_property("zebra", "false"));
  ST_CHECK_EQ(table.zebra(), false);
}

// —— 横向滚动（内部 offset + Shift 滚轮）——

ST_TEST(ui_table_horizontal_scroll) {
  st::ui::Theme theme = st::ui::Theme::light();
  TableTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Table table;
  table.set_columns({{"A", 300.0f, st::ui::TextAlign::Start}, {"B", 300.0f, st::ui::TextAlign::End}});
  table.add_row({"a", "b"});
  table.apply_theme(theme);
  table.arrange(context, st::math::Rect{0.0f, 0.0f, 400.0f, 68.0f});

  ST_CHECK_NEAR(table.total_width(), 600.0f, 0.01f);
  ST_CHECK_NEAR(table.max_scroll(), 200.0f, 0.01f);
  ST_CHECK(table.semantics_flags().scrollable);

  // 普通滚轮不滚动（Shift + 滚轮才滚）
  st::ui::Event plain;
  plain.kind = st::ui::EventKind::Wheel;
  plain.position = st::math::Point{10.0f, 40.0f};
  plain.wheel_delta = 1.0f;
  ST_CHECK(!table.on_event(context, plain));
  ST_CHECK_NEAR(table.scroll_offset(), 0.0f, 0.01f);

  st::ui::Event shifted = plain;
  shifted.shift = true;
  ST_CHECK(table.on_event(context, shifted));
  ST_CHECK(shifted.handled);
  ST_CHECK_NEAR(table.scroll_offset(), 48.0f, 0.01f);
  ST_CHECK_NEAR(table.cell_rect(0U, 1U).x, 252.0f, 0.01f);  // 300 - 48

  // 键盘：End 到最大偏移、Home 回零
  st::ui::Event key;
  key.kind = st::ui::EventKind::KeyDown;
  key.key = "End";
  ST_CHECK(table.on_event(context, key));
  ST_CHECK_NEAR(table.scroll_offset(), 200.0f, 0.01f);
  key.key = "Home";
  ST_CHECK(table.on_event(context, key));
  ST_CHECK_NEAR(table.scroll_offset(), 0.0f, 0.01f);

  // 偏移夹取 + 属性面
  ST_CHECK(table.set_property("scroll_offset", "9999"));
  ST_CHECK_NEAR(table.scroll_offset(), 200.0f, 0.01f);
  ST_CHECK(table.invoke_action("scroll_to", "60"));
  ST_CHECK_NEAR(table.scroll_offset(), 60.0f, 0.01f);
  ST_CHECK(table.invoke_action("add_row", "c|d"));
  ST_CHECK_EQ(table.row_count(), 2U);
  ST_CHECK_EQ(table.cell(1U, 1U), std::string_view{"d"});
  ST_CHECK(table.invoke_action("clear_rows", ""));
  ST_CHECK_EQ(table.row_count(), 0U);
}

// —— 行 hover 高亮 ——

ST_TEST(ui_table_hover_row) {
  st::ui::Theme theme = st::ui::Theme::light();
  TableTestTextPort port;
  st::ui::RenderContext context{theme, &port, 0.0};

  st::ui::Table table;
  table.set_columns({{"名称", 0.0f, st::ui::TextAlign::Start}});
  table.add_row({"a"});
  table.add_row({"b"});
  table.apply_theme(theme);
  table.arrange(context, st::math::Rect{0.0f, 0.0f, 200.0f, 100.0f});

  st::ui::Event move;
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{10.0f, 52.0f};  // 第 0 行
  ST_CHECK(table.on_event(context, move));
  ST_CHECK_EQ(table.hovered_row(), 0U);

  st::raster::Canvas canvas(200, 100);
  canvas.clear(theme.colors().bg);
  table.paint(context, canvas);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{100.0f, 52.0f}) == theme.colors().surface_alt);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{100.0f, 84.0f}) == theme.colors().bg);

  // 点击行触发回调
  std::size_t clicked = 99U;
  table.set_on_row_click([&clicked](std::size_t index) { clicked = index; });
  st::ui::Event click;
  click.kind = st::ui::EventKind::Click;
  click.position = st::math::Point{10.0f, 84.0f};  // 第 1 行
  ST_CHECK(table.on_event(context, click));
  ST_CHECK_EQ(clicked, 1U);

  // 表头区不触发回调
  clicked = 99U;
  click.position = st::math::Point{10.0f, 10.0f};
  ST_CHECK(!table.on_event(context, click));
  ST_CHECK_EQ(clicked, 99U);
}

// —— ⑥ 空 TextPort 下不崩溃（UiRoot 集成路径）——

ST_TEST(ui_table_null_text_port_via_root) {
  st::ui::Theme dark = st::ui::Theme::dark();
  st::ui::UiRoot root;
  root.set_theme(dark);
  root.set_viewport(st::math::Size{400.0f, 200.0f});
  root.set_text_port(nullptr);  // 无字体环境

  auto table = std::make_unique<st::ui::Table>();
  table->set_columns({{"name", 0.0f, st::ui::TextAlign::Start},
                      {"value", 120.0f, st::ui::TextAlign::End}});
  table->add_row({"alpha", "1"});
  table->set_row_height(32.0f);
  st::ui::Table* added = table.get();
  root.set_content(std::move(table));
  root.layout(true);

  ST_CHECK_NEAR(added->bounds().width, 400.0f, 0.5f);
  // 根内容按「至少铺满视口」排布（`UiRoot::layout`）：表格自然高 68 < 视口 200，
  // 因此拿到的是视口高度——这是根节点语义（否则根层的 `grow` 无法生效，整页会"缩在上半截"）。
  ST_CHECK_NEAR(added->bounds().height, 200.0f, 0.5f);
  ST_CHECK_NEAR(added->measured_size().height, 68.0f, 0.5f);  // 自然高度仍如实记录

  st::raster::Canvas canvas(400, 200);
  canvas.clear(dark.colors().bg);
  root.paint(canvas);
  // 表头（surface_alt）与行底（无斑马 → 透明，露出画布底色）都在，且过程无崩溃
  ST_CHECK(canvas.pixel_at_point(st::math::Point{200.0f, 18.0f}) == dark.colors().surface_alt);
  ST_CHECK(canvas.pixel_at_point(st::math::Point{200.0f, 52.0f}) == dark.colors().bg);
}
