/// **主题重算只在「主题变更」与「新元素上树」时发生** —— 本次重构的语义契约。
///
/// ## 这一组测试要钉住什么
///
/// 2026-11-23 之前，`UiRoot::layout()` 把「主题下发」放在脏检查**之前**且**无条件**，
/// 而 `paint()` 第一行就是 `layout()` ⇒ **每帧 × 每个元素**都在重算主题。
///
/// 由此链出一条正反馈（本次要切断的东西）：
///
/// ```
/// 每帧重算 → 组件 apply_theme 把 style_ 重置成主题值 → 调用方的显式声明被吃掉
///          → 只好在 RenderExtras 里存存档回放 → 存档又被每帧重算改写
///          → 再存一份原始值（`top_highlight_base_alpha` = 补偿的二次方）
/// ```
///
/// 实测速率：2026-11-19 一天之内 `RenderExtras` 涨了 8 个 `_override`/`_declared` 字段。
///
/// ## 为什么这些测试必须在改动前是红的
///
/// 按 `CONVENTIONS.md` §7.1：**先让测试红，再让它绿**。若 `theme_apply_not_per_frame`
/// 在改动前就是绿的，那它没有验证任何东西（说明探针没挂对）。
/// 本文件的计数断言用派生探针类数 `apply_theme` 命中次数——改动前连跑 3 帧必然得到 3
/// （每帧一次），改动后是 1（仅首帧）。
///
/// ## 一个必须说清的边界：为什么 setter 要显式请求重算
///
/// 取消每帧重算会切断**第二条**隐式通道：全仓 31 处 setter 只改成员 + 标脏，
/// 靠下一帧的 `apply_theme` 把成员翻译成 `style_`。只做「版本号」不做「setter 请求」，
/// 会把「声明被覆盖」换成「setter 静默失效」（设了参数、`get` 回读对、画面不变）。
/// `setter_still_reaches_style_across_frames` 就是钉这条的。

#include <memory>
#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
namespace math = st::math;

/// 探针元素：数 `apply_theme` 被调了几次（本文件所有计数类断言的量尺）。
///
/// 为什么用派生类而不是给 `Element` 加测试钩子：加钩子会把测试关注点
/// 钉进生产基类（`Element` 已经 223 个成员函数，本次正是要减不是加）；
/// 派生探针零侵入，且 `apply_theme` 本来就是虚函数——那正是它的公开扩展点。
struct CountingLeaf : Element {
  int apply_count{0};

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CountingLeaf"; }

  void apply_theme(const Theme& theme) override {
    ++apply_count;
    Element::apply_theme(theme);   // 基类实现 = 统一层回放（保持真实行为）
  }
};

/// 把一棵带探针的树挂进 `UiRoot`，按创建顺序返回探针裸指针。
auto mount_probes(UiRoot& root, int leaves) -> std::vector<CountingLeaf*> {
  auto column = std::make_unique<Panel>(FlexDirection::Column);
  std::vector<CountingLeaf*> probes;
  for (int index = 0; index < leaves; ++index) {
    auto leaf = std::make_unique<CountingLeaf>();
    leaf->set_id(std::format("leaf-{}", static_cast<unsigned>(index)));
    probes.push_back(leaf.get());
    column->add_child(std::move(leaf));
  }
  root.set_viewport({320.0f, 240.0f});
  root.set_content(std::move(column));
  return probes;
}

}  // namespace

ST_TEST(theme_apply_not_per_frame) {
  // **本次重构的核心契约**：无主题变更时，连跑多帧只应重算一次（首帧）。
  //
  // 改动前：每帧一次 → 3 帧得到 3；改动后：只首帧 → 1。
  UiRoot root;
  const std::vector<CountingLeaf*> probes = mount_probes(root, 3);
  ST_REQUIRE(probes.size() == 3U);

  st::raster::Canvas canvas{320, 240, 1.0F};
  const auto frame = [&](double time) {
    root.set_time(time);
    root.paint_frame(canvas);   // paint 第一行就是 layout()——主题传播在其中
    root.clear_dirty();
  };

  frame(1.0);   // 首帧：元素从未应用过主题（applied_theme_version == 0）→ 必算
  for (const CountingLeaf* probe : probes) {
    ST_CHECK_EQ(probe->apply_count, 1);
  }

  // 后续帧：主题没变、元素没请求 → **一次都不该重算**。
  // 这条是「每帧重算」被真正切断的唯一证据。
  for (int index = 0; index < 3; ++index) {
    frame(1.1 + static_cast<double>(index) * 0.1);
  }
  for (const CountingLeaf* probe : probes) {
    ST_CHECK_EQ(probe->apply_count, 1);
  }
}

