/// 桌面图标的**可点区域必须盖住图标**（而不是落到它下面）。
///
/// ## 缺陷（2026-11-15，用户实测报"点击触发位置在图标下方"）
///
/// 图标格外层是 `Panel(Column)`，子元素按声明顺序流式排布：图标、文字、以及
/// 叠在最上方的透明 `Button`（整格可点）。`Button` 直接 `add_child` 就是
/// **第三个流内子元素**——它被排到文字**下面**而不是盖在图标上。
///
/// 实测（1280×820，图标格 112×112，图标 68px）：
///
/// | 元素 | 位置 | 尺寸 |
/// |---|---|---|
/// | 图标 | y=82..150 | 68×68 |
/// | **可点区（错）** | **y=182..216** | **34×34** |
///
/// 于是用户点图标没反应、点图标下方的空白反而启动了应用。
///
/// ## 修法
///
/// 可点区绝对定位 + 四边归零（`left/top/right/bottom = 0`）→ 铺满整个图标格。
/// 这条测试钉的就是这个几何关系：**可点区必须包含图标矩形**，且面积显著大于它
/// （图标只有 68px，而 macOS 的桌面图标整格可点）。
///
/// ## 为什么用几何断言而不是"点一下看有没有反应"
///
/// 点击路径要真起子进程（`spawn_detached`），在单元测试里既不稳也慢；
/// 而"点错位置"的根因是**布局**，几何断言正好钉在根因上。
/// 端到端的点击验证由 `tools/desktop_shell_probe.py` 与手动 `click` 覆盖。
#include <cmath>
#include <memory>

#include "st/test/test.hpp"

#include "st/ui/components/basic.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
namespace {

using st::ui::Button;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Panel;

/// 与 apps/st_desktop 同一形状的图标格（不链接应用代码：桌面应用是 exe，不是库）。
///
/// 刻意复制结构而不是 import：这里测的是**布局契约**（绝对定位铺满 vs 流内排布），
/// 而结构与常量一旦漂移，测试就该跟着改——复制 12 行换来的是"不依赖 exe"。
[[nodiscard]] auto make_tile(float tile_size, float icon_size, bool absolute_hit)
    -> std::pair<std::unique_ptr<Panel>, std::pair<Element*, Element*>> {
  auto cell = std::make_unique<Panel>(FlexDirection::Column);
  cell->style().width = tile_size;
  cell->style().height = tile_size;
  cell->style().gap = 8.0f;
  cell->style().align_items = st::ui::Align::Center;
  cell->style().padding = st::math::Insets::all(6.0f);

  auto icon = std::make_unique<Panel>(FlexDirection::Column);
  icon->set_id("icon");
  icon->style().width = icon_size;
  icon->style().height = icon_size;
  Element* icon_ptr = icon.get();

  auto hit = std::make_unique<Button>("", st::ui::Variant::Ghost);
  hit->set_id("hit");
  if (absolute_hit) {
    hit->style().absolute = true;
    hit->style().left = 0.0f;
    hit->style().top = 0.0f;
    hit->style().right = 0.0f;
    hit->style().bottom = 0.0f;
  }
  Element* hit_ptr = hit.get();

  // 顺序与桌面一致：先图标、后按钮（按钮该盖在上面）。
  cell->add_child(std::move(icon));
  cell->add_child(std::move(hit));
  return {std::move(cell), {icon_ptr, hit_ptr}};
}

[[nodiscard]] auto contains(const st::math::Rect& outer, const st::math::Rect& inner) -> bool {
  return inner.x >= outer.x - 0.5f && inner.y >= outer.y - 0.5f &&
         inner.right() <= outer.right() + 0.5f && inner.bottom() <= outer.bottom() + 0.5f;
}

}  // namespace

ST_TEST(absolute_hit_area_covers_icon_and_whole_tile) {
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};
  auto [cell, parts] = make_tile(112.0f, 68.0f, /*absolute_hit=*/true);
  cell->measure(context, st::ui::Constraints{});
  cell->arrange(context, st::math::Rect{0.0f, 0.0f, 112.0f, 112.0f});

  const auto [icon, hit] = parts;
  // ① 可点区必须包含图标（这条就是缺陷的直接判据：错版里两者不相交）。
  ST_CHECK(contains(hit->bounds(), icon->bounds()));
  // ② 并且铺满整格（四边归零 = 与边框盒重合，**不受 padding 影响**）。
  //
  // 这条断言钉的是“左右基准必须统一”：`left` 以内容盒为原点、`right` 以边框盒右缘
  // 为基准时，同一份 112px 格子、padding 6 的拉伸结果是 **106**（左进 6、右不进）。
  ST_CHECK(std::abs(hit->bounds().width - 112.0f) < 1.0f);
  ST_CHECK(std::abs(hit->bounds().height - 112.0f) < 1.0f);
  ST_CHECK(std::abs(hit->bounds().x - 0.0f) < 0.5f);
  ST_CHECK(std::abs(hit->bounds().y - 0.0f) < 0.5f);
  // ③ 可点面积显著大于图标（整格可点，而不是只有图标那一小块）。
  ST_CHECK(hit->bounds().width > icon->bounds().width * 1.2f);
}

ST_TEST(flow_hit_area_falls_below_icon_without_absolute) {
  // 对照组：证明①这条断言真的能区分两种写法（否则它就是恒绿的空断言）。
  // 这是**已知的错误形态**，写进测试是为了让"修好"有可对照的基线。
  const st::ui::Theme theme = st::ui::Theme::dark();
  const st::ui::RenderContext context{theme, nullptr, 0.0};
  auto [cell, parts] = make_tile(112.0f, 68.0f, /*absolute_hit=*/false);
  cell->measure(context, st::ui::Constraints{});
  cell->arrange(context, st::math::Rect{0.0f, 0.0f, 112.0f, 112.0f});

  const auto [icon, hit] = parts;
  // 流内排布：按钮被排到图标**下面**（不相交）。
  ST_CHECK(!contains(hit->bounds(), icon->bounds()));
  ST_CHECK(hit->bounds().y > icon->bounds().bottom());
}
