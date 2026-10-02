// 声明式 UI 的 JS 宿主测试（DeclarativeHost + declarative.js）。
//
// 验收（docs/declarative.md §9）：
// - compose/useState 全链路：声明 → 挂载 → 真值树结构正确；
// - 状态写驱动重组：JS 事件回调写 state → tick 重组 → 协议 get 读回新文本；
// - 双宿主一致性：与 C++ dsl 的 counter 场景同构（文本/id 形态一致）。

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/core/string.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/declarative_host.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 等宽假文本端口（与 ui_script_host_test 同款）。
class DeclarativeTestTextPort final : public st::ui::TextPort {
 public:
  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> st::math::Size override {
    return st::math::Size{8.0F * static_cast<float>(st::utf8_length(utf8)), size * 1.45F};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                     st::text::FontRole role = st::text::FontRole::Proportional) const -> float override {
    (void)role;
    (void)size;
    return 8.0F * static_cast<float>(st::utf8_length(utf8));
  }
  [[nodiscard]] auto line_height(float size) const -> float override { return size * 1.45F; }
  void draw(st::raster::Surface&, std::string_view, st::math::Point, float, st::math::Color,
            st::text::FontRole = st::text::FontRole::Proportional) const override {}
  [[nodiscard]] auto ellipsize(std::string_view utf8, float, float) const -> std::string override {
    return std::string(utf8);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string(utf8)};
  }
};

struct Fixture {
  DeclarativeTestTextPort port{};
  st::ui::UiRoot root{};
  std::unique_ptr<st::ui::ScriptHost> script{};
  std::unique_ptr<st::ui::DeclarativeHost> decl{};

  Fixture() {
    root.set_text_port(&port);
    root.set_viewport(st::math::Size{600.0F, 400.0F});
    // 占位根：ScriptHost 快照需要非空树（声明式挂载后即被替换）
    root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
    root.layout(true);
    script = std::make_unique<st::ui::ScriptHost>(root);
    decl = st::ui::DeclarativeHost::attach(*script, root);
  }

  [[nodiscard]] auto ready() const -> bool { return decl != nullptr; }

  /// 真实点击：走公开的 dispatch（命中测试 + 冒泡 + 观察者在元素处理后）
  auto real_click(st::ui::Element& target) -> bool {
    st::ui::Event event;
    event.kind = st::ui::EventKind::Click;
    event.position = st::math::Point{target.bounds().x + 2.0F, target.bounds().y + 2.0F};
    event.button = 1;
    event.click_count = 1;
    return root.dispatch(event);
  }

  [[nodiscard]] auto run(const std::string& code) -> std::string {
    auto outcome = script->eval(code);
    if (!outcome) return "错误:" + outcome.error().message;
    return outcome->is_string() ? st::json_as_string(*outcome) : st::json_dump(*outcome);
  }
};

}  // namespace

ST_TEST(declarative_host_attaches_with_compose_api) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  // declarative.js 的 API 已注入
  ST_CHECK_EQ(fx.run("typeof compose"), std::string("function"));
  ST_CHECK_EQ(fx.run("typeof useState"), std::string("function"));
  ST_CHECK_EQ(fx.run("typeof __d_reconcile"), std::string("function"));
}

ST_TEST(declarative_compose_mounts_tree) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    compose('Hello', () => column({gap: 8}, [
      text('你好声明式'),
      row({gap: 4}, [
        button('点我', () => log('clicked')),
        badge('v1'),
      ]),
    ]))
  )JS");
  if (!status.has_value()) {
    // 错误信息带上便于调试
    ST_FAIL("compose 执行失败: " + status.error().message);
  }
  ST_REQUIRE(status.has_value());
  // 首挂即重组（compose 内部调 __d_reconcile），变更集已提交
  st::ui::Element* content = fx.root.content();
  ST_CHECK(content != nullptr);
  ST_CHECK_EQ(content->type(), std::string_view("Panel"));
  ST_CHECK_EQ(content->child_count(), 2U);           // Text + Row(Panel)
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("你好声明式"));
  ST_CHECK_EQ(content->child_at(1)->child_count(), 2U);  // Button + Badge
}

