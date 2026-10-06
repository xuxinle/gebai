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

#include "st/core/print.hpp"
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
  //
  // ⚠ 判据用**相对位置**，不用绝对坐标：`Icon::path` 会对每个图标做
  // **光学归一化**（等比缩放到统一的可视尺寸并居中）——那是当前契约，
  // 绝对坐标必然不等于设计数据。互换会把“下右”变成“右下”，
  // 这个不等式不受缩放影响，判得住。
  const auto polylines = flatten("check");
  ST_CHECK_EQ(polylines.size(), 1U);
  ST_CHECK_EQ(polylines.front().points.size(), 3U);
  const Point p0 = point_at(polylines, 0, 0);
  const Point p1 = point_at(polylines, 0, 1);
  const Point p2 = point_at(polylines, 0, 2);
  // 源数据里 p0→p1 是“下右”、p1→p2 是“上右”。互换后第一个会变成“右下”。
  ST_CHECK(p1.x > p0.x);
  ST_CHECK(p1.y > p0.y);
  ST_CHECK(p2.x > p1.x);
  ST_CHECK(p2.y < p1.y);
  ST_CHECK(p0.x < p2.x);
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
  //
  // ⚠ 判据用**子路径之间的相对位置**（含 x 与 y 两个方向），不用绝对坐标：
  // `Icon::path` 会做光学归一化（居中+缩放），绝对坐标必然与设计数据差一个偏移。
  // 屋顶（y 最小）→ 墙（中间）→ 门（贴底）的**上下次序**是设计意图，也不受缩放影响。
  const auto polylines = flatten("home");
  ST_CHECK_EQ(polylines.size(), 3U);
  const Point roof_left = point_at(polylines, 0, 0);
  const Point roof_top = point_at(polylines, 0, 1);
  const Point wall_top = point_at(polylines, 1, 0);
  const Point door_top = point_at(polylines, 2, 0);
  // 屋顶：中间点更高（y 更小），且左端在顶点左侧
  ST_CHECK(roof_top.y < roof_left.y);
  ST_CHECK(roof_left.x < roof_top.x);
  // 墙顶（屋顶下方）比屋顶顶点低、但高于门顶
  ST_CHECK(wall_top.y > roof_top.y);
  // 门在墙内：门顶低于墙顶
  ST_CHECK(door_top.y > wall_top.y);
}

ST_TEST(ui_icon_path_cubic_control_points_keep_order) {
  // `C` 有 6 个数（两组控制点 + 终点）。实参求值顺序问题在 6 参数下更严重：
  // 这里用 `search` 两段子路径的**相对位置**来钉住顺序——
  // 手柄在圆环的**右下角**（源数据 `M17.5 17.5` 对圆环起点 `(10.5, 3.5)`）。
  // 互换会把手柄翻到左上，而不等式不受光学归一化影响。
  const auto polylines = flatten("search");
  ST_CHECK_EQ(polylines.size(), 2U);
  const Point handle_0 = point_at(polylines, 0, 0);
  const Point handle_1 = point_at(polylines, 0, 1);
  const Point ring_0 = point_at(polylines, 1, 0);
  // 第二条是圆环：`Z` 让它闭合
  ST_CHECK(polylines[1].closed);
  ST_CHECK(handle_0.x > ring_0.x);
  ST_CHECK(handle_0.y > ring_0.y);
  ST_CHECK(handle_1.x > handle_0.x);
  ST_CHECK(handle_1.y > handle_0.y);
}

