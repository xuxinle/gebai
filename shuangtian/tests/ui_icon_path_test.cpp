/// 图标路径解析的**坐标契约**测试。
///
/// 背景（一个只在 MSVC 上现形的真实缺陷）：解析写成
/// `map(next_number(cursor), next_number(cursor))` —— C++ **不规定函数实参的求值顺序**，
/// MSVC 从右往左求值，于是两个数被颠倒着取，**x 与 y 互换**。
///
/// 它的隐蔽性值得记一笔：所有图标都照常"画得出来"，只是方向不对——
/// `check` 交换后仍像对钩、`search` 仍像放大镜，肉眼看单个图标几乎发现不了；
/// 只有把**图形与名字并排摆出来**（图标全集页）才能一眼看出
/// `chevron-down` 指向右、`home` 变成 `<E`。而 GCC/Clang 通常从左往右求值，
/// **Linux 上完全正常**——这正是"本机看不出跨平台问题"的又一例。
///
/// 因此这里断言的是**解析出的坐标本身**（而不是"能画出像素"）：
/// 像素断言挡不住这个缺陷（交换后依然有像素），坐标顺序才挡得住。

#include "st/test/test.hpp"

#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/path.hpp"
#include "st/ui/icon.hpp"

namespace {

using st::math::Point;
using st::math::Rect;
using st::raster::Path;
using st::ui::Icon;

inline constexpr float kTolerance = 0.05f;

[[nodiscard]] auto flatten(std::string_view name) -> std::vector<st::raster::Polyline> {
  const Path path = Icon::path(name, Rect{0.0f, 0.0f, 24.0f, 24.0f}, 2.0f);
  return path.flatten(0.25f);
}

/// 第 `index` 条折线的第 `point` 个点。
/// 用 `operator[]` 而非 `.at()`：这是 `L12` 的硬红线（且测试里越界直接崩，
/// 比抛异常更好定位）。
[[nodiscard]] auto point_at(const std::vector<st::raster::Polyline>& polylines,
                            std::size_t index, std::size_t point) -> Point {
  return polylines[index].points[point];
}

}  // namespace

ST_TEST(ui_icon_path_keeps_x_before_y) {
  // 源数据：`M4 12.5 L9.5 18 L20 6`（对钩）。x/y 一旦互换，
  // 第一个点会变成 (12.5, 4) —— 这就是当年的现象。
  const auto polylines = flatten("check");
  ST_CHECK_EQ(polylines.size(), 1U);
  ST_CHECK_EQ(polylines.front().points.size(), 3U);
  ST_CHECK_NEAR(point_at(polylines, 0, 0).x, 4.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 0).y, 12.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 1).x, 9.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 1).y, 18.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 2).x, 20.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 2).y, 6.0f, kTolerance);
}

ST_TEST(ui_icon_path_direction_is_correct) {
  // `chevron-down` 是 V 形：中间点必须**更靠下**（y 更大）。
  // 若 x/y 互换，中间点会变成"更靠右" —— 图标指向右（当年的样子）。
  const auto polylines = flatten("chevron-down");
  const Point left = point_at(polylines, 0, 0);
  const Point middle = point_at(polylines, 0, 1);
  const Point right = point_at(polylines, 0, 2);
  ST_CHECK_NEAR(middle.x, (left.x + right.x) * 0.5f, kTolerance);
  ST_CHECK(middle.y > left.y);
  ST_CHECK(middle.y > right.y);
}

ST_TEST(ui_icon_path_keeps_subpaths_separate) {
  // `home` 是三段子路径（屋顶 / 墙 / 门）。解析必须按 `M` 断开：
  // 若把子路径连成一条折线，描边时会多出穿过图形的连线（`[E` 的来源之一）。
  const auto polylines = flatten("home");
  ST_CHECK_EQ(polylines.size(), 3U);
  // 屋顶：`M3.5 10.5 L12 3.5 L20.5 10.5`
  ST_CHECK_NEAR(point_at(polylines, 0, 0).x, 3.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 0).y, 10.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 1).x, 12.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 1).y, 3.5f, kTolerance);
  // 墙：`M6 9.5 L6 20 L18 20 L18 9.5`
  ST_CHECK_NEAR(point_at(polylines, 1, 0).x, 6.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 1, 0).y, 9.5f, kTolerance);
  // 门：`M10 20 L10 14 L14 14 L14 20`
  ST_CHECK_NEAR(point_at(polylines, 2, 0).x, 10.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 2, 0).y, 20.0f, kTolerance);
}

