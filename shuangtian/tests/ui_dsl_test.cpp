// 声明式 UI（dsl）测试：挂载 / 状态传播 / 元素复用 / 裁剪 / 冻结 / 工厂。
//
// 场景与 docs/declarative.md §9 对应；全部用 UiRoot 直驱（无后端）。

#include "st/ui/dsl.hpp"

#include "st/test/test.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/select.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/components/tree.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace st::ui;
using namespace st::ui::dsl;

/// 计数器页（最小形态）：验证「声明式描述 + 状态驱动重组」全链路。
struct CounterPage : Component {
  State<int> count{0};
  State<bool> show_extra{false};

  void build(Composer& c) override {
    column(c, {.gap = 12.0f, .padding = 16.0f}, [&] {
      text(c, [&] { return std::to_string(count.value()); }, {.key = "count"});
      row(c, {.gap = 8.0f}, [&] {
        button(c, "+1", [this] { count.set(count.value() + 1); }, {.key = "inc"});
        button(c, "切换", [this] { show_extra.set(!show_extra.value()); }, {.key = "toggle"});
      });
      if (show_extra.value()) {
        text(c, [] { return std::string("额外内容"); }, {.key = "extra"});
      }
    });
  }
};

// find_text 辅助不再需要（直接按结构导航）——保留空实现占位避免未用警告。

// ── 1. 挂载与结构 ────────────────────────────────────────────────────────

ST_TEST(dsl_mount_builds_tree) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto host = dsl::mount(root, std::make_shared<CounterPage>());
  ST_CHECK(host != nullptr);
  root.layout();
  // 结构：根 Panel(Column) > Text + Panel(Row) > 2×Button
  Element* content = root.content();
  ST_CHECK(content != nullptr);
  ST_CHECK(content->type() == std::string_view("Panel"));
  ST_CHECK(content->child_count() == 2);  // Text + Row
  ST_CHECK(content->child_at(0)->type() == std::string_view("Text"));
  ST_CHECK(content->child_at(1)->type() == std::string_view("Panel"));
  ST_CHECK(content->child_at(1)->child_count() == 2);  // +1 / 切换
  // 语义面：按钮可被 get（属性面同一套）
  const auto snapshot = element_snapshot(*content->child_at(1)->child_at(0));
  const st::Json* label = nullptr;
  if (snapshot.is_object()) {
    if (auto props_it = st::json_find(snapshot, "props"); props_it != nullptr) {
      label = st::json_find(*props_it, "label");
    }
  }
  ST_CHECK(label != nullptr && st::json_as_string(*label) == std::string("+1"));
}

// ── 2. 状态传播（点击 → 重组 → 文本变化）────────────────────────────────

ST_TEST(dsl_state_drives_reconcile) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto host = dsl::mount(root, std::make_shared<CounterPage>());
  root.layout();
  Element* counter_text = root.content()->child_at(0);
  const ElementId id_before = counter_text->derived_id();
  ST_CHECK(counter_text->semantics_text() == std::string("0"));
  // 点击 +1（事件回调里写状态 → 标脏）
  Element* inc_button = root.content()->child_at(1)->child_at(0);
  if (auto* button = dynamic_cast<Button*>(inc_button); button != nullptr) {
    button->on_click();  // 事件回调（thread-local 无 Composer → 全局注册表通知）
  }
  ST_CHECK(host->dirty());
  (void)host->tick();  // 帧首重组
  root.layout();
  ST_CHECK(counter_text->semantics_text() == std::string("1"));
  ST_CHECK(counter_text->derived_id() == id_before);  // 复用：身份不变
  // 再点两次
  if (auto* button = dynamic_cast<Button*>(inc_button); button != nullptr) {
    button->on_click();
    button->on_click();
  }
  (void)host->tick();
  root.layout();
  ST_CHECK(counter_text->semantics_text() == std::string("3"));
}

// ── 3. 条件渲染与裁剪 ───────────────────────────────────────────────────

