// 回归测试：**显式声明必须活过组件 `apply_theme`**。
//
// ## 这一族缺陷的共同形态
//
// 调用方在声明里写了某个外观字段（`top_highlight` / `border_width` / `shadow`），
// 而组件 `apply_theme` 每帧按自己的档位**无条件重写**它 ⇒ 声明静默失效，且
// **没有任何报错**：`set glass=…` 回包 `changed: ["glass"]`、回读属性也“看起来对”，
// 只有画面不对。
//
// ## 实测代价（2026-11-19，用户反馈“边缘立体感不够 / 面板玻璃高光加强”）
//
// · `top_highlight`：玻璃卡片必须 `elevation = Flat` 关投影 ⇒
//   `Card::apply_theme` 按 `(elevated_ || shadow_level_ > 0)` 判据把它设为
//   `theme.colors().highlight`（**亮色档 alpha 恒为 0**）⇒ `highlight:0.55`
//   完全看不见，而回读 `glass_highlight` 报 0.549（读的是 `Style`，
//   渲染用的是同一份，但被 `refresh_theme` 在下一帧盖了）。
// · `border_width`：被重置成主题 1px，且**描边画在高光之后**，把亮边盖成灰线。
//
// 两条都靠“声明存档 → 组件之后回放”（`apply_declared_paint`）修掉。
//
// ## 这条测试为什么用 `refresh_theme` 而不是 `apply_theme`
//
// 缺陷恰恰在两者的**顺序**上（组件先写、声明后回放）。只调 `apply_theme`
// 会把这条顺序绕过去，测出来恒绿——实测踩过：注入缺陷后测试仍绿。

#include <memory>
#include <optional>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;
namespace math = st::math;

/// 一张玻璃卡片：`Flat` 档位（关投影，聊天气泡的真实用法）+ 显式高光 + 无描边。
struct DeclaredPaintPage : Component {
  void build(Composer& c) override {
    BoxProps props{.radius = 14.0f,
                   .border_none = true,
                   .top_highlight = math::Color{255, 255, 255, 140},
                   .highlight_width = 4.0f,
                   .elevation = std::optional<Elevation>{Elevation::Flat},
                   .key = "bubble"};
    (void)card(c, props, [&] { (void)text(c, [] { return std::string("玻璃"); }); });
  }
};

/// 挂页面并**强制跑一次主题刷新**（组件 `apply_theme` + 声明回放的真路径）。
auto mount_and_refresh(UiRoot& root, std::shared_ptr<DeclaredPaintPage> page) -> const Element* {
  root.set_viewport({400.0f, 300.0f});
  auto host = dsl::mount(root, page);
  if (host == nullptr) return nullptr;
  root.layout();
  // 关键：走 `UiRoot::apply_theme` —— 它内部对每个元素调 `refresh_theme`，
  // 那才是“组件覆盖 + 声明回放”的真实顺序。
  root.set_theme(Theme::light());
  root.layout();
  return root.content();
}

}  // namespace

ST_TEST(declared_top_highlight_survives_card_theme) {
  // **核心回归 ①**：显式高光不能被 `Card::apply_theme` 抹成透明。
  //
  // 玻璃卡片必然是 `elevation = Flat`（要关投影），而 Flat 档位下组件给的是
  // `theme.colors().highlight`，**亮色档的 alpha 恒为 0**。
  // 声明被打败时读到 0 —— 那正是“高光完全看不见、边缘没有立体感”的直接成因。
  UiRoot root;
  auto page = std::make_shared<DeclaredPaintPage>();
  const Element* bubble = mount_and_refresh(root, page);
  ST_REQUIRE(bubble != nullptr);
  ST_CHECK(bubble->style().top_highlight.a == 140);
  ST_CHECK(bubble->style().highlight_width == 4.0f);
}

ST_TEST(declared_border_none_survives_card_theme) {
  // **核心回归 ②**：显式“不要描边”必须生效。
  //
  // 主题默认 `border_width = 1px`，而**描边绘制在 `top_highlight` 之后**——
  // 不关掉它，亮边就被盖成一道灰线（放大 6 倍才看得清，这是本次最隐蔽的一条）。
  UiRoot root;
  auto page = std::make_shared<DeclaredPaintPage>();
  const Element* bubble = mount_and_refresh(root, page);
  ST_REQUIRE(bubble != nullptr);
  ST_CHECK(bubble->style().border_width == 0.0f);
}