ST_TEST(theme_change_triggers_full_refresh) {
  // 换主题必须让**整棵树**重算一次（版本号路径）。
  //
  // 为什么这条不能省：如果把传播条件写成「只在 applied_theme_version == 0 时算」，
  // 上一条测试会绿，而换肤彻底不生效——那正是本次最该防的「切了主题界面不变」。
  UiRoot root;
  const std::vector<CountingLeaf*> probes = mount_probes(root, 3);
  st::raster::Canvas canvas{320, 240, 1.0F};

  root.set_time(1.0);
  root.paint_frame(canvas);
  for (const CountingLeaf* probe : probes) ST_CHECK_EQ(probe->apply_count, 1);

  root.set_theme(Theme::dark());
  root.set_time(1.1);
  root.paint_frame(canvas);
  for (const CountingLeaf* probe : probes) ST_CHECK_EQ(probe->apply_count, 2);
}

ST_TEST(new_element_gets_current_theme) {
  // 新上树的元素必须立刻拿到**当前**主题（不能因为「主题没变」就被漏掉）。
  //
  // 这条守着 `applied_theme_version == 0` 那个条件：它同时承担「新元素」与
  // 「从别处搬来的子树」两种情况。
  UiRoot root;
  const std::vector<CountingLeaf*> probes = mount_probes(root, 1);
  st::raster::Canvas canvas{320, 240, 1.0F};
  root.set_time(1.0);
  root.paint_frame(canvas);
  ST_CHECK_EQ(probes[0]->apply_count, 1);

  // 主题现在已经「应用过一次」，之后再挂新元素——它也必须算一次。
  root.set_theme(Theme::dark());
  root.set_time(1.1);
  root.paint_frame(canvas);
  ST_CHECK_EQ(probes[0]->apply_count, 2);

  auto late = std::make_unique<CountingLeaf>();
  CountingLeaf* late_probe = late.get();
  root.content()->add_child(std::move(late));
  root.set_time(1.2);
  root.paint_frame(canvas);
  ST_CHECK_EQ(late_probe->apply_count, 1);
  // 老元素不该被牵连（它们的版本号已经等于当前版本）。
  ST_CHECK_EQ(probes[0]->apply_count, 2);
}

ST_TEST(detached_element_reattached_gets_refresh) {
  // 子树从树 A 摘下挂到树 B：主题必须重新应用一次。
  //
  // 为什么需要清版本号：`applied_theme_version_` 只记数字、不知道那个数字属于哪棵树，
  // 而所有 `UiRoot` 的版本号都从 1 起——撞号就会漏算。实测存在的搬家路径：
  // `control/server.cpp` 的 `ui.create`（包一层再放回）、桌面壳换 Dock、`set_content`。
  UiRoot root_a;
  const std::vector<CountingLeaf*> probes = mount_probes(root_a, 1);
  st::raster::Canvas canvas{320, 240, 1.0F};
  root_a.set_time(1.0);
  root_a.paint_frame(canvas);
  ST_CHECK_EQ(probes[0]->apply_count, 1);

  // 取出整棵内容树（`take_content` 会清版本号），挂进另一个 UiRoot。
  std::unique_ptr<Element> moved = root_a.take_content();
  ST_REQUIRE(moved != nullptr);
  UiRoot root_b;
  root_b.set_viewport({320.0f, 240.0f});
  root_b.set_content(std::move(moved));
  root_b.set_time(1.0);
  root_b.paint_frame(canvas);
  // 新宿主下必须重算一次（否则配色停在上一个宿主的主题上）。
  ST_CHECK_EQ(probes[0]->apply_count, 2);
}