ST_TEST(dsl_conditional_render_and_prune) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto host = dsl::mount(root, std::make_shared<CounterPage>());
  root.layout();
  Element* content = root.content();
  ST_CHECK(content->child_count() == 2);  // 无 extra

  // 打开 extra
  if (auto* toggle = dynamic_cast<Button*>(content->child_at(1)->child_at(1))) {
    toggle->on_click();
  }
  (void)host->tick();
  root.layout();
  ST_CHECK(content->child_count() == 3);
  ST_CHECK(content->child_at(2)->semantics_text() == std::string("额外内容"));

  // 关掉 extra → 裁剪（不留残）
  if (auto* toggle = dynamic_cast<Button*>(content->child_at(1)->child_at(1))) {
    toggle->on_click();
  }
  (void)host->tick();
  root.layout();
  ST_CHECK(content->child_count() == 2);
}

// ── 4. 冻结护栏 ────────────────────────────────────────────────────────

struct ThrowingPage : Component {
  State<int> mode{0};
  State<bool> trigger{false};   // 触发器：被 build 读过 → 写它必标脏（重组的前提）
  void build(Composer& c) override {
    const int current = mode.value();   // 读：登记依赖（后续 set(mode) 才会触发重组）
    const bool should_throw = trigger.value() && current > 0;  //lint-allow: L5 谬误注入测试：被测行为就是抛异常
    if (should_throw) throw std::runtime_error("build 失败");  // lint-allow: L5 谬误注入测试：被测行为就是抛异常
    column(c, {}, [&] { text(c, [] { return std::string("正常"); }); });
  }
};

ST_TEST(dsl_build_throw_freezes) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<ThrowingPage>();
  auto host = dsl::mount(root, page);
  root.layout();
  ST_CHECK(root.content()->child_at(0)->semantics_text() == std::string("正常"));

  page->trigger.set(true);       // 先开触发器
  page->mode.set(1);             // 再写被订阅的状态 → 标脏 → 下一帧重组（build 会抛）
  const auto stats = host->tick();
  ST_CHECK(!stats.error.empty());
  ST_CHECK(root.content()->child_at(0)->semantics_text() == std::string("正常"));  // 冻结
  // 冻结后不再重试
  ST_CHECK(!host->dirty());
}

// ── 5. 元素工厂 ────────────────────────────────────────────────────────

ST_TEST(dsl_make_element_covers_builtin_types) {
  ST_CHECK(make_element("Text") != nullptr);
  ST_CHECK(make_element("Button") != nullptr);
  ST_CHECK(make_element("Input") != nullptr);
  ST_CHECK(make_element("Checkbox") != nullptr);
  ST_CHECK(make_element("Switch") != nullptr);
  ST_CHECK(make_element("Slider") != nullptr);
  ST_CHECK(make_element("List") != nullptr);
  ST_CHECK(make_element("ProgressBar") != nullptr);
  ST_CHECK(make_element("Badge") != nullptr);
  ST_CHECK(make_element("Card") != nullptr);
  ST_CHECK(make_element("Unknown") == nullptr);
}

// ── 5b. 逃生舱 custom<T> / icon / tabs 数据驱动 ────────────────────

ST_TEST(dsl_custom_escape_hatch) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  struct Page : Component {
    void build(Composer& c) override {
      column(c, {.gap = 6.0F}, [&] {
        // custom<T>：声明式创建 + 强类型指针接一等接口
        auto& tabs = custom<Tabs>(c, [](Tabs& t) { t.set_id("my-tabs"); }, {.key = "tabs"});
        std::vector<Tabs::Tab> items{{.key = "a", .label = "A"},
                                     {.key = "b", .label = "B"}};
        tabs.sync_tabs(items);
        tabs.set_active(1);
        (void)icon(c, "sun", 16.0F, {.key = "sun"});
      });
    }
  };
  auto host = dsl::mount(root, std::make_shared<Page>());
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  Element* content = root.content();
  ST_CHECK_EQ(content->child_count(), 2U);
  ST_CHECK_EQ(std::string(content->child_at(0)->type()), std::string("Tabs"));
  ST_CHECK_EQ(content->child_at(0)->derived_id(), std::string("my-tabs"));   // 配置回调生效
  auto* tabs = dynamic_cast<Tabs*>(content->child_at(0));
  ST_CHECK(tabs != nullptr);
  if (tabs != nullptr) {
    ST_CHECK_EQ(tabs->tab_count(), 2U);
    ST_CHECK_EQ(std::string(tabs->active_key()), std::string("b"));
  }
  ST_CHECK_EQ(std::string(content->child_at(1)->type()), std::string("Icon"));
}

