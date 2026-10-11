/// `st::ui::MergeView` 三方合并视图测试。
///
/// 界面类组件最容易写出"单测全绿但用户看不见"的假护栏，所以本文件的用例分两类：
/// **行为类**（选择状态、合成结果、导航）与**真的画出来了类**（在 `Canvas` 上取像素）。
/// 后者是必要的：`set_take_theirs` 返回 true 不等于按钮画对了。

#include <cmath>
#include <memory>
#include <string>

#include "st/app/text_port.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::Point;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::MergeView;
using st::ui::RenderContext;
using st::ui::Theme;

/// 视口**不在原点**：坐标 bug 在原点会被 0 掩盖（这类组件最常见的假绿来源）。
constexpr float kOriginX = 31.0f;
constexpr float kOriginY = 19.0f;
constexpr float kWidth = 600.0f;
constexpr float kHeight = 400.0f;

struct Fixture {
  Theme theme{Theme::light()};
  std::shared_ptr<st::text::FontStack> stack{};
  std::unique_ptr<st::text::TextRenderer> renderer{};
  std::unique_ptr<st::ui::TextPort> port{};
  MergeView view{};
  RenderContext context{theme, nullptr, 0.0};

  Fixture() {
    auto loaded = st::text::FontStack::system_default();
    if (loaded.has_value()) {
      stack = std::make_shared<st::text::FontStack>(std::move(*loaded));
      renderer = std::make_unique<st::text::TextRenderer>(*stack);
      port = st::app::make_text_port(*renderer);
      context.text = port.get();
    }
    view.set_id("merge");
    view.apply_theme(theme);
  }

  void layout() {
    view.measure(context, st::ui::Constraints{kWidth, kHeight});
    view.arrange(context, st::math::Rect{kOriginX, kOriginY, kWidth, kHeight});
  }

  /// 造一个含**一个真冲突**的三方场景。
  void load_conflicting() {
    view.set_sides("a\ntarget\nc\n", "a\nOURS\nc\n", "a\nTHEIRS\nc\n");
    layout();
  }

  [[nodiscard]] auto render() const {
    st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(700, 500, 1.0f);
    canvas.clear(Color::rgb(255, 255, 255));
    view.paint(context, canvas);
    return canvas;
  }
};

[[nodiscard]] auto near(float a, float b, float tolerance = 0.01f) -> bool {
  return std::fabs(a - b) <= tolerance;
}

}  // namespace

ST_TEST(merge_view_splits_segments_and_counts_conflicts) {
  Fixture fx;
  fx.load_conflicting();
  // 三方合并的划分来自 `st::text`（已在那边单测过），这里只断言"接上了"。
  ST_CHECK_EQ(fx.view.conflict_count(), std::size_t{1});
  ST_CHECK_EQ(fx.view.resolved_count(), fx.view.plan().segments.size() - 1);
  // 四个非冲突段 + 一个冲突段（未变/我方改 等）。
  ST_CHECK(fx.view.plan().segments.size() >= 3U);
  // 一装载就停在第一个冲突段上（打开合并视图的人下一步就是处理它）。
  const auto& current = fx.view.plan().segments[fx.view.current_segment()];
  ST_CHECK(current.kind == st::text::MergeKind::Conflict);
}

ST_TEST(merge_view_take_sides_changes_result) {
  Fixture fx;
  fx.load_conflicting();
  // 缺省：冲突段取我方（`merge_apply` 的缺省语义），且有**未决定**的冲突。
  ST_CHECK(fx.view.has_pending_conflicts());
  ST_CHECK_EQ(fx.view.result(), std::string("a\nOURS\nc\n"));

  const std::size_t conflict = fx.view.current_segment();
  ST_CHECK(fx.view.set_take_theirs(conflict, true));
  ST_CHECK(!fx.view.has_pending_conflicts());
  ST_CHECK_EQ(fx.view.result(), std::string("a\nTHEIRS\nc\n"));

  // 切回我方。
  ST_CHECK(fx.view.set_take_theirs(conflict, false));
  ST_CHECK_EQ(fx.view.result(), std::string("a\nOURS\nc\n"));

  // **非冲突段如实拒绝**：允许改会让界面出现一个"按了不生效"的按钮。
  std::size_t non_conflict = fx.view.plan().segments.size();
  for (std::size_t index = 0; index < fx.view.plan().segments.size(); ++index) {
    if (fx.view.plan().segments[index].kind != st::text::MergeKind::Conflict) {
      non_conflict = index;
      break;
    }
  }
  ST_REQUIRE(non_conflict < fx.view.plan().segments.size());
  ST_CHECK(!fx.view.set_take_theirs(non_conflict, true));
  // 越界也拒绝。
  ST_CHECK(!fx.view.set_take_theirs(999, true));
}

