/// 元素身份与生命周期的安全性测试。
///
/// 两条都会**崩溃**（未定义行为）的路径，以前没有任何覆盖：
/// 1. `UiRoot::focused()` / 事件分发拿到的焦点元素可能已被销毁——界面每帧都可能重建子树
///    （列表按数据刷新、页面被替换），而 `focused_` 是裸指针。元素从树上摘下时无法通知
///    到 root（`Element` 没有 root 反指），所以采用"用前校验"：只做指针比较、不解引用。
/// 2. 自动 id 必须只依赖"身份"而不依赖"位置"，否则外部按 id 引用会在刷新后错位。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/ui_root.hpp"

namespace {

/// 造一个"页面 + 一个可聚焦输入框"的内容树，返回输入框裸指针（生命周期由 root 持有）。
[[nodiscard]] auto make_page(st::ui::UiRoot& root) -> st::ui::Input* {
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->set_id("page");
  auto input = std::make_unique<st::ui::Input>();
  input->set_id("field");
  st::ui::Input* raw = input.get();
  (void)page->add_child(std::move(input));
  root.set_content(std::move(page));
  root.layout(true);
  return raw;
}

}  // namespace

ST_TEST(ui_root_focus_survives_content_replacement) {
  st::ui::UiRoot root;
  st::ui::Input* field = make_page(root);
  ST_CHECK(field != nullptr);

  root.set_focus(field);
  ST_CHECK(root.focused() == field);

  // 整个页面被替换：`field` 已被销毁，`focused_` 若不校验就是悬垂指针
  (void)make_page(root);

  // 只做指针比较的结果：安全地报"没有焦点元素"
  ST_CHECK(root.focused() == nullptr);
  // 分发事件也不能踩到已销毁的元素（键事件会用到焦点）
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = "Tab";
  (void)root.dispatch(event);
  event.kind = st::ui::EventKind::TextInput;
  event.text = "x";
  (void)root.dispatch(event);
}

ST_TEST(ui_root_focus_survives_subtree_removal) {
  st::ui::UiRoot root;
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->set_id("page");
  auto row = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Row);
  row->set_id("row");
  auto input = std::make_unique<st::ui::Input>();
  input->set_id("field");
  st::ui::Input* field = input.get();
  (void)row->add_child(std::move(input));
  st::ui::Panel* row_ptr = row.get();
  (void)page->add_child(std::move(row));
  root.set_content(std::move(page));
  root.layout(true);

  root.set_focus(field);
  ST_CHECK(root.focused() == field);

  // 焦点所在的那棵子树被摘掉（销毁）：等价于"列表正在刷新"
  st::ui::Panel* page_ptr = nullptr;
  {
    auto* top = root.content();
    ST_CHECK(top != nullptr);
    page_ptr = static_cast<st::ui::Panel*>(top);
  }
  if (page_ptr != nullptr) (void)page_ptr->remove_child(row_ptr);

  ST_CHECK(root.focused() == nullptr);
}

ST_TEST(ui_root_focus_cleared_when_content_cleared) {
  // "页面整个消失"的最直接形态
  st::ui::UiRoot root;
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  page->set_id("page");
  auto input = std::make_unique<st::ui::Input>();
  input->set_id("field");
  st::ui::Input* field = input.get();
  (void)page->add_child(std::move(input));
  root.set_content(std::move(page));
  root.layout(true);

  root.set_focus(field);
  // 清空内容：焦点元素随之销毁，`focused_` 立刻变悬垂
  root.set_content(nullptr);
  ST_CHECK(root.focused() == nullptr);

  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = "Tab";
  (void)root.dispatch(event);
}