ST_TEST(dsl_tabs_data_driven) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  struct Page : Component {
    State<std::size_t> active{0};
    std::vector<TabData> items{{.key = "x", .label = "X"}, {.key = "y", .label = "Y"},
                               {.key = "z", .label = "Z"}};
    void build(Composer& c) override {
      tabs(c, items, active.value(), [this](std::size_t index) { active.set(index); });
    }
  };
  auto page = std::make_shared<Page>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  auto* tabs = dynamic_cast<Tabs*>(root.content());
  ST_CHECK(tabs != nullptr);
  if (tabs == nullptr) return;
  ST_CHECK_EQ(tabs->tab_count(), 3U);
  ST_CHECK_EQ(std::string(tabs->active_key()), std::string("x"));
  // 数据驱动活动态：改 state → 重组 → active 跟随
  page->active.set(2);
  (void)host->tick();
  ST_CHECK_EQ(std::string(tabs->active_key()), std::string("z"));
}

// ── 5g. 嵌套作用域树 + 取消语义 ───────────────────────────

ST_TEST(dsl_nested_scopes_and_cancel) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});

  // 三层：Page → Panel(子) → Leaf(孙)；每层有自己状态
  struct Leaf : Component {
    State<int> value{0};
    int builds{0};
    void build(Composer& c) override {
      ++builds;
      column(c, {}, [&] { text(c, [&] { return "叶:" + std::to_string(value.value()); }); });
    }
  };
  struct Panel : Component {
    State<int> value{0};
    int builds{0};
    std::shared_ptr<Leaf> leaf{std::make_shared<Leaf>()};
    void build(Composer& c) override {
      ++builds;
      column(c, {}, [&] {
        text(c, [&] { return "面板:" + std::to_string(value.value()); });
        dsl::sub_component(c, leaf, "leaf");
      });
    }
  };
  struct Page : Component {
    int builds{0};
    std::shared_ptr<Panel> panel{std::make_shared<Panel>()};
    void build(Composer& c) override {
      ++builds;
      column(c, {}, [&] { dsl::sub_component(c, panel, "panel"); });
    }
  };
  auto page = std::make_shared<Page>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  ST_CHECK(page->panel->builds > 0);
  ST_CHECK(page->panel->leaf->builds > 0);   // 孙也跑了（嵌套生效）

  // —— 改孙状态：只跑孙（页、面板都不跑）——
  const int page_b = page->builds;
  const int panel_b = page->panel->builds;
  const int leaf_b = page->panel->leaf->builds;
  page->panel->leaf->value.set(5);
  const auto stats = host->tick();
  root.layout(true);
  ST_CHECK_EQ(page->panel->leaf->builds, leaf_b + 1);   // 孙跑
  ST_CHECK_EQ(page->panel->builds, panel_b);            // 子不跑
  ST_CHECK_EQ(page->builds, page_b);                    // 页不跑
  ST_CHECK_EQ(stats.scopes_rerun, 1);
  // 内容：页 → 列 → 面板（面板 → 列 → 叶）
  Element* panel_panel = root.content()->child_at(0);
  Element* leaf_panel = panel_panel->child_at(1);
  ST_CHECK_EQ(leaf_panel->child_at(0)->semantics_text(), std::string("叶:5"));

  // —— 改子状态：子跑（连带孙跑——父重建了子树）——
  const int leaf_b2 = page->panel->leaf->builds;
  page->panel->value.set(9);
  (void)host->tick();
  root.layout(true);
  ST_CHECK_EQ(page->panel->builds, panel_b + 1);
  ST_CHECK_EQ(page->panel->leaf->builds, leaf_b2 + 1);   // 孙随之重跑（对齐）
  ST_CHECK_EQ(root.content()->child_at(0)->child_at(0)->semantics_text(), std::string("面板:9"));

  // —— 取消：输入变化后旧代的结果不得写入 ——
  struct AsyncPage : Component {
    State<std::string> query{"a"};
    void build(Composer& c) override {
      // fetcher 收取消牌：模拟耗时任务（睡眠后检查取消）
      const auto& data = resource<std::string>(
          c,
          [](const std::string& key, AsyncCancel cancel) {
            for (int step = 0; step < 50; ++step) {
              if (cancel.is_cancelled()) return std::string("[cancelled]") + key;
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return std::string("完成:") + key;
          },
          query.value());
      column(c, {}, [&] {
        text(c, [&] {
          const auto& current = data.value();
          return current.status == AsyncStatus::Ok ? current.value : std::string("[pending]");
        });
      });
    }
  };
  auto async_page = std::make_shared<AsyncPage>();
  auto async_host = dsl::mount(root, async_page);
  ST_REQUIRE(async_host != nullptr);
  // 首挂 → 开始取 "a"；立即改输入 → 旧代翻牌
  async_page->query.set("b");
  std::string got;
  for (int attempt = 0; attempt < 400; ++attempt) {
    (void)async_host->tick();
    got = root.content()->child_at(0)->semantics_text();
    if (got == "完成:b" || got == "[cancelled]") break;   // 注：取消是给 fetcher 的提示，可能不来
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // 关键：最终值必须是**新输入**的结果，旧代（完成:a）不得覆写
  ST_CHECK_EQ(got, std::string("完成:b"));
  ST_CHECK(got.find("完成:a") == std::string::npos);
}

// ── 5f. 多作用域细粒度重组（子组件独立作用域）────────────

ST_TEST(dsl_multi_scope_fine_grained) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});

  // 子组件：有自己状态，只影响自己的文本
  struct Sidebar : Component {
    State<int> count{0};
    int builds{0};
    void build(Composer& c) override {
      ++builds;
      column(c, {}, [&] {
        text(c, [&] { return "侧栏:" + std::to_string(count.value()); });
      });
    }
  };
  struct Page : Component {
    State<int> title{0};
    int builds{0};
    std::shared_ptr<Sidebar> sidebar{std::make_shared<Sidebar>()};
    void build(Composer& c) override {
      ++builds;
      column(c, {.gap = 4.0F}, [&] {
        text(c, [&] { return "标题:" + std::to_string(title.value()); }, {.key = "title"});
        dsl::sub_component(c, sidebar, "sidebar");
      });
    }
  };
  auto page = std::make_shared<Page>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  const int page_builds_before = page->builds;
  const int sidebar_builds_before = page->sidebar->builds;
  ST_CHECK(page_builds_before > 0);
  ST_CHECK(sidebar_builds_before > 0);

  // —— 关键断言：改侧栏状态 → **只有侧栏重跑**（页面不重跑）——
  page->sidebar->count.set(7);
  ST_CHECK(host->dirty());
  const auto stats = host->tick();
  root.layout(true);
  ST_CHECK_EQ(page->sidebar->builds, sidebar_builds_before + 1);   // 侧栏重跑
  ST_CHECK_EQ(page->builds, page_builds_before);                   // 页面**没**重跑
  ST_CHECK_EQ(stats.scopes_rerun, 1);                              // 只一个作用域
  // 内容正确更新
  Element* sidebar_panel = root.content()->child_at(1);
  ST_CHECK_EQ(sidebar_panel->child_at(0)->semantics_text(), std::string("侧栏:7"));

  // —— 改页面状态 → 页面重跑（侧栏随之重跑：它的宿主位置在页面树里）——
  page->title.set(3);
  (void)host->tick();
  root.layout(true);
  ST_CHECK_EQ(page->builds, page_builds_before + 1);
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("标题:3"));
}

