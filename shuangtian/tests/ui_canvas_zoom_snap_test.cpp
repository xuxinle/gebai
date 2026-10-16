/// louyue 视口回流（第二跳）的档位不变量回归钉。
///
/// 回流后缩放真值在内置 `Viewport`（连续量），louyue 的产品语义是**离散档**
/// （设计文档 §3.3：25/50/100/200/400/800——连续缩放难停准 100%）。档位不变量
/// 的收口是 `CanvasViewport::snap_zoom`（挂 `on_view_changed`）：任意来源的
/// 连续值（协议 `set zoom`、`fit` 适配值）终值恒在档位表上。
///
/// 旧隐疾（本测试根治的对象）：协议 `set zoom 0.3` 能让连续值混进档位系统
/// （显示 30%，下一步 zoom_in 却跳 50%）——档位语义静默漂移。
///
/// 注意测试**直接驱动外壳的公共面**（set_zoom/snap_zoom/zoom_in/zoom_out/
/// measure/arrange），不经控制通道——档位语义是元素层属性，单测层就能钉死。

#include <cmath>
#include <memory>

#include "st/test/test.hpp"
#include "st/ui/theme.hpp"

#include "../../examples/louyue/canvas_view.hpp"

namespace {

using louyue::kZoomSteps;
using louyue::nearest_zoom_step;

[[nodiscard]] auto near(float a, float b, float tolerance = 1.0e-4f) -> bool {
  return std::fabs(a - b) <= tolerance;
}

/// 与 ui_viewport_test.cpp 的 Fixture 同形态（Theme + RenderContext + Panel 内容）。
/// 被测对象是 louyue 的外壳：`CanvasViewport` 的档位/步进/首帧 fit 全是头内
/// inline（只依赖基类与档位表），本测试**不链也不需要** CanvasView——
/// tests 目标不含 examples 源，链它才是坑。
struct Fixture {
  st::ui::Theme theme{st::ui::Theme::light()};
  st::ui::RenderContext context{theme, nullptr, 0.0};
  louyue::CanvasViewport shell{};

  Fixture() {
    auto content = std::make_unique<st::ui::Panel>();
    content->set_id("content");
    content->style().width = 320.0f;   // 与 louyue 默认文档同尺寸
    content->style().height = 200.0f;
    shell.set_id("shell");
    shell.set_content(std::move(content));
    shell.apply_theme(theme);
  }

  /// 600×400 的视口里装 320×200 的内容。
  void layout() {
    shell.measure(context, st::ui::Constraints{600.0f, 400.0f});
    shell.arrange(context, st::math::Rect{0.0f, 0.0f, 600.0f, 400.0f});
  }
};

}  // namespace

ST_TEST(canvas_zoom_steps_match_design_doc) {
  // 六档 25/50/100/200/400/800（设计文档 §3.3；单值都是二进制精确分数，
  // 相等比较无舍入问题）。
  static_assert(std::size(kZoomSteps) == 6U);
  ST_CHECK_EQ(kZoomSteps[0], 0.25f);
  ST_CHECK_EQ(kZoomSteps[1], 0.5f);
  ST_CHECK_EQ(kZoomSteps[2], 1.0f);
  ST_CHECK_EQ(kZoomSteps[3], 2.0f);
  ST_CHECK_EQ(kZoomSteps[4], 4.0f);
  ST_CHECK_EQ(kZoomSteps[5], 8.0f);
}

ST_TEST(canvas_zoom_snap_pulls_arbitrary_values_to_steps) {
  // 吸附核心：任意连续值 → 最近档。并列取靠前（1.5 在 1.0/2.0 正中 → 1.0），
  // 超出档位范围的一律收口到边界档（这正是「不变量」的语义）。
  const struct {
    float given;
    float expected;
  } cases[] = {{0.3f, 0.25f}, {0.6f, 0.5f}, {1.5f, 1.0f}, {3.9f, 4.0f},
               {12.0f, 8.0f}, {0.01f, 0.25f}};
  for (const auto& item : cases) {
    ST_CHECK(near(nearest_zoom_step(item.given), item.expected));
  }
}

ST_TEST(canvas_viewport_snap_after_continuous_set) {
  // 旧隐疾的根治路径：协议 set 进来的连续值，snap 后终值恒在档位表上。
  Fixture fx;
  fx.shell.set_zoom(0.3f);
  fx.shell.snap_zoom();
  ST_CHECK(near(fx.shell.zoom(), 0.25f));
  // 已是档位值时 snap 无操作（不会漂移到别的档）。
  fx.shell.set_zoom(0.5f);
  fx.shell.snap_zoom();
  ST_CHECK(near(fx.shell.zoom(), 0.5f));
}

ST_TEST(canvas_viewport_step_walks_discrete_table) {
  // zoom_in/zoom_out（e2e 契约动作）按档爬，且在边界档停住（不空转、不越界）。
  Fixture fx;
  fx.shell.set_zoom(1.0f);
  fx.shell.zoom_in();
  ST_CHECK(near(fx.shell.zoom(), 2.0f));
  fx.shell.zoom_in();
  fx.shell.zoom_in();
  ST_CHECK(near(fx.shell.zoom(), 8.0f));
  fx.shell.zoom_in();   // 已在顶档：夹住
  ST_CHECK(near(fx.shell.zoom(), 8.0f));
  fx.shell.zoom_out();
  ST_CHECK(near(fx.shell.zoom(), 4.0f));
}

ST_TEST(canvas_viewport_step_from_off_step_value_lands_on_table) {
  // 连续值混入后的下一步也落在档位表上：0.3 → 就近档 0.25 → +1 = 0.5
  //（而不是凭空跳档——step 的取档基准是「就近档」而不是「当前值」）。
  Fixture fx;
  fx.shell.set_zoom(0.3f);
  fx.shell.zoom_in();
  ST_CHECK(near(fx.shell.zoom(), 0.5f));
}

ST_TEST(canvas_viewport_first_arrange_fits_content) {
  // 首次落位 = 一次 fit：320×200 的内容在 600×400 的视口里取
  // min(600/320, 400/200, 1) = 1.0，居中（内容左上 = (140, 100)）。
  Fixture fx;
  fx.layout();
  ST_CHECK(near(fx.shell.zoom(), 1.0f));
  const st::ui::Element* content = fx.shell.content();
  ST_REQUIRE(content != nullptr);
  ST_CHECK(near(content->bounds().x, 140.0f));
  ST_CHECK(near(content->bounds().y, 100.0f));
  ST_CHECK(near(content->bounds().width, 320.0f));
  ST_CHECK(near(content->bounds().height, 200.0f));
}

ST_TEST(canvas_viewport_rearrange_keeps_view_state) {
  // 首帧之后位置**保持**：resize/重组不再自动重居中（旧满铺实现每次布局
  // 都居中，会把用户的平移悄悄重置）。fit 只在首次落位发生一次。
  Fixture fx;
  fx.layout();
  const float zoom_before = fx.shell.zoom();
  fx.shell.set_pan(st::math::Point{31.0f, -17.0f});   // 模拟用户平移过
  fx.layout();                                         // 再排一次（同尺寸）
  ST_CHECK(near(fx.shell.pan().x, 31.0f));
  ST_CHECK(near(fx.shell.pan().y, -17.0f));
  ST_CHECK(near(fx.shell.zoom(), zoom_before));
}
