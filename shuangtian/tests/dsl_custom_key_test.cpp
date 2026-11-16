/// `custom<T>` 的 `BoxProps.key` 必须参与**元素对齐**，而不只是写进属性。
///
/// ## 缺陷（2026-11-16 实测，用户截图：“日志行右上是两个一模一样的刷新图标”）
///
/// `custom<T>` 的实现是：先 `create_element(type, props, key_参数)` 做对齐，再
/// `apply_box` 把 `BoxProps` 写进元素。gbcode 全部 `custom<Button>` 都只传
/// `BoxProps.key`（第 4 个参数留空）⇒ 对齐时拿到的是**空 key** ⇒ 退回纯位置对齐。
/// 而 `apply_box` 事后补上的 key **下一帧同样不参与对齐**（下一帧仍然传空）。
///
/// 于是一旦兄弟数量变化（例如条件声明的那段 `if (!branch.empty()) text(...)` 从无到有），
/// 整行元素**整体右移一格**，每个按钮都按位置抓到了"上一个角色的元素"：
/// 写 `b.set_label("仅当前分支")` 的那个按钮，拿到了原本属于刷新按钮的实例，
/// 于是它保留着 `icon=refresh` / `hint=重新读取日志`——画面上就是**两个刷新图标**。
///
/// 这类"属性泄漏"最难查的地方：`set_*` 从未在这条路径上被调过，值却在那儿；
/// 单看任一帧的代码都完全正确，错的是**跨帧的元素身份**。
///
/// 判据（本文件）：插入一个条件兄弟后，按 key 声明的元素必须**还是原来那个实例**。

#include "st/test/test.hpp"
#include "st/ui/dsl.hpp"

#include <memory>
#include <string>

#include "st/ui/components/basic.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using namespace st::ui;
using namespace st::ui::dsl;

/// 一页三按钮：`show_extra` 打开时在最前面插一个 Text（模拟 gbcode 的条件分支文字）。
///
/// 关键点：三个按钮的身份**只写在 `BoxProps.key` 里**（与 gbcode 写法一致）。
struct ShiftPage : Component {
  State<bool> show_extra{false};

  void build(Composer& c) override {
    column(c, {}, [&] {
      if (show_extra.value()) {
        // 条件兄弟：它一出现，后面所有兄弟的位置都右移一格。
        (void)text(c, [] { return std::string("插入的一行"); });
      }
      // 三个按钮各设一个**互不相同**的可见属性，用来识别“身份是否跟着 key 走”。
      (void)custom<Button>(c, [](Button& b) { b.set_label("第一个"); },
                          {.height = 24.0f, .key = "btn-first"});
      (void)custom<Button>(c, [](Button& b) { b.set_label("第二个"); },
                          {.height = 24.0f, .key = "btn-second"});
      (void)custom<Button>(c, [](Button& b) { b.set_label("第三个"); },
                          {.height = 24.0f, .key = "btn-third"});
    });
  }
};

/// 在内容树里按 **label** 找按钮（身份错位时 label 会跟着跑到别的实例上）。
[[nodiscard]] auto find_button_by_label(const Element& parent, std::string_view label)
    -> const Button* {
  for (std::size_t index = 0; index < parent.content_child_count(); ++index) {
    const Element* child = parent.child_at(index);
    if (child == nullptr) continue;
    if (const auto* button = dynamic_cast<const Button*>(child); button != nullptr) {
      if (button->label() == label) return button;
    }
  }
  return nullptr;
}

}  // namespace

/// 插入条件兄弟后，三个按钮必须各自复用自己的实例（key 驱动对齐）。
///
/// 修复前：整体右移一格 ⇒ `第二个` 拿到 `第一个` 的实例、`第三个` 拿到 `第二个` 的；
/// 而 label 是 configure 里重设的，所以**光看 label 全对**——必须比实例地址。
ST_TEST(dsl_custom_box_prop_key_drives_alignment_across_shift) {
  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<ShiftPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* content = root.content();
  ST_REQUIRE(content != nullptr);
  ST_REQUIRE(content->content_child_count() == 3);
  const Button* first_before = find_button_by_label(*content, "第一个");
  const Button* second_before = find_button_by_label(*content, "第二个");
  const Button* third_before = find_button_by_label(*content, "第三个");
  ST_REQUIRE(first_before != nullptr);
  ST_REQUIRE(second_before != nullptr);
  ST_REQUIRE(third_before != nullptr);

  // 打开条件兄弟 → 后面的兄弟全部右移一格 → 重组
  page->show_extra.set(true);
  (void)host->tick();
  root.layout();

  const Element* after = root.content();
  ST_REQUIRE(after != nullptr);
  ST_REQUIRE(after->content_child_count() == 4);
  const Button* first_after = find_button_by_label(*after, "第一个");
  const Button* second_after = find_button_by_label(*after, "第二个");
  const Button* third_after = find_button_by_label(*after, "第三个");
  ST_REQUIRE(first_after != nullptr);
  ST_REQUIRE(second_after != nullptr);
  ST_REQUIRE(third_after != nullptr);

  // 身份必须跟着 key 走：同一个按钮还是**同一个实例**。
  ST_CHECK(first_after == first_before);
  ST_CHECK(second_after == second_before);
  ST_CHECK(third_after == third_before);
}