// ── 5e. 构造期属性组件（Select/Table/Tree）────────

ST_TEST(dsl_constructor_prop_widgets) {
  UiRoot root;
  root.set_viewport({500.0F, 400.0F});
  struct Page : Component {
    State<std::size_t> picked{0};
    State<std::size_t> row_clicked{999};
    State<std::string> toggled{"—"};
    void build(Composer& c) override {
      column(c, {.gap = 8.0F}, [&] {
        // Select：选项数组 + 选中索引 + 变更回调（回调拿索引）
        (void)dsl::select(c, {{.value = "a", .label = "选项 A"},
                              {.value = "b", .label = "选项 B"}},
                          picked.value(),
                          [this](std::size_t index) { picked.set(index); }, {.key = "sel"});
        // Table：列 + 行数据
        (void)dsl::table(c, {{.label = "名称"}, {.label = "大小"}},
                         {{"a.cpp", "12 KB"}, {"b.rs", "4 KB"}},
                         [this](std::size_t row) { row_clicked.set(row); }, {.key = "tbl"});
        // Tree：节点数组（扁平 + depth）
        (void)tree(c, {{.key = "src", .label = "src", .expanded = true, .is_dir = true},
                       {.key = "src/a.cpp", .label = "a.cpp", .depth = 1}},
                   [this](const std::string& key, bool expanded) {
                     toggled.set(key + (expanded ? ":open" : ":close"));
                   },
                   {}, {.key = "tree"});
      });
    }
  };
  auto page = std::make_shared<Page>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  Element* content = root.content();
  ST_CHECK_EQ(content->child_count(), 3U);

  // Select：选项已设（属性面可见）+ 选中跟随状态
  auto* select_widget = dynamic_cast<Select*>(content->child_at(0));
  ST_REQUIRE(select_widget != nullptr);
  ST_CHECK_EQ(select_widget->option_count(), 2U);
  // 状态驱动：改 picked → 重组 → 选中跟随
  page->picked.set(1);
  (void)host->tick();
  ST_CHECK_EQ(select_widget->selected_index().value_or(99), 1U);

  // Table：列与行都到位
  auto* table_widget = dynamic_cast<Table*>(content->child_at(1));
  ST_REQUIRE(table_widget != nullptr);
  ST_CHECK_EQ(table_widget->column_count(), 2U);
  ST_CHECK_EQ(table_widget->row_count(), 2U);
  // Table 的 set_on_row_click 链路经协议 invoke 也通
  table_widget->set_selected_row(1);
  ST_CHECK_EQ(table_widget->selected_row().value_or(99), 1U);

  // Tree：节点按 key 对齐
  auto* tree_widget = dynamic_cast<Tree*>(content->child_at(2));
  ST_REQUIRE(tree_widget != nullptr);
  ST_CHECK_EQ(tree_widget->node_count(), 2U);

  // 数据驱动刷新：改数据 → 重组 → sync（同 key 复用）
  (void)host->tick();
  ST_CHECK_EQ(tree_widget->node_count(), 2U);
}