ST_TEST(setter_still_reaches_style_across_frames) {
  // setter 改动必须跨帧落到 `style_`（钉「隐式第二条通道」被显式化）。
  //
  // `Card::set_shadow_level` 只改成员 `shadow_level_`，阴影是 `apply_theme` 算的。
  // 改动前靠每帧重算兜住；改动后靠 setter 自己 `request_theme_refresh()`。
  // 若漏了那个调用，本用例会看到「`shadow_level()` 读回 0，`style().shadow` 仍是旧档」。
  UiRoot root;
  auto card = std::make_unique<Card>();
  card->set_id("card");
  Card* raw = card.get();
  root.set_content(std::move(card));
  root.set_viewport({320.0f, 240.0f});

  st::raster::Canvas canvas{320, 240, 1.0F};
  root.set_time(1.0);
  root.paint_frame(canvas);
  ST_REQUIRE(raw->style().shadow.visible());   // 默认档位有投影（前提）

  raw->set_shadow_level(0);
  root.set_time(1.1);
  root.paint_frame(canvas);
  ST_CHECK(!raw->style().shadow.visible());

  raw->set_shadow_level(2);
  root.set_time(1.2);
  root.paint_frame(canvas);
  ST_CHECK(raw->style().shadow.visible());
}

ST_TEST(disabled_state_still_reaches_style_across_frames) {
  // `set_enabled` 也必须跨帧落到样式（6 个组件的 `apply_theme` 读 `enabled_`）。
  //
  // 这是本次重构里**最容易漏**的一条：`set_enabled` 原先连 `mark_dirty()` 都不写，
  // 完全靠「每帧重算主题」把它送下去。取消每帧重算后不补 `request_theme_refresh()`
  // 就会「设了 disabled、`enabled()` 读回 false、看着仍是可用态」。
  //
  // 判据用 `Button` 的主色底：禁用态走 `inactive` 分支换成 `primary_soft`。
  UiRoot root;
  auto button = std::make_unique<Button>("动作", Button::Variant::Primary);
  button->set_id("btn");
  Button* raw = button.get();
  root.set_content(std::move(button));
  root.set_viewport({320.0f, 240.0f});

  st::raster::Canvas canvas{320, 240, 1.0F};
  root.set_time(1.0);
  root.paint_frame(canvas);
  const Theme light = Theme::light();
  const math::Color normal = light.colors().primary;
  ST_CHECK(raw->style().background.r == normal.r);
  ST_CHECK(raw->style().background.g == normal.g);

  raw->set_enabled(false);
  root.set_time(1.1);
  root.paint_frame(canvas);
  const math::Color disabled_fill = light.colors().primary_soft;
  ST_CHECK_EQ(raw->style().background.r, disabled_fill.r);
  ST_CHECK_EQ(raw->style().background.g, disabled_fill.g);

  raw->set_enabled(true);
  root.set_time(1.2);
  root.paint_frame(canvas);
  ST_CHECK_EQ(raw->style().background.r, normal.r);
}

ST_TEST(theme_token_change_survives_many_frames) {
  // 运行时改主题 token 后连跑多帧仍生效（控制通道 `theme.set` 走的就是 set_theme）。
  //
  // 这条守着「版本号只在 set_theme 里 bump」这个收敛点：若改动后忘了 bump，
  // 换 token 会一次都不重算（界面完全不变）；若 bump 了但条件写错成每帧，
  // 上一条 `theme_apply_not_per_frame` 会红。两条互补。
  UiRoot root;
  const std::vector<CountingLeaf*> probes = mount_probes(root, 2);
  st::raster::Canvas canvas{320, 240, 1.0F};
  root.set_time(1.0);
  root.paint_frame(canvas);
  for (const CountingLeaf* probe : probes) ST_CHECK_EQ(probe->apply_count, 1);

  Theme custom = Theme::light();
  custom.colors().primary = math::Color{0xFF, 0x00, 0x00, 0xFF};
  root.set_theme(custom);
  root.set_time(1.1);
  root.paint_frame(canvas);
  ST_CHECK(root.theme().colors().primary.r == 0xFFU);
  ST_CHECK(root.theme().colors().primary.g == 0x00U);
  for (const CountingLeaf* probe : probes) ST_CHECK_EQ(probe->apply_count, 2);

  // 连跑 10 帧：不该再有重算（主题没再变），且 token 也不该被改回去。
  for (int index = 0; index < 10; ++index) {
    root.set_time(1.2 + static_cast<double>(index) * 0.1);
    root.paint_frame(canvas);
    ST_CHECK_EQ(root.theme().colors().primary.r, 0xFFU);
  }
  for (const CountingLeaf* probe : probes) ST_CHECK_EQ(probe->apply_count, 2);
}
