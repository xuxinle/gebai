/// 布局引擎的**容器语义**测试：换行（`wrap`）与不换行的对照。
///
/// 背景（实测）：`Style::wrap` 字段一直存在，但 `measure` / `layout_children`
/// **都没有实现它**——行容器仍按"全部子节点累加"算尺寸，于是宽度是 72 个格子的总和、
/// 高度只等于最高的一行。表现是**第二行起被父容器裁掉**：
/// 图标全表（72 个图标）只顯示一行，"看着像是列数不够"，而实际是容器高度算错了。
///
/// 这类"字段存在但没实现"的缺口编译器不会报、代码审查也容易漏
/// （同 `DESIGN.md` §8.2 第 26 条：`Input` 从未实现属性面），所以这里把语义钉住。

#include "st/test/test.hpp"

#include <memory>

#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Rect;
using st::ui::Constraints;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Panel;
using st::ui::RenderContext;
using st::ui::Theme;

inline constexpr float kUnbounded = st::ui::kUnbounded;

/// 尺寸固定的测试块（只关心布局，不画东西）。
class FixedBox : public Element {
 public:
  FixedBox(float width, float height) : size_{width, height} {}
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "FixedBox"; }
  void measure(const RenderContext& context, const Constraints& constraints) override {
    (void)context;
    (void)constraints;
    measured_ = size_;
  }
  void paint_content(const RenderContext& context, st::raster::Surface& canvas) const override {
    (void)context;
    (void)canvas;
  }

 private:
  st::math::Size size_{};
};

struct Fixture {
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
};

[[nodiscard]] auto make_wrapping_row(float gap) -> std::unique_ptr<Panel> {
  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().wrap = true;
  row->style().gap = gap;
  return row;
}

}  // namespace

ST_TEST(ui_layout_wrap_measures_height_by_lines) {
  Fixture fixture;
  auto row = make_wrapping_row(10.0f);
  for (int index = 0; index < 5; ++index) {
    row->add_child(std::make_unique<FixedBox>(100.0f, 30.0f));
  }
  // 可用宽 340：一行放 3 个（100+10+100+10+100 = 320），第 4 个换行 → 2 行
  row->measure(fixture.context,
               Constraints{.max_width = 340.0f, .max_height = kUnbounded});
  ST_CHECK_EQ(static_cast<int>(row->measured_size().width), 320);
  // 2 行 × 30 + 1 个行间隙 10 = 70
  ST_CHECK_EQ(static_cast<int>(row->measured_size().height), 70);
}

ST_TEST(ui_layout_wrap_arranges_children_per_line) {
  Fixture fixture;
  auto row = make_wrapping_row(10.0f);
  for (int index = 0; index < 5; ++index) {
    auto box = std::make_unique<FixedBox>(100.0f, 30.0f);
    box->set_id(std::format("box-{}", index));
    row->add_child(std::move(box));
  }
  row->measure(fixture.context, Constraints{.max_width = 340.0f, .max_height = kUnbounded});
  row->arrange(fixture.context, Rect{0.0f, 0.0f, 340.0f, 200.0f});

  // 第一行：3 个，x = 0 / 110 / 220，y = 0
  for (int index = 0; index < 3; ++index) {
    const Rect bounds = row->child_at(static_cast<std::size_t>(index))->bounds();
    ST_CHECK_EQ(static_cast<int>(bounds.x), index * 110);
    ST_CHECK_EQ(static_cast<int>(bounds.y), 0);
  }
  // 第二行：2 个，x = 0 / 110，y = 30 + 10 = 40
  for (int index = 3; index < 5; ++index) {
    const Rect bounds = row->child_at(static_cast<std::size_t>(index))->bounds();
    ST_CHECK_EQ(static_cast<int>(bounds.x), (index - 3) * 110);
    ST_CHECK_EQ(static_cast<int>(bounds.y), 40);
  }
}

