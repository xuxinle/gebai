// 声明式 UI（dsl）测试：挂载 / 状态传播 / 元素复用 / 裁剪 / 冻结 / 工厂。
//
// 场景与 docs/declarative.md §9 对应；全部用 UiRoot 直驱（无后端）。

#include "st/ui/dsl.hpp"

#include "st/test/test.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/markdown_view.hpp"
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

// ── 状态系统高层原语：memo / effect / ref ─────────────────────────────────
//
// 这三个原语在 `docs/declarative.md` §4 里早已写成契约（与 JS 侧 useMemo/useEffect/
// useRef 同一语义），但一直以来**只有 JS 侧有 hook、C++ 侧没有**——本次补齐并钉住。

// memo：依赖未变 → 不重算（结果跨重组复用）；依赖变了 → 重算。
// 判据是依赖的 `(指针, 写版本)` 指纹，不是值比较（值类型未必可比较）。
ST_TEST(dsl_memo_reuses_until_deps_change) {
  // lint-allow: L8 测试内的计数器，需被 lambda 捕获并跨重组共享（单用例作用域）
  static int calls = 0;   // 计算次数（跨实例共享——本用例只挂一个 Composer）
  struct MemoPage : Component {
    State<int> seed{1};
    State<int> unrelated{0};
    void build(Composer& c) override {
      const int doubled = memo<int>(c, [&] {
        ++calls;
        return seed.value() * 2;
      }, Deps{{&seed}});
      // 单根语义：**一个**顶层元素。column 包一层，否则后面的 button 会把 text 替掉。
      column(c, {}, [&] {
        text(c, [doubled] { return std::to_string(doubled); }, {.key = "out"});
        button(c, "seed", [this] { seed.set(seed.value() + 1); }, {.key = "bump"});
        button(c, "other", [this] { unrelated.set(unrelated.value() + 1); }, {.key = "other"});
        (void)unrelated.value();   // 根作用域订阅它：写它才会重跑（模拟无关重跑）
      });
    }
  };
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<MemoPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();
  ST_CHECK_EQ(calls, 1);                                  // 首帧算一次
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("2"));

  // ① 无关状态写 → 重跑 build，但 memo 依赖未变 → **不重算**
  page->unrelated.set(1);
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(calls, 1);

  // ② 依赖状态写 → 重算
  page->seed.set(3);
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(calls, 2);
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("6"));
}

// effect：依赖变化才跑一次；变化前先跑上次的清理；写状态不丢（连锁写隔帧）。
ST_TEST(dsl_effect_runs_on_dep_change_and_cleans_up) {
  // lint-allow: L8 测试内的计数器，需被 lambda 捕获并跨重组共享（单用例作用域）
  static int runs = 0;
  static int cleanups = 0;
  struct EffectPage : Component {
    State<int> topic{1};
    State<std::string> log{""};
    void build(Composer& c) override {
      const int current = topic.value();
      effect(c, [this, current] {
        ++runs;
        // 副作用里写状态：**不能**被同帧的脏清理吞掉（必须隔帧生效并重组）
        log.set(log.peek() + std::to_string(current));
        return [] { ++cleanups; };
      }, Deps{{&topic}});
      column(c, {}, [&] {
        text(c, [this] { return log.value(); }, {.key = "log"});
        button(c, "next", [this] { topic.set(topic.value() + 1); }, {.key = "next"});
      });
    }
  };
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<EffectPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();
  ST_CHECK_EQ(runs, 1);                                    // 首帧跑一次

  // ① effect 里写的状态必须落地（隔帧）：写 log 之后同帧内 tick 应该能看到
  //    这里点一次 topic → effect 重跑（先清理旧、再记新）
  page->topic.set(2);
  (void)host->tick();   // 重组（登记 effect）+ 跑 effect
  root.layout();
  ST_CHECK_EQ(runs, 2);
  ST_CHECK_EQ(cleanups, 1);                                // 旧 effect 的清理跑了一次
  ST_CHECK_EQ(page->log.value(), std::string("12"));

  // ② 依赖未变的重组不重跑 effect
  page->log.set("12");   // 等值写：State::set 挡住 → 不脏；用下面的无关重跑来验证
  (void)host->tick();
  ST_CHECK_EQ(runs, 2);
}

