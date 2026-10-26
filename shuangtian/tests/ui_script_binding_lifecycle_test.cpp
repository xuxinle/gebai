/// 事件绑定的**生命周期与规模**回归：选择器缓存 + 死绑定回收。
///
/// ## 为什么需要这份用例
///
/// `examples/xiangqi` 实测踩到：交互面板反复"清空 + 重建"后，进程直接
/// `std::bad_alloc` 崩掉，**没有任何 JS 异常**，日志停在业务日志上。
///
/// 根因在框架这一层，两处叠加：
/// 1. `dispatch_event` 每次事件都**逐个 parse 选择器**（绑定表越大越慢）；
/// 2. 元素被销毁后其绑定**永不回收**（重建式界面每轮净增一批）。
///
/// 二者一起让"每次点击"从 O(1) 变成 O(绑定总数 × 选择器解析)，
/// 最终把进程压死。本用例把这两条钉住。
///
/// ## 断言口径
///
/// 1. 选择器**只在绑定时解析一次**（重复 dispatch 不重复解析）
/// 2. 指向已销毁元素的绑定**能被回收**（`prune_dead_bindings`）
/// 3. 非 `#id` 选择器**不被误删**（重建后可能又有同类元素）
/// 4. 反复重建后绑定总数**不单调增长**（这是崩溃的直接前兆）

#include "st/test/test.hpp"

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

#include "st/core/json.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::test::FixedAdvanceTextPort;

/// 造一个落在目标元素上的真实点击事件（等价于用户点击）。
auto make_click(st::ui::Element& target) -> st::ui::Event {
  st::ui::Event event;
  event.kind = st::ui::EventKind::Click;
  event.position = st::math::Point{target.bounds().x + 2.0f, target.bounds().y + 2.0f};
  event.button = 1;
  event.click_count = 1;
  return event;
}

/// 一个可"整片重建"的容器：`root → content → Column(id=host)`。
struct Rebuildable {
  FixedAdvanceTextPort port{};
  st::ui::UiRoot root{};
  st::ui::Element* host{nullptr};
  std::unique_ptr<st::ui::ScriptHost> host_api{};

  explicit Rebuildable(bool enable_script = true) {
    root.set_text_port(&port);
    root.set_viewport(st::math::Size{600.0f, 400.0f});
    auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto column = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    column->set_id("host");
    host = page->add_child(std::move(column));
    root.set_content(std::move(page));
    root.layout(true);
    if (enable_script) host_api = std::make_unique<st::ui::ScriptHost>(root);
  }

  /// 清空 host 下全部子元素（模拟 `$('#host').clear()`）。
  void clear_host() {
    while (host->child_count() > 0) {
      (void)host->remove_child(host->child_at(0));
    }
  }

  /// 在 host 下建 n 个带 id 的按钮，返回其指针。
  auto build_buttons(const std::string& prefix, int count) -> std::vector<st::ui::Button*> {
    std::vector<st::ui::Button*> out;
    for (int i = 0; i < count; ++i) {
      auto button = std::make_unique<st::ui::Button>("b" + std::to_string(i));
      button->set_id(prefix + "-" + std::to_string(i));
      auto* raw = dynamic_cast<st::ui::Button*>(host->add_child(std::move(button)));
      out.push_back(raw);
    }
    return out;
  }

  auto eval(const std::string& code) -> std::string {
    auto outcome = host_api->eval(code);
    if (!outcome) return "错误:" + outcome.error().message;
    return outcome->is_string() ? st::json_as_string(*outcome) : st::json_dump(*outcome);
  }
};

}  // namespace

ST_TEST(script_binding_selector_is_parsed_once) {
  Rebuildable fx;
  ST_REQUIRE(fx.host_api != nullptr);
  auto buttons = fx.build_buttons("btn", 3);
  ST_REQUIRE(buttons.size() == 3);

  // 绑 3 条 id 选择器
  ST_CHECK(fx.eval("on('#btn-0', 'click', () => log('a')); 'ok'") == "ok");
  ST_CHECK(fx.eval("on('#btn-1', 'click', () => log('b')); 'ok'") == "ok");
  ST_CHECK(fx.eval("on('#btn-2', 'click', () => log('c')); 'ok'") == "ok");
  ST_CHECK(fx.host_api->bindings().size() == 3);

  // 每条绑定的选择器都应有缓存（解析成功）
  for (const auto& binding : fx.host_api->bindings()) {
    ST_CHECK(binding.parsed != nullptr);
    ST_CHECK(!binding.parsed->primary_id().empty());
  }

  // 重复 dispatch 不应改变绑定表（缓存命中，无副作用）
  const auto before = fx.host_api->bindings().size();
  for (int i = 0; i < 50; ++i) {
    auto click = make_click(*buttons[0]);
    (void)fx.root.dispatch(click);
  }
  ST_CHECK(fx.host_api->bindings().size() == before);
}

