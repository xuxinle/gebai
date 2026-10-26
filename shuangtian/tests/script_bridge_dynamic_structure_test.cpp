/// 脚本桥的动态结构路径：`__bridgeBuild` + `__bridgeAppend` + `__bridgeClear`。
///
/// ## 为什么单独测这条路径
///
/// `examples/xiangqi` 实测：JS 侧 `list.clear(); 然后 append 十几张卡片元素`
/// 会让进程 `std::bad_alloc` 崩掉，**没有任何 JS 异常**。
///
/// 走的是桥的这条链路：
///
/// ```
/// JS  append('Column', {...})
///  → __bridgeBuild  （建元素，先挂到 root->content()）
///  → __bridgeAppend （从 root->content() 挪到目标父下）
/// ```
///
/// 也就是说**每个新建元素都要在根内容下短暂停留再搬走**。元素一多，
/// 根容器的子列表被反复增删——这条路径在直接调 `add_child` 的用例里覆盖不到。
///
/// 本用例把它钉住：模拟"清空容器 → 重建 N 个嵌套元素 → 再清空 → 再重建"的循环。

#include "st/test/test.hpp"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/ui_root.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::Json;
using st::test::FixedAdvanceTextPort;

/// 桥接路径的测试台：真实 `ScriptHost` + 真实 `ScriptBridge`。
struct BridgeFixture {
  FixedAdvanceTextPort port{};
  st::ui::UiRoot root{};
  std::unique_ptr<st::ui::ScriptHost> host{};
  std::unique_ptr<st::ui::ScriptBridge> bridge{};
  st::ui::Element* container{nullptr};

  BridgeFixture() {
    root.set_text_port(&port);
    root.set_viewport(st::math::Size{600.0f, 400.0f});

    auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto slot = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    slot->set_id("host");
    container = page->add_child(std::move(slot));
    root.set_content(std::move(page));
    root.layout(true);

    host = std::make_unique<st::ui::ScriptHost>(root);
    bridge = std::make_unique<st::ui::ScriptBridge>(*host, &root);
  }

  auto eval(const std::string& code) -> std::string {
    auto outcome = host->eval(code);
    if (!outcome) return "错误:" + outcome.error().message;
    return outcome->is_string() ? st::json_as_string(*outcome) : st::json_dump(*outcome);
  }
};

}  // namespace

ST_TEST(bridge_build_and_append_does_not_crash) {
  BridgeFixture fx;
  ST_REQUIRE(fx.host != nullptr);
  ST_REQUIRE(fx.host->valid());

  // 建 N 个元素并全挂到 #host 下（等价于 JS 里一串 $('#host').append(...)）
  const auto script = R"JS(
    (function () {
      let built = 0;
      for (let i = 0; i < 24; i++) {
        const row = $('#host').append('Row', { id: 'row-' + i, gap: '8' });
        for (let j = 0; j < 2; j++) {
          $('#row-' + i).append('Text', { id: 'cell-' + i + '-' + j, text: 'x' });
        }
        built++;
      }
      return built;
    })()
  )JS";
  const auto result = fx.eval(script);
  ST_CHECK(result == "24");
  ST_CHECK(fx.container->child_count() == 24);
}

ST_TEST(bridge_clear_then_rebuild_does_not_crash) {
  BridgeFixture fx;
  ST_REQUIRE(fx.host != nullptr);
  ST_REQUIRE(fx.host->valid());

  // 核心复现：清空 → 重建（12 轮 × 12 个元素）。
  // 每一轮都换新 id（真实场景：卡片被处理掉、新卡进来）。
  for (int round = 0; round < 12; ++round) {
    std::string ok = "(function(){ var h = $('#host'); h.clear();";
    for (int i = 0; i < 12; ++i) {
      ok += " h.append('Column', { id: 'r" + std::to_string(round) + "-c" + std::to_string(i) + "' });";
    }
    ok += " return count('#host') > 0; })()";
    const auto outcome = fx.eval(ok);
    ST_CHECK(outcome == "true");
    ST_CHECK(fx.container->child_count() == 12);
  }
}

ST_TEST(bridge_clear_releases_previous_ids) {
  BridgeFixture fx;
  ST_REQUIRE(fx.host != nullptr);
  ST_REQUIRE(fx.host->valid());

  (void)fx.eval("(function(){ var h=$('#host'); h.clear(); h.append('Column',{id:'old-1'}); return true; })()");
  ST_CHECK(fx.root.find("old-1") != nullptr);

  (void)fx.eval("(function(){ $('#host').clear(); return true; })()");
  // 清空后旧 id 必须真的不可寻址
  ST_CHECK(fx.root.find("old-1") == nullptr);
  ST_CHECK(fx.container->child_count() == 0);

  // 同 id 复用必须干净
  (void)fx.eval("(function(){ $('#host').append('Column',{id:'old-1'}); return true; })()");
  auto* back = fx.root.find("old-1");
  ST_REQUIRE(back != nullptr);
  ST_CHECK(dynamic_cast<st::ui::Panel*>(back) != nullptr);
}

