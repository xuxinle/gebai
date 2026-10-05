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

#include "st/ext/json.hpp"
#include "tests/support/text_port_fixtures.hpp"

// 测试在匿名命名空间内，`st::test::X` 得写全；用具名别名让用例读起来干净。
using st::test::FixedAdvanceTextPort;

namespace {


struct Fixture {
  FixedAdvanceTextPort port{};
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

// ── useMemo / useEffect / useRef / usePersisted（hooks 槽位体系）───────────
//
// 这四个与 `useResource` **共用一个调用点游标**（同一 build 里第 N 个 hook = 第 N 个槽）——
// 所以必须与 useResource 混用时槽也不串（下面的用例就故意混着写）。

// useMemo：依赖未变不重算（依赖按值 JSON 比较）。
ST_TEST(declarative_use_memo_caches_by_deps) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let calls = 0;
    let seed = null;
    let other = null;
    compose('Memo', () => {
      if (seed === null) seed = useState(1);
      if (other === null) other = useState(0);
      const doubled = useMemo(() => { calls++; return seed.value * 2; }, [seed.value]);
      return column({}, [
        text(() => 'v=' + doubled),
        text(() => 'calls=' + calls),
        text(() => 'other=' + other.value),
        button('bump', () => { seed.value = seed.value + 1; }),
        button('mine', () => { other.value = other.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("v=2"));
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("calls=1"));

  // ① 无关状态写 → 重跑 build，但依赖未变 → 不重算
  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(4), "click", ""));   // mine
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("calls=1"));
  ST_CHECK_EQ(root->child_at(2)->semantics_text(), std::string("other=1"));

  // ② 依赖写 → 重算
  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(3), "click", ""));   // bump
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("v=4"));
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("calls=2"));
}