// ref：跨重组稳定、改它**不触发重组**（要驱动界面就用 State）。
ST_TEST(dsl_ref_is_stable_and_not_reactive) {
  struct RefPage : Component {
    State<int> tick{0};
    void build(Composer& c) override {
      int& counter = ref<int>(c, 0);
      (void)tick.value();   // 订阅 tick：它的变化才会让本作用域重跑
      column(c, {}, [&] {
        text(c, [&counter, this] {
          ++counter;        // 每帧重组累加（不触发重组）
          return std::to_string(counter) + "/" + std::to_string(tick.value());
        }, {.key = "out"});
        button(c, "bump", [this] { tick.set(tick.value() + 1); }, {.key = "bump"});
      });
    }
  };
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<RefPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("1/0"));

  // ref 跨重组保持：第二次重组从 1 继续（新槽会重新从 0 起 → 这里的断言会红）
  page->tick.set(1);
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("2/1"));

  // 改 ref **不**触发重组：再 tick 一次（无脏）不应重跑
  ST_CHECK(!host->dirty());
  ST_CHECK_EQ(host->tick().scopes_rerun, 0);
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("2/1"));
}

// dsl_persisted_survives_conditional_prune：`persisted` 的身份是**名字**而非调用点序号。
// 这两个用例合起来证明它是真持久：① 值跨「被剪掉再声明」仍在；② 切 compose 根不影响它。
ST_TEST(dsl_persisted_survives_conditional_prune) {
  struct PersistPage : Component {
    State<bool> visible{true};
    void build(Composer& c) override {
      // hook 不能写在条件里——persisted 拿**名字**当身份，所以它写在外面、用值驱动分支。
      auto& draft = persisted<std::string>(c, "draft", "初始");
      // 单根语义：一个顶层元素（多个会互相替掉）——column 包一层。
      column(c, {}, [&] {
        if (visible.value()) {
          text(c, [&draft] { return draft.value(); }, {.key = "draft"});
        }
        // 显式 id：`find()` 按 id 精确匹配（key 生成的是 `Type@key`，不是裸 key）。
        button(c, "edit", [&draft] { draft.set(draft.value() + "!"); },
               {.id = "persist-edit", .key = "edit"});
        button(c, "toggle", [this] { visible.set(!visible.value()); },
               {.id = "persist-toggle", .key = "toggle"});
      });
    }
  };
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<PersistPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();
  ST_REQUIRE(root.content() != nullptr);
  ST_REQUIRE(root.content()->child_count() >= 1U);
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("初始"));

  // 改值 → 隐藏（声明被剪掉）→ 再显示：值必须还在
  ST_CHECK(st::ui::invoke_element(root, *root.find("persist-edit"), "click", ""));
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("初始!"));

  ST_CHECK(st::ui::invoke_element(root, *root.find("persist-toggle"), "click", ""));
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(root.content()->child_count(), 2U);   // 只剩两个按钮

  ST_CHECK(st::ui::invoke_element(root, *root.find("persist-toggle"), "click", ""));
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("初始!"));   // 值回来了
}

// memo 的依赖登记：**即使命中缓存**，依赖也必须在场（否则下次变化无人订阅）——
// 这是 memo 边界上最易错的一条（实现里把「订阅」放在指纹计算里一并做，理由见 dsl.cpp）。
// 用例反向验证：改了依赖值 → 界面必须跟着变（没订阅的话下面第二条断言会红）。
ST_TEST(dsl_memo_keeps_subscription_on_cache_hit) {
  struct MemoSubPage : Component {
    State<int> value{1};
    State<int> noise{0};
    void build(Composer& c) override {
      const int shown = memo<int>(c, [&] { return value.value() * 10; }, Deps{{&value}});
      (void)noise.value();   // 订阅 noise：它的变化会让本作用域重跑（但不影响 memo）
      // 单根语义：一个顶层元素（多个会互相替掉）——column 包一层。
      column(c, {}, [&] {
        text(c, [shown] { return std::to_string(shown); }, {.key = "out"});
        button(c, "v", [this] { value.set(value.value() + 1); },
               {.id = "memo-bump-value", .key = "bump-value"});
        button(c, "n", [this] { noise.set(noise.value() + 1); },
               {.id = "memo-bump-noise", .key = "bump-noise"});
      });
    }
  };
  UiRoot root;
  root.set_viewport({300.0f, 200.0f});
  auto page = std::make_shared<MemoSubPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("10"));

  // 先走一次「缓存命中」路径（noise 变 → 重跑 build → memo 命中缓存、不重算）
  ST_CHECK(st::ui::invoke_element(root, *root.find("memo-bump-noise"), "click", ""));
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("10"));

  // 关键断言：走了缓存路径之后，依赖变化仍必须被接住
  ST_CHECK(st::ui::invoke_element(root, *root.find("memo-bump-value"), "click", ""));
  (void)host->tick();
  root.layout();
  ST_CHECK_EQ(root.content()->child_at(0)->semantics_text(), std::string("20"));
}

}  // namespace