ST_TEST(bridge_clear_rebuild_with_bindings_does_not_crash) {
  BridgeFixture fx;
  ST_REQUIRE(fx.host != nullptr);
  ST_REQUIRE(fx.host->valid());

  // 这是 `examples/xiangqi` 的真实形态：清空后重建卡片，**每张卡的按钮都重新 on()**。
  //
  // 与上一条用例的差别就在这个 `on()`：它会往宿主 bindings 表里加一条。
  // 若绑定不随元素销毁而回收，表会随轮次线性增长，最终把进程压死
  // （实测症状：`std::bad_alloc`，无任何 JS 异常）。
  for (int round = 0; round < 12; ++round) {
    std::string ok = "(function(){ var h = $('#host'); h.clear(); globalThis.__hits = 0;";
    for (int i = 0; i < 8; ++i) {
      const std::string card = "r" + std::to_string(round) + "-c" + std::to_string(i);
      ok += " var c = h.append('Column', { id: '" + card + "' });";
      ok += " c.append('Button', { id: '" + card + "-btn', label: 'x' });";
      // 每轮都重新绑（元素是新的，旧绑定若没被回收就是死绑定）
      ok += " on('#" + card + "-btn', 'click', function(){ globalThis.__hits++; });";
    }
    ok += " return count('#host') > 0; })()";
    const auto outcome = fx.eval(ok);
    ST_CHECK(outcome == "true");
    ST_CHECK(fx.container->child_count() == 8);
  }

  // 绑定表不该随轮次线性增长（12 轮 × 8 条 = 96 条是缺陷形态）
  const auto total = fx.host->bindings().size();
  if (total > 16) {
    std::fprintf(stderr, "[诊断] 绑定表 %zu 条（12 轮 × 8 条应为 8 条左右）\n", total);
  }
  ST_CHECK(total <= 16);
}

ST_TEST(bridge_clear_inside_event_handler_does_not_crash) {
  BridgeFixture fx;
  ST_REQUIRE(fx.host != nullptr);
  ST_REQUIRE(fx.host->valid());

  // 这是 `examples/xiangqi` 真正的崩溃形态：**在事件处理器里清空并重建**。
  //
  // ```
  // $('#btn').on('click', () => { host.clear(); 重建一批元素... })
  // ```
  //
  // 与上一条用例的差别：上一条是在脚本顶层（eval 期间）clear，
  // 而这里 clear 发生在**宿主正在派发事件、调用栈里还握着被点元素**的时刻。
  // 若 `clear()` 把该元素释放掉，回调返回后宿主手里的引用就成了悬垂——
  // 表现为退出时 `std::bad_alloc`（分配器元数据被踩）。
  const std::string setup =
      "(function(){ var h = $('#host'); h.clear();"
      " h.append('Column', { id: 'card' });"
      " $('#card').append('Button', { id: 'card-btn', label: 'ok' });"
      " globalThis.__rounds = 0;"
      " on('#card-btn', 'click', function(){"
      "   globalThis.__rounds++;"
      "   var h2 = $('#host');"
      "   h2.clear();"
      "   h2.append('Column', { id: 'card' });"
      "   $('#card').append('Button', { id: 'card-btn', label: 'ok' });"
      "   on('#card-btn', 'click', function(){ globalThis.__rounds++; });"
      " });"
      " return true; })()";
  ST_CHECK(fx.eval(setup) == "true");

  auto* button = fx.root.find("card-btn");
  ST_REQUIRE(button != nullptr);

  // 连点 10 次：每次都在回调里清空并重建
  for (int i = 0; i < 10; ++i) {
    button = fx.root.find("card-btn");
    ST_REQUIRE(button != nullptr);
    st::ui::Event click;
    click.kind = st::ui::EventKind::Click;
    click.position = st::math::Point{button->bounds().x + 2.0f, button->bounds().y + 2.0f};
    click.button = 1;
    click.click_count = 1;
    (void)fx.root.dispatch(click);
  }
  ST_CHECK(fx.root.find("card-btn") != nullptr);
  ST_CHECK(fx.eval("globalThis.__rounds") != "0");
}