ST_TEST(ui_icon_path_scales_and_centers_in_box) {
  // 图标数据是 24×24 视口：放进 48×48 的盒子应按 2 倍等比映射。
  //
  // ⚠ 判据是**整体等比**（两点跨度按盒宽缩放）与**居中落盒**，
  // 不是某个绝对坐标——`Icon::path` 还会按各图标墨迹做光学归一化，
  // 所以“源点 × 2”不再成立。这两条性质才是“放进任意盒子都对”的内容。
  const auto in_24 = flatten("check");
  const Path path = Icon::path("check", Rect{10.0f, 20.0f, 48.0f, 48.0f}, 2.0f);
  const auto polylines = path.flatten(0.25f);
  ST_CHECK_EQ(polylines.size(), 1U);
  const float small_span = point_at(in_24, 0, 2).x - point_at(in_24, 0, 0).x;
  const float big_span = point_at(polylines, 0, 2).x - point_at(polylines, 0, 0).x;
  ST_CHECK(small_span > 0.0f);
  ST_CHECK_NEAR(big_span / small_span, 2.0f, 0.02f);
  const Point first = point_at(polylines, 0, 0);
  float min_x = first.x;
  float max_x = first.x;
  float min_y = first.y;
  float max_y = first.y;
  for (const auto& polyline : polylines) {
    for (const Point& point : polyline.points) {
      min_x = std::min(min_x, point.x);
      max_x = std::max(max_x, point.x);
      min_y = std::min(min_y, point.y);
      max_y = std::max(max_y, point.y);
    }
  }
  // 墨迹落在盒内（居中且不溢出）
  ST_CHECK(min_x >= 10.0f);
  ST_CHECK(max_x <= 58.0f);
  ST_CHECK(min_y >= 20.0f);
  ST_CHECK(max_y <= 68.0f);
  // 中心对齐到盒中心（48×48 的盒中心是 (34, 44)）
  ST_CHECK_NEAR((min_x + max_x) * 0.5f, 34.0f, 0.5f);
  ST_CHECK_NEAR((min_y + max_y) * 0.5f, 44.0f, 0.5f);
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
  // 👈 在**未归一化的 24 视框**下比：`Icon::path` 会按 `view_bounds` 把墨迹
  // 缩放居中到统一尺寸，那时“点是否落在 `view_bounds` 里”已不成立
  // （归一化本来就是要把小字形**移出**它原来的范围）。
  // 这里量的是**数据层**契约：每个图标的设计包围盒都落在 24 视框内
  // （越界会让它画到隔壁格子里，而单看一个图标看不出来）。
  const auto names = Icon::names();
  ST_CHECK(names.size() >= 70U);
  for (const auto& name : names) {
    const Rect bounds = Icon::view_bounds(name);
    ST_CHECK(bounds.width > 0.0f || bounds.height > 0.0f);
    ST_CHECK(bounds.x >= -0.5f);
    ST_CHECK(bounds.y >= -0.5f);
    ST_CHECK(bounds.right() <= 24.5f);
    ST_CHECK(bounds.bottom() <= 24.5f);
  }
}

/// **光学尺寸一致**：各图标墨迹的最长边归一化后应当彼此接近。
///
/// 这是“看着一样大”的可量形式，也是用户报“图标不精致”的首要来源：
/// 归一化之前实测最大/最小相差 **2.2 倍**（`list` 0.42 对 `git-branch` 0.78）。
/// 判据取**尺寸的相对离散度**（而不是绝对尺寸）：
/// 图形本身有不可避免的形状差异（圆形/三角形在等包围盒下看着偏小），
/// 但归一化后应当落在同一档。
ST_TEST(ui_icon_optical_size_is_consistent) {
  const auto names = Icon::names();
  ST_CHECK(names.size() >= 70U);
  float min_extent = 1.0e9f;
  float max_extent = 0.0f;
  for (const auto& name : names) {
    const Path path = Icon::path(name, Rect{0.0f, 0.0f, 24.0f, 24.0f}, 2.0f);
    const auto polylines = path.flatten(0.25f);
    ST_REQUIRE(!polylines.empty());
    float min_x = 1.0e9f;
    float max_x = -1.0e9f;
    float min_y = 1.0e9f;
    float max_y = -1.0e9f;
    for (const auto& polyline : polylines) {
      for (const Point& point : polyline.points) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
      }
    }
    const float extent = std::max(max_x - min_x, max_y - min_y);
    ST_CHECK(extent <= 24.0f);            // 不得溢出视框
    ST_CHECK(min_x >= -0.01f);            // 居中后不会跑到框外
    ST_CHECK(min_y >= -0.01f);
    ST_CHECK(max_x <= 24.01f);
    ST_CHECK(max_y <= 24.01f);
    min_extent = std::min(min_extent, extent);
    max_extent = std::max(max_extent, extent);
  }
  st::print("[icon-optical] 墨迹最长边 min={:.2f} max={:.2f} 比={:.2f}\n",
            static_cast<double>(min_extent), static_cast<double>(max_extent),
            static_cast<double>(max_extent / min_extent));
  // 归一化前实测比 1.9×（0.42 ↔ 0.78 逻辑px 的 24 倍口径）；收紧到 1.25×
  ST_CHECK(max_extent / min_extent < 1.25f);
}

