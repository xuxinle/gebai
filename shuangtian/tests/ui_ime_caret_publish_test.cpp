/// 输入法插入点上报契约（`UiRoot::publish_ime_caret`）的**空矩形不外出**规则。
///
/// ## 守的是什么
///
/// 用户报：**首次输入时输入法候选窗没对准光标，第二次就对了**。
///
/// 真窗口实测日志（`[ime]` 追踪，2026-11-28）给出完整时序：
///
/// ```
/// [ime] set_caret x=0.0 y=0.0 w=0.0 h=0.0        ← 空矩形被当成“位置”发了出去
/// [ime] apply screen=(153,113) logical=(0.0,0.0) ← 候选窗被钉到客户区左上角
/// [ime] SETCONTEXT caret=0.0,0.0                 ← 输入法就用这个值弹窗 = 错位
/// ...
/// [ime] set_caret x=38.0 y=1228.7                ← 真实值，但已经晚了
/// ```
///
/// 语义修正：**空矩形 = “本元素这一刻没有插入点”，不是一个坐标**，不能下发。
///
/// ## 判据是怎么选的
///
/// 直接调 `UiRoot::publish_ime_caret` 的私有路径不可达，因此本用例走公开入口
/// `set_focus`（真实聚焦路径）：聚焦时元素插入点缓存为空 ⇒ 修复前必然下发一个
/// `(0,0,0,0)`，修复后 sink 一次都不该被调。

#include <vector>

#include "st/math/geometry.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/text_area.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Rect;
using st::math::Size;
using st::ui::TextArea;
using st::ui::UiRoot;

/// 装配一个「已排版、但插入点缓存被清空」的输入框 —— 即首次聚焦那一瞬的状态。
struct Fixture {
  UiRoot root;
  TextArea* area{nullptr};
  std::vector<Rect> reported;

  Fixture() {
    root.set_theme(st::ui::Theme::light());
    root.set_viewport(Size{400.0F, 300.0F});
    root.ime_caret_sink = [this](Rect caret) { reported.push_back(caret); };
    auto owned = std::make_unique<TextArea>();
    area = owned.get();
    root.set_content(std::move(owned));
    root.layout(true);
  }
};

}  // namespace

ST_TEST(ime_caret_publish_skips_empty_rect) {
  Fixture fixture;
  // 聚焦前把缓存清空（真实启动里这个状态由“内容刚挂上还没绘制就聚焦”造成）。
  fixture.area->invalidate_caret_for_test();
  const std::size_t before = fixture.reported.size();

  ST_CHECK(fixture.root.set_focus(fixture.area));

  // 核心断言：**空矩形一次都不许下发**。
  for (std::size_t i = before; i < fixture.reported.size(); ++i) {
    ST_CHECK(!fixture.reported[i].is_empty());
  }
}

ST_TEST(ime_caret_publish_sends_real_value_when_geometry_ready) {
  // 对照组：几何就绪 + 缓存有效时，聚焦必须正常上报一个有效位置
  // （证明上面的修复没有把功能整个关掉）。
  Fixture fixture;
  ST_CHECK(fixture.root.set_focus(fixture.area));
  ST_REQUIRE(!fixture.reported.empty());
  const Rect caret = fixture.reported.back();
  ST_CHECK(!caret.is_empty());
  ST_CHECK(caret.height > 1.0F);
  ST_CHECK(caret.y > 0.0F);   // 落在输入框那一行，不是客户区顶端
}

ST_TEST(ime_caret_publish_no_position_without_focus) {
  // 没有焦点元件时不该凭空下发位置（`focused_ == nullptr` 同样走空矩形分支）。
  Fixture fixture;
  for (const Rect& caret : fixture.reported) ST_CHECK(!caret.is_empty());
}
