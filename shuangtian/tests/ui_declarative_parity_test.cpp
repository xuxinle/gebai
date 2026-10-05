// 双宿主一致性 fixture：同一场景（docs/declarative.md 不变式 4）——
// C++（st::ui::dsl）与 JS（DeclarativeHost）各建一遍，产出树的结构/文本必须一致。

#include <memory>
#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/declarative_host.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/ui_root.hpp"

#include "st/ext/json.hpp"
#include "tests/support/text_port_fixtures.hpp"

// 测试在匿名命名空间内，`st::test::X` 得写全；用具名别名让用例读起来干净。
using st::test::FixedAdvanceTextPort;

namespace {


/// 结构性签名：type + 语义文本（不含几何与 id 细节——双宿主的 id 生成路径不同，
/// 语义面才是"同一界面"的判据）。
[[nodiscard]] auto signature(const st::ui::Element& element) -> std::string {
  std::string out = std::string(element.type());
  const std::string text = element.semantics_text();
  if (!text.empty()) {
    out += "[";
    out += text;
    out += "]";
  }
  out += "{";
  for (std::size_t index = 0; index < element.child_count(); ++index) {
    out += signature(*element.child_at(index));
    if (index + 1 < element.child_count()) out += ",";
  }
  out += "}";
  return out;
}

/// C++ 宿主场景：计数 + 条件分支。
struct CppCounter : st::ui::dsl::Component {
  st::ui::dsl::State<int> count{0};
  st::ui::dsl::State<bool> extra{false};

  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;
    column(c, {.gap = 12.0F, .padding = 24.0F}, [&] {
      text(c, [] { return std::string("霜天声明式"); });
      text(c, [&] { return std::format("点击了 {} 次", count.value()); });
      button(c, "+1", [this] { count.set(count.value() + 1); });
      if (extra.value()) {
        text(c, [] { return std::string("额外内容"); });
      }
    });
  }
};

/// JS 宿主场景：同一界面（compose 写法）。
constexpr std::string_view kJsCounter = R"JS(
let count = null;
let extra = null;
compose('Fixture', () => {
  if (count === null) count = useState(0);
  if (extra === null) extra = useState(false);
  const kids = [
    text('霜天声明式'),
    text(() => '点击了 ' + count.value + ' 次'),
    button('+1', () => { count.value = count.value + 1; }),
  ];
  if (extra.value) kids.push(text('额外内容'));
  return column({gap: 12, padding: 24}, kids);
});
)JS";

}  // namespace

ST_TEST(dual_host_mount_signature_matches) {
  FixedAdvanceTextPort port;
  // —— C++ 宿主 ——
  st::ui::UiRoot cpp_root;
  cpp_root.set_text_port(&port);
  cpp_root.set_viewport({600.0F, 400.0F});
  auto cpp_host = st::ui::dsl::mount(cpp_root, std::make_shared<CppCounter>());
  ST_REQUIRE(cpp_host != nullptr);
  cpp_root.layout(true);
  const std::string cpp_sig = signature(*cpp_root.content());

  // —— JS 宿主 ——
  st::ui::UiRoot js_root;
  js_root.set_text_port(&port);
  js_root.set_viewport({600.0F, 400.0F});
  js_root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  js_root.layout(true);
  auto script = std::make_unique<st::ui::ScriptHost>(js_root);
  auto decl = st::ui::DeclarativeHost::attach(*script, js_root);
  ST_REQUIRE(decl != nullptr);
  auto status = decl->run(std::string(kJsCounter), "<fixture>");
  ST_REQUIRE(status.has_value());
  js_root.layout(true);
  const std::string js_sig = signature(*js_root.content());

  ST_CHECK_EQ(js_sig, cpp_sig);
}

