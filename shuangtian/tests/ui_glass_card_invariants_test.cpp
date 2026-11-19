/// 玻璃卡片的两个**必守不变式**——都由 `PITFALLS.md` 场景 8 的真实缺陷换来。
///
/// ## ① 玻璃元素底下不能有投影
///
/// `backdrop_blur` / `refract_region` 采样的是**已绘制好的背板**，而投影也是背板。
/// 于是模糊把周围一圈暗晕平均进玻璃中心，整块被压暗——而 `tint` / `blur` /
/// `refraction` 怎么调都无效（它们只在线性插值里挪，底色本身已经错了）。
///
/// 实测（亮色底 239.7）：气泡内 203.0，反推玻璃采到的背板 **188.3（暗 51）**；
/// 关掉投影后气泡内 243.0，反推背板 **239.6**（与真实背景只差 0.1）。
/// 而 `Card` 的 `shadow_level_` **默认就是 2（md）**——所以“直接拿 `Card`
/// 做玻璃卡片”一定发灰。
///
/// 本文件锁住两条可用解法：
/// - `BoxProps.elevation = Elevation::Flat`（声明式，推荐）；
/// - `custom_container<Card>` + `set_shadow_level(0)`（组件级逃生）。
///
/// ## ② `BoxProps.fill` 必须扛得住 `apply_theme`
///
/// `Element::refresh_theme` 是两步：组件自己的 `apply_theme` → 回放显式声明。
/// `Card::apply_theme` 会把 `background` 重置为主题实色、`border_color` 重置为
/// `border` 令牌。若 `BoxProps.fill` 只写一次 Style，**第二帧就被盖回去**：
/// 玻璃的半透明白变回实心白、白色高光边变淡灰（现场症状就是“灰灰的、不通透”）。
///
/// 修法是把它存成声明（`set_fill_override` / `set_border_color_override`），
/// 由 `apply_declared_overrides` 在组件之后回放。本文件锁住“**连过两帧仍然有效**”。
#include <memory>
#include <optional>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;
namespace math = st::math;

/// 一张玻璃卡片：填白、描边、带模糊——模拟聊天气泡的用法。
struct GlassCardPage : Component {
  bool flat{true};

  void build(Composer& c) override {
    // 设计符顺序必须与 `BoxProps` 声明顺序一致：
    // radius → fill → border_width → border_color → backdrop_* → elevation → key
    BoxProps props{.radius = 14.0f,
                   .fill = math::Color{255, 255, 255, 255},
                   .border_width = 1.0f,
                   .border_color = math::Color{255, 255, 255, 140},
                   .backdrop_blur = 6.0f,
                   .backdrop_tint = 0.22f,
                   .elevation = flat ? std::optional<Elevation>{Elevation::Flat} : std::nullopt,
                   .key = "bubble"};
    (void)card(c, props, [&] { (void)text(c, [] { return std::string("玻璃"); }); });
  }
};

}  // namespace

ST_TEST(glass_card_elevation_flat_removes_shadow) {
  // **核心回归 ①**：声明 `elevation = Flat` 后，卡片不能有投影。
  //
  // 判据是 `style().shadow.visible()` 而不是像素：投影的伤害路径是
  // “被背板滤镜平均进来”，而 `visible()` 直接回答“这块会不会先画一圈暗晕”。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<GlassCardPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* bubble = root.content();
  ST_REQUIRE(bubble != nullptr);
  ST_CHECK(!bubble->style().shadow.visible());
}

ST_TEST(glass_card_default_shadow_would_pollute_backdrop) {
  // **反向护栏**：不声明 `elevation` 时，`Card` 的默认投影确实存在。
  //
  // 这条不是为了“保护缺陷”，而是把**默认值**钉住：若有人把
  // `Card::shadow_level_` 的默认从 2 改掉，这条会红，提醒他去看
  // 玻璃元素那条约束还成不成立。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<GlassCardPage>();
  page->flat = false;
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* bubble = root.content();
  ST_REQUIRE(bubble != nullptr);
  ST_CHECK(bubble->style().shadow.visible());
}

ST_TEST(glass_card_fill_survives_theme_refresh) {
  // **核心回归 ②**：`fill` / `border_color` 必须**跨帧存活**。
  //
  // 走真路径：`refresh_theme` 会先跑 `Card::apply_theme`（把 background
  // 重置为主题实色），再回放显式声明。只写一次 Style 的实现会在这里露馅。
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<GlassCardPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* bubble = root.content();
  ST_REQUIRE(bubble != nullptr);

  // **必须走 `set_theme`**：`layout(force)` 只重排、不下发主题，
  // 用它验“跨主题刷新存活”会恒绿（`refresh_theme` 根本不会被调到）。
  for (int i = 0; i < 2; ++i) {
    root.set_theme(Theme::light());
    root.layout(true);
    const Style& style = bubble->style();
    ST_CHECK(style.background.a == 255U);
    ST_CHECK(style.background.r == 255U);
    ST_CHECK(style.background.g == 255U);
    ST_CHECK(style.background.b == 255U);
    // 描边必须还是那道半透明白，而不是主题的 `border` 令牌（淡灰）。
    ST_CHECK(style.border_color.a == 140U);
  }
}
