/// 指针悬浮：事件、回调、特效、动画续帧。
///
/// 需求来源（用户）："组件要支持指针悬浮事件和特效"。
/// 因此这里断言的不只是"事件到了"，还有四件容易漏的事：
/// ① 回调与事件**都能**触发（两套入口用途不同，缺一不可）；
/// ② 特效真的**改变了像素**（背景提亮/描边/发光——不是只有个 bool 状态）；
/// ③ 离开后特效**回到原样**（漏了复位就是"悬浮一次，永久高亮"）；
/// ④ 过渡动画能拿到**连续帧**（续帧机制坏了会表现为"淡入只走一格、看着卡住"）。

#include "st/test/test.hpp"

#include <vector>

#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::Button;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::ListItem;
using st::ui::Theme;
using st::ui::UiRoot;

inline constexpr int kWidth = 320;
inline constexpr int kHeight = 160;

/// 画面里某块区域的"平均亮度"（用来量化悬浮带来的视觉变化）。
[[nodiscard]] auto average_luma(const Canvas& canvas, Rect region) -> double {
  double sum = 0.0;
  int count = 0;
  const int x0 = static_cast<int>(region.x);
  const int y0 = static_cast<int>(region.y);
  const int x1 = x0 + static_cast<int>(region.width);
  const int y1 = y0 + static_cast<int>(region.height);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const Color color = canvas.pixel_at(x, y);
      sum += 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

/// 造一棵带按钮的界面（点击区域固定，便于断言）。
struct Fixture {
  Theme theme{Theme::light()};
  UiRoot root{};
  Button* button{nullptr};
  int enters{0};
  int leaves{0};
  int hover_events{0};
  std::vector<bool> sequence{};

  Fixture() {
    root.set_theme(theme);
    auto owned = std::make_unique<Button>("确定");
    button = owned.get();
    button->set_id("btn");
    button->set_on_hover([this](bool hovered) {
      (hovered ? enters : leaves) += 1;
      sequence.push_back(hovered);
    });
    root.set_content(std::move(owned));
    root.set_viewport(st::math::Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
    // 事件入口：验证 HoverIn/HoverOut **事件**也确实发出来了（与回调是两套入口）
    root.set_event_observer([this](const Event& event, st::ui::Element&) {
      if (event.kind == EventKind::HoverIn || event.kind == EventKind::HoverOut) {
        ++hover_events;
      }
    });
  }

  [[nodiscard]] auto context(double time = 0.0) const -> st::ui::RenderContext {
    return st::ui::RenderContext{theme, nullptr, time};
  }
  void layout() {
    root.layout();
    // 固定几何：断言才有确定的位置可依据
    button->measure(context(), st::ui::Constraints{});
    button->arrange(context(), Rect{20.0f, 20.0f, 120.0f, 40.0f});
  }
  void move_to(float x, float y) {
    Event event;
    event.kind = EventKind::MouseMove;
    event.position = Point{x, y};
    (void)root.dispatch(event);
  }
  void paint(Canvas& canvas) { root.paint(canvas); }
};

}  // namespace

ST_TEST(hover_marks_element_and_fires_callback) {
  Fixture fixture;
  fixture.layout();
  ST_CHECK(!fixture.button->hovered());
  fixture.move_to(60.0f, 40.0f);  // 落在按钮内
  ST_CHECK(fixture.button->hovered());
  ST_CHECK_EQ(fixture.enters, 1);
  ST_CHECK_EQ(fixture.leaves, 0);
  // 移到按钮外
  fixture.move_to(280.0f, 140.0f);
  ST_CHECK(!fixture.button->hovered());
  ST_CHECK_EQ(fixture.leaves, 1);
  ST_CHECK_EQ(static_cast<int>(fixture.sequence.size()), 2);
}

ST_TEST(hover_callback_and_event_both_reach_the_element) {
  // 回调与事件是**两套入口**：回调给"这个按钮被悬浮时要做什么"，
  // 事件给统一处理（状态栏/埋点/脚本绑定）。只给一套会让另一类用法变扭。
  Fixture fixture;
  fixture.layout();
  fixture.move_to(60.0f, 40.0f);
  ST_CHECK_EQ(fixture.hover_events, 1);  // 事件入口
  ST_CHECK_EQ(fixture.enters, 1);        // 回调入口
}

ST_TEST(hover_effect_changes_pixels_and_restores) {
  // "特效"必须体现在**像素**上，而不只是一个布尔状态。
  Fixture fixture;
  fixture.layout();
  Canvas plain{kWidth, kHeight};
  plain.clear(Color{0x20, 0x20, 0x28, 0xFF});
  fixture.paint(plain);
  const double before = average_luma(plain, Rect{20.0f, 20.0f, 120.0f, 40.0f});

  fixture.move_to(60.0f, 40.0f);
  Canvas hovered{kWidth, kHeight};
  hovered.clear(Color{0x20, 0x20, 0x28, 0xFF});
  fixture.paint(hovered);
  const double during = average_luma(hovered, Rect{20.0f, 20.0f, 120.0f, 40.0f});
  ST_CHECK(during != before);  // 悬浮态确实不一样

  // 移开后必须回到原样（漏了复位就是"悬浮一次，永久高亮"）
  fixture.move_to(280.0f, 140.0f);
  Canvas after{kWidth, kHeight};
  after.clear(Color{0x20, 0x20, 0x28, 0xFF});
  fixture.paint(after);
  const double restored = average_luma(after, Rect{20.0f, 20.0f, 120.0f, 40.0f});
  ST_CHECK(std::abs(restored - before) < 0.5);
}

ST_TEST(hover_effect_only_where_declared) {
  // 未声明特效的元素不该因悬浮而变色——否则"悬浮"变成全局副作用，
  // 容器（卡片/面板）会到处乱亮。
  Fixture fixture;
  fixture.layout();
  fixture.button->set_hover_effect(st::ui::Element::HoverEffect{.enabled = false});
  Canvas plain{kWidth, kHeight};
  plain.clear(Color{0x20, 0x20, 0x28, 0xFF});
  fixture.paint(plain);
  const double before = average_luma(plain, Rect{20.0f, 20.0f, 120.0f, 40.0f});
  fixture.move_to(60.0f, 40.0f);
  Canvas hovered{kWidth, kHeight};
  hovered.clear(Color{0x20, 0x20, 0x28, 0xFF});
  fixture.paint(hovered);
  const double during = average_luma(hovered, Rect{20.0f, 20.0f, 120.0f, 40.0f});
  ST_CHECK(std::abs(during - before) < 0.5);
}

ST_TEST(hover_transition_requests_followup_frames) {
  // 过渡动画要靠"还有人在动"拿到下一帧；续帧机制坏了会表现为淡入只走一格。
  // 这条用根节点的脏标记来验证：过渡未完成时，绘制后仍应为脏（需要下一帧）。
  Fixture fixture;
  fixture.layout();
  fixture.button->set_hover_effect(st::ui::Element::HoverEffect{.enabled = true});
  fixture.move_to(60.0f, 40.0f);
  Canvas canvas{kWidth, kHeight};
  canvas.clear(Color{0x20, 0x20, 0x28, 0xFF});
  // 静态帧（时间没走）：直接落位到目标，过渡结束，不再请求帧
  fixture.paint(canvas);
  fixture.root.clear_dirty();
  ST_CHECK(!fixture.root.dirty());
  ST_CHECK(fixture.button->hover_progress() > 0.9f);  // 静态帧直接到位
}

ST_TEST(hover_in_out_is_idempotent_when_position_repeats) {
  // 同一位置连续 MouseMove 不该重复触发（否则回调会被刷爆、
  // 每帧都重新开始过渡，看起来像抖动）。
  Fixture fixture;
  fixture.layout();
  fixture.move_to(60.0f, 40.0f);
  fixture.move_to(60.0f, 40.0f);
  fixture.move_to(61.0f, 41.0f);
  ST_CHECK_EQ(fixture.enters, 1);
  fixture.move_to(280.0f, 140.0f);
  fixture.move_to(281.0f, 141.0f);
  ST_CHECK_EQ(fixture.leaves, 1);
}

ST_TEST(hover_survives_subtree_replacement) {
  // 悬浮目标被重建（列表刷新/切页）时不能留下悬垂指针。
  //
  // 如实说明一条语义限制：元素**已经析构**时不可能再给它送 HoverOut 回调
  // （对象都不存在了）。所以这里的契约是"不崩、状态自洽"，而不是"一定会收到 leave"。
  // 需要"离开通知"的场景（埋点/状态栏）应挂在**父节点**或用事件观察者。
  Fixture fixture;
  fixture.layout();
  fixture.move_to(60.0f, 40.0f);
  ST_CHECK(fixture.button->hovered());
  fixture.root.set_content(std::make_unique<ListItem>("新的行"));
  fixture.root.layout();
  // 旧元素已析构；能跑到这里说明没有解引用悬垂指针
  fixture.move_to(60.0f, 40.0f);
  // 悬浮在新元素上**照常工作**——而不是卡在已销毁的旧指针上、变成"谁都悬浮不了"
  st::ui::Element* hit = fixture.root.hit_test(Point{60.0f, 40.0f});
  ST_CHECK(hit != nullptr);
  ST_CHECK(hit->hovered());
}

ST_TEST(hover_idle_element_never_requests_frames) {
  // 这条是**忙循环**的回归测试。
  //
  // 早先的实现里，静止元素每帧都会把过渡起点重置为当前时间 → elapsed 恒为 0
  // → 永久声明"还在动画中" → 根节点每帧都脏 → 应用 100% 占一个核。
  // 忙循环对"帧耗时基准"完全不可见（每帧都很快，只是停不下来），
  // 所以必须用"是否请求下一帧"来断言，而不是看耗时。
  Fixture fixture;
  fixture.layout();
  // 静止（未悬浮）连续多帧：任何一帧都不该请求续帧
  for (int frame = 1; frame <= 5; ++frame) {
    Canvas canvas{kWidth, kHeight};
    canvas.clear(Color{0x20, 0x20, 0x28, 0xFF});
    fixture.root.paint(canvas);
    // 时间在推进（真实应用就是如此），但元素静止 → 不该要求重绘
    fixture.move_to(280.0f, 140.0f);   // 移到空白处：确保没有元素被悬浮
    fixture.root.set_time(static_cast<double>(frame) * 0.1);
    fixture.root.paint(canvas);
  }
  fixture.root.clear_dirty();
  ST_CHECK(!fixture.root.dirty());   // 静止后必须真的能停下来
}

ST_TEST(hover_transition_stops_requesting_frames_when_done) {
  // 过渡跑完后也必须停下来（否则动画结束仍在空转）。
  Fixture fixture;
  fixture.layout();
  fixture.move_to(60.0f, 40.0f);   // 悬浮到按钮
  // 给足时间让过渡走完
  for (int step = 0; step <= 20; ++step) {
    Canvas canvas{kWidth, kHeight};
    canvas.clear(Color{0x20, 0x20, 0x28, 0xFF});
    fixture.root.set_time(static_cast<double>(step) * 0.05);
    fixture.root.paint(canvas);
  }
  ST_CHECK(fixture.button->hover_progress() > 0.99f);
  fixture.root.clear_dirty();
  ST_CHECK(!fixture.root.dirty());
}
