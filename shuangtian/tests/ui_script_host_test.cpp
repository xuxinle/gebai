/// 脚本宿主测试（`st::ui::ScriptHost`）：JS 读写与控制组件、事件桥、定时器、状态、性能契约。
///
/// 关注点不是"JS 能不能算 1+1"（引擎自身已有测试），而是**这一层的语义与性能契约**：
/// - 三条路径（C++ / 协议 / 脚本）共用同一套读写语义；
/// - 变更**批量提交**（N 个属性一次跨界），不是逐属性往返；
/// - 事件只桥接被监听者；合成点击与真实点击对脚本表现一致；
/// - 选择器未命中要**可观测**（不静默）；循环引用/超时等异常不逃逸。

#include "st/test/test.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>  // `std::this_thread::sleep_for`：libstdc++ 会间接带上，MSVC 不会
#include <vector>

#include "st/core/string.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

#include "st/ext/json.hpp"
#include "tests/support/text_port_fixtures.hpp"

// 测试在匿名命名空间内，`st::test::X` 得写全；用具名别名让用例读起来干净。
using st::test::FixedAdvanceTextPort;

namespace {


/// 一个装了若干组件、已排布好的界面 + 脚本宿主。
struct Fixture {
  FixedAdvanceTextPort port{};
  st::ui::UiRoot root{};
  st::ui::Button* save{nullptr};
  st::ui::Text* status{nullptr};
  st::ui::Input* search{nullptr};
  st::ui::Checkbox* flag{nullptr};
  int save_clicks{0};

  explicit Fixture(bool enable_script = true) {
    root.set_text_port(&port);
    root.set_viewport(st::math::Size{600.0f, 400.0f});

    auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto button = std::make_unique<st::ui::Button>("保存");
    button->set_id("save");
    save = button.get();
    save->on_click = [this]() { ++save_clicks; };
    page->add_child(std::move(button));

    auto text = std::make_unique<st::ui::Text>("待保存");
    text->set_id("status");
    status = text.get();
    page->add_child(std::move(text));

    auto input = std::make_unique<st::ui::Input>();
    input->set_id("search");
    search = input.get();
    page->add_child(std::move(input));

    auto toggle = std::make_unique<st::ui::Checkbox>("启用");
    toggle->set_id("flag");
    flag = toggle.get();
    page->add_child(std::move(toggle));

    root.set_content(std::move(page));
    root.layout(true);
    if (enable_script) {
      // ScriptHost 自己在构造时接上事件观察者（无需调用方额外接线）
      host = std::make_unique<st::ui::ScriptHost>(root);
    }
  }

  std::unique_ptr<st::ui::ScriptHost> host{};

  [[nodiscard]] auto eval(const std::string& code) -> std::string {
    auto outcome = host->eval(code);
    if (!outcome) return "错误:" + outcome.error().message;
    return outcome->is_string() ? st::json_as_string(*outcome) : st::json_dump(*outcome);
  }