ST_TEST(dual_host_state_step_signature_matches) {
  FixedAdvanceTextPort port;
  // C++：点 3 次 + 展开
  st::ui::UiRoot cpp_root;
  cpp_root.set_text_port(&port);
  cpp_root.set_viewport({600.0F, 400.0F});
  auto page = std::make_shared<CppCounter>();
  auto cpp_host = st::ui::dsl::mount(cpp_root, page);
  ST_REQUIRE(cpp_host != nullptr);
  cpp_root.layout(true);
  for (int index = 0; index < 3; ++index) {
    st::ui::Element* button = cpp_root.content()->child_at(2);
    (void)st::ui::invoke_element(cpp_root, *button, "click", "");
    (void)cpp_host->tick();
  }
  page->extra.set(true);
  (void)cpp_host->tick();
  cpp_root.layout(true);
  const std::string cpp_sig = signature(*cpp_root.content());

  // JS：点 3 次 + 展开
  st::ui::UiRoot js_root;
  js_root.set_text_port(&port);
  js_root.set_viewport({600.0F, 400.0F});
  js_root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  js_root.layout(true);
  auto script = std::make_unique<st::ui::ScriptHost>(js_root);
  auto decl = st::ui::DeclarativeHost::attach(*script, js_root);
  ST_REQUIRE(decl != nullptr);
  // 展开由 JS 侧状态控制——加一个开关按钮让两侧都能触发（此处直接操 JS 状态）
  ST_REQUIRE(decl->run(std::string(kJsCounter), "<fixture>").has_value());
  js_root.layout(true);
  for (int index = 0; index < 3; ++index) {
    st::ui::Element* button = js_root.content()->child_at(2);
    (void)st::ui::invoke_element(js_root, *button, "click", "");
    (void)decl->tick();
  }
  // JS 侧展开：直接写状态（等价于 extra.value = true）
  ST_REQUIRE(script->eval("extra.value = true", "<fixture-extra>").has_value());
  (void)decl->tick();
  js_root.layout(true);
  const std::string js_sig = signature(*js_root.content());

  ST_CHECK_EQ(js_sig, cpp_sig);
}

// ── 一致性扩展：列表（key 复用）、条件裁剪、片段摊平 ─────────────────────
//
// 前两个用例只覆盖「静态结构 + 状态推进」。这里补上**最容易两宿主走偏**的三处：
// ① 数据列表（`for_each` / `forEach`）的 key 语义——同一 key 的项在增删/重排后
//    必须还是同一个元素（两宿主都要满足，且结构签名一致）；
// ② 条件分支收起后的裁剪（不留残）；
// ③ 片段摊平（`forEach` 结果塞进 kids 数组——JS 侧曾因此静默少一块）。
namespace {

struct ParityRow {
  std::string id{};
  std::string name{};
  auto operator==(const ParityRow& other) const -> bool = default;
};

/// C++ 侧：列表 + 条件内容。
struct CppList : st::ui::dsl::Component {
  st::ui::dsl::State<std::vector<ParityRow>> rows{
      std::vector<ParityRow>{{"a", "甲"}, {"b", "乙"}, {"c", "丙"}}};
  st::ui::dsl::State<bool> extra{false};

  void build(st::ui::dsl::Composer& c) override {
    using namespace st::ui::dsl;
    column(c, {.gap = 2.0F}, [&] {
      for_each<ParityRow>(
          c, rows.value(), [](const ParityRow& row) { return row.id; },
          [&](const ParityRow& row) { text(c, [row] { return row.name; }, {.key = row.id}); });
      if (extra.value()) text(c, [] { return std::string("额外"); }, {.key = "extra"});
    });
  }
};

/// JS 侧：同一界面（compose 风格 + 小写 forEach 塞进 kids 数组）。
constexpr std::string_view kJsList = R"JS(
let rows = null;
let extra = null;
compose('ParityList', () => {
  if (rows === null) rows = useState([{id:'a',n:'甲'},{id:'b',n:'乙'},{id:'c',n:'丙'}]);
  if (extra === null) extra = useState(false);
  // 小写 API 返回**裸 VNode**（不是链对象）——key 经 props 传。
  // 不额外套一层容器：`forEach` 的结果与后续条件内容必须与 C++ 侧同层级。
  const kids = forEach(rows.value, (it) => it.id, (it) => text(it.n, {key: it.id}));
  if (extra.value) kids.push(text('额外', {key: 'extra'}));
  return column({gap: 2}, kids);
});
)JS";

}  // namespace

