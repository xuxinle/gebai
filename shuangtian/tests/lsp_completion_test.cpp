/// 补全测试（LSP 阶段 4）：语义层（响应解析 / snippet / 词范围）+ 弹层组件。

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/completion.hpp"
#include "st/test/test.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"
#include "st/ui/components/completion_popup.hpp"
#include "st/ui/ui_root.hpp"
#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::CompletionItemView;
using st::ui::CompletionPopup;

/// 补全语义层的样例响应（贴近 clangd 的真实形态）。
[[nodiscard]] auto sample_completion_list() -> st::Json {
  return st::json_parse(R"json({
    "isIncomplete": true,
    "items": [
      {"label":"width","kind":5,"detail":"int","sortText":"01","filterText":"width"},
      {"label":"height","kind":5,"detail":"int"},
      {"label":"write","kind":3,"detail":"void write(std::string)",
       "insertText":"write","insertTextFormat":1},
      {"label":"wprintf","kind":3,"textEdit":{"range":{"start":{"line":0,"character":0},
        "end":{"line":0,"character":3}},"newText":"wprintf"}},
      {"label":"widget_fn","kind":3,"insertText":"widget_${1:name}($0)","insertTextFormat":2},
      {"label":"with_docs","kind":7,
       "documentation":{"kind":"markdown","value":"这是文档"}}
    ]
  })json")
      .value();
}

}  // namespace

ST_TEST(lsp_completion_parses_list_and_array_forms) {
  const auto entries = st::lsp::parse_completion(sample_completion_list());
  ST_CHECK_EQ(entries.size(), std::size_t{6});
  ST_CHECK_EQ(entries.front().label, std::string("width"));
  ST_CHECK_EQ(entries.front().detail, std::string("int"));
  ST_CHECK_EQ(entries.front().badge, std::string("·"));   // kind 5 = Field
  ST_CHECK_EQ(entries[2].badge, std::string("f"));        // kind 3 = Function
  ST_CHECK_EQ(entries[5].badge, std::string("C"));        // kind 7 = Class
  ST_CHECK_EQ(entries[5].documentation, std::string("这是文档"));
  ST_CHECK(st::lsp::completion_is_incomplete(sample_completion_list()));

  // 裸数组形态（协议允许）也要认。
  const auto array_form =
      st::json_parse(R"json([{"label":"alpha","kind":6}])json").value();
  const auto array_entries = st::lsp::parse_completion(array_form);
  ST_CHECK_EQ(array_entries.size(), std::size_t{1});
  ST_CHECK_EQ(array_entries.front().label, std::string("alpha"));
  ST_CHECK(!st::lsp::completion_is_incomplete(array_form));

  // 空/畸形输入不崩。
  ST_CHECK(st::lsp::parse_completion(st::Json()).empty());
  ST_CHECK(st::lsp::parse_completion(st::json_parse("{}").value()).empty());
  ST_CHECK(st::lsp::parse_completion(st::json_parse("null").value()).empty());
}

ST_TEST(lsp_completion_insert_text_priority) {
  const auto entries = st::lsp::parse_completion(sample_completion_list());
  // 没给 insertText/textEdit：用 label。
  ST_CHECK_EQ(entries[1].insert_text, std::string("height"));
  // 给了 insertText：用它。
  ST_CHECK_EQ(entries[2].insert_text, std::string("write"));
  // **textEdit 优先**（哪怕没给 insertText）——实测过的坑：
  // 只认 insertText 会把已输入的 `wpr` 补成 `wprwprintf`。
  ST_CHECK_EQ(entries[3].insert_text, std::string("wprintf"));
  // Snippet：占位符展开（`${1:name}` → `name`，`$0` 去掉）。
  ST_CHECK_EQ(entries[4].insert_text, std::string("widget_name()"));
}

ST_TEST(lsp_snippet_expansion_minimal) {
  using st::lsp::expand_snippet_minimal;
  ST_CHECK_EQ(expand_snippet_minimal("plain text"), std::string("plain text"));
  ST_CHECK_EQ(expand_snippet_minimal("foo($1)"), std::string("foo()"));
  ST_CHECK_EQ(expand_snippet_minimal("foo(${1:bar})"), std::string("foo(bar)"));
  ST_CHECK_EQ(expand_snippet_minimal("${1:a}${2:b}"), std::string("ab"));
  ST_CHECK_EQ(expand_snippet_minimal("$0"), std::string{});
  ST_CHECK_EQ(expand_snippet_minimal("a${1|one,two|}b"), std::string("aoneb"));
  ST_CHECK_EQ(expand_snippet_minimal("cost \\$5"), std::string("cost $5"));   // 字面 `$`
  ST_CHECK_EQ(expand_snippet_minimal("int x = 1;"), std::string("int x = 1;"));
  // 不完整/畸形 snippet 不崩、不吞字符。
  ST_CHECK_EQ(expand_snippet_minimal("${1:unclosed"), std::string("unclosed"));
  ST_CHECK_EQ(expand_snippet_minimal("end$"), std::string("end"));
}