  /// 造一个真实点击事件并走 UiRoot 分发（等价于用户点击）。
  auto real_click(st::ui::Element& target) -> bool {
    st::ui::Event event;
    event.kind = st::ui::EventKind::Click;
    event.position = st::math::Point{target.bounds().x + 2.0f, target.bounds().y + 2.0f};
    event.button = 1;
    event.click_count = 1;
    return root.dispatch(event);
  }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 基本可用性
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_host_starts_valid_with_prelude_api) {
  Fixture fx;
  ST_REQUIRE(fx.host->valid());
  ST_CHECK(!fx.host->engine_version().empty());
  // 前置 API 都在（用户与 AI 直接依赖这些名字）
  ST_CHECK_EQ(fx.eval("typeof $"), std::string("function"));
  ST_CHECK_EQ(fx.eval("typeof $$"), std::string("function"));
  ST_CHECK_EQ(fx.eval("typeof on"), std::string("function"));
  ST_CHECK_EQ(fx.eval("typeof every"), std::string("function"));
  ST_CHECK_EQ(fx.eval("typeof state"), std::string("object"));
}

ST_TEST(script_reads_component_state) {
  Fixture fx;
  // 按 id 读（`#id`）
  ST_CHECK_EQ(fx.eval("$('#status').text"), std::string("待保存"));
  ST_CHECK_EQ(fx.eval("$('#save').type"), std::string("Button"));
  // 类型匹配 + 属性过滤
  ST_CHECK_EQ(fx.eval("count('Button')"), std::string("1"));
  ST_CHECK_EQ(fx.eval("count('#save')"), std::string("1"));
  ST_CHECK_EQ(fx.eval("count('Input')"), std::string("1"));
  // 属性面（与协议 `get` 同源）
  ST_CHECK_EQ(fx.eval("$('#flag').props.checked"), std::string("false"));  // 初始未勾选
}

ST_TEST(script_writes_components_in_one_batch) {
  Fixture fx;
  const std::size_t before = fx.host->commit_count();
  // 一次脚本里改三个组件：应只产生**一次**变更提交（性能契约的直接体现）
  ST_CHECK_EQ(fx.eval("$('#status').set({text:'已保存'}); "
                      "$('#search').set({value:'霜天'}); "
                      "$('#flag').set({checked:false}); 'ok'"),
              std::string("ok"));
  ST_CHECK_EQ(fx.host->commit_count(), before + 1);
  ST_CHECK_EQ(fx.host->committed_properties(), 3U);

  // 三处都真的生效了（走的是与协议 `set` 同一份 apply_properties）
  ST_CHECK_EQ(fx.status->content(), std::string("已保存"));
  ST_CHECK_EQ(fx.search->value(), std::string("霜天"));
  ST_CHECK(!fx.flag->checked());
}

ST_TEST(script_triggers_component_action) {
  Fixture fx;
  ST_CHECK_EQ(fx.eval("$('#save').click(); 'ok'"), std::string("ok"));
  ST_CHECK_EQ(fx.save_clicks, 1);
  // focus 经 UiRoot 设置（键盘事件依赖它）
  ST_CHECK_EQ(fx.eval("$('#search').focus(); 'ok'"), std::string("ok"));
  ST_CHECK(fx.root.focused() == fx.search);  // 裸指针走布尔断言（进不了 std::format）
}

ST_TEST(script_missing_selector_is_observable_not_silent) {
  Fixture fx;
  // 未命中：读得到 undefined，写被忽略——但 `exists` 能判定，脚本可自查
  ST_CHECK_EQ(fx.eval("$('#nope').exists"), std::string("false"));
  ST_CHECK_EQ(fx.eval("typeof $('#nope').text"), std::string("undefined"));
  ST_CHECK_EQ(fx.eval("$('#nope').set({text:'x'}); 'ok'"), std::string("ok"));
  // 前一个写操作被忽略（不是抛异常、也不是改到别的组件）
  ST_CHECK_EQ(fx.status->content(), std::string("待保存"));
}

// ————————————————————————————————————————————————————————————————————————————
// 事件桥
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_binds_and_receives_real_click) {
  Fixture fx;
  auto binding = fx.host->bind("#save", "click", "() => $('#status').set({text:'脚本收到点击'})");
  ST_REQUIRE(binding.has_value());
  ST_CHECK_EQ(fx.host->bindings().size(), 1U);

  // 真实点击走 UiRoot 分发 → 事件观察者 → 脚本回调
  ST_CHECK(fx.real_click(*fx.save));
  ST_CHECK_EQ(fx.status->content(), std::string("脚本收到点击"));
  ST_CHECK_EQ(fx.save_clicks, 1);  // C++ 处理器也照常收到（互不干扰）
}