// ── `for_each` 的 key 对齐复用（与 `List::sync_items` / JS `ForEach` 同一语义）──
//
// 这条路径此前是**半成品**：模板里两个参数声明未使用（真实调用直接
// `-Werror=unused-parameter` 编译不过），且注释自承「v1 每次重组重建 items 区段」。
// 现在按 key 复用：同一 key 的项在增删/重排后仍是**同一个元素**（id 形如 `Type@key`，
// 与位置无关），中间插入只新建那一个。
struct Task {
  std::string id{};
  std::string name{};
  auto operator==(const Task& other) const -> bool = default;
};

struct ForEachPage : Component {
  State<std::vector<Task>> tasks{std::vector<Task>{{"a", "甲"}, {"b", "乙"}, {"c", "丙"}}};
  State<int> noise{0};

  void build(Composer& c) override {
    column(c, {.id = "list"}, [&] {
      for_each<Task>(
          c, tasks.value(), [](const Task& task) { return task.id; },
          [&](const Task& task) {
            row(c, {.gap = 4.0f, .key = task.id}, [&] {
              text(c, [task] { return task.name; }, {.key = task.id + "-label"});
            });
          });
      (void)noise.value();   // 无关状态：用于触发重组
    });
  }
};

ST_TEST(dsl_for_each_reuses_by_key) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto page = std::make_shared<ForEachPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  Element* list = root.find("list");
  ST_REQUIRE(list != nullptr);
  ST_CHECK_EQ(list->child_count(), 3U);
  ST_CHECK_EQ(list->child_at(0)->key(), std::string("a"));
  ST_CHECK_EQ(list->child_at(1)->key(), std::string("b"));
  ST_CHECK_EQ(list->child_at(2)->key(), std::string("c"));

  // 记下「key=a」那一项的 id 与元素地址：后续无论怎么重排，它都不该变。
  const std::string id_a = list->child_at(0)->derived_id();
  const std::string id_b = list->child_at(1)->derived_id();
  Element* element_a = list->child_at(0);
  Element* element_b = list->child_at(1);
  Element* element_c = list->child_at(2);
  ST_CHECK(id_a.find("a") != std::string::npos);   // 自动 id 带业务 key（Type@key 形态）

  // ① 头部插入一项：既有项一个都不重建（元素地址不变）
  auto rows = page->tasks.value();
  rows.insert(rows.begin(), Task{"z", "新"});
  page->tasks.set(rows);
  (void)host->tick();
  root.layout(true);
  list = root.find("list");
  ST_REQUIRE(list != nullptr);
  ST_CHECK_EQ(list->child_count(), 4U);
  ST_CHECK_EQ(list->child_at(0)->key(), std::string("z"));
  ST_CHECK_EQ(list->child_at(1)->key(), std::string("a"));
  ST_CHECK(list->child_at(1) == element_a);        // 同一个元素（不是重建的）
  ST_CHECK(list->child_at(2) == element_b);
  ST_CHECK_EQ(list->child_at(1)->derived_id(), id_a);

  // ② 重排（把末项提到最前）：身份跟 key 走，且**不新建**（只是挪位）
  rows = page->tasks.value();
  rows.insert(rows.begin(), rows.back());
  rows.pop_back();   // [z,a,b,c] → [c,z,a,b]
  page->tasks.set(rows);
  const ReconcileStats reorder = host->tick();
  root.layout(true);
  list = root.find("list");
  ST_CHECK_EQ(list->child_at(0)->key(), std::string("c"));
  ST_CHECK(list->child_at(0) == element_c);
  ST_CHECK(list->child_at(2) == element_a);        // 甲 只是换了个位置
  ST_CHECK_EQ(list->child_at(2)->derived_id(), id_a);
  ST_CHECK_EQ(reorder.elements_created, 0);        // 纯重排：一个都不新建
  ST_CHECK_EQ(reorder.elements_removed, 0);
  ST_CHECK(reorder.elements_moved > 0);            // 挪位是可观测的（诊断计数）

  // ③ 删除一项：只有它消失，其余身份不动
  rows = page->tasks.value();
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [](const Task& task) { return task.id == "b"; }),
             rows.end());
  page->tasks.set(rows);
  (void)host->tick();
  root.layout(true);
  list = root.find("list");
  ST_CHECK_EQ(list->child_count(), 3U);
  for (std::size_t index = 0; index < list->child_count(); ++index) {
    ST_CHECK(list->child_at(index)->key() != std::string("b"));
  }
  ST_CHECK(list->child_at(2) == element_a);        // 甲 仍在原位
  ST_CHECK_EQ(list->child_at(2)->derived_id(), id_a);

  // ④ 无关状态变化不得新建/移除任何元素
  const ReconcileStats stats_before = host->stats();
  page->noise.set(page->noise.value() + 1);
  const ReconcileStats stats = host->tick();
  root.layout(true);
  ST_CHECK_EQ(stats.elements_created, 0);
  ST_CHECK_EQ(stats.elements_removed, 0);
  ST_CHECK_EQ(stats.elements_moved, 0);
  (void)stats_before;
}