ST_TEST(lsp_word_range_before_cursor) {
  const std::string text = "int main() { foo.ba }";
  // 光标在 `ba` 之后（下标 19）。
  const auto [begin, end] = st::lsp::word_range_before_cursor(text, 19);
  ST_CHECK_EQ(end, std::size_t{19});
  ST_CHECK_EQ(text.substr(begin, end - begin), std::string("ba"));
  // 光标在 `foo` 之后：`.` 不是词字符，所以只覆盖 `foo`。
  const std::size_t after_foo = text.find("foo") + 3;
  const auto [b2, e2] = st::lsp::word_range_before_cursor(text, after_foo);
  ST_CHECK_EQ(text.substr(b2, e2 - b2), std::string("foo"));
  // 光标在空白后：空词（begin == end）。
  const auto [b3, e3] = st::lsp::word_range_before_cursor(text, 4);
  ST_CHECK_EQ(b3, e3);
  // 中文标识符不被切碎（UTF-8 高位字节算词字符）。
  const std::string cjk = "变量名";
  const auto [b4, e4] = st::lsp::word_range_before_cursor(cjk, cjk.size());
  ST_CHECK_EQ(b4, std::size_t{0});
  ST_CHECK_EQ(e4, cjk.size());
  // 越界光标夹取。
  const auto [b5, e5] = st::lsp::word_range_before_cursor("ab", 99);
  ST_CHECK_EQ(e5, std::size_t{2});
  ST_CHECK_EQ(b5, std::size_t{0});
}

ST_TEST(lsp_completion_trigger_characters) {
  const std::vector<std::string> triggers = {".", ">", ":"};
  ST_CHECK(st::lsp::should_trigger(triggers, '.'));
  ST_CHECK(st::lsp::should_trigger(triggers, ':'));
  ST_CHECK(!st::lsp::should_trigger(triggers, 'a'));
  ST_CHECK(!st::lsp::should_trigger({}, '.'));
}

ST_TEST(lsp_completion_detail_extraction) {
  const auto list = sample_completion_list();
  const auto& with_docs = st::json_at(st::json_at(list, "items")[0], "documentation");
  (void)with_docs;
  // 从原始 item 取详情：detail + documentation 拼接。
  const st::Json item = st::json_parse(
                            R"json({"label":"f","detail":"void f()","documentation":"做某事"})json")
                            .value();
  const std::string detail = st::lsp::parse_completion_detail(item);
  ST_CHECK(detail.find("void f()") != std::string::npos);
  ST_CHECK(detail.find("做某事") != std::string::npos);
  // 只有 documentation 时也给。
  const st::Json only_docs =
      st::json_parse(R"json({"label":"g","documentation":"只有文档"})json").value();
  ST_CHECK_EQ(st::lsp::parse_completion_detail(only_docs), std::string("只有文档"));
}

// ———————————————————————————— 弹层组件 ————————————————————————————

namespace {

struct PopupFixture {
  std::optional<st::text::FontStack> stack{};
  std::unique_ptr<st::test::RendererTextPort> port{};
  st::ui::UiRoot root{};
  CompletionPopup* popup{nullptr};

  PopupFixture() {
    if (const auto loaded = st::text::FontStack::system_default(); loaded.has_value()) {
      stack.emplace(std::move(*loaded));
      port = std::make_unique<st::test::RendererTextPort>(*stack);
      root.set_text_port(port.get());
    }
    root.set_viewport(st::math::Size{800.0F, 600.0F});
    root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
    auto owned = std::make_unique<CompletionPopup>();
    popup = owned.get();
    popup->set_visible(true);
    root.add_overlay(std::move(owned), st::ui::UiRoot::OverlayLayout::FillViewport);
  }