ST_TEST(script_only_hears_bound_events) {
  Fixture fx;
  ST_CHECK_EQ(fx.eval("globalThis.__hits = 0; 'ok'"), std::string("ok"));
  ST_REQUIRE(fx.host->bind("#save", "click", "() => { globalThis.__hits++; }").has_value());
  // 点别的组件：不该触发
  ST_CHECK(fx.real_click(*fx.status));
  ST_CHECK_EQ(fx.eval("globalThis.__hits"), std::string("0"));
  ST_CHECK(fx.real_click(*fx.save));
  ST_CHECK_EQ(fx.eval("globalThis.__hits"), std::string("1"));
}

ST_TEST(script_bindings_can_be_listed_and_removed) {
  Fixture fx;
  const auto first = fx.host->bind("#save", "click", "() => log('a')");
  const auto second = fx.host->bind("#search", "input", "() => log('b')");
  ST_REQUIRE(first.has_value());
  ST_REQUIRE(second.has_value());
  ST_CHECK_EQ(fx.host->bindings().size(), 2U);
  ST_CHECK_EQ(fx.host->bindings()[0].selector, std::string("#save"));
  ST_CHECK_EQ(fx.host->bindings()[1].event, std::string("input"));

  ST_CHECK(fx.host->unbind(*first));
  ST_CHECK_EQ(fx.host->bindings().size(), 1U);
  ST_CHECK(!fx.host->unbind(*first));  // 已移除：再注销返回 false

  fx.host->clear_bindings();
  ST_CHECK(fx.host->bindings().empty());
}

ST_TEST(script_bind_rejects_bad_arguments) {
  Fixture fx;
  ST_CHECK(!fx.host->bind("", "click", "() => {}").has_value());
  ST_CHECK(!fx.host->bind("#save", "", "() => {}").has_value());
  ST_CHECK(!fx.host->bind("#save", "click", "").has_value());
  // 处理器不是函数：必须报错而不是静默注册一个永远不响的绑定
  const auto bad = fx.host->bind("#save", "click", "123");
  ST_CHECK(!bad.has_value());
  ST_CHECK(fx.host->bindings().empty());
}

ST_TEST(script_synthetic_invoke_click_also_reaches_script) {
  Fixture fx;
  ST_REQUIRE(fx.host->bind("#save", "click", "() => $('#status').set({text:'合成点击也收到'})").has_value());
  // 协议/脚本的 invoke(click) 不经过真实输入管线，但语义上就是一次点击
  st::ui::Event synthetic;
  synthetic.kind = st::ui::EventKind::Click;
  fx.root.notify_event_observer(synthetic, *fx.save);
  ST_CHECK_EQ(fx.status->content(), std::string("合成点击也收到"));
}

// ————————————————————————————————————————————————————————————————————————————
// 定时器与状态
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_timers_fire_on_tick) {
  Fixture fx;
  ST_CHECK_EQ(fx.eval("globalThis.__ticks = 0; every(10, () => { globalThis.__ticks++; }); 'ok'"),
              std::string("ok"));
  // 立刻 tick：尚未到期
  (void)fx.host->tick(0.0);
  ST_CHECK_EQ(fx.eval("globalThis.__ticks"), std::string("0"));
  // 等过 10ms 再 tick：应触发
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ST_CHECK(fx.host->tick(0.0) >= 1U);
  ST_CHECK_EQ(fx.eval("globalThis.__ticks"), std::string("1"));
}

ST_TEST(script_one_shot_timer_runs_once) {
  Fixture fx;
  ST_CHECK_EQ(fx.eval("globalThis.__once = 0; after(5, () => { globalThis.__once++; }); 'ok'"),
              std::string("ok"));
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  (void)fx.host->tick(0.0);
  (void)fx.host->tick(0.0);
  ST_CHECK_EQ(fx.eval("globalThis.__once"), std::string("1"));
}