// ── 5d. 异步资源 resource（对应 JS useResource）──────────────────

ST_TEST(dsl_async_resource) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  struct Page : Component {
    State<std::string> query{"a"};
    void build(Composer& c) override {
      // 异步取数（fetcher 在**工作线程**上跑）；输入 query 变化则重发
      const auto& data = resource<std::string>(
          c, [](const std::string& key) { return std::string("值:") + key; }, query.value());
      column(c, {}, [&] {
        text(c, [&] {
          const AsyncValue<std::string>& current = data.value();
          switch (current.status) {
            case AsyncStatus::Pending: return std::string("[pending]");
            case AsyncStatus::Ok: return current.value;
            case AsyncStatus::Error: return std::string("[error]") + current.error;
          }
          return std::string();
        }, {.key = "out"});
      });
    }
  };
  auto page = std::make_shared<Page>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  Element* out = root.content()->child_at(0);
  ST_CHECK_EQ(out->semantics_text(), std::string("[pending]"));

  // 等线程完成 + 主线程泵（重试到就绪——不固定 sleep，避免机器慢时假失败）
  std::string got;
  for (int attempt = 0; attempt < 200; ++attempt) {
    (void)host->tick();   // 内部先 pump_async（执行投递的结果）
    root.layout(true);
    got = root.content()->child_at(0)->semantics_text();
    if (got == "值:a") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ST_CHECK_EQ(got, std::string("值:a"));

  // 改输入 → 重发 → 新值
  page->query.set("b");
  for (int attempt = 0; attempt < 200; ++attempt) {
    (void)host->tick();
    root.layout(true);
    got = root.content()->child_at(0)->semantics_text();
    if (got == "值:b") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ST_CHECK_EQ(got, std::string("值:b"));

  // 相同输入不重发（槽位指纹相等）——值保持
  page->query.set("b");
  (void)host->tick();
  root.layout(true);
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("值:b"));
}