ST_TEST(merge_view_choices_feed_merge_apply) {
  Fixture fx;
  fx.load_conflicting();
  const std::size_t conflict = fx.view.current_segment();
  ST_CHECK(fx.view.set_take_theirs(conflict, true));
  // `choices()` 必须能**直接**喂回 `merge_apply` 且结果与 `result()` 一致
  // ——非冲突段填的是"实际会取的一侧"，所以不需要再过滤一遍。
  const auto choices = fx.view.choices();
  ST_CHECK_EQ(choices.size(), fx.view.plan().segments.size());
  ST_CHECK_EQ(st::text::merge_apply(fx.view.plan(), choices), fx.view.result());

  // "全部取我方/他方"两个批量动作。
  fx.view.set_all_conflicts(false);
  ST_CHECK_EQ(fx.view.result(), std::string("a\nOURS\nc\n"));
  ST_CHECK(!fx.view.has_pending_conflicts());
  fx.view.set_all_conflicts(true);
  ST_CHECK_EQ(fx.view.result(), std::string("a\nTHEIRS\nc\n"));
}

ST_TEST(merge_view_conflict_navigation_skips_decided) {
  // 两处冲突：`next_conflict` 必须跳到**未决定**的那一处（跳到已决定的重看一遍是白费）。
  Fixture fx;
  fx.view.set_sides("one\ntwo\nthree\nfour\nfive\n",
                    "one\nO2\nthree\nO4\nfive\n",
                    "one\nT2\nthree\nT4\nfive\n");
  fx.layout();
  ST_CHECK_EQ(fx.view.conflict_count(), std::size_t{2});

  const std::size_t first = fx.view.current_segment();
  ST_CHECK(fx.view.plan().segments[first].kind == st::text::MergeKind::Conflict);
  ST_CHECK(fx.view.set_take_theirs(first, false));   // 决定第一处

  const auto second = fx.view.next_pending_conflict();
  ST_REQUIRE(second.has_value());
  ST_CHECK(*second != first);   // 第一处已决定，跳到的是第二处
  ST_CHECK(fx.view.plan().segments[*second].kind == st::text::MergeKind::Conflict);

  // 只剩这一处未决定：两个方向都应返回**它自己**（"跳转"最不容易误解的语义）。
  // ⚠ 两个方向必须对称，否则会出现"F7 跳过眼前那处、Shift+F7 又回到它"的循环。
  const auto forward = fx.view.next_pending_conflict();
  ST_REQUIRE(forward.has_value());
  ST_CHECK_EQ(*forward, *second);
  const auto back = fx.view.previous_pending_conflict();
  ST_REQUIRE(back.has_value());
  ST_CHECK_EQ(*back, *second);

  // 决定它之后：没有待决定的了。
  ST_CHECK(fx.view.set_take_theirs(*second, true));
  ST_CHECK(!fx.view.has_pending_conflicts());
  ST_CHECK(!fx.view.next_pending_conflict().has_value());
  ST_CHECK(!fx.view.previous_pending_conflict().has_value());
}

ST_TEST(merge_view_geometry_and_hit_testing) {
  Fixture fx;
  fx.load_conflicting();
  // 三栏等宽（含 base 时 3 栏）。
  ST_CHECK_EQ(fx.view.column_count(), 3);
  const auto c0 = fx.view.column_rect(0);
  const auto c1 = fx.view.column_rect(1);
  const auto c2 = fx.view.column_rect(2);
  ST_CHECK(near(c0.width, c1.width));
  ST_CHECK(near(c1.width, c2.width));
  ST_CHECK(c0.right() <= c1.x + 0.01f);
  ST_CHECK(c1.right() <= c2.x + 0.01f);
  ST_CHECK(near(c2.right(), kOriginX + kWidth));

  // 隐藏 base 后只剩两栏，且**不保留空位**（留空栏比"两栏各宽一点"更让人困惑）。
  fx.view.set_show_base(false);
  fx.layout();
  ST_CHECK_EQ(fx.view.column_count(), 2);
  ST_CHECK(fx.view.column_rect(1).width > c1.width);
  fx.view.set_show_base(true);
  fx.layout();

  // 只有冲突段有按钮；非冲突段没有（`take_ours_rect` 返回空）。
  const std::size_t conflict = fx.view.current_segment();
  ST_CHECK(!fx.view.take_ours_rect(conflict).is_empty());
  ST_CHECK(!fx.view.take_theirs_rect(conflict).is_empty());
  for (std::size_t index = 0; index < fx.view.plan().segments.size(); ++index) {
    if (fx.view.plan().segments[index].kind != st::text::MergeKind::Conflict) {
      ST_CHECK(fx.view.take_ours_rect(index).is_empty());
      ST_CHECK(fx.view.take_theirs_rect(index).is_empty());
    }
  }

  // 点"取他方"按钮：选择真的变了（这是命中与几何**同一份**的来源带来的好处）。
  const auto theirs_button = fx.view.take_theirs_rect(conflict).center();
  Event down;
  down.kind = EventKind::MouseDown;
  down.button = 1;
  down.position = theirs_button;
  ST_CHECK(fx.view.on_event(fx.context, down));
  ST_CHECK_EQ(fx.view.result(), std::string("a\nTHEIRS\nc\n"));
  // 点"取我方"切回来。
  down.position = fx.view.take_ours_rect(conflict).center();
  ST_CHECK(fx.view.on_event(fx.context, down));
  ST_CHECK_EQ(fx.view.result(), std::string("a\nOURS\nc\n"));
}