ST_TEST(dual_host_list_and_condition_signature_matches) {
  FixedAdvanceTextPort port;
  // —— C++ ——
  st::ui::UiRoot cpp_root;
  cpp_root.set_text_port(&port);
  cpp_root.set_viewport({600.0F, 400.0F});
  auto page = std::make_shared<CppList>();
  auto cpp_host = st::ui::dsl::mount(cpp_root, page);
  ST_REQUIRE(cpp_host != nullptr);
  cpp_root.layout(true);
  const std::string cpp_first = signature(*cpp_root.content());

  // —— JS ——
  st::ui::UiRoot js_root;
  js_root.set_text_port(&port);
  js_root.set_viewport({600.0F, 400.0F});
  js_root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  js_root.layout(true);
  auto script = std::make_unique<st::ui::ScriptHost>(js_root);
  auto decl = st::ui::DeclarativeHost::attach(*script, js_root);
  ST_REQUIRE(decl != nullptr);
  ST_REQUIRE(decl->run(std::string(kJsList), "<parity-list>").has_value());
  js_root.layout(true);
  const std::string js_first = signature(*js_root.content());
  ST_CHECK_EQ(js_first, cpp_first);

  // —— 同一步骤：删除中间项 + 展开条件内容 ——
  auto rows = page->rows.value();
  rows.erase(rows.begin() + 1);
  page->rows.set(rows);
  page->extra.set(true);
  (void)cpp_host->tick();
  cpp_root.layout(true);

  ST_REQUIRE(script->eval("rows.value = [{id:'a',n:'甲'},{id:'c',n:'丙'}]; extra.value = true",
                          "<step>").has_value());
  (void)decl->tick();
  js_root.layout(true);

  const std::string cpp_second = signature(*cpp_root.content());
  const std::string js_second = signature(*js_root.content());
  ST_CHECK_EQ(js_second, cpp_second);
  // 结构也确实是「甲 + 丙 + 额外」
  ST_CHECK(cpp_second.find("甲") != std::string::npos);
  ST_CHECK(cpp_second.find("乙") == std::string::npos);
  ST_CHECK(cpp_second.find("额外") != std::string::npos);
}

// key 复用的一致性：两宿主在「头部插入」后，既有项的**元素身份**都必须保持
// （不是“结构看起来一样”——id 稳定性才是 key 复用的意义所在）。
ST_TEST(dual_host_key_identity_survives_insert) {
  FixedAdvanceTextPort port;
  // C++
  st::ui::UiRoot cpp_root;
  cpp_root.set_text_port(&port);
  cpp_root.set_viewport({600.0F, 400.0F});
  auto page = std::make_shared<CppList>();
  auto cpp_host = st::ui::dsl::mount(cpp_root, page);
  ST_REQUIRE(cpp_host != nullptr);
  cpp_root.layout(true);
  const auto keyed_ids = [](const st::ui::Element& root) {
    std::vector<std::string> ids;
    const auto walk = [&](auto&& self, const st::ui::Element& element) -> void {
      if (!element.key().empty()) ids.push_back(element.derived_id());
      for (std::size_t index = 0; index < element.child_count(); ++index) {
        self(self, *element.child_at(index));
      }
    };
    walk(walk, root);
    return ids;
  };
  const std::vector<std::string> cpp_before = keyed_ids(*cpp_root.content());
  auto rows = page->rows.value();
  rows.insert(rows.begin(), ParityRow{"z", "新"});
  page->rows.set(rows);
  (void)cpp_host->tick();
  cpp_root.layout(true);
  const std::vector<std::string> cpp_after = keyed_ids(*cpp_root.content());
  // 既有三个 id 一个不少（新增一项多一个）
  for (const std::string& id : cpp_before) {
    ST_CHECK(std::find(cpp_after.begin(), cpp_after.end(), id) != cpp_after.end());
  }

  // JS：同一步骤
  st::ui::UiRoot js_root;
  js_root.set_text_port(&port);
  js_root.set_viewport({600.0F, 400.0F});
  js_root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  js_root.layout(true);
  auto script = std::make_unique<st::ui::ScriptHost>(js_root);
  auto decl = st::ui::DeclarativeHost::attach(*script, js_root);
  ST_REQUIRE(decl != nullptr);
  ST_REQUIRE(decl->run(std::string(kJsList), "<parity-key>").has_value());
  js_root.layout(true);
  const std::vector<std::string> js_before = keyed_ids(*js_root.content());
  ST_REQUIRE(script->eval("rows.value = [{id:'z',n:'新'}].concat(rows.value)", "<step>")
                 .has_value());
  (void)decl->tick();
  js_root.layout(true);
  const std::vector<std::string> js_after = keyed_ids(*js_root.content());
  for (const std::string& id : js_before) {
    ST_CHECK(std::find(js_after.begin(), js_after.end(), id) != js_after.end());
  }
}