// ── 5c. 声明式 overlay 生命周期（同 key 复用 / 未声明即回收）──────────

ST_TEST(dsl_overlay_lifecycle) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  struct Page : Component {
    State<bool> open{false};
    void build(Composer& c) override {
      column(c, {}, [&] { text(c, [] { return std::string("内容"); }); });
      if (open.value()) {
        (void)overlay(c, "dialog", {.padding = 20.0F, .id = "my-overlay"}, [&] {
          text(c, [] { return std::string("浮层内容"); });
        });
      }
    }
  };
  auto page = std::make_shared<Page>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  // 未声明 → 无 overlay
  ST_CHECK(root.find("my-overlay") == nullptr);

  // 打开 → 挂上（显式 id `my-overlay` 优先于槽位默认 id `overlay-dialog`）
  page->open.set(true);
  (void)host->tick();
  root.layout(true);
  Element* slot = root.find("my-overlay");
  ST_REQUIRE(slot != nullptr);
  ST_CHECK_EQ(slot->child_count(), 1U);   // 浮层内容

  // 关→开同一帧内最终为开：同 key 复用（元素身份保持，不重复挂）
  page->open.set(false);
  page->open.set(true);
  (void)host->tick();
  ST_CHECK(root.find("my-overlay") == slot);   // 同一元素
  ST_CHECK_EQ(slot->child_count(), 1U);        // 内容没被重复追加

  // 关闭 → sweep 回收
  page->open.set(false);
  (void)host->tick();
  root.layout(true);
  ST_CHECK(root.find("my-overlay") == nullptr);
}

// ── 6. 列表（数据驱动 sync 路径）──────────────────────────────────────

struct ListPage : Component {
  State<int> selection{0};
  std::vector<ListItemData> items{{"a", "Alpha"}, {"b", "Beta"}, {"c", "Gamma"}};

  void build(Composer& c) override {
    list(c, items, [this](std::size_t index) { selection.set(static_cast<int>(index)); },
         {.key = "items"});
  }
};

ST_TEST(dsl_list_data_driven) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<ListPage>();
  auto host = dsl::mount(root, page);
  root.layout();
  Element* list_element = root.content();
  ST_CHECK(list_element->type() == std::string_view("List"));
  ST_CHECK(list_element->child_count() == 3);

  // 激活第二项：点击链路 ListItem::activate → List::select → on_select 回调
  if (auto* item = dynamic_cast<ListItem*>(list_element->child_at(1))) {
    item->activate();
  }
  ST_CHECK(page->selection.value() == 1);
}

// ── 子树挂载（`mount_into`）：宿主界面里的一页用声明式描述 ───────────────
//
// 动机：示例整合把声明式小示例收进 gallery 的一页——而 `mount()` 是**单根语义**
// （替换 `UiRoot::content()`），宿主界面必须能只把一个子树交给声明式。
struct AnchoredPage : Component {
  State<int> hits{0};
  void build(Composer& c) override {
    column(c, {.gap = 4.0f, .padding = 6.0f}, [&] {
      text(c, [&] { return "命中 " + std::to_string(hits.value()); }, {.key = "label"});
      button(c, "+1", [this] { hits.set(hits.value() + 1); }, {.key = "inc"});
      if (hits.value() >= 2) {
        text(c, [] { return std::string("条件内容"); }, {.key = "extra"});
      }
    });
  }
};