ST_TEST(ui_layout_wrap_uses_tallest_child_as_line_height) {
  Fixture fixture;
  auto row = make_wrapping_row(0.0f);
  // 第一行：高 20 与高 50 混排 → 行高 50；第二行从 y = 50 开始
  row->add_child(std::make_unique<FixedBox>(100.0f, 20.0f));
  row->add_child(std::make_unique<FixedBox>(100.0f, 50.0f));
  auto third = std::make_unique<FixedBox>(100.0f, 20.0f);
  third->set_id("third");
  row->add_child(std::move(third));

  row->measure(fixture.context,
               Constraints{.max_width = 200.0f, .max_height = kUnbounded});
  ST_CHECK_EQ(static_cast<int>(row->measured_size().height), 70);  // 50 + 20

  row->arrange(fixture.context, Rect{0.0f, 0.0f, 200.0f, 200.0f});
  ST_CHECK_EQ(static_cast<int>(row->child_at(2)->bounds().y), 50);
}

ST_TEST(ui_layout_wrap_centers_within_line_height) {
  Fixture fixture;
  auto row = make_wrapping_row(0.0f);
  row->style().align_items = st::ui::Align::Center;
  row->add_child(std::make_unique<FixedBox>(100.0f, 50.0f));
  auto small = std::make_unique<FixedBox>(100.0f, 10.0f);
  small->set_id("small");
  row->add_child(std::move(small));

  row->measure(fixture.context, Constraints{.max_width = 250.0f, .max_height = kUnbounded});
  row->arrange(fixture.context, Rect{0.0f, 0.0f, 250.0f, 200.0f});
  // 行高 50，10 高的子块居中 → y = 20
  ST_CHECK_EQ(static_cast<int>(row->child_at(1)->bounds().y), 20);
}

ST_TEST(ui_layout_without_wrap_keeps_single_line) {
  // 回归：不设 `wrap` 时行为必须与从前完全一致（宽度累加、高度取最大）
  Fixture fixture;
  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().gap = 10.0f;
  for (int index = 0; index < 5; ++index) {
    row->add_child(std::make_unique<FixedBox>(100.0f, 30.0f));
  }
  row->measure(fixture.context, Constraints{.max_width = 340.0f, .max_height = kUnbounded});
  ST_CHECK_EQ(static_cast<int>(row->measured_size().width), 340);  // 被 max_width 夹住
  ST_CHECK_EQ(static_cast<int>(row->measured_size().height), 30);  // 单行

  row->arrange(fixture.context, Rect{0.0f, 0.0f, 340.0f, 200.0f});
  // 单行语义：所有子块 y 相同、x 依次向右（溢出也不换行）
  ST_CHECK_EQ(static_cast<int>(row->child_at(4)->bounds().y), 0);
  ST_CHECK_EQ(static_cast<int>(row->child_at(4)->bounds().x), 440);
}

ST_TEST(ui_layout_wrap_ignored_without_width_bound) {
  // 没有可用宽上界时换行无意义（永远装得下）→ 必须退化为单行，
  // 否则"测量阶段不知道宽度"会把内容排成每行一个，界面整片塌掉。
  Fixture fixture;
  auto row = make_wrapping_row(10.0f);
  for (int index = 0; index < 4; ++index) {
    row->add_child(std::make_unique<FixedBox>(100.0f, 30.0f));
  }
  row->measure(fixture.context,
               Constraints{.max_width = kUnbounded, .max_height = kUnbounded});
  ST_CHECK_EQ(static_cast<int>(row->measured_size().height), 30);
  ST_CHECK_EQ(static_cast<int>(row->measured_size().width), 430);
}

ST_TEST(ui_layout_wrap_does_not_affect_column_direction) {
  // 列方向没有"行"的概念：`wrap` 对它无效（否则是静默改变既有语义）
  Fixture fixture;
  auto column = std::make_unique<Panel>(FlexDirection::Column);
  column->style().wrap = true;
  column->style().gap = 10.0f;
  for (int index = 0; index < 5; ++index) {
    column->add_child(std::make_unique<FixedBox>(100.0f, 30.0f));
  }
  column->measure(fixture.context,
                  Constraints{.max_width = 50.0f, .max_height = kUnbounded});
  ST_CHECK_EQ(static_cast<int>(column->measured_size().height), 190);  // 5×30 + 4×10
  column->arrange(fixture.context, Rect{0.0f, 0.0f, 120.0f, 400.0f});
  for (std::size_t index = 0; index < 5; ++index) {
    ST_CHECK_EQ(static_cast<int>(column->child_at(index)->bounds().x), 0);
    ST_CHECK_EQ(static_cast<int>(column->child_at(index)->bounds().y),
                static_cast<int>(index) * 40);
  }
}