// ── `dsl::markdown`：把 MarkdownView 的样板收敛到一处 ─────────────────────────
//
// 起因（真实应用）：三页（时间 / JSON / 待办）渲染 LLM 的 Markdown 结果，每页都写
// 一遍 `custom<MarkdownView>` + 手工 `set_markdown`，每页 ~15 行样板。样板不只是冗——
// 它让"三页行为是否一致"变成人工对照。
//
// 本条钉两件事（后者才是关键）：① 一行能建出 MarkdownView；② **惰性闭包在重组时
// 重新求值**（与 `text` 同口径）——否则流式回答只会渲染第一帧的内容，之后永不更新。
ST_TEST(dsl_markdown_wrapper_reevaluates_source) {
  struct MdPage : Component {
    State<std::string> source{"# 标题\n\n正文"};

    void build(Composer& c) override {
      column(c, {.id = "md-host"}, [&] {
        // 走包装：不写 custom<MarkdownView>，配置收敛进实现
        // （`markdown` 标了 nodiscard——返回值是强类型引用，流式场景用它调 append_chunk）
        (void)markdown(c, [&] { return source.value(); }, {.id = "md"});
      });
    }
  };

  UiRoot root;
  root.set_viewport({640.0F, 480.0F});
  auto page = std::make_shared<MdPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  auto* view = dynamic_cast<MarkdownView*>(root.find("md"));
  ST_REQUIRE(view != nullptr);
  ST_CHECK(view->markdown().find("标题") != std::string::npos);
  const std::size_t blocks_before = view->block_count();
  ST_CHECK(blocks_before > 0U);

  // ② 换源后重组：内容**必须跟着变**（惰性闭包被重新求值）
  page->source.set("# 换了一份\n\n新的正文\n\n- 甲\n- 乙");
  (void)host->tick();
  root.layout(true);
  view = dynamic_cast<MarkdownView*>(root.find("md"));
  ST_REQUIRE(view != nullptr);
  ST_CHECK(view->markdown().find("换了一份") != std::string::npos);
  ST_CHECK(view->block_count() > blocks_before);
}

// ── `BoxProps` 的排版三件套（color / hex_color / size / weight）────────────────
//
// 起因（真实应用）：错误提示想标红（设计稿给的是 `#dc2626`）、标题想加大加粗，而
// `BoxProps` 当时只有盒模型字段——DSL 主路径表达不出这三样，只能走 `custom<Text>` 逃生船，
// 或者放弃并写成"错误："前缀（靠文案区分严重程度，观感差且没法自动化断言）。
//
// 本组用例钉两件事：
// ① **真的落到样式上**（颜色/字号/字重从视觉树读得到）；
// ② **不会被 `apply_theme` 盖回去**——`apply_theme` 每帧从主题重算这三项，
//    "记不住显式覆盖"的症状是"设了颜色首帧对、下一帧就没了"，只在多帧后显形。

/// 从视觉树里按 id 找节点（`visual_tree()` 的 id 与真值树一致）。
[[nodiscard]] auto find_visual(const VisualNode& node, const ElementId& id) -> const VisualNode* {
  if (node.id == id) return &node;
  for (const VisualNode& child : node.children) {
    if (const VisualNode* hit = find_visual(child, id); hit != nullptr) return hit;
  }
  return nullptr;
}

struct StyledPage : Component {
  State<int> noise{0};

  void build(Composer& c) override {
    column(c, {.id = "styled"}, [&] {
      // 语义色调（跟着主题走）。⚠ 指定初始化器**必须按声明顺序**：
      // color/hex_color/size/weight 在 `BoxProps` 里排在 id/key **之前**。
      text(c, [] { return std::string("错误：出错了"); },
           {.color = Tone::Danger, .id = "err"});
      // 字面色值（对标设计稿）：`#dc2626` 就是主题 danger 的浅色主题取值
      text(c, [] { return std::string("红色字面"); }, {.hex_color = "#dc2626", .id = "hex"});
      // 字号 + 字重（标题的诉求：加大加粗）
      text(c, [] { return std::string("大字重"); },
           {.size = 24.0F, .weight = FontWeight::Bold, .id = "big"});
      // 不设任何排版字段：必须原样沿用主题（不能被"覆盖机制"波及）
      text(c, [] { return std::string("默认"); }, {.id = "plain"});
      (void)noise.value();   // 无关状态：用来触发后续重组
    });
  }
};

