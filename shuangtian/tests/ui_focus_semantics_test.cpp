/// 焦点语义与「基类状态一致性」测试。
///
/// 背景——这是**系统性模式缺陷**，同类问题曾在多个组件反复出现：
///
/// ① **成员遮蔽**：组件 hpp 里再声明一个与 `Element` 保护成员同名的成员即构成遮蔽，
///    而且**编译零警告**（不在同一作用域重复声明、也没有 `-Wshadow` 覆盖成员遮蔽）。
///    `CodeEditor` 曾自带 `bool focused_{false}`：`UiRoot::set_focus` 写基类成员、
///    `paint_content` 读遮蔽副本 → 光标永不绘制、括号匹配高亮失效。
///    单元测试直接调 `set_focused(true)` 时读写落在同一侧，**完全掩盖**了缺陷——
///    只有真实应用（走 `UiRoot::set_focus`）才暴露。
///    `KeyValueRow` 曾同名遮蔽 `Element::key_`（基类的 key 是稳定逻辑身份，参与自动 id
///    `Type@key`；显示标签不是身份）。
///
/// ② **语义标志重建**：`semantics_flags()` 覆写若以 `SemanticsFlags flags{}` 起手，
///    会丢掉基类结果（visible/enabled/focused/hovered/pressed）——焦点经 UiRoot 设置时
///    控制通道语义树与 `:focused` 选择器恒报 false。
///
/// 因此本文件**不直接调用 `set_focused`**：一切焦点都经 `UiRoot::set_focus`
/// （与真实应用同一条路径，这正是原缺陷的暴露方式）。

#include "st/test/test.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/select.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Size;
using st::raster::Canvas;
using st::ui::Element;
using st::ui::UiRoot;

inline constexpr int kWidth = 320;
inline constexpr int kHeight = 140;

/// 两帧画布的像素差异包围盒（`any == false` 表示逐像素完全一致）。
struct PixelDiff {
  bool any{false};
  int min_x{0};
  int min_y{0};
  int max_x{0};
  int max_y{0};
  [[nodiscard]] auto width() const noexcept -> int { return max_x - min_x + 1; }
  [[nodiscard]] auto height() const noexcept -> int { return max_y - min_y + 1; }
};

[[nodiscard]] auto diff_pixels(const Canvas& before, const Canvas& after) -> PixelDiff {
  PixelDiff out;
  const std::span<const std::uint32_t> a = before.pixels();
  const std::span<const std::uint32_t> b = after.pixels();
  if (a.size() != b.size() || a.empty()) return out;
  const auto width = static_cast<std::size_t>(after.physical_width());
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index] == b[index]) continue;
    const auto x = static_cast<int>(index % width);
    const auto y = static_cast<int>(index / width);
    if (!out.any) {
      out = PixelDiff{true, x, y, x, y};
      continue;
    }
    out.min_x = std::min(out.min_x, x);
    out.min_y = std::min(out.min_y, y);
    out.max_x = std::max(out.max_x, x);
    out.max_y = std::max(out.max_y, y);
  }
  return out;
}

/// 渲染一帧到新画布（透明底）。
[[nodiscard]] auto render(UiRoot& root) -> Canvas {
  Canvas canvas = Canvas::for_logical_size(kWidth, kHeight, 1.0f);
  canvas.clear(Color{0, 0, 0, 0});
  root.paint(canvas);
  return canvas;
}