ST_TEST(ui_layout_wrap_handles_many_small_items) {
  // 画廊的真实场景：72 个 68×58 的图标格在 976 宽的容器里
  Fixture fixture;
  auto grid = make_wrapping_row(2.0f);
  for (int index = 0; index < 72; ++index) {
    grid->add_child(std::make_unique<FixedBox>(68.0f, 58.0f));
  }
  grid->measure(fixture.context,
                Constraints{.max_width = 976.0f, .max_height = kUnbounded});
  // 每行 **13** 个（13×68 + 12×2 = 908 ≤ 976；再加一个就要 978 > 976，放不下）
  // 72 / 13 = 5.54 → 6 行；高 = 6×58 + 5×2 = 358
  ST_CHECK_EQ(static_cast<int>(grid->measured_size().height), 358);
  grid->arrange(fixture.context, Rect{0.0f, 0.0f, 976.0f, 400.0f});
  // 第 14 个（index 13）应在第二行首位
  ST_CHECK_EQ(static_cast<int>(grid->child_at(13)->bounds().x), 0);
  ST_CHECK_EQ(static_cast<int>(grid->child_at(13)->bounds().y), 60);
  // 第 15 个（index 14）在第二行第二位（同 y、x 增一个步长）
  ST_CHECK_EQ(static_cast<int>(grid->child_at(14)->bounds().x), 70);
  ST_CHECK_EQ(static_cast<int>(grid->child_at(14)->bounds().y), 60);
  // 最后一个（index 71）在第 6 行
  ST_CHECK_EQ(static_cast<int>(grid->child_at(71)->bounds().y), 300);
}

ST_TEST(ui_layout_button_respects_width_constraint) {
  // 回归（2026-10-06）：`Button::measure` 曾 `(void)constraints` ——宽度只看标签文本，
  // 而**不听父容器给的可用宽**。实测后果：270px 宽的侧栏里，一个带长路径标签的
  // 按钮被量成 400+px 宽，直接画到相邻的编辑器面板上方（看起来像绘制错乱）。
  //
  // 为何用 20px 这么窄的约束：`NullTextPort` 量宽恒 0，自然宽 = 控件高（几十像素）。
  // 约束必须**小于控件高**，否则夹与不夹结果一样——测试会变得**恒绿**
  // （实测踩过：约束写成 120 时，回退修复后测试依然通过）。
  Fixture fixture;
  auto button = std::make_unique<st::ui::Button>("ok");
  button->measure(fixture.context, Constraints{.max_width = 20.0f, .max_height = kUnbounded});
  ST_CHECK(button->measured_size().width <= 20.0f);
  // 宽松约束下不应被无条件压小
  auto wide = std::make_unique<st::ui::Button>("ok");
  wide->measure(fixture.context, Constraints{.max_width = kUnbounded, .max_height = kUnbounded});
  ST_CHECK(wide->measured_size().width > 20.0f);
}

ST_TEST(ui_layout_spacer_never_reports_negative_extent) {
  // 回归（2026-10-06）：`Spacer` 曾把负尺寸矩形原样当 `bounds`（实测：应用里
  // 30px 高的工具栏行内，`grow` 占位块的 `bounds.height == −18`）。
  // 负尺寸会被协议/测试当成异常值读走——“这块多大”的答案不该是负数。
  //
  // 这里直接钉住 `Spacer::arrange` 的不变量（容器给什么都要夹到非负）；
  // 不声称复现了那次的产生路径（它由父容器的交叉轴计算得出，尚未定位到单个表达式）。
  Fixture fixture;
  auto spacer = std::make_unique<st::ui::Spacer>();
  spacer->arrange(fixture.context, Rect{10.0f, 20.0f, 100.0f, -18.0f});
  ST_CHECK(spacer->bounds().height >= 0.0f);
  spacer->arrange(fixture.context, Rect{10.0f, 20.0f, -5.0f, 30.0f});
  ST_CHECK(spacer->bounds().width >= 0.0f);
}