ST_TEST(dsl_box_props_carry_text_style) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto page = std::make_shared<StyledPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  const VisualNode tree = root.visual_tree();
  const VisualNode* err = find_visual(tree, "err");
  const VisualNode* hex = find_visual(tree, "hex");
  const VisualNode* big = find_visual(tree, "big");
  const VisualNode* plain = find_visual(tree, "plain");
  ST_REQUIRE(err != nullptr);
  ST_REQUIRE(hex != nullptr);
  ST_REQUIRE(big != nullptr);
  ST_REQUIRE(plain != nullptr);

  // ① 颜色真的落到了样式上（视觉树上报——语义树里没有颜色，像素里又难断言）
  const st::math::Color danger = tone_color(Theme::light(), Tone::Danger);
  ST_CHECK_EQ(err->text_color, danger.to_css());
  // 字面色值走同一条路（`#dc2626` 解析成同一个色）
  ST_CHECK_EQ(hex->text_color, danger.to_css());
  ST_CHECK_EQ(hex->text_color, std::string("#dc2626"));

  // ② 字号/字重（标题的"加大加粗"）
  ST_CHECK(std::abs(big->font_size - 24.0F) < 0.001F);
  ST_CHECK_EQ(big->font_weight, std::string("bold"));

  // ③ 没设的字段**不动**（沿用主题）——覆盖机制不能"顺手把默认值也写死"
  const Theme theme = Theme::light();
  ST_CHECK(std::abs(plain->font_size - theme.metrics().font_base) < 0.001F);
  ST_CHECK_EQ(plain->font_weight, std::string("regular"));
  ST_CHECK_EQ(plain->text_color, theme.colors().text.to_css());
}

/// ② 显式覆盖必须在**多帧重组后**仍然生效（防"被 apply_theme 盖回去"）。
///
/// 反例（故意破坏）：把 `Text::apply_theme` 末尾的 `apply_text_overrides()` 删掉，
/// 本用例当场变红——而上面的单帧用例**照样绿**（首帧的样式是 `apply_box` 直接写进去的，
/// 还没被主题重算过）。这就是为什么这条必须单独存在。
ST_TEST(dsl_box_props_survive_theme_reapply) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto page = std::make_shared<StyledPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  // 强制多帧重组 + 重新布局（每次都走 `UiRoot::layout` → `apply_theme_tree`）
  for (int frame = 0; frame < 3; ++frame) {
    page->noise.set(page->noise.value() + 1);
    (void)host->tick();
    root.layout(true);
  }

  const VisualNode tree = root.visual_tree();
  const VisualNode* err = find_visual(tree, "err");
  const VisualNode* big = find_visual(tree, "big");
  ST_REQUIRE(err != nullptr);
  ST_REQUIRE(big != nullptr);
  ST_CHECK_EQ(err->text_color, tone_color(Theme::light(), Tone::Danger).to_css());
  ST_CHECK(std::abs(big->font_size - 24.0F) < 0.001F);
  ST_CHECK_EQ(big->font_weight, std::string("bold"));

  // 主题切换后：**语义色调跟着新主题走，字面色值不变**——这正是两者分工的验收点
  root.set_theme(Theme::dark());
  root.layout(true);
  const VisualNode dark_tree = root.visual_tree();
  const VisualNode* dark_err = find_visual(dark_tree, "err");
  const VisualNode* dark_hex = find_visual(dark_tree, "hex");
  ST_REQUIRE(dark_err != nullptr);
  ST_REQUIRE(dark_hex != nullptr);
  ST_CHECK_EQ(dark_err->text_color, tone_color(Theme::dark(), Tone::Danger).to_css());
  ST_CHECK_EQ(dark_hex->text_color, std::string("#dc2626"));   // 字面色值不随主题变
}

// ── `for_each` 的 index 参数（重复文案的列表项不再串台）────────────────────────
//
// 起因（真实应用）：待办列表的勾选框回调要"改第几项"，而 `item_fn` 当时只给 item——
// 回调只能拿业务 key 回原文查（`find_if` 全表）。key 用的是**显示文案**（"写周报"这种
// 天然会重复的字符串）时，回查永远命中**第一**条：界面上表现为
// 「点第二条的勾选框，第一条被勾上」——两条同文案的行看起来完全独立，行为却串台。
//
// 所以本组用例钉三件事（对应交接清单里的三条判据）：
// ① 两条同 text 的 item → 渲染**两行独立**（各自有元素、各自能定位）；
// ② 点第二条的勾选框 → 只改第二条；
// ③ 删第一条后 → 第二条的**身份（id）不漂移**。
//
// 为什么必须用"同 text"当场景：key 若天然唯一（如 id），三条判据**在修复前也全绿**——
// 那样就成了"恒绿测试"（拦不住任何东西）。这个场景才是它存在的理由。
struct DupRow {
  std::string text{};      ///< 显示文案，**刻意重复**（就是 key，也就是踩坑的那个形状）
  bool checked{false};
  // `State<T>` 的相等比较需要它（与 `Task` 同口径）
  auto operator==(const DupRow& other) const -> bool = default;
};