ST_TEST(ui_icon_path_cubic_control_points_keep_order) {
  // `C` 有 6 个数（两组控制点 + 终点）。实参求值顺序问题在 6 参数下更严重：
  // 这里用 `search` 的第二段（放大镜圆环）的**起点**来钉住顺序：
  // 源数据 `… M10.5 3.5 C…`，起点必须是 (10.5, 3.5)（互换则为 (3.5, 10.5)）。
  const auto polylines = flatten("search");
  ST_CHECK_EQ(polylines.size(), 2U);
  // 第一条是手柄 `M17.5 17.5 L21 21`
  ST_CHECK_NEAR(point_at(polylines, 0, 0).x, 17.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 0).y, 17.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 1).x, 21.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 1).y, 21.0f, kTolerance);
  // 第二条是圆环：起点 (10.5, 3.5)，且 `Z` 让它闭合
  ST_CHECK(polylines[1].closed);
  ST_CHECK_NEAR(point_at(polylines, 1, 0).x, 10.5f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 1, 0).y, 3.5f, kTolerance);
}

ST_TEST(ui_icon_path_scales_and_centers_in_box) {
  // 图标数据是 24×24 视口：放进 48×48 的盒子应等比放大 2 倍并居中
  const Path path = Icon::path("check", Rect{10.0f, 20.0f, 48.0f, 48.0f}, 2.0f);
  const auto polylines = path.flatten(0.25f);
  ST_CHECK_EQ(polylines.size(), 1U);
  // 源点 (4,12.5) → 10 + 4×2 = 18，20 + 12.5×2 = 45
  ST_CHECK_NEAR(point_at(polylines, 0, 0).x, 18.0f, kTolerance);
  ST_CHECK_NEAR(point_at(polylines, 0, 0).y, 45.0f, kTolerance);
}

ST_TEST(ui_icon_path_handles_every_builtin_glyph) {
  // 全集体检：每个内置图标都要能解析出至少一条非空折线，
  // 且**所有点都落在视口范围内**（越界点会让图标画到隔壁格子里，
  // 而这种错误在单个图标上看不出来）。
  const auto names = Icon::names();
  ST_CHECK(names.size() >= 70U);
  for (const auto& name : names) {
    const auto polylines = flatten(name);
    ST_CHECK(!polylines.empty());
    for (const auto& polyline : polylines) {
      ST_CHECK(polyline.points.size() >= 2U);
      for (const Point& point : polyline.points) {
        ST_CHECK(point.x >= -2.0f);
        ST_CHECK(point.y >= -2.0f);
        ST_CHECK(point.x <= 26.0f);
        ST_CHECK(point.y <= 26.0f);
      }
    }
  }
}

ST_TEST(ui_icon_view_bounds_wraps_the_drawn_geometry) {
  // `view_bounds` 与实际绘制用同一份路径数据：两者必须落在相近的范围里
  // （否则"按包围盒居中"会把图标推偏）。
  const Rect bounds = Icon::view_bounds("check");
  const auto polylines = flatten("check");
  for (const auto& polyline : polylines) {
    for (const Point& point : polyline.points) {
      ST_CHECK(point.x >= bounds.x - kTolerance);
      ST_CHECK(point.x <= bounds.right() + kTolerance);
      ST_CHECK(point.y >= bounds.y - kTolerance);
      ST_CHECK(point.y <= bounds.bottom() + kTolerance);
    }
  }
}

ST_TEST(ui_icon_unknown_name_is_empty_not_crash) {
  ST_CHECK(!Icon::has("definitely-not-an-icon"));
  const Path path = Icon::path("definitely-not-an-icon", Rect{0.0f, 0.0f, 24.0f, 24.0f}, 2.0f);
  ST_CHECK(path.is_empty());
  // 空盒子也不应崩
  ST_CHECK(Icon::path("check", Rect{0.0f, 0.0f, 0.0f, 0.0f}, 2.0f).is_empty());
}