/// 同一个缺陷的**症状面**：错位时元素会把上一个角色的属性带过来。
///
/// 这条直接断言“按钮身上不该出现的属性”。修复前 `第三个` 会带着 `第二个` 的
/// 那个属性值（gbcode 里就是 `icon=refresh` / `hint=重新读取日志` 跑到“仅当前分支”上）。
ST_TEST(dsl_custom_box_prop_key_prevents_stale_prop_leak) {
  struct IconPage : Component {
    State<bool> show_extra{false};
    void build(Composer& c) override {
      column(c, {}, [&] {
        if (show_extra.value()) (void)text(c, [] { return std::string("插入的一行"); });
        // A 有图标，B 没有图标——B 若抓到了 A 的实例，就会带着 A 的图标。
        (void)custom<Button>(c, [](Button& b) { b.set_icon("refresh"); },
                            {.height = 24.0f, .key = "with-icon"});
        (void)custom<Button>(c, [](Button& b) { b.set_label("无图标"); },
                            {.height = 24.0f, .key = "no-icon"});
      });
    }
  };

  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<IconPage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  page->show_extra.set(true);
  (void)host->tick();
  root.layout();

  const Element* content = root.content();
  ST_REQUIRE(content != nullptr);
  ST_REQUIRE(content->content_child_count() == 3);
  const Button* no_icon = find_button_by_label(*content, "无图标");
  ST_REQUIRE(no_icon != nullptr);
  // 这个按钮**从没被 set_icon 过**：它的图标必须是空的（走属性面读，不从私有成员读）。
  const auto icon = no_icon->get_property("icon");
  ST_CHECK(icon.has_value() && icon->empty());
}

/// **同类型兄弟位移**：这条钉住 `custom<T>` 的 `props.key` 必须交给对齐器。
///
/// 为何前面两条还不够：兄弟类型不同时，“插入新元素到游标位”本身就把它挡住了；
/// 而同类兄弟位移时，仅靠位置对齐会**直接复用错元素**（类型对得上），
/// 只写 `props.key` 的调用在这里才真正兼不了位。
///
/// 场景：三个同类型按钮，`show_extra` 打开时在最前面插一个**同类型**按钮
/// ⇒ 后面每个都应靠 key 找对自己的实例，而不是按位置抓到上一个。
ST_TEST(dsl_custom_box_prop_key_drives_alignment_for_same_type_siblings) {
  struct SameTypePage : Component {
    State<bool> show_extra{false};
    void build(Composer& c) override {
      column(c, {}, [&] {
        if (show_extra.value()) {
          (void)custom<Button>(c, [](Button& b) { b.set_label("新插的"); },
                              {.height = 24.0f, .key = "btn-extra"});
        }
        (void)custom<Button>(c, [](Button& b) { b.set_label("甲"); },
                            {.height = 24.0f, .key = "btn-a"});
        (void)custom<Button>(c, [](Button& b) { b.set_label("乙"); },
                            {.height = 24.0f, .key = "btn-b"});
      });
    }
  };

  UiRoot root;
  root.set_viewport({400.0f, 300.0f});
  auto page = std::make_shared<SameTypePage>();
  auto host = dsl::mount(root, page);
  ST_REQUIRE(host != nullptr);
  root.layout();

  const Element* before = root.content();
  ST_REQUIRE(before != nullptr);
  ST_REQUIRE(before->content_child_count() == 2);
  const Button* a_before = find_button_by_label(*before, "甲");
  const Button* b_before = find_button_by_label(*before, "乙");
  ST_REQUIRE(a_before != nullptr);
  ST_REQUIRE(b_before != nullptr);

  page->show_extra.set(true);
  (void)host->tick();
  root.layout();

  const Element* after = root.content();
  ST_REQUIRE(after != nullptr);
  ST_REQUIRE(after->content_child_count() == 3);
  const Button* a_after = find_button_by_label(*after, "甲");
  const Button* b_after = find_button_by_label(*after, "乙");
  ST_REQUIRE(a_after != nullptr);
  ST_REQUIRE(b_after != nullptr);
  // 同类型 ⇒ 位置对齐会“成功”地复用错元素，只有 key 能救。
  ST_CHECK(a_after == a_before);
  ST_CHECK(b_after == b_before);
}