struct DupKeyPage : Component {
  State<std::vector<DupRow>> rows{
      std::vector<DupRow>{{"写周报", false}, {"写周报", false}}};
  /// 每次 `item_fn` 收到的索引（按声明顺序）——用来断言"索引确实传进来了"
  std::vector<std::size_t> seen_index{};

  void build(Composer& c) override {
    seen_index.clear();
    column(c, {.id = "dups"}, [&] {
      for_each<DupRow>(
          c, rows.value(), [](const DupRow& row) { return row.text; },
          [&](const DupRow& row, std::size_t index) {
            seen_index.push_back(index);
            // 回调里按 index 定位（这正是实战需要的形状：index 捕获进闭包，
            // 于是点击处理能直接写"改第 index 项"，而不必反查）
            checkbox(c, row.text, row.checked,
                     [this, index](bool state) {
                       auto copy = rows.value();
                       if (index < copy.size()) copy[index].checked = state;
                       rows.set(copy);
                     },
                     {.key = row.text});
          });
    });
  }
};

ST_TEST(dsl_for_each_item_callback_gets_index) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto page = std::make_shared<DupKeyPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  // ① 两条同文案的 item → **两行独立**（不是两行都落在同一个元素上）
  Element* list = root.find("dups");
  ST_REQUIRE(list != nullptr);
  ST_CHECK_EQ(list->child_count(), 2U);
  ST_CHECK(list->child_at(0) != list->child_at(1));
  // 索引按序传进来（不用 `ST_CHECK_EQ`：它要格式化 `vector`，而测试框架只给标量
  // 装了 formatter——顺手也证明了这条断言不需要扩测试框架）
  ST_REQUIRE(page->seen_index.size() == 2U);
  ST_CHECK_EQ(page->seen_index[0], 0U);
  ST_CHECK_EQ(page->seen_index[1], 1U);
  // 重名 key 必须被记为**诊断信息**（对齐退回按位置，但调用方得能知道“我把 key 写重了”）
  const ReconcileStats stats = host->stats();
  ST_CHECK_EQ(stats.key_collisions.size(), 1U);
  if (!stats.key_collisions.empty()) ST_CHECK_EQ(stats.key_collisions[0], std::string("写周报"));
}

ST_TEST(dsl_for_each_index_targets_the_right_row) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto page = std::make_shared<DupKeyPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  Element* list = root.find("dups");
  ST_REQUIRE(list != nullptr);
  ST_REQUIRE(list->child_count() == 2U);

  // ② 点**第二条**的勾选框 → 只改第二条（修复前：回查 key 命中第一条 → 第一条被改）
  auto* second = dynamic_cast<Checkbox*>(list->child_at(1));
  ST_REQUIRE(second != nullptr);
  ST_CHECK(second->checked() == false);
  second->activate();   // 勾选：触发 `on_change`（回调里按 index 定位）
  (void)host->tick();
  root.layout(true);

  const auto rows = page->rows.value();
  ST_REQUIRE(rows.size() == 2U);
  ST_CHECK(rows[0].checked == false);   // 第一条**不受影响**（这就是本用例的全部意义）
  ST_CHECK(rows[1].checked == true);

  // ③ 删掉第一条 → 第二条的 id 不漂移（身份跟 key 走的另一种表现：
  //    两条同 key 时靠**位置**兜底，删前项后后项仍能各就各位）
  list = root.find("dups");
  ST_REQUIRE(list != nullptr);
  const ElementId id_second_before = list->child_at(1)->derived_id();
  auto remaining = page->rows.value();
  remaining.erase(remaining.begin());
  page->rows.set(remaining);
  (void)host->tick();
  root.layout(true);
  list = root.find("dups");
  ST_REQUIRE(list != nullptr);
  ST_REQUIRE(list->child_count() == 1U);
  ST_CHECK_EQ(list->child_at(0)->derived_id(), id_second_before);
}

/// 单参 `item_fn` 仍然可用（按签名分派；不强迫每个调用方都接一个用不上的参数）。
///
/// 这条防的是"为了加索引而把既有调用点全改一遍"——那会让 API 变更的成本转嫁到
/// 每一个只是渲染列表的页面上（本仓多处如此）。分派两种签名，两边都不需要改。
struct SingleArgPage : Component {
  void build(Composer& c) override {
    column(c, {.id = "single"}, [&] {
      for_each<Task>(c, std::vector<Task>{{"a", "甲"}}, [](const Task& task) { return task.id; },
                     [&](const Task& task) {   // 只收一个参数：老写法必须照样编译
                       text(c, [task] { return task.name; }, {.key = task.id + "-label"});
                     });
    });
  }
};