/// 装配一个只含单个元素的根（元素即内容根）。
/// `UiRoot` 不可拷贝也不可移动（持有树与句柄），因此就地装配而不返回。
void mount(UiRoot& root, std::unique_ptr<Element> element) {
  root.set_theme(st::ui::Theme::light());
  root.set_viewport(Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
  root.set_content(std::move(element));
  root.layout(true);
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 遮蔽：焦点经 UiRoot 设置后必须真的可见（光标）+ 语义标志为真
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(code_editor_focus_via_root_renders_cursor_and_reports_flags) {
  // 编辑器是唯一「焦点直接决定绘制内容」的组件（光标竖线），因此用它做像素级断言。
  // 空 TextPort：画布上除光标外没有别的东西，聚焦前后的差异**就是**光标。
  UiRoot root;
  mount(root, std::make_unique<st::ui::CodeEditor>());
  auto* editor = static_cast<st::ui::CodeEditor*>(root.content());
  ST_REQUIRE(editor != nullptr);
  root.set_text_port(nullptr);
  editor->set_text("alpha\nbeta");

  const Canvas unfocused = render(root);
  ST_CHECK(root.set_focus(editor));
  ST_CHECK(root.focused() == editor);
  const Canvas focused = render(root);

  const PixelDiff diff = diff_pixels(unfocused, focused);
  // 遮蔽缺陷下 `paint_content` 读到的焦点恒为 false → 两帧逐像素一致 → 这里必然失败
  ST_CHECK(diff.any);
  // 光标是 1.6px 宽的竖线：抗锯齿最多铺开 2~3 列，且高度接近行高（不是零散像素）
  ST_CHECK(diff.width() <= 3);
  ST_CHECK(diff.height() > 8);

  // 语义标志保留基类结果（②）：focused 必须为真
  const st::ui::SemanticsFlags flags = editor->semantics_flags();
  ST_CHECK(flags.focused);
  ST_CHECK(flags.visible);
  ST_CHECK(flags.enabled);
  // 基类成员被写入（①：组件不得自带同名遮蔽成员）
  ST_CHECK(editor->focused());

  // 失焦后回到未聚焦帧的像素（光标只在聚焦时画）
  ST_CHECK(root.set_focus(nullptr));
  const Canvas blurred = render(root);
  ST_CHECK(!diff_pixels(unfocused, blurred).any);
}

ST_TEST(code_editor_click_focuses_without_keyboard_activation) {
  // P2-C 的真实症状：点击聚焦走 `hit_test → focusable() && set_focus`，
  // 而键盘激活（activate）又要求先有焦点——默认不可聚焦时是死循环，点击永远聚焦不了编辑器。
  UiRoot root;
  mount(root, std::make_unique<st::ui::CodeEditor>());
  auto* editor = static_cast<st::ui::CodeEditor*>(root.content());
  ST_REQUIRE(editor != nullptr);
  ST_CHECK(root.focused() == nullptr);

  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.position = editor->bounds().center();
  (void)root.dispatch(down);
  ST_CHECK(root.focused() == editor);
  ST_CHECK(editor->focused());
}

// ————————————————————————————————————————————————————————————————————————————
// ② 文本类组件默认可聚焦（P2-C）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(text_components_are_focusable_by_default) {
  st::ui::Input input;
  st::ui::TextArea area;
  st::ui::CodeEditor editor;
  ST_CHECK(input.focusable());
  ST_CHECK(area.focusable());
  ST_CHECK(editor.focusable());
  // 只读态同样可聚焦：只读影响「能否编辑」，不影响「能否持有焦点」（要能选中/复制/滚动）
  st::ui::CodeEditor viewer;
  viewer.set_read_only(true);
  ST_CHECK(viewer.focusable());
}

// ————————————————————————————————————————————————————————————————————————————
// ③ semantics_flags 覆写必须保留基类状态（P1-B 全量覆盖）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(semantics_flags_preserve_base_state_for_every_override) {
  // 覆盖所有**覆写了 semantics_flags 且可聚焦**的组件：以 `SemanticsFlags flags{}` 起手
  // 会丢掉基类的 focused/visible/enabled——这里逐类经 UiRoot 聚焦后断言三项都在。
  using Factory = std::unique_ptr<Element> (*)();
  // 工厂显式声明返回 `unique_ptr<Element>`：lambda 隐式转函数指针时**返回类型不协变**
  const std::vector<std::pair<const char*, Factory>> cases = {
      {"Button", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Button>("确定"); }},
      {"Input", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Input>(); }},
      {"TextArea", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::TextArea>(); }},
      {"CodeEditor",
       []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::CodeEditor>(); }},
      {"Checkbox",
       []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Checkbox>("勾选"); }},
      {"Radio", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Radio>("单选"); }},
      {"Switch",
       []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Switch>("开关"); }},
      {"Slider", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Slider>(0.5f); }},
      {"Select", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Select>(); }},
      {"ListItem",
       []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::ListItem>("行"); }},
      {"List", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::List>(); }},
      {"Table", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Table>(); }},
      {"Tabs", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Tabs>(); }},
      {"Tree", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Tree>(); }},
      {"ScrollView",
       []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::ScrollView>(); }},
      {"MarkdownView",
       []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::MarkdownView>(); }},
      {"MenuBar", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::MenuBar>(); }},
      {"MenuPanel",
       []() -> std::unique_ptr<Element> {
         return std::make_unique<st::ui::MenuPanel>(std::vector<st::ui::MenuItem>{});
       }},
      {"SelectPanel",
       []() -> std::unique_ptr<Element> {
         return std::make_unique<st::ui::SelectPanel>(std::vector<st::ui::SelectOption>{},
                                                      std::nullopt);
       }},
  };

  for (const auto& [name, factory] : cases) {
    UiRoot root;
    mount(root, factory());
    Element* element = root.content();
    ST_REQUIRE(element != nullptr);
    (void)name;
    if (element->focusable()) {
      ST_CHECK(root.set_focus(element));
      ST_CHECK(element->semantics_flags().focused);
      ST_CHECK(element->focused());
    } else {
      // 不可聚焦的（列表行/下拉面板：键盘经**浮层通道**直达，不持有焦点）——
      // 同时验证 P2-D 的严格语义：这些元素拒绝接受焦点。
      ST_CHECK(!root.set_focus(element));
      ST_CHECK(!element->semantics_flags().focused);
      ST_CHECK(root.focused() == nullptr);
    }
    // 基类状态无条件保留（与是否可聚焦无关）
    ST_CHECK(element->semantics_flags().visible);
    ST_CHECK(element->semantics_flags().enabled);
    // 悬停标志同样来自基类（语义标志重建时会丢）
    element->set_hovered(true);
    ST_CHECK(element->semantics_flags().hovered);
  }
}

