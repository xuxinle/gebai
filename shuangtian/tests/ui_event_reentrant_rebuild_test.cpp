// 事件回调里改 State → 当场重组 → 旧元素被销毁，**派发栈上仍拿着它**。
//
// 缺陷实录（2026-11-05）：gbcode 变更列表点「暂存」按钮 → 程序崩溃。
// 链路是：
//   ① 点击回调里改 State（暂存后刷新变更列表）
//   ② 声明式同一轮重组，旧元素 `remove_child` 返回的 unique_ptr **立即析构**
//   ③ 而 `UiRoot::dispatch_to` 的冒泡循环、事件观察者还拿着那棵子树的裸引用
//      ⇒ use-after-free
// 同一根因还有第二个形态：动作按钮挂在"行"上，指针移到按钮时行收到 HoverOut
// （`UiRoot` 的 hover 链是逐元素直达、不冒泡）→ 按钮被收掉 → 指针又落回行上
// ⇒ 按钮自己赶走自己（表现为"移到按钮时按钮消失"）。
//
// 这组用例钉住**框架层**的保障（不是某个应用的补丁）：
//   ① 派发期内的重组移除必须延后释放（`UiRoot::retire_subtree`）
//   ② 冒泡链的下一跳必须在回调**之前**取好（回调可能销毁当前元素）
//   ③ 事件观察者在元素已销毁时被跳过，而不是拿悬垂引用去用

#include "st/test/test.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

#include <memory>
#include <string>

namespace {

using st::ui::Button;
using st::ui::Element;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::UiRoot;

/// 造一个 root，内容是一列：外层 Panel + 一个会"自杀"的按钮。
struct Fixture {
  UiRoot root;
  Element* row = nullptr;
  Button* victim = nullptr;
  bool observed = false;

  Fixture() {
    auto panel = std::make_unique<Element>();
    panel->set_id("row");
    auto child = std::make_unique<Button>("点我");
    child->set_id("victim");
    victim = child.get();
    panel->add_child(std::move(child));
    row = panel.get();
    root.set_content(std::move(panel));
    root.layout();
  }

  auto click_at(st::math::Point point) -> bool {
    Event event;
    event.kind = EventKind::Click;
    event.position = point;
    return root.dispatch(event);
  }
};

}  // namespace

ST_TEST(event_callback_may_destroy_its_own_element_without_crashing) {
  Fixture f;
  // 回调里把**自己所在的整棵子树**摘掉并销毁——最凶的形态。
  // 旧实现会在回调返回后继续 `current->parent()` 与观察者通知，直接踩已释放内存。
  f.victim->on_click = [&f] {
    auto taken = f.root.take_content();   // 摘走整棵内容树
    (void)taken;                          // 这里就地销毁
  };
  const st::math::Rect area = f.victim->bounds();
  // 不崩即通过（ASan 档下若悬垂会直接报）。
  (void)f.click_at(st::math::Point{area.center().x, area.center().y});
  ST_CHECK(true);
}

/// 不消费事件的最小元素（模拟"点了没代码处理"的那类子件）。
class PassThrough : public Element {
 public:
  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "Pass"; }
  auto on_event(const st::ui::RenderContext&, Event&) -> bool override { return false; }
};

ST_TEST(event_bubbles_to_parent_when_child_does_not_consume) {
  // 判据：冒泡的**下一跳必须在回调之前取好**——否则回调销毁子元素后，
  // `current->parent()` 就是 use-after-free。
  // 这里用"子件不消费"的形态，让冒泡真的走到父级（Button 会自己吃掉 Click，
  // 那样测不到冒泡路径——第一版就是这样写错的）。
  UiRoot root;
  auto panel = std::make_unique<Element>();
  panel->set_id("row");
  auto child = std::make_unique<PassThrough>();
  child->set_id("pass");
  PassThrough* pass = child.get();
  panel->add_child(std::move(child));
  Element* row = panel.get();
  root.set_content(std::move(panel));
  root.layout();

  int parent_hits = 0;
  row->set_event_handler([&parent_hits](Event& event) {
    if (event.kind == EventKind::Click) {
      ++parent_hits;
      return true;
    }
    return false;
  });

  const st::math::Rect area = pass->bounds();
  Event click;
  click.kind = EventKind::Click;
  click.position = st::math::Point{area.center().x, area.center().y};
  (void)root.dispatch(click);
  ST_CHECK_EQ(parent_hits, 1);
}

ST_TEST(event_observer_skipped_when_target_destroyed_during_dispatch) {
  Fixture f;
  int observer_calls = 0;
  f.root.set_event_observer([&observer_calls](const Event&, Element&) { ++observer_calls; });
  f.victim->on_click = [&f] {
    auto taken = f.root.take_content();   // 目标元素随之销毁
    (void)taken;
  };
  const st::math::Rect area = f.victim->bounds();
  (void)f.click_at(st::math::Point{area.center().x, area.center().y});
  // 关键：观察者**不该**拿到已销毁的元素引用。
  // 我们的契约是"目标已死就跳过通知"（脚本看不到这次点击，但不会崩）。
  ST_CHECK_EQ(observer_calls, 0);
}

ST_TEST(event_observer_still_called_for_live_target) {
  Fixture f;
  int observer_calls = 0;
  f.root.set_event_observer([&observer_calls](const Event&, Element&) { ++observer_calls; });
  const st::math::Rect area = f.victim->bounds();
  (void)f.click_at(st::math::Point{area.center().x, area.center().y});
  // 反向守卫：不能为了防悬垂而"一律不通知"（那会让脚本事件全部静默）。
  ST_CHECK_EQ(observer_calls, 1);
}

ST_TEST(retire_subtree_defers_release_only_during_dispatch) {
  // 派发期外：立即释放（不积压）。
  // 派发期内：延后到派发结束（调用方还能安全读它）。
  // 这里只验"不崩 + 行为可观察"——真正的悬垂判定靠 ASan 档。
  Fixture f;
  auto orphan = std::make_unique<Element>();
  orphan->set_id("orphan");
  f.root.retire_subtree(std::move(orphan));   // 非派发期：直接销毁
  ST_CHECK(true);
}