  void layout() { root.layout(true); }
};

[[nodiscard]] auto items_from(const std::vector<st::lsp::CompletionEntry>& entries)
    -> std::vector<CompletionItemView> {
  std::vector<CompletionItemView> views;
  views.reserve(entries.size());
  for (const auto& entry : entries) {
    CompletionItemView view{};
    view.label = entry.label;
    view.detail = entry.detail;
    view.insert_text = entry.insert_text;
    view.badge = entry.badge;
    view.filter_text = entry.filter_text;
    views.push_back(std::move(view));
  }
  return views;
}

}  // namespace

ST_TEST(completion_popup_navigates_and_accepts) {
  PopupFixture fixture;
  CompletionPopup& popup = *fixture.popup;
  popup.set_anchor(st::math::Rect{100.0F, 200.0F, 2.0F, 16.0F});
  popup.set_items(items_from(st::lsp::parse_completion(sample_completion_list())));
  fixture.layout();

  ST_CHECK_EQ(popup.item_count(), std::size_t{6});
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{0});

  std::string accepted;
  bool dismissed = false;
  bool accepted_flag = false;
  popup.on_accept = [&](const std::string& text) { accepted = text; };
  popup.on_dismiss = [&](bool was_accepted) {
    dismissed = true;
    accepted_flag = was_accepted;
  };

  // ↓ 移动（不环绕：到底停住）。
  ST_CHECK(popup.invoke_action("next", ""));
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{1});
  for (int step = 0; step < 20; ++step) (void)popup.invoke_action("next", "");
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{5});   // 停在最后
  ST_CHECK(popup.invoke_action("prev", ""));
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{4});

  // 接受：插入文本来自 `insert_text`（snippet 已展开）。
  ST_CHECK(popup.invoke_action("accept", ""));
  ST_CHECK_EQ(accepted, std::string("widget_name()"));
  ST_CHECK(dismissed);
  ST_CHECK(accepted_flag);

  // Esc → 未接受地关闭。
  // （用新的局部标志而不是重置旧变量：重置后立刻被回调覆盖的写法会被 lint 的
  //  "赋值后未读" 规则拦下——换个写法更清楚，也少一次误判。）
  // lint-allow: L8 函数内局部（lambda 捕获用；L8 的意图是进程级可变全局）
  bool dismiss_called = false;
  bool dismiss_accepted = true;
  popup.on_dismiss = [&](bool was_accepted) {
    dismiss_called = true;
    dismiss_accepted = was_accepted;
  };
  ST_CHECK(popup.invoke_action("dismiss", ""));
  ST_CHECK(dismiss_called);
  ST_CHECK(!dismiss_accepted);
}

ST_TEST(completion_popup_filters_and_keeps_selection_consistent) {
  PopupFixture fixture;
  CompletionPopup& popup = *fixture.popup;
  popup.set_anchor(st::math::Rect{100.0F, 200.0F, 2.0F, 16.0F});
  popup.set_items(items_from(st::lsp::parse_completion(sample_completion_list())));
  fixture.layout();
  ST_CHECK_EQ(popup.visible_count(), std::size_t{6});

  // 过滤 `w`（子串匹配、大小写不敏感）：6 项里只有 `height` 不含 w。
  popup.set_filter("w");
  ST_CHECK_EQ(popup.visible_count(), std::size_t{5});
  popup.set_filter("width");
  ST_CHECK_EQ(popup.visible_count(), std::size_t{1});
  ST_CHECK_EQ(popup.get_property("visible_labels").value_or("-"), std::string("width"));
  // 过滤后选中项必须在可见集合里（否则"看不见却被选中"——按 Enter 会插入意外内容）。
  ST_CHECK_EQ(popup.get_property("selected_label").value_or("-"), std::string("width"));

  // 清空过滤恢复全部。
  popup.set_filter("");
  ST_CHECK_EQ(popup.visible_count(), std::size_t{6});

  // 过滤到空：无选中，接受无效果（不崩）。
  popup.set_filter("zzzz-no-match");
  ST_CHECK_EQ(popup.visible_count(), std::size_t{0});
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{-1});
  bool accepted_called = false;
  popup.on_accept = [&](const std::string&) { accepted_called = true; };
  (void)popup.invoke_action("accept", "");
  ST_CHECK(!accepted_called);
}