ST_TEST(dsl_mount_into_host_element) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});

  // 宿主界面：根内容是一个手搭的列容器，声明式树挂到「其中一个子容器」下。
  auto host_panel = std::make_unique<Panel>(FlexDirection::Column);
  host_panel->set_id("host-page");
  host_panel->add_child(std::make_unique<Text>("宿主手搭的标题"));
  auto anchor = std::make_unique<Panel>(FlexDirection::Column);
  anchor->set_id("anchor");
  Panel* anchor_ptr = anchor.get();
  host_panel->add_child(std::move(anchor));
  root.set_content(std::move(host_panel));

  auto page = std::make_shared<AnchoredPage>();
  auto decl_host = dsl::mount_into(root, *anchor_ptr, page);
  ST_REQUIRE(decl_host != nullptr);
  root.layout();

  // ① 宿主根未被替换（单根语义 vs 子树形态的分界）
  ST_CHECK(root.content() != nullptr && root.content()->id() == std::string_view("host-page"));
  ST_CHECK(root.content()->child_count() == 2);
  // ② 声明式树落在锚点下
  ST_REQUIRE(anchor_ptr->child_count() == 1);
  Element* decl_column = anchor_ptr->child_at(0);
  ST_CHECK(decl_column->child_count() == 2);   // label + button
  // ③ 锚点元素本身仍在真值树上（协议 id 寻址不变）
  ST_CHECK(root.find("anchor") != nullptr);

  // ④ 状态驱动重组（子树内）：阈值前 2 子、达阈值 3 子、回落再见 2 子（裁剪）
  page->hits.set(1);
  (void)decl_host->tick();
  ST_CHECK(decl_column->child_count() == 2);
  page->hits.set(2);
  (void)decl_host->tick();
  ST_CHECK(decl_column->child_count() == 3);   // label + button + 条件内容
  page->hits.set(0);
  (void)decl_host->tick();
  ST_CHECK(decl_column->child_count() == 2);

  // ⑤ 锚点之外的宿主内容不受影响
  ST_CHECK(root.content()->child_count() == 2);
}

// ── 声明式 input 的幂等写入（光标不被重组推走）──────────────────────────
//
// `Input::set_text` 把光标推到末尾（程序化写入语义）；声明式每次重组都重写
// value 的话，打字中的光标会被推走（build 因任一状态变更而重跑）。
ST_TEST(dsl_input_write_is_idempotent_for_cursor) {
  struct InputPage : Component {
    State<std::string> value{"abc"};
    void build(Composer& c) override {
      input(c, value.value(), [this](std::string next) { value.set(std::move(next)); },
            {.key = "field"});
    }
  };
  UiRoot root;
  root.set_viewport({400.0f, 200.0f});
  auto page = std::make_shared<InputPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  auto* field = dynamic_cast<Input*>(root.content());
  ST_REQUIRE(field != nullptr);
  ST_CHECK(field->value() == std::string("abc"));
  // 模拟用户在中间落光标（真实编辑路径）
  field->set_cursor_index(1);
  ST_CHECK_EQ(field->cursor_index(), 1U);
  // 重组（状态未变）——不该把光标推走
  (void)host->tick();
  ST_CHECK_EQ(field->cursor_index(), 1U);
  // 状态真变了 → 写下去（set_text 会把光标回到末尾，属预期）
  page->value.set(std::string("abcd"));
  (void)host->tick();
  ST_CHECK(field->value() == std::string("abcd"));
}

// ── 载体内部子元素不被声明式裁剪（`ScrollView` 的滚动条）──────────────
//
// 回归：`ScrollView` 把自己的滚动条放进 `children_`（构造时加的），
// 而声明式的「位置对齐 + 末尾裁剪」原先按 `child_count()` 算——切到另一个
// 分支时把滚动条当成「上一帧多声明的残留」移除，组件持有的 `bar_` 裸指针
// 随即悬垂。下一个 layout 在 `bar_->arrange` 上段错误（实测：声明式里
// ScrollView 分支 ↔ List 分支互切）。
// 修法：引入 `Element::content_child_count()`（含量 = 调用方子元素数，
// 载体内部件在末尾不计入），声明式的对齐/裁剪全改用它。
ST_TEST(dsl_scrollview_internal_scrollbar_survives_branch_switch) {
  struct BranchPage : Component {
    State<int> which{0};
    void build(Composer& c) override {
      column(c, {.gap = 0.0f, .id = "page"}, [&] {
        text(c, [] { return std::string("头"); }, {.key = "head"});
        if (which.value() == 0) {
          // 分支 A：ScrollView（自持滚动条）包一段文本
          (void)custom_container<ScrollView>(
              c, [&] { text(c, [] { return std::string("终端输出"); }, {.key = "out"}); },
              [](ScrollView& s) { s.set_id("scroller"); }, {.grow = true, .key = "scroller"});
        } else {
          // 分支 B：数据驱动 List（与 A 的类型不同 → 走「移除旧位 + 插新位」路径）
          std::vector<ListItemData> items{{.key = "x", .label = "X"}};
          (void)list(c, items, [](std::size_t) {}, {.grow = true, .id = "lister"});
        }
      });
    }
  };
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<BranchPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  // 互切两次（A → B → A）；若滚动条被裁掉，此处的 layout 就会段错误
  page->which.set(1);
  (void)host->tick();
  root.layout();
  page->which.set(0);
  (void)host->tick();
  root.layout();
  // 回到 A：滚动条还在（载体子元素 1 个 = 内容；再为滚动条 1 个）
  Element* scroller = root.find("scroller");
  ST_REQUIRE(scroller != nullptr);
  ST_CHECK(scroller->child_count() == 2);          // 内容 + 滚动条
  ST_CHECK(scroller->content_child_count() == 1);  // 内容只算 1
}

