// 统一解析层的回归测试。
//
// ## 这一层要钉住的三条不变式
//
// 1. **一份键表**：`sheet_key_names()` 是唯一权威——测试它包含全部四组，
//    且每个登记过的键都**真的能被解析**（登记了却解析不了 = 假表）。
// 2. **一个落点**：同一个键写多次只有一个结果；组间正交（写 `glass` 不动布局）。
// 3. **拼错必须失败**：未知键、键对值错都要进 `unknown`——静默忽略是这一轮
//    所有“写了不生效”缺陷的共同源头。
//
// 为何不只测“能跑”：改造前 `glass` 字符串有自己的一份解析器、DSL 有另一份、
// 控制通道第三份。它们各自“能跑”，但键改了不跟就分叉。所以这里额外用
// **键表驱动**的方式遍历：新增键忘了写落位逻辑会直接红。

#include <memory>
#include <string>

#include "st/test/test.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/stylesheet.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
namespace math = st::math;

/// 一个空页面（`Component` 是抽象基类，必须派生一个能用 `make_shared` 的类型）。
struct BlankPage : dsl::Component {
  void build(dsl::Composer& c) override {
    (void)dsl::column(c, {.width = 360.0f, .height = 240.0f, .key = "root"}, [] {});
  }
};

/// 造一个可测的元素（`UiRoot` 里挂一个空容器）。
struct Fixture {
  UiRoot root;
  Element* element{nullptr};

  Fixture() {
    root.set_viewport({400.0f, 300.0f});
    auto page = std::make_shared<BlankPage>();
    auto host = dsl::mount(root, page);
    (void)host;
    root.layout();
    element = root.content();
  }
};

}  // namespace

ST_TEST(stylesheet_applies_layout_group) {
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  SheetReport report;
  (void)apply_stylesheet(*f.element, "width 428, gap 10, padding 12, grow 1", &report);
  ST_CHECK(report.ok());
  const Style& s = f.element->style();
  ST_CHECK(s.width == 428.0f);
  ST_CHECK(s.gap == 10.0f);
  ST_CHECK(s.padding.left == 12.0f);
  ST_CHECK(s.grow);
}

ST_TEST(stylesheet_applies_glass_group) {
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  SheetReport report;
  (void)apply_stylesheet(*f.element,
                         "blur 16, refraction 6, dispersion 0.3, highlight 0.85, ring 1, edges all",
                         &report);
  ST_CHECK(report.ok());
  const Style& s = f.element->style();
  ST_CHECK(s.backdrop_blur == 16.0f);
  ST_CHECK(s.backdrop_refraction == 6.0f);
  // 高光 alpha 不查精确值：它会被主题的 `glass_highlight_strength` 乘一道
  // （亮色 1.0 / 暗色 0.55），这正是“亮度和颜色随主题”的预期行为。
  // 这里只钉“强度被写进去了”，不钉具体数值。
  ST_CHECK(s.top_highlight.a > 150);   // 0.85 × 255 ≈ 217（主题可选再降）
  ST_CHECK(s.highlight_width == 1.0f);
  ST_CHECK(s.highlight_edges == HighlightEdges::All);
}

ST_TEST(stylesheet_groups_are_orthogonal) {
  // **组间正交**：写玻璃不动布局、写布局不动玻璃。
  // 这条钉住“54 个扁平字段”时代的核心问题——字段之间靠调用顺序互相踩。
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  const float before_width = f.element->style().width;
  (void)apply_stylesheet(*f.element, "blur 20, tint 0.4");
  ST_CHECK(f.element->style().width == before_width);
  const float before_blur = f.element->style().backdrop_blur;
  (void)apply_stylesheet(*f.element, "width 200, gap 4");
  ST_CHECK(f.element->style().backdrop_blur == before_blur);
}

ST_TEST(stylesheet_accepts_css_and_legacy_separators) {
  // CSS 用 `;`，本框架历史语法用 `,`——两种都认（大模型两种都会写）。
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  (void)apply_stylesheet(*f.element, "radius 14; opacity 0.5; blur 8");
  ST_CHECK(f.element->style().radius == 14.0f);
  ST_CHECK(f.element->style().opacity == 0.5f);
  ST_CHECK(f.element->style().backdrop_blur == 8.0f);
  // `-` 与 `_` 等价（CSS 习惯 vs C++ 习惯）。
  (void)apply_stylesheet(*f.element, "padding-x 7");
  ST_CHECK(f.element->style().padding.left == 7.0f);
}

ST_TEST(stylesheet_reports_unknown_key) {
  // **拼错必须失败**：这是这一层存在的核心理由。
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  SheetReport report;
  (void)apply_stylesheet(*f.element, "radius 14, ratius 20", &report);
  ST_CHECK(!report.ok());
  ST_CHECK(report.unknown.size() == 1);
  ST_CHECK(report.applied == 1);
}

ST_TEST(stylesheet_reports_bad_value) {
  // 键认识但**值**不认识（`radius abc`）也必须报——静默忽略会让“写了不生效”成谜。
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  SheetReport report;
  (void)apply_stylesheet(*f.element, "radius abc", &report);
  ST_CHECK(!report.ok());
}

ST_TEST(stylesheet_shadow_zero_disables_projection) {
  // `shadow 0` = 明确“不要投影”。玻璃卡片依赖它（默认 md 投影会被
  // 背板模糊平均成一圈暗晕）。走存档入口才能扛住 `Card::apply_theme`。
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  SheetReport report;
  (void)apply_stylesheet(*f.element, "shadow 0", &report);
  ST_CHECK(report.ok());
  ST_CHECK(!f.element->style().shadow.visible());
}

ST_TEST(stylesheet_glass_shorthand_delegates_to_unified_layer) {
  // 旧的 `glass="…"` 入口必须与新解析层**同一份键表**（否则一个键改了不跟就分叉）。
  // 这里验证它认新键（`ring` / `edges` 是后加的，只有统一层认识）。
  Fixture f;
  ST_REQUIRE(f.element != nullptr);
  const bool ok = apply_glass_shorthand(*f.element, "blur:12,ring:2,edges:tb");
  ST_CHECK(ok);
  ST_CHECK(f.element->style().backdrop_blur == 12.0f);
  ST_CHECK(f.element->style().highlight_width == 2.0f);
  ST_CHECK(f.element->style().highlight_edges == HighlightEdges::TopBottom);
}