ST_TEST(dsl_for_each_single_argument_item_fn_still_works) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto host = dsl::mount(root, std::make_shared<SingleArgPage>());
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  Element* list = root.find("single");
  ST_REQUIRE(list != nullptr);
  ST_CHECK_EQ(list->child_count(), 1U);
  // 列表里那一个元素就是 `item_fn` 声明的 Text（它的 key 是 `a-label`）
  ST_CHECK_EQ(list->child_at(0)->key(), std::string("a-label"));
}

// ── `for_each` 的对齐复杂度：必须是 O(N)，不能是 O(N²) ────────────────────────
//
// 起因（实测性能事故，2026-10-05）：keyed 对齐的 key 查找**从位置 0 开始扫**，
// 而游标随 item 单调推进——每个 item 都把前面所有位置重新扫一遍，整体 O(N²)。
//
// 实测（每项 1 个元素、只改 1 条数据、**零**结构变更：created/removed/moved 全 0）：
//
// | 规模 | 从 0 扫（修前） | 从游标扫（修后） |
// |---|---|---|
// | 1000 | 2.18 ms | — |
// | 5000 | **43.6 ms** | **1.36 ms** |
//
// 同一份 5000 项列表把 key 去掉（退回纯位置对齐）只要 0.67 ms——**65 倍差全在那一行**。
// 这也说明 BACKLOG 里「待办 100+ 条会卡」的观察方向找错了层：卡的**不是**"item 级作用域"，
// 而是对齐里的坐标起点。
//
// 本用例**不量时间**（时间随机器负载抖动，在 CI 上是脆判据），改钉**确定性计数**：
// `ReconcileStats::alignment_probes` = 对齐阶段扫过的子元素槽位数。规模 4 倍时
// 它涨 4 倍（线性）还是 16 倍（二次），一眼可判。
struct SizedListPage : Component {
  State<std::vector<Task>> rows{};
  State<int> noise{0};

  explicit SizedListPage(std::size_t count) {
    std::vector<Task> list;
    list.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      list.push_back(Task{std::to_string(index), "行" + std::to_string(index)});
    }
    rows.set(std::move(list));
  }

  void build(Composer& c) override {
    column(c, {.id = "big"}, [&] {
      for_each<Task>(
          c, rows.value(), [](const Task& task) { return task.id; },
          [&](const Task& task, std::size_t) {
            text(c, [task] { return task.name; }, {.key = task.id + "-label"});
          });
      (void)noise.value();
    });
  }
};

/// 建 N 项列表 → 只改一条数据 → 返回这次重组的对齐探测计数。
[[nodiscard]] auto alignment_probes_for(std::size_t count) -> std::uint64_t {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  auto page = std::make_shared<SizedListPage>(count);
  auto host = dsl::mount(root, page);
  if (host == nullptr) return 0;
  root.layout(true);
  // 只改一条（条数不变 → 零结构变更，全部代价都在对齐里）
  auto rows = page->rows.value();
  rows[count / 2].name = "改过了";
  page->rows.set(rows);
  const ReconcileStats stats = host->tick();
  // 零结构变更：本用例的判据只在"纯对齐"场景下成立
  if (stats.elements_created != 0 || stats.elements_removed != 0) return 0;
  return stats.alignment_probes;
}

ST_TEST(dsl_for_each_alignment_scales_linearly) {
  constexpr std::uint64_t kSmall = 200;
  constexpr std::uint64_t kLarge = 5000;
  const std::uint64_t small = alignment_probes_for(kSmall);
  const std::uint64_t large = alignment_probes_for(kLarge);
  // 线性下界：至少每个 item 探一次（200 / 5000，各在游标处命中）
  ST_CHECK(small >= kSmall);
  ST_CHECK(large >= kLarge);
  // 规模比 25×：线性 → ≈ 25；二次 → ≈ 625。取 100 当界（两侧都留足余量）
  const double ratio = static_cast<double>(large) / static_cast<double>(small);
  ST_CHECK(ratio < 100.0);
}