ST_TEST(declarative_state_drives_reconcile) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let count = null;
    compose('Counter', () => {
      if (count === null) count = useState(0);
      return column({gap: 8}, [
        text(() => '点击了 ' + count.value + ' 次'),
        button('+1', () => { count.value = count.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* content = fx.root.content();
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("点击了 0 次"));

  // 真实点击：按钮事件 → 观察者（在元素处理后）→ JS 回调写 state → dirty
  st::ui::Element* button = content->child_at(1);
  ST_CHECK(fx.real_click(*button));
  // 下一帧重组
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("点击了 1 次"));
  // 再点两次 → 3
  ST_CHECK(fx.real_click(*button));
  ST_CHECK(fx.real_click(*button));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("点击了 3 次"));
}

// 协议路径：控制通道 `invoke click` 走 ui::invoke_element（合成事件通知观察者）——
// 与真实鼠标点击对 JS 声明式层必须表现一致（DESIGN §6.7 事件顺序）。
ST_TEST(declarative_protocol_invoke_click_drives_state) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let n = null;
    compose('Proto', () => {
      if (n === null) n = useState(0);
      return column({}, [
        text(() => 'n=' + n.value),
        button('inc', () => { n.value = n.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* content = fx.root.content();
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("n=0"));

  // 协议路径：ui::invoke_element('click') 与控制通道 invoke 同一份实现
  st::ui::Element* button = content->child_at(1);
  ST_CHECK(st::ui::invoke_element(fx.root, *button, "click", ""));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("n=1"));

  // 连续 10 次（每次点击恰一次变化，无丢失/重复）
  for (int index = 0; index < 10; ++index) {
    (void)st::ui::invoke_element(fx.root, *button, "click", "");
    (void)fx.decl->tick();
  }
  fx.root.layout(true);
  ST_CHECK_EQ(content->child_at(0)->semantics_text(), std::string("n=11"));
}

// 条件分支（JS 侧）：if 多声明一项后收起 → 裁剪不留残。
ST_TEST(declarative_conditional_prunes_in_js) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let open = null;
    compose('Cond', () => {
      if (open === null) open = useState(false);
      const kids = [
        text(() => 'head'),
        button('toggle', () => { open.value = !open.value; }),
      ];
      if (open.value) kids.push(text(() => 'extra'));
      return column({}, kids);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* content = fx.root.content();
  ST_CHECK_EQ(content->child_count(), 2U);

  st::ui::Element* toggle = content->child_at(1);
  ST_CHECK(st::ui::invoke_element(fx.root, *toggle, "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  ST_CHECK_EQ(content->child_count(), 3U);

  // 收起：裁剪（不留残）
  ST_CHECK(st::ui::invoke_element(fx.root, *toggle, "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  ST_CHECK_EQ(content->child_count(), 2U);
}

// ArkTS 风格（≈ ArkUI）：大写组件名 + 链式修饰（.padding().gap().onClick()）。
// 与 compose 风格产出同一 VNode，与 C++ 宿主同一语义。
ST_TEST(declarative_arkts_style_chain_api) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let n = null;
    compose('ArkTs', () => {
      if (n === null) n = useState(0);
      return Column([
        Text(() => '计数 ' + n.value).padding(6),
        Row([
          Button('+1').onClick(() => { n.value = n.value + 1; }).gap(4),
          Text('固定').gap(4),
        ]).gap(8),
        ForEach([{id: 'a', label: 'Alpha'}, {id: 'b', label: 'Beta'}],
                (it) => it.id,
                (it) => Text(it.label)),
      ]).gap(12).padding(20);
    })
  )JS");
  if (!status.has_value()) ST_FAIL(status.error().message);
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  ST_CHECK_EQ(std::string(root->type()), std::string("Panel"));
  // Column 的三个直接子：Text + Row + （ForEach 展平后的两项 → 共 4 项）
  ST_CHECK_EQ(root->child_count(), 4U);
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("计数 0"));
  ST_CHECK_EQ(std::string(root->child_at(1)->type()), std::string("Panel"));  // Row
  ST_CHECK_EQ(root->child_at(2)->semantics_text(), std::string("Alpha"));
  ST_CHECK_EQ(root->child_at(3)->semantics_text(), std::string("Beta"));

  // 点击链式按钮 → 状态驱动
  st::ui::Element* button = root->child_at(1)->child_at(0);
  ST_CHECK_EQ(std::string(button->type()), std::string("Button"));
  ST_CHECK(st::ui::invoke_element(fx.root, *button, "click", ""));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("计数 1"));
}

// ArkTS 风格与 compose 风格混用 + 与 C++ 宿主结构一致（跨风格同一语义）。
ST_TEST(declarative_arkts_and_compose_mix) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    compose('Mix', () => Column([
      Text('大写声明'),
      row({gap: 4}, [text('小写声明')]),
    ]).gap(6));
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  ST_CHECK_EQ(root->child_count(), 2U);
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("大写声明"));
  // child_at(1) 是 row() 建的 Row 容器；文本在其子节点
  ST_CHECK_EQ(std::string(root->child_at(1)->type()), std::string("Panel"));
  ST_CHECK_EQ(root->child_at(1)->child_at(0)->semantics_text(), std::string("小写声明"));
}

// useResource：异步状态（含代次取消）。微任务由 tick() 里的 pump_jobs 泵。
ST_TEST(declarative_use_resource_async_state) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    const fakeFetch = (key) => Promise.resolve('数据:' + key);
    let key = null;
    compose('Async', () => {
      if (key === null) key = useState('a');
      const res = useResource((k) => fakeFetch(k), key.value);
      return column({}, [
        text(() => res.value.status === 'ok' ? res.value.value : ('[' + res.value.status + ']')),
        button('换', () => { key.value = 'b'; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  // 首帧：pending（微任务还没泵）
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("[pending]"));

  // 泵微任务 → 结果落地标脏 → 重组
  (void)fx.decl->pump_jobs();
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("数据:a"));

  // 换输入 → 重发 → 新结果
  st::ui::Element* button = fx.root.content()->child_at(1);
  ST_CHECK(st::ui::invoke_element(fx.root, *button, "click", ""));
  (void)fx.decl->pump_jobs();
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("数据:b"));
}

// useResource 错误路径：fetcher 抛 → status='error'（不崩、不白屏）。
ST_TEST(declarative_use_resource_error_path) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    const failing = () => Promise.reject(new Error('取数失败'));
    compose('Err', () => {
      const res = useResource(() => failing(), 'x');
      return text(() => res.value.status);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  (void)fx.decl->pump_jobs();
  (void)fx.decl->tick();
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->semantics_text(), std::string("error"));
}

// key 对齐：列表插入/重排时，**同 key 项沿用同一元素**（id/事件绑定保持）——
// 语义同 `List::sync_items`（顺序变了，"那一项"不变）。
ST_TEST(declarative_for_each_key_alignment) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let items = null;
    compose('List', () => {
      if (items === null) items = useState([{id: 'a', label: 'A'}, {id: 'b', label: 'B'}, {id: 'c', label: 'C'}]);
      return column({}, [
        ForEach(items.value, (it) => it.id, (it) => Text(it.label)),
        button('重排', () => { items.value = [items.value[2], items.value[0], items.value[1]]; }),
        button('删除B', () => { items.value = items.value.filter((it) => it.id !== 'b'); }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  ST_CHECK_EQ(root->child_count(), 5U);   // A B C + 两个按钮
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("A"));
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("B"));
  ST_CHECK_EQ(root->child_at(2)->semantics_text(), std::string("C"));

  // 重排为 C A B：元素身份（id）应跟着 key 走
  const std::string id_a = root->child_at(0)->derived_id();
  const std::string id_c = root->child_at(2)->derived_id();
  st::ui::Element* shuffle = root->child_at(3);
  ST_CHECK(st::ui::invoke_element(fx.root, *shuffle, "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("C"));
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("A"));
  ST_CHECK_EQ(root->child_at(2)->semantics_text(), std::string("B"));
  // 元素身份保持：A 还是同一个元素（"那一项"没被换成别的内容）
  ST_CHECK_EQ(root->child_at(1)->derived_id(), id_a);
  ST_CHECK_EQ(root->child_at(0)->derived_id(), id_c);

  // 删除 B：C A（其余不动）
  st::ui::Element* remove = root->child_at(4);   // 0..2 项目，3 重排，4 删除B
  ST_CHECK(st::ui::invoke_element(fx.root, *remove, "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("C"));
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("A"));
  ST_CHECK_EQ(root->child_at(1)->derived_id(), id_a);   // 身份仍保持
  ST_CHECK_EQ(root->child_count(), 4U);                 // C A + 两个按钮（B 已删）
}