ST_TEST(script_binding_of_destroyed_element_is_pruned) {
  Rebuildable fx;
  ST_REQUIRE(fx.host_api != nullptr);
  (void)fx.build_buttons("old", 5);

  for (int i = 0; i < 5; ++i) {
    ST_CHECK(fx.eval("on('#old-" + std::to_string(i) + "', 'click', () => 0); '1'").empty() == false);
  }
  ST_CHECK(fx.host_api->bindings().size() == 5);

  // 销毁这些元素（绑定指向它们 → 全部成为死绑定）
  fx.clear_host();
  ST_CHECK(fx.root.find("old-0") == nullptr);

  const auto pruned = fx.host_api->prune_dead_bindings();
  ST_CHECK(pruned == 5);
  ST_CHECK(fx.host_api->bindings().empty());
}

ST_TEST(script_binding_non_id_selector_survives_prune) {
  Rebuildable fx;
  ST_REQUIRE(fx.host_api != nullptr);
  (void)fx.build_buttons("btn", 2);

  // 类型选择器（Button）：即使当前没有命中，重建后可能又有按钮出现 → 不能回收
  ST_CHECK(fx.eval("on('Button', 'click', () => 0); 'ok'") == "ok");
  ST_CHECK(fx.eval("on('#btn-0', 'click', () => 0); 'ok'") == "ok");
  ST_CHECK(fx.host_api->bindings().size() == 2);

  // 清空后：类型选择器要保留，id 选择器要回收
  fx.clear_host();
  const auto pruned = fx.host_api->prune_dead_bindings();
  ST_CHECK(pruned == 1);
  ST_CHECK(fx.host_api->bindings().size() == 1);
  const auto remaining = fx.host_api->bindings();   // 拷贝：bindings() 返回临时
  ST_REQUIRE(remaining.size() == 1);
  ST_CHECK(remaining.front().selector == "Button");
  // 非 id 选择器的 primary_id 为空（这正是"不能被当作失效"的判据）
  ST_CHECK(remaining.front().parsed != nullptr);
  ST_CHECK(remaining.front().parsed->primary_id().empty());
}

ST_TEST(script_binding_does_not_grow_across_rebuilds) {
  Rebuildable fx;
  ST_REQUIRE(fx.host_api != nullptr);

  // 模拟重建式界面：每轮清空 + 新建同 id 元素 + 重新绑事件。
  // 若不回收死绑定，绑定表会随轮次单调增长——这正是 bad_alloc 的直接前兆。
  for (int round = 0; round < 20; ++round) {
    fx.clear_host();
    auto buttons = fx.build_buttons("r" + std::to_string(round), 4);
    for (int i = 0; i < 4; ++i) {
      (void)fx.eval("on('#r" + std::to_string(round) + "-" + std::to_string(i) +
                    "', 'click', () => 0); '1'");
    }
    // 触发一次"未命中"的 dispatch → 宿主自动清扫死绑定
    // （用上一轮已销毁的元素地址不可取，改用 host 容器本身作为无关目标）
    if (round > 0) {
      auto stale = fx.build_buttons("tmp" + std::to_string(round), 1);
      auto click = make_click(*stale[0]);
      (void)fx.root.dispatch(click);
      // 这个临时元素随后被清掉（下一轮 clear_host 会带走它）
    }
  }

  // 关键断言：绑定表不该积累到"每轮 4 条 × 20 轮 = 80 条"。
  // 允许有少量残留（最近一轮的 + 未被清扫的），但绝不能是线性累积。
  const auto all = fx.host_api->bindings();
  // 诊断：统计其中"指向已销毁元素"的条数（这些才是真正的泄漏）
  std::size_t dead = 0;
  for (const auto& b : all) {
    if (b.parsed == nullptr) { ++dead; continue; }
    const auto id = b.parsed->primary_id();
    if (!id.empty() && fx.root.find(id) == nullptr) ++dead;
  }
  // 先断言"没有死绑定"（这是真正的缺陷判据）；
  // 再断言总量（间接指标，能抓到"死绑定没被识别"的情况）。
  if (dead != 0 || all.size() > 12) {
    std::fprintf(stderr, "[诊断] 死绑定 %zu 条 / 总数 %zu 条\n", dead, all.size());
  }
  ST_CHECK(dead == 0);
  ST_CHECK(all.size() <= 12);
}

ST_TEST(script_dead_binding_dispatch_is_fast) {
  Rebuildable fx;
  ST_REQUIRE(fx.host_api != nullptr);

  // 制造大量死绑定（模拟长期运行的常驻面板）
  for (int round = 0; round < 30; ++round) {
    fx.clear_host();
    (void)fx.build_buttons("g" + std::to_string(round), 8);
    for (int i = 0; i < 8; ++i) {
      (void)fx.eval("on('#g" + std::to_string(round) + "-" + std::to_string(i) +
                    "', 'click', () => 0); '1'");
    }
  }

  // 清空后点一下 host 下的临时元素：应快速返回（不是扫 240 条 × 逐个 parse）
  fx.clear_host();
  auto survivor = fx.build_buttons("survivor", 1);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) {
    auto click = make_click(*survivor[0]);
    (void)fx.root.dispatch(click);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  // 200 次点击在 1 秒内完成（阈值宽松：CI 慢机也不该红；真回归会是数量级差异）
  ST_CHECK(ms < 1000);
  // 清扫生效后绑定表应已收敛
  ST_CHECK(fx.host_api->bindings().size() <= 8);
}