ST_TEST(completion_popup_anchors_below_and_flips_up) {
  PopupFixture fixture;
  CompletionPopup& popup = *fixture.popup;
  popup.set_items(items_from(st::lsp::parse_completion(sample_completion_list())));
  // 锚点在上半屏：弹层应在下方。
  popup.set_anchor(st::math::Rect{100.0F, 100.0F, 2.0F, 16.0F});
  fixture.layout();
  ST_CHECK(popup.panel_rect().y >= 116.0F);
  // 锚点贴底：弹层应翻到上方（不越出视口）。
  popup.set_anchor(st::math::Rect{100.0F, 580.0F, 2.0F, 16.0F});
  fixture.layout();
  ST_CHECK(popup.panel_rect().bottom() <= 600.0F);
  ST_CHECK(popup.panel_rect().y < 580.0F);
  // 锚点贴右缘：弹层不越界。
  popup.set_anchor(st::math::Rect{795.0F, 200.0F, 2.0F, 16.0F});
  fixture.layout();
  ST_CHECK(popup.panel_rect().right() <= 800.0F);
}

ST_TEST(completion_popup_keyboard_through_root_dispatch) {
  // 真键盘路径（不是直接调 invoke）：验证浮层能吃键并路由到正确动作。
  PopupFixture fixture;
  CompletionPopup& popup = *fixture.popup;
  popup.set_anchor(st::math::Rect{100.0F, 200.0F, 2.0F, 16.0F});
  popup.set_items(items_from(st::lsp::parse_completion(sample_completion_list())));
  fixture.layout();

  std::string accepted;
  popup.on_accept = [&](const std::string& text) { accepted = text; };
  const auto press = [&](const std::string& key) {
    st::ui::Event event;
    event.kind = st::ui::EventKind::KeyDown;
    event.key = key;
    // 走根派发（浮层是顶层可见元素，键会路由到它）——比直调组件更接近真实链路。
    (void)fixture.root.dispatch(event);
  };
  press("ArrowDown");
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{1});
  press("ArrowUp");
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{0});
  press("Enter");
  ST_CHECK_EQ(accepted, std::string("width"));   // 第 0 项是 width（insert_text 空 → label）
}

ST_TEST(completion_popup_property_surface) {
  PopupFixture fixture;
  CompletionPopup& popup = *fixture.popup;
  popup.set_items(items_from(st::lsp::parse_completion(sample_completion_list())));
  popup.set_anchor(st::math::Rect{100.0F, 200.0F, 2.0F, 16.0F});
  fixture.layout();
  ST_CHECK_EQ(popup.get_property("items").value_or("-"), std::string("6"));
  ST_CHECK_EQ(popup.get_property("selected_label").value_or("-"), std::string("width"));
  ST_CHECK(popup.set_property("filter", "height"));
  ST_CHECK_EQ(popup.get_property("visible_items").value_or("-"), std::string("1"));
  ST_CHECK_EQ(popup.get_property("selected_label").value_or("-"), std::string("height"));
  // 程序化选中（按 items 下标）。
  ST_CHECK(popup.invoke_action("select", "3"));
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{3});
  ST_CHECK(!popup.invoke_action("select", "not-a-number"));
  // detail 属性面。
  popup.set_detail("示例详情");
  ST_CHECK_EQ(popup.get_property("detail").value_or("-"), std::string("示例详情"));
}

ST_TEST(completion_popup_hidden_skips_keyboard_dispatch) {
  // 回归：**不可见的浮层收不到键**（`UiRoot::dispatch_key_into` 会跳过 `!visible()`
  // 的子元素）。实测代价：声明式 overlay 只负责挂载，弹层构造时是隐藏的——
  // 忘了 `set_visible(true)` 就会出现"画面正常但 ArrowDown/Enter 全不生效"，
  // 而动作面（`invoke`）却正常工作，非常容易误判成"键盘路由坏了"。
  PopupFixture fixture;
  CompletionPopup& popup = *fixture.popup;
  popup.set_anchor(st::math::Rect{100.0F, 200.0F, 2.0F, 16.0F});
  popup.set_items(items_from(st::lsp::parse_completion(sample_completion_list())));
  fixture.layout();
  const auto press = [&](const std::string& key) {
    st::ui::Event event;
    event.kind = st::ui::EventKind::KeyDown;
    event.key = key;
    (void)fixture.root.dispatch(event);
  };
  // 可见：键生效。
  popup.set_visible(true);
  press("ArrowDown");
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{1});
  // 隐藏：键**不该**生效（也不该崩、不该改动状态）。
  popup.set_visible(false);
  press("ArrowDown");
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{1});   // 未变
  // 恢复可见后继续可用。
  popup.set_visible(true);
  press("ArrowDown");
  ST_CHECK_EQ(popup.selected_index(), std::ptrdiff_t{2});
}