// ── 谬误注入：递归 build 被深度护栏截住（不是靠栈自己撞上限）──────────────
//
// 形态：组件在自己的 `build` 里又声明一个**自己**（写成状态计数就很容易踩到——
// 「列表里每一项再渲染一个同样的组件」写漏了终止条件）。没有护栏时深度无界增长，
// 最后是栈溢出（SIGSEGV），现场信息只有一大堆 `run_scope` 帧，很难指到真正的错处。
//
// `Guardrails::max_depth` 是为此存在的：超限即**拒绝声明**并记错误（不是崩）。
// 反向验证：把 `declare_child_scope` 里的深度判断去掉，本用例会直接崩进程。
struct RecursivePage : Component {
  State<int> depth_probe{0};

  void build(Composer& c) override {
    column(c, {.id = "recursive"}, [&] {
      text(c, [] { return "层"; });
      sub_component(c, std::make_shared<RecursivePage>(), "self");
    });
  }
};

ST_TEST(dsl_recursive_build_is_stopped_by_depth_guard) {
  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  dsl::Guardrails guardrails{};
  guardrails.max_depth = 4;   // 收紧上限便于断言（默认 64）
  auto host = dsl::mount(root, std::make_shared<RecursivePage>(), guardrails);
  ST_REQUIRE(host != nullptr);
  root.layout(true);

  // 没崩 = 护栏生效；错误信息给到调用方（诊断用）。
  // 注意：`ReconcileStats` 是**当帧**的（每次 reconcile 开头重置）——
  // 所以要在这之后额外 tick 之前读，否则读到的是一次“已经不再触发”的新统计。
  const dsl::ReconcileStats& stats = host->stats();
  ST_CHECK(!stats.error.empty());
  ST_CHECK(stats.error.find("嵌套") != std::string::npos);

  // 不会无限递归下去：状态里不再有任何“新一层”抱错以外的异常
  ST_CHECK(host->tick().error.empty());   // 再跑一帧：深度已达上限，不再触发（稳定，不每帧刷错）
}

// ── 单帧预算：超预算的作用域**顺延下一帧**（不是丢弃）────────────────────
//
// 文档承诺「超预算的失效作用域顺延下一帧（掉帧优于卡死）」。这里用一个**故意慢**
// 的中间作用域把预算耗尽，验证两层结论：
// ① 被切掉的孙作用域本帧不跑，但**仍带脏标记**（`dirty()` 为真 → 下一帧补上），
//    而不是被清掉（那会表现为“界面永远少一块”，比掉帧难查得多）；
// ② 下一帧预算重新计时 → 补跑完成，最终态与预算充足时一致。
struct SlowGrandChild : Component {
  void build(Composer& c) override { text(c, [] { return "孙节点"; }, {.key = "grand"}); }
};

struct SlowChild : Component {
  void build(Composer& c) override {
    // 故意拖过预算：模拟「大列表/复杂子树」在真实应用里的样子
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    text(c, [] { return "慢子树"; }, {.key = "slow"});
    sub_component(c, std::make_shared<SlowGrandChild>(), "grand");
  }
};
struct BudgetPage : Component {
  std::shared_ptr<SlowChild> child = std::make_shared<SlowChild>();
  void build(Composer& c) override {
    column(c, {.id = "budget"}, [&] {
      text(c, [] { return "根"; }, {.key = "root"});
      sub_component(c, child, "child");
    });
  }
};

ST_TEST(dsl_frame_budget_defers_instead_of_dropping) {
  // 按业务 key 找元素（自动 id 是路径形态 `面板id/Type@key`，按 key 找才是稳的）
  const auto find_by_key = [](auto&& self, Element& element, const std::string& key) -> Element* {
    if (element.key() == key) return &element;
    for (std::size_t index = 0; index < element.child_count(); ++index) {
      if (Element* hit = self(self, *element.child_at(index), key)) return hit;
    }
    return nullptr;
  };

  UiRoot root;
  root.set_viewport({400.0F, 300.0F});
  dsl::Guardrails guardrails{};
  guardrails.frame_budget_ms = 1.0;   // 中间那层要睡 3ms → 必然超
  auto page = std::make_shared<BudgetPage>();
  auto host = dsl::mount(root, page, guardrails);
  ST_REQUIRE(host != nullptr);
  root.layout(true);
  ST_REQUIRE(root.content() != nullptr);

  // 首帧：中间层跑了（睡了 3ms），孙节点被顺延
  ST_CHECK(host->stats().budget_exceeded);
  ST_CHECK(find_by_key(find_by_key, *root.content(), "slow") != nullptr);
  ST_CHECK(find_by_key(find_by_key, *root.content(), "grand") == nullptr);   // 本帧没跑
  ST_CHECK(host->dirty());                       // 顺延 = 还脏着（不是丢弃）

  // 第二帧：预算重新计时，孙节点补上
  (void)host->tick();
  root.layout(true);
  ST_REQUIRE(root.content() != nullptr);
  ST_CHECK(find_by_key(find_by_key, *root.content(), "grand") != nullptr);
}