// ── `spacer()` 默认是**弹性空隙**（不是 0 宽固定块）────────────────────
//
// 回归：`spacer()` 是右对齐的惯用写法（`… 左侧内容 … spacer() … 右侧内容 …`），
// 而早先它把 `size` 直接写进 `width/height`——默认 `size = 0` 就得到一个
// **固定 0 宽的块**，在布局里等同于“不存在”，右对齐静默失效。
// 实测：codeeditor 标题栏的 — □ × 紧跟在标题文字后面（x=209），而非贴右缘（1268）。
// 现在 `size <= 0` ⇒ `grow = true`（真弹性）；`size > 0` 仍为固定块。
ST_TEST(dsl_spacer_default_is_elastic_and_pushes_to_edges) {
  struct SpacerPage : Component {
    void build(Composer& c) override {
      row(c, {.width = 400.0f, .height = 40.0f, .id = "bar"}, [&] {
        text(c, [] { return std::string("左"); }, {.key = "left"});
        (void)spacer(c);   // 默认参数：必须弹性
        text(c, [] { return std::string("右"); }, {.key = "right"});
      });
    }
  };
  UiRoot root;
  root.set_viewport({400.0f, 100.0f});
  auto page = std::make_shared<SpacerPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  Element* bar = root.find("bar");
  ST_REQUIRE(bar != nullptr);
  ST_REQUIRE(bar->child_count() == 3);
  const auto left = bar->child_at(0)->bounds();
  const auto gap = bar->child_at(1)->bounds();
  const auto right = bar->child_at(2)->bounds();

  // ① 空隙真的把两侧推开了：宽度远大于 0
  ST_CHECK(gap.width > 100.0f);
  // ② 右侧内容**贴右缘**（这正是 `spacer()` 存在的意义；0 宽固定块这里会失败）
  ST_CHECK(gap.right() <= right.x + 0.5f);
  ST_CHECK_NEAR(right.right(), bar->bounds().right(), 0.5f);
  // ③ 左侧内容仍在左端
  ST_CHECK_NEAR(left.x, bar->bounds().x, 0.5f);
}

// `size > 0` 仍然是固定尺寸的占位（旧语义保留）。
ST_TEST(dsl_spacer_positive_size_is_fixed) {
  struct FixedPage : Component {
    void build(Composer& c) override {
      row(c, {.width = 400.0f, .height = 40.0f, .id = "bar"}, [&] {
        (void)spacer(c, 24.0f);
        text(c, [] { return std::string("后"); }, {.key = "after"});
      });
    }
  };
  UiRoot root;
  root.set_viewport({400.0f, 100.0f});
  auto page = std::make_shared<FixedPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();
  Element* bar = root.find("bar");
  ST_REQUIRE(bar != nullptr);
  ST_REQUIRE(bar->child_count() == 2);
  ST_CHECK_NEAR(bar->child_at(0)->bounds().width, 24.0f, 0.5f);
  // 固定块不弹性 → 后续内容紧跟其后，不贴右缘
  ST_CHECK(bar->child_at(1)->bounds().x < bar->bounds().right() - 50.0f);
}

}  // namespace