/// `restore`（窗口“还原”）的后窗**不得伸进前窗内部**。
///
/// 用户报的现象（标题栏那个按钮）：后窗的右边缘一路画到前窗内部，
/// 多出一截“插进窗里”的竖线。根因是设计数据写成 `… L16 4 L16 16`——
/// 收尾点 `16,16` 已在前窗（`8,8`–`20,20`）**内部**；正确收尾点是
/// 后窗右边缘与前窗上缘的交点 `y=8`。
///
/// 判据用**几何包含关系**：归一化是同一个仿射变换，包含关系不受影响。
ST_TEST(ui_icon_restore_back_square_stays_outside_the_front) {
  const auto polylines = flatten("restore");
  ST_REQUIRE(polylines.size() >= 2U);
  // 前窗是那段闭合子路径
  const auto& front = polylines[1];
  ST_CHECK(front.closed);
  float fx0 = front.points[0].x;
  float fx1 = front.points[0].x;
  float fy0 = front.points[0].y;
  float fy1 = front.points[0].y;
  for (const Point& point : front.points) {
    fx0 = std::min(fx0, point.x);
    fx1 = std::max(fx1, point.x);
    fy0 = std::min(fy0, point.y);
    fy1 = std::max(fy1, point.y);
  }
  // 内缩一个容差，避开“紧贴在窗框上”（那是正确的相交）
  constexpr float kInside = 0.5f;
  for (const Point& point : polylines[0].points) {
    const bool inside = point.x > fx0 + kInside && point.x < fx1 - kInside &&
                        point.y > fy0 + kInside && point.y < fy1 - kInside;
    ST_CHECK(!inside);
  }
}

/// 图标描边用**圆头端帽**——它直接决定零长度风格的圆点能不能画出来。
///
/// 为何必须守：`more-horizontal`/`more-vertical`/`list` 用 `M6 12 L6.02 12` 这类
/// **0.02 长**的线段当圆点。raster 缺省的 butt 端帽下，带四边形的矩形端面与端点平齐，
/// 外侧那半个圆被切掉——此时该像素的覆盖率只有 **0.086**（带长 0.02×缩放，
/// 再乘宽），落在底色与黑之间的“几乎看不见”区间。圆头端帽把带外扩到端点之外，
/// 圆点才是真正的圆。
///
/// ⚠ **判据必须落在“看得出是点”而不是“有非底色像素”**：
/// 旧版本用 `pixel != background` 计数，而 butt 下那 0.086 覆盖率也满足，
/// 于是**回退圆头依旧全绿**（逆向验证实测拓到）。现在要求该像素确实被
/// 明显涂黑（亮度低于底色与墨色中点的四分位），才计一笔。
ST_TEST(ui_icon_dot_style_glyphs_actually_render) {
  constexpr int kSize = 64;
  const auto dot_runs = [](std::string_view name, bool horizontal) -> std::size_t {
    st::raster::Canvas canvas{kSize, kSize, 1.0f};
    const st::math::Color background = st::math::Color::rgb(0xFF, 0xFF, 0xFF);
    canvas.clear(background);
    Icon::draw(canvas, name, Rect{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)},
               st::math::Color::rgb(0x00, 0x00, 0x00), 2.0f);
    // “看得出是点”的门槛：通道值 < 0xC0（即覆盖率 > 25%）。
    // butt 端帽下实测只有 ~0.086（值 ≈ 0xE9），会被这道门槛擂住。
    constexpr int kInkThreshold = 0xC0;
    std::size_t runs = 0;
    bool in_ink = false;
    for (int step = 0; step < kSize; ++step) {
      const st::math::Color pixel =
          horizontal ? canvas.pixel_at(step, kSize / 2) : canvas.pixel_at(kSize / 2, step);
      const bool ink = static_cast<int>(pixel.r) < kInkThreshold;
      if (ink && !in_ink) ++runs;
      in_ink = ink;
    }
    return runs;
  };
  st::print("[icon-dots] 三点菜单：横向 {} 段 · 纵向 {} 段（应为各 3）\n",
            dot_runs("more-horizontal", true), dot_runs("more-vertical", false));
  ST_CHECK_EQ(dot_runs("more-horizontal", true), 3U);
  ST_CHECK_EQ(dot_runs("more-vertical", false), 3U);
}

ST_TEST(ui_icon_unknown_name_is_empty_not_crash) {
  ST_CHECK(!Icon::has("definitely-not-an-icon"));
  const Path path = Icon::path("definitely-not-an-icon", Rect{0.0f, 0.0f, 24.0f, 24.0f}, 2.0f);
  ST_CHECK(path.is_empty());
  // 空盒子也不应崩
  ST_CHECK(Icon::path("check", Rect{0.0f, 0.0f, 0.0f, 0.0f}, 2.0f).is_empty());
}