// useEffect：依赖变化才跑（首帧跑一次、依赖不变不重跑），且副作用里写的状态必须落地。
ST_TEST(declarative_use_effect_runs_on_dep_change) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let topic = null;
    let log = null;
    compose('Effect', () => {
      if (topic === null) topic = useState(1);
      if (log === null) log = useState('');
      useEffect(() => { log.value = log.value + topic.value; }, [topic.value]);
      return column({}, [
        text(() => 'log=' + log.value),
        button('next', () => { topic.value = topic.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("log=1"));

  // 点一次 → effect 重跑 → 它写的状态必须能收敛（引擎在同次 tick 内再重组一帧）
  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(1), "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("log=12"));

  // 再来一次：每轮 topic 变化恰好追加一次（无重复执行）
  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(1), "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("log=123"));
}

// useRef：跨重组稳定、改它不触发重组（要驱动界面用 useState）。
ST_TEST(declarative_use_ref_is_stable_and_not_reactive) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let tick = null;
    compose('Ref', () => {
      if (tick === null) tick = useState(0);
      const counter = useRef(0);
      counter.current = counter.current + 1;      // 每帧重组累加
      return column({}, [
        text(() => 'n=' + counter.current + '/t=' + tick.value),
        button('bump', () => { tick.value = tick.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("n=1/t=0"));

  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(1), "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  // 跨重组保持（新槽会从 0 起 → 这里会变 n=1）
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("n=2/t=1"));

  // 无脏时不重跑（ref 的写不标脏；这里也没有别的东西标脏）
  ST_CHECK_EQ(fx.decl->tick(), false);
}

// usePersisted：初值从宿主状态仓读回（协议 `script.state` 同源）、写穿透回去。
ST_TEST(declarative_use_persisted_round_trips_host_state) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  ST_CHECK(fx.script->set_state(*st::json_parse(R"({"draft":"草稿"})")).has_value());
  auto status = fx.decl->run(R"JS(
    let draft = null;
    compose('Persist', () => {
      if (draft === null) draft = usePersisted('draft', '');
      return column({}, [
        text(() => 'd=' + draft.value),
        button('edit', () => { draft.value = draft.value + '!'; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  // 初值来自宿主状态仓（不是 fallback ''）
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("d=草稿"));

  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(1), "click", ""));
  (void)fx.decl->tick();
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("d=草稿!"));
  // 写穿透：宿主状态仓里也是新值
  auto host_state = fx.script->state();
  ST_REQUIRE(host_state.has_value());
  ST_CHECK_EQ(st::json_get_string(*host_state, "draft", ""), std::string("草稿!"));
}

// hooks 混合：useState/useMemo/useEffect/useRef/useResource 同在一个 build 里，
// 槽位按调用点对齐（串了就会出现「第 2 个 hook 拿到第 1 个的缓存」这类静默错位）。
ST_TEST(declarative_hook_slots_do_not_mix) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let n = null;
    compose('Mix', () => {
      if (n === null) n = useState(2);
      const twice = useMemo(() => n.value * 2, [n.value]);
      const seen = useRef('');
      useEffect(() => { seen.current = 'e' + n.value; }, [n.value]);
      const res = useResource(() => Promise.resolve('r' + n.value), n.value);
      return column({}, [
        text(() => 'twice=' + twice),
        text(() => 'seen=' + seen.current),
        text(() => 'res=' + (res.value.status === 'ok' ? res.value.value : res.value.status)),
        button('bump', () => { n.value = n.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  // 首帧：memo 槽拿到的是 n=2（不是 resource 的 pending 对象）
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("twice=4"));
  // effect 在重组末尾跑（首挂走 compose 的 __d_reconcile，不经 tick）——tick 一次让它落地
  (void)fx.decl->tick();

  // 点一次：四个 hook 的槽都跟着 n 前进（串槽的话这里会读到旧值/pending）
  ST_CHECK(st::ui::invoke_element(fx.root, *root->child_at(3), "click", ""));
  (void)fx.decl->tick();   // 同一次 tick 内：重组 → effect → 泵微任务 → 再重组（异步结果同帧可见）
  fx.root.layout(true);
  root = fx.root.content();
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("twice=6"));
  // effect 在本帧内跑完并收敛（seen 是 ref：effect 写了它，收敛那一帧的 build 看得到）
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("seen=e3"));
  ST_CHECK_EQ(root->child_at(2)->semantics_text(), std::string("res=r3"));
}

// ── 片段摊平：`forEach()` 的结果塞进 kids 数组（compose 风格最常见的写法）──
//
// `forEach()` 返回的是**子节点列表**，写法上常被直接放进数组：
// `column({}, [text('头'), forEach(...)])`。若只认 `__fragment` 容器而不摊平数组，
// 这个数组会被当成一个 VNode（`type` 是 `undefined`）——创建环节报一行错就整段消失，
// 且**不崩**：界面少一块（实测：顶层子元素 0 个、Text 计数 0，而大写
// `Column([ForEach(...)])` 同写法正常）。这类静默失效最难查，必须有用例钉住。
ST_TEST(declarative_for_each_result_flattens_inside_array) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    const rows = [{id: 'a', n: 'Alpha'}, {id: 'b', n: 'Beta'}];
    compose('Flat', () => column({gap: 2}, [
      text('头部'),
      forEach(rows, (it) => it.id, (it) => text(it.n)),
      text('尾部'),
    ]))
  )JS");
  if (!status.has_value()) ST_FAIL(status.error().message);
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  st::ui::Element* root = fx.root.content();
  // 头部 + Alpha + Beta + 尾部（片段摊平后与兄弟同级，不多套一层）
  ST_REQUIRE(root != nullptr);
  ST_CHECK_EQ(root->child_count(), 4U);
  ST_CHECK_EQ(root->child_at(0)->semantics_text(), std::string("头部"));
  ST_CHECK_EQ(root->child_at(1)->semantics_text(), std::string("Alpha"));
  ST_CHECK_EQ(root->child_at(2)->semantics_text(), std::string("Beta"));
  ST_CHECK_EQ(root->child_at(3)->semantics_text(), std::string("尾部"));

  // 嵌套片段：数组里再嵌数组，同样摊平
  Fixture nested;
  ST_REQUIRE(nested.ready());
  auto nested_status = nested.decl->run(R"JS(
    compose('Nest', () => column({}, [
      [text('内层一'), [text('内层二')]],
    ]))
  )JS");
  ST_REQUIRE(nested_status.has_value());
  nested.root.layout(true);
  ST_CHECK_EQ(nested.root.content()->child_count(), 2U);
}

// ── 事件绑定身份：跨帧复用**不得**重绑 ──────────────────────────────────
//
// 回调闭包每帧重建（`() => { n.value++ }` 是 build 里的新函数对象）。若实现按
// 「回调变了就重绑」，等价于每帧把整棵树的事件全部注销重绑——实测三帧的绑定
// id 是 b1..b3 → b4..b6 → b7..b9（codeeditor 整页声明式每帧都在付这笔钱）。
//
// 现在绑定按**元素 id** 持有、只在首次出现回调时登记：跨帧的绑定 id 必须不变
// （反向验证：把 `ensureEventBinding` 改回无条件 release+on，本用例立即红）。
ST_TEST(declarative_event_bindings_survive_reuse_frames) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let n = null;
    compose('Bind', () => {
      if (n === null) n = useState(0);
      return column({}, [
        text(() => 'n=' + n.value),
        button('a', () => { n.value = n.value + 1; }),
        button('b', () => { n.value = n.value + 1; }),
        button('c', () => { n.value = n.value + 1; }),
      ]);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  auto binding_ids = [&]() {
    std::vector<std::string> ids;
    for (const auto& binding : fx.script->bindings()) ids.push_back(binding.id);
    return ids;
  };
  const std::vector<std::string> first = binding_ids();
  ST_CHECK_EQ(first.size(), 3U);

  // 连续重组三帧：绑定集合必须**逐项不变**
  for (int index = 0; index < 3; ++index) {
    ST_CHECK(st::ui::invoke_element(fx.root, *fx.root.content()->child_at(1), "click", ""));
    ST_CHECK(fx.decl->tick());
  }
  fx.root.layout(true);
  const std::vector<std::string> after = binding_ids();
  ST_CHECK_EQ(after.size(), first.size());
  for (std::size_t index = 0; index < first.size() && index < after.size(); ++index) {
    ST_CHECK_EQ(after[index], first[index]);   // 绑定 id 逐项不变：复用帧不重绑
  }
  // 且回调仍是**本帧**的（n 已推进 3 次 → 文本跟着走）
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("n=3"));

  // 事件仍可用：再点一次（间接层派发到最新 VNode 的回调）
  ST_CHECK(st::ui::invoke_element(fx.root, *fx.root.content()->child_at(2), "click", ""));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("n=4"));
}

// ── 事件绑定随卸载反注册（不泄漏）──────────────────────────────────────
ST_TEST(declarative_event_bindings_release_on_unmount) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  auto status = fx.decl->run(R"JS(
    let open = null;
    compose('Release', () => {
      if (open === null) open = useState(true);
      const kids = [button('切换', () => { open.value = !open.value; })];
      if (open.value) kids.push(button('临时', () => log('tmp')));
      return column({}, kids);
    })
  )JS");
  ST_REQUIRE(status.has_value());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.script->bindings().size(), 2U);

  // 收起：临时按钮的绑定应被反注册（1 个切换按钮 + 1 个临时 → 剩 1）
  ST_CHECK(st::ui::invoke_element(fx.root, *fx.root.content()->child_at(0), "click", ""));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_count(), 1U);
  ST_CHECK_EQ(fx.script->bindings().size(), 1U);

  // 再展开：重新登记（不是复用已失效的旧绑定）
  ST_CHECK(st::ui::invoke_element(fx.root, *fx.root.content()->child_at(0), "click", ""));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_count(), 2U);
  ST_CHECK_EQ(fx.script->bindings().size(), 2U);
}

// ── 场景卸载：再 compose 一页不叠树、hook 槽不复用旧界面 ────────────────
//
// 卸载路径有三件事必须做对，漏一件都不报错：
// ① 真值树：新一页直接挂在旧一页下面（旧界面的元素不会自己消失）；
// ② effect 槽：hook 槽按**调用点序号**对齐，不清就跳到旧界面的第 N+1 个槽
//    （新页面第 1 个 hook 拿到旧页面的缓存）——同一类「静默串味」；
// ③ effect 清理：被卸掉的界面里的清理函数永不被调（连接/定时器泄漏）。
ST_TEST(declarative_compose_replaces_previous_page) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  ST_CHECK(fx.decl->run(R"JS(
    compose('First', () => column({}, [text('第一页'), text('甲的'), text('乙的')]))
  )JS").has_value());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_count(), 3U);

  // 换一页：旧的整棵卸掉（不是叠上去）
  ST_CHECK(fx.decl->run(R"JS(
    compose('Second', () => column({}, [text('第二页')]))
  )JS").has_value());
  fx.root.layout(true);
  ST_REQUIRE(fx.root.content() != nullptr);
  ST_CHECK_EQ(fx.root.content()->child_count(), 1U);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("第二页"));

  // hook 槽不复用旧页：新页第一个 hook 是崭新的计数（从 1 起，不是接在旧页后面）
  ST_CHECK(fx.decl->run(R"JS(
    let n = null;
    compose('Third', () => {
      if (n === null) n = useState(1);
      return column({}, [
        text(() => 'n=' + n.value),
        button('inc', () => { n.value = n.value + 1; }),
      ]);
    })
  )JS").has_value());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("n=1"));
  ST_CHECK(st::ui::invoke_element(fx.root, *fx.root.content()->child_at(1), "click", ""));
  ST_CHECK(fx.decl->tick());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("n=2"));
}

// effect 清理：卸载时跑（`unmount_declarative`），且只跑一次。
ST_TEST(declarative_unmount_runs_effect_cleanups) {
  Fixture fx;
  ST_REQUIRE(fx.ready());
  ST_CHECK(fx.decl->run(R"JS(
    state.cleaned = 0;
    compose('Lifecycle', () => {
      const ref = useRef(null);
      useEffect(() => {
        ref.current = 'open';                      // 模拟「建立连接」
        return () => { state.cleaned = state.cleaned + 1; };   // 「关闭连接」
      }, []);
      return column({}, [text('资源页')]);
    })
  )JS").has_value());
  fx.root.layout(true);
  (void)fx.decl->tick();   // effect 在重组末尾跑（首挂那次已在 compose 内跑过）

  ST_CHECK(fx.decl->unmount_declarative());
  ST_CHECK_EQ(fx.run("state.cleaned"), std::string("1"));
  // 再卸一次：没有东西可卸（幂等，不重复清理）
  ST_CHECK(!fx.decl->unmount_declarative());
  ST_CHECK_EQ(fx.run("state.cleaned"), std::string("1"));

  // 卸完还能装新的一页（状态干净）
  ST_CHECK(fx.decl->run(R"JS(
    compose('Again', () => column({}, [text('重装页')]))
  )JS").has_value());
  fx.root.layout(true);
  ST_CHECK_EQ(fx.root.content()->child_at(0)->semantics_text(), std::string("重装页"));
}
