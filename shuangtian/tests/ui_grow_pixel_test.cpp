/// `grow` 均分必须落到**整像素**——半像素宽度会让 1px 描边被劈成两半。
///
/// ## 守的是什么（实测代价）
///
/// `leftover / grow_weight` 常常落在半像素上。gallery 首页的四张统计卡是
/// 858 可用宽 − 3×8 gap = 834，÷4 = **208.5**，于是每张卡宽 214.5、
/// 第二张卡的 x 落在 **490.5**。
///
/// 后果：卡片的**左右边界跨在半个像素上**，1px 描边被劈成两半（各 50% 覆盖）。
/// 实测同一张卡两个方向的边界剖面（暗色）：
///
/// ```
/// 上边缘（y 为整数）  14 → 88        一处锐利过渡
/// 右边缘（x 为 .5）   54 → 50 → 9    分散在两三个像素上
/// ```
///
/// 这种"只在一个方向发虚"最容易被读成渲染 bug。
///
/// ## 判据为何是"整像素 + 总和不变"两条一起
///
/// 只断言"整像素"的话，一个把所有子项都向下取整的实现也能过——
/// 但那会让总宽短一截（末尾露出空隙）。所以同时要求**宽度总和等于可用宽**。
/// 这也是"按顺序发余数"与"逐个四舍五入"的区别所在：前者总和守恒，后者不。

#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::Panel;
using st::ui::Theme;

/// 建一个固定宽度的 row 容器，往里放 `count` 个 `grow` 子项。
auto make_grow_row(int count, float width, float gap, Panel& row_out) -> void {
  row_out.style().direction = st::ui::FlexDirection::Row;
  row_out.style().width = width;
  row_out.style().height = 60.0f;
  row_out.style().gap = gap;
  row_out.style().padding = st::math::Insets{};
  for (int i = 0; i < count; ++i) {
    auto child = std::make_unique<Panel>(st::ui::FlexDirection::Column);
    child->style().grow = true;
    child->style().height = 40.0f;
    row_out.add_child(std::move(child));
  }
}

}  // namespace

ST_TEST(grow_distributes_whole_pixels_and_keeps_total) {
  const Theme theme = Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};

  // 四个组合：正好整除（对照）、除不尽产生 .5、产生 .25、产生 .75
  struct Case {
    int count;
    float width;
    float gap;
    const char* name;
  };
  const Case cases[] = {
      {4, 840.0f, 8.0f, "整除（对照）"},   // 可用 816 / 4 = 204 整
      {4, 858.0f, 8.0f, "余 2（.5）"},     // 可用 834 / 4 = 208.5 ← gallery 实测
      {3, 400.0f, 4.0f, "余 1（.33）"},    // 可用 392 / 3 = 130.67
      {2, 301.0f, 0.0f, "余 1（.5）"},     // 可用 301 / 2 = 150.5
  };

  for (const Case& item : cases) {
    Panel row;
    make_grow_row(item.count, item.width, item.gap, row);
    row.measure(context, st::ui::Constraints{item.width, 600.0f});
    row.arrange(context, st::math::Rect{0.0f, 0.0f, item.width, 60.0f});

    // 收集 grow 子项的 x 与 width
    std::vector<float> xs;
    std::vector<float> ws;
    for (const auto& child : row.children()) {
      const st::math::Rect b = child->bounds();
      xs.push_back(b.x);
      ws.push_back(b.width);
    }
    ST_CHECK_EQ(xs.size(), static_cast<std::size_t>(item.count));

    float total = 0.0f;
    for (std::size_t i = 0; i < ws.size(); ++i) {
      // ① 宽度必须是整像素（半像素宽度 = 描边被劈开）
      ST_CHECK(std::abs(ws[i] - std::round(ws[i])) <= 1e-3F);
      // ② 横坐标也必须是整像素（否则边界同样跨在 .5 上）
      ST_CHECK(std::abs(xs[i] - std::round(xs[i])) <= 1e-3F);
      total += ws[i];
    }
    // ③ 总和守恒：按顺序发余数不能让总宽变短（那是"逐个向下取整"的错法）
    const float expected = item.width - item.gap * static_cast<float>(item.count - 1);
    ST_CHECK(std::abs(total - expected) <= 1e-3F);

    // ④ 各子项宽度差不超过 1px（余数应当均摊，而不是全给第一个）
    const auto [lo, hi] = std::minmax_element(ws.begin(), ws.end());
    st::log::out("[grow] {}：宽 {}..{}  总 {:.1f}（期望 {:.1f}）\n", item.name, *lo, *hi, total,
              expected);
    ST_CHECK(*hi - *lo <= 1.0f + 1e-3F);
  }
}