ST_TEST(input_and_textarea_render_focus_ring_when_focused_via_root) {
  // 有焦点绘制的组件都要经 `UiRoot::set_focus`（真实路径）验证**可见变化**：
  // 直接 set_focused 会读写同侧，遮蔽缺陷下双双通过。
  const std::vector<std::pair<const char*, std::unique_ptr<Element> (*)()>> cases = {
      {"Input", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::Input>(); }},
      {"TextArea", []() -> std::unique_ptr<Element> { return std::make_unique<st::ui::TextArea>(); }},
  };
  for (const auto& [name, factory] : cases) {
    (void)name;
    UiRoot root;
    mount(root, factory());
    Element* element = root.content();
    ST_REQUIRE(element != nullptr);

    const Canvas blurred = render(root);
    ST_CHECK(root.set_focus(element));
    const Canvas focused = render(root);

    // 焦点环 + 边框/图标提色：两帧必须有可见差异
    const PixelDiff diff = diff_pixels(blurred, focused);
    ST_CHECK(diff.any);
    // 其后的失焦渲染回到未聚焦帧（同一路径可逆）
    ST_CHECK(root.set_focus(nullptr));
    ST_CHECK(!diff_pixels(blurred, render(root)).any);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// ⑤ 自动化读得到的框架级状态（`get` 的 props）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(element_snapshot_exposes_framework_state_including_focus) {
  // `get` 的 props 是自动化看元素状态的入口。框架级属性必须齐备：`set` 支持
  // `enabled`/`visible`/`focused` 而 `get` 读不到时，「点一下再看焦点」这类验证动作
  // 就只剩截图猜（同族缺陷：只写不读的静默缺口）。
  UiRoot root;
  mount(root, std::make_unique<st::ui::Input>());
  auto* input = static_cast<st::ui::Input*>(root.content());
  ST_REQUIRE(input != nullptr);

  const auto focused_before = st::ui::element_snapshot(*input);
  const st::Json* props = st::json_find(focused_before, "props");
  ST_REQUIRE(props != nullptr);
  const st::Json* focused = st::json_find(*props, "focused");
  ST_REQUIRE(focused != nullptr);
  ST_CHECK(!st::json_as_bool(*focused, true));

  ST_CHECK(root.set_focus(input));
  const auto focused_after = st::ui::element_snapshot(*input);
  const st::Json* props_after = st::json_find(focused_after, "props");
  ST_REQUIRE(props_after != nullptr);
  const st::Json* focused_now = st::json_find(*props_after, "focused");
  ST_REQUIRE(focused_now != nullptr);
  ST_CHECK(st::json_as_bool(*focused_now));

  // 其余框架级属性同样在（enabled/visible/pressed/hovered）
  for (const std::string_view key : {"enabled", "visible", "pressed", "hovered"}) {
    ST_CHECK(st::json_find(*props_after, key) != nullptr);
  }
}

// ————————————————————————————————————————————————————————————————————————————
// ④ set_focus 与 focusable 的语义一致性（P2-D）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(set_focus_rejects_non_focusable_and_reports_it) {
  // 不可聚焦元素（Tab 环按 focusable() 筛选）拒绝接受焦点：否则出现
  // 「root 焦点指向它、键盘派发给它、而 Tab 环跳过它」的状态分裂。
  UiRoot root;
  root.set_theme(st::ui::Theme::light());
  root.set_viewport(Size{static_cast<float>(kWidth), static_cast<float>(kHeight)});
  auto page = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto input = std::make_unique<st::ui::Input>();
  auto* input_ptr = input.get();
  page->add_child(std::move(input));
  auto* page_ptr = page.get();
  root.set_content(std::move(page));
  root.layout(true);

  ST_CHECK(!page_ptr->focusable());  // 面板本就不该持有焦点
  ST_CHECK(input_ptr->focusable());

  // 可聚焦：应用成功
  ST_CHECK(root.set_focus(input_ptr));
  ST_CHECK(root.focused() == input_ptr);
  ST_CHECK(input_ptr->focused());

  // 不可聚焦：拒绝，且**焦点保持不变**（不会把已有焦点顶掉）
  ST_CHECK(!root.set_focus(page_ptr));
  ST_CHECK(root.focused() == input_ptr);
  ST_CHECK(!page_ptr->focused());
  ST_CHECK(input_ptr->focused());

  // 清除焦点恒合法（nullptr 不需要可聚焦）；重复调用是幂等的成功
  ST_CHECK(root.set_focus(nullptr));
  ST_CHECK(root.focused() == nullptr);
  ST_CHECK(root.set_focus(nullptr));
  ST_CHECK(root.focused() == nullptr);

  // Tab 环：拒绝的请求没有留下「焦点在环外」的残留——`focus_next` 从头开始仍落在可聚焦元素
  root.focus_next();
  ST_CHECK(root.focused() == input_ptr);
}