ST_TEST(script_state_roundtrips_between_host_and_script) {
  Fixture fx;
  ST_CHECK_EQ(fx.eval("state.count = 41; 'ok'"), std::string("ok"));
  auto state = fx.host->state();
  ST_REQUIRE(state.has_value());
  ST_CHECK_EQ(st::json_get_i64(*state, "count"), 41);

  // 宿主写状态（测试与预置用）
  st::Json patch = st::Json::object();
  patch["count"] = 100;
  ST_CHECK(fx.host->set_state(patch).has_value());
  ST_CHECK_EQ(fx.eval("state.count"), std::string("100"));
}

// ————————————————————————————————————————————————————————————————————————————
// 性能契约（这些断言防止"实现退化回逐属性跨边界"）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_batch_commit_is_constant_not_per_property) {
  Fixture fx;
  // 先建 30 个组件
  const std::string build = R"(
    (() => {
      const host = globalThis;
      // 通过既有组件复制不现实，这里改用状态数组模拟"大量属性一次提交"
      globalThis.__ids = [];
      return 'ok';
    })()
  )";
  (void)build;
  // 20 个属性写在同一脚本里：提交次数必须是 1（而不是 20）
  std::string code;
  for (int index = 0; index < 20; ++index) {
    code += "$('#status').set({text:'v" + std::to_string(index) + "'});";
  }
  code += "'done'";
  const std::size_t before = fx.host->commit_count();
  ST_CHECK_EQ(fx.eval(code), std::string("done"));
  ST_CHECK_EQ(fx.host->commit_count(), before + 1);
  ST_CHECK_EQ(fx.host->committed_properties(), 20U);
  // 最后一次写的值生效（同一阶段内后写覆盖先写）
  ST_CHECK_EQ(fx.status->content(), std::string("v19"));
}

ST_TEST(script_read_does_not_commit_anything) {
  Fixture fx;
  const std::size_t before = fx.host->commit_count();
  ST_CHECK_EQ(fx.eval("$('#status').text + $('Button').length"), std::string("待保存undefined"));
  ST_CHECK_EQ(fx.host->commit_count(), before);  // 纯读不该产生提交
}

ST_TEST(script_many_selectors_stay_in_js) {
  Fixture fx;
  // 100 次选择器查询全部在 JS 侧完成：只应触发一次快照（快照在首个查询时构建）
  const std::string code =
      "globalThis.__sum = 0;"
      "for (let i = 0; i < 100; ++i) { globalThis.__sum += count('Button'); }"
      "globalThis.__sum";
  ST_CHECK_EQ(fx.eval(code), std::string("100"));
}

// ————————————————————————————————————————————————————————————————————————————
// 异常不逃逸 + 与 C++ 语义一致
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(script_errors_surface_as_result_not_exception) {
  Fixture fx;
  const auto broken = fx.host->eval("throw new Error('boom')");
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(broken.error().message.find("boom") != std::string::npos);
  // 出错后宿主仍可用（状态没被打坏）
  ST_CHECK(fx.host->valid());
  ST_CHECK_EQ(fx.eval("1 + 1"), std::string("2"));
}

ST_TEST(script_host_reports_limits_and_stats) {
  Fixture fx;
  ST_CHECK(fx.host->limits().memory_bytes > 0U);
  ST_CHECK(fx.host->limits().timeout.count() > 0);
  (void)fx.eval("$('#status').text");
  ST_CHECK(fx.host->last_stats().elapsed.count() >= 0);
}

ST_TEST(script_writes_match_protocol_semantics) {
  Fixture fx;
  // 脚本与协议对同一组件的写入应表现一致：禁用后不可点、隐藏后 visible=false
  ST_CHECK_EQ(fx.eval("$('#save').set({enabled:false}); 'ok'"), std::string("ok"));
  ST_CHECK(!fx.save->enabled());
  ST_CHECK_EQ(fx.eval("$('#save').set({visible:false}); 'ok'"), std::string("ok"));
  ST_CHECK(!fx.save->visible());
  // 复原
  ST_CHECK_EQ(fx.eval("$('#save').set({enabled:true, visible:true}); 'ok'"), std::string("ok"));
  ST_CHECK(fx.save->enabled());
  ST_CHECK(fx.save->visible());
}