ST_TEST(merge_view_paints_three_columns_and_buttons) {
  // **真的画出来了**：只断言 setter 存下来是假绿。
  Fixture fx;
  fx.load_conflicting();
  const auto canvas = fx.render();

  // 三栏各自有底：取同一 y、三栏内的点，应当能取到**至少两种**不同颜色
  //（我方面 vs 别处；若三栏同色，说明栏底色没画或画成一样了）。
  const float probe_y = static_cast<int>(kOriginY + 20.0f);
  const Color left = canvas.pixel_at(static_cast<int>(kOriginX + 20.0f),
                                     static_cast<int>(probe_y));
  const Color middle = canvas.pixel_at(static_cast<int>(kOriginX + kWidth * 0.5f),
                                      static_cast<int>(probe_y));
  ST_CHECK(left != middle);

  // 冲突段有警示色底：段带内的颜色与"完全空白处"（最底部，无段）不同。
  const std::size_t conflict = fx.view.current_segment();
  const auto band = fx.view.segment_rect(conflict);
  const Color in_band = canvas.pixel_at(static_cast<int>(band.x + 4.0f),
                                        static_cast<int>(band.y + band.height - 2.0f));
  const Color empty = canvas.pixel_at(static_cast<int>(kOriginX + 4.0f),
                                      static_cast<int>(kOriginY + kHeight - 4.0f));
  ST_CHECK(in_band != empty);

  // 当前段的左缘指示条：x 在 band.x..band.x+2 内应当与 band 中部不同。
  const Color indicator = canvas.pixel_at(static_cast<int>(band.x + 1.0f),
                                          static_cast<int>(band.y + 6.0f));
  const Color band_middle = canvas.pixel_at(static_cast<int>(band.x + kWidth * 0.35f),
                                            static_cast<int>(band.y + 6.0f));
  ST_CHECK(indicator != band_middle);

  // 按钮真的画了：按钮区内的颜色与同段、同栏、按钮之外的地方不同。
  const auto ours_button = fx.view.take_ours_rect(conflict);
  const Color in_button = canvas.pixel_at(static_cast<int>(ours_button.x + 2.0f),
                                          static_cast<int>(ours_button.y + 2.0f));
  const Color outside_button = canvas.pixel_at(static_cast<int>(ours_button.x + 2.0f),
                                               static_cast<int>(band.y + 2.0f));
  ST_CHECK(in_button != outside_button);
}

ST_TEST(merge_view_properties_and_actions_for_control_channel) {
  Fixture fx;
  fx.load_conflicting();
  // 属性面：控制通道（e2e 脚本）靠它读冲突数与当前段。
  const auto conflicts = fx.view.get_property("conflicts");
  ST_REQUIRE(conflicts.has_value());
  ST_CHECK_EQ(*conflicts, std::string("1"));
  const auto pending = fx.view.get_property("pending");
  ST_REQUIRE(pending.has_value());
  ST_CHECK_EQ(*pending, std::string("1"));
  ST_CHECK(fx.view.set_property("show_base", "false"));
  ST_CHECK(!fx.view.show_base());
  ST_CHECK(!fx.view.set_property("show_base", "yes"));   // 非法值如实拒绝
  fx.view.set_show_base(true);

  // 动作面：`take_*` 无参数时作用于**当前段**（配合 F7 键盘流）。
  ST_CHECK(fx.view.invoke_action("take_theirs", {}));
  ST_CHECK_EQ(fx.view.result(), std::string("a\nTHEIRS\nc\n"));
  ST_CHECK(fx.view.invoke_action("take_ours", {}));
  ST_CHECK_EQ(fx.view.result(), std::string("a\nOURS\nc\n"));
  ST_CHECK(fx.view.invoke_action("take_theirs_all", {}));
  ST_CHECK(!fx.view.has_pending_conflicts());
  ST_CHECK(fx.view.invoke_action("take_ours_all", {}));
  ST_CHECK(!fx.view.invoke_action("不存在的动作", {}));

  // 语义文本（自动化断言用）。
  const std::string semantics = fx.view.semantics_text();
  ST_CHECK(semantics.find("冲突") != std::string::npos);
}

ST_TEST(merge_view_handles_no_conflict_and_empty) {
  // 无冲突（两侧改不同位置）：仍然可用，且 `pending` 为 0。
  Fixture fx;
  fx.view.set_sides("one\ntwo\nthree\n", "ONE\ntwo\nthree\n", "one\ntwo\nTHREE\n");
  fx.layout();
  ST_CHECK_EQ(fx.view.conflict_count(), std::size_t{0});
  ST_CHECK(!fx.view.has_pending_conflicts());
  ST_CHECK_EQ(fx.view.result(), std::string("ONE\ntwo\nTHREE\n"));

  // 空输入：不崩，且如实说明"没有内容"。
  fx.view.clear();
  fx.layout();
  ST_CHECK(fx.view.plan().segments.empty());
  ST_CHECK_EQ(fx.view.result(), std::string(""));
  (void)fx.render();   // 画空态不能崩
}
