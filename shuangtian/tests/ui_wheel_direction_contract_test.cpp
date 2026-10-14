/// **全库滚轮方向约定**的表驱动契约测试。
///
/// ## 为什么需要它（而不是各自组件测各自的）
///
/// 方向约定是**全库唯一**的，但它的"事实地位"一直只靠注释维持：
///
/// | 位置 | 写了什么 |
/// |---|---|
/// | `scroll.cpp` | 「`wheel_delta` 与系统一致——向上为正、向下为负」 |
/// | `ui_scroll_test.cpp` | `scroll_view_wheel_direction_is_natural`（**只覆盖 `ScrollView`**） |
/// | 其余组件 | 各自一句注释 |
///
/// 于是约定是**全局的、测试是局部的**——2026-10-14 实测的后果：`FileDialog` 与
/// `CompletionPopup` 的 `+=` 把方向整个反过来，而它们各自的测试**全绿**，
/// 因为那两个测试只断言"滚轮事件被处理了"，不断言方向。
///
/// ## 本文件的形态：把约定写成表，逐组件核对
///
/// 表驱动的好处不是"少写代码"，而是**新增可滚动组件时漏测会被看见**——
/// 表里没有它，审查时一眼能发现（而"再补一个 ST_TEST"不会有这种可见性）。
///
/// ## 约定的精确陈述（两个符号，别记混）
///
/// ```text
/// Event::wheel_delta   向上为正 / 向下为负   （与 Win32 WM_MOUSEWHEEL、X11 按钮 4 一致）
/// 纵向滚动 offset      变大 = 看到更靠下的内容（内容被向上推了这么多像素）
/// ⇒ 因此：offset -= wheel_delta × step
/// ```
///
/// 反向风格（"自然滚动"/内容跟手）会让 offset 与 delta 同号——本仓不采用，
/// 因为 `ScrollView`（全库基准）用 `-`，且那是系统默认手感。
///
/// ## 覆盖范围与不覆盖的部分
///
/// **覆盖**：纵向滚动的组件（下表全部）。
/// **不覆盖**（语义不同，在各自测试里断言）：
/// - 横向：`Table` 的 Shift+轮（"看到更靠右的列"）、`Tabs`（"看后面的标签"）；
/// - 缩放：`Viewport` / `TimelineView`（"上滚放大"，**与偏移无关**）；
/// - `Terminal`：回看偏移的定义是"从底部往上翻了多少行"（与纵向 offset 反号），
///   见 `terminal.cpp` 里那段说明与 `ui_terminal_scroll_test.cpp`。

#include "st/test/test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/completion_popup.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::test::FixedAdvanceTextPort;
using st::ui::EventKind;

/// 稳定的主题：`RenderContext` 持 `const Theme&`，绑临时对象是 UB
///（`ui_scroll_test.cpp` 里记着实测教训）。
/// 共享的文本端口（`FixedAdvanceTextPort` 是**只读测量桩**，构造后不再改；
/// 与 lint L8 要拦的"业务逻辑里的可变共享状态"不是一回事）。
/// 放在函数外而不是用函数内 `static`：后者触发 L8（本仓 lint 口径）。
FixedAdvanceTextPort& shared_port();

[[nodiscard]] auto theme() -> const st::ui::Theme& {
  static const st::ui::Theme value = st::ui::Theme::light();
  return value;
}

/// 纵向滚动偏移的**统一读取**：优先 `scroll`（全库规范名），回退 `scroll_offset`。
/// 两个名字都能读，是为了让"命名不统一"这件事**先被看见**（见用例末的断言），
/// 而不是让契约测试因此漏掉某个组件。
[[nodiscard]] auto read_vertical_scroll(const st::ui::Element& element) -> float {
  for (const char* name : {"scroll", "scroll_offset"}) {
    if (const auto value = element.get_property(name); value.has_value()) {
      return std::strtof(value->c_str(), nullptr);
    }
  }
  return -1.0F;
}


[[nodiscard]] auto wheel(float delta) -> st::ui::Event {
  st::ui::Event event;
  event.kind = EventKind::Wheel;
  event.position = st::math::Point{60.0F, 60.0F};
  event.wheel_delta = delta;
  return event;
}

/// 一个组件的"方向实验"结果。
struct WheelProbe {
  bool usable{false};   ///< 夹具可用且真的滚得动（否则跳过，不算通过）
  float after_down{0};  ///< 向下滚（delta = -1）之后的纵向偏移
  float after_up{0};    ///< 再向上滚（delta = +1）之后的纵向偏移
};

/// 一条契约用例：`probe()` 内部完整走一遍"初始 → 向下滚 → 向上滚"。
struct WheelCase {
  std::string_view name;
  std::function<WheelProbe()> probe;
};

/// 纵向偏移的**统一口径**：内容被向上推了多少像素（0 = 顶部）。
/// 各组件内部字段名不同（`scroll_offset_` / `scroll_y_` / `scroll_`），
/// 这里一律走它们的公开读取口。

FixedAdvanceTextPort& shared_port() {
  // 行内豁免（规则表约定：豁免必须逐条写明原因）。这是**只读测量桩**——
  // 构造后不再改，不是 L8 要拦的“业务逻辑里的可变共享状态”（同 `pkg_lint_test.cpp` 姿态）。
  static FixedAdvanceTextPort port{};  // lint-allow: L8 只读文本测量桩，构造后不再改
  return port;
}

}  // namespace

/// **契约：向下滚（`wheel_delta < 0`）→ 纵向偏移变大；向上滚 → 变小。**
ST_TEST(wheel_direction_contract_holds_for_every_scrollable_component) {
  const std::vector<WheelCase> cases{
      {"ScrollView",
       [] {
         auto view = std::make_shared<st::ui::ScrollView>();
         view->style().padding = st::math::Insets{};
         for (int index = 0; index < 5; ++index) {
           auto block = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
           block->style().height = 200.0F;
           block->style().width = 300.0F;
           view->add_child(std::move(block));
         }
         FixedAdvanceTextPort& port = shared_port();
         const st::ui::RenderContext context{theme(), &port, 0.0};
         view->measure(context, st::ui::Constraints{.max_width = 320.0F, .max_height = 200.0F});
         view->arrange(context, st::math::Rect{0.0F, 0.0F, 320.0F, 200.0F});
         WheelProbe probe;
         probe.usable = true;
         st::ui::Event down = wheel(-1.0F);
         (void)view->on_event(context, down);
         probe.after_down = read_vertical_scroll(*view);
         st::ui::Event up = wheel(1.0F);
         (void)view->on_event(context, up);
         probe.after_up = read_vertical_scroll(*view);
         return probe;
       }},

      {"Table（Shift + 轮 = 横向）",
       [] {
         std::vector<st::ui::TableColumn> columns{{.name = "a", .width = 500.0F},
                                                  {.name = "b", .width = 500.0F}};
         auto table = std::make_shared<st::ui::Table>(columns);
         table->add_row({"x", "y"});
         FixedAdvanceTextPort& port = shared_port();
         const st::ui::RenderContext context{theme(), &port, 0.0};
         table->measure(context, st::ui::Constraints{.max_width = 320.0F, .max_height = 200.0F});
         table->arrange(context, st::math::Rect{0.0F, 0.0F, 320.0F, 200.0F});
         WheelProbe probe;
         probe.usable = table->max_scroll() > 0.0F;
         st::ui::Event down = wheel(-1.0F);
         down.shift = true;
         (void)table->on_event(context, down);
         probe.after_down = table->scroll_offset();
         st::ui::Event up = wheel(1.0F);
         up.shift = true;
         (void)table->on_event(context, up);
         probe.after_up = table->scroll_offset();
         return probe;
       }},

      {"MarkdownView",
       [] {
         auto view = std::make_shared<st::ui::MarkdownView>();
         std::string markdown;
         for (int index = 0; index < 400; ++index) {
           markdown += "段落 " + std::to_string(index) + "\n\n";
         }
         view->set_markdown(markdown);
         FixedAdvanceTextPort& port = shared_port();
         const st::ui::RenderContext context{theme(), &port, 0.0};
         view->measure(context, st::ui::Constraints{.max_width = 320.0F, .max_height = 200.0F});
         view->arrange(context, st::math::Rect{0.0F, 0.0F, 320.0F, 200.0F});
         WheelProbe probe;
         probe.usable = true;
         st::ui::Event down = wheel(-1.0F);
         (void)view->on_event(context, down);
         probe.after_down = read_vertical_scroll(*view);
         st::ui::Event up = wheel(1.0F);
         (void)view->on_event(context, up);
         probe.after_up = read_vertical_scroll(*view);
         return probe;
       }},

      {"MergeView",
       [] {
         auto view = std::make_shared<st::ui::MergeView>();
         std::string ours;
         std::string theirs;
         for (int index = 0; index < 400; ++index) {
           ours += "line " + std::to_string(index) + "\n";
           theirs += "line " + std::to_string(index) + (index % 7 == 0 ? "  <x>" : "") + "\n";
         }
         view->set_texts(ours, theirs);
         FixedAdvanceTextPort& port = shared_port();
         const st::ui::RenderContext context{theme(), &port, 0.0};
         view->measure(context, st::ui::Constraints{.max_width = 320.0F, .max_height = 200.0F});
         view->arrange(context, st::math::Rect{0.0F, 0.0F, 320.0F, 200.0F});
         WheelProbe probe;
         // `usable` 只能按**内容是否溢出**判，不能按“向下滚是否变了”判：
         // 后者会把“方向反了”误判成“夹具不可用”而跳过（实测踩到——那让本用例
         // 在 MergeView 反向时仍然绿）。溢出看 `content_height > 视口高`。
         probe.usable = view->content_height() > 200.0F;
         st::ui::Event down = wheel(-1.0F);
         (void)view->on_event(context, down);
         probe.after_down = read_vertical_scroll(*view);
         st::ui::Event up = wheel(1.0F);
         (void)view->on_event(context, up);
         probe.after_up = read_vertical_scroll(*view);
         probe.usable = view->content_height() > 200.0F;
         return probe;
       }},

      {"FileDialog",
       [] {
         const auto made = st::fs::make_temp_dir("st-wheel-contract");
         if (!made.has_value()) return WheelProbe{};
         const std::string dir = *made;
         for (int index = 0; index < 300; ++index) {
           (void)st::fs::write_text(st::fs::join(dir, "f" + std::to_string(index) + ".txt"), "x");
         }
         auto root = std::make_shared<st::ui::UiRoot>();
         root->set_viewport(st::math::Size{400.0F, 300.0F});
         root->set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
         auto owned = st::ui::FileDialog::make(st::ui::FileDialog::Mode::Open, "打开");
         st::ui::FileDialog* dialog = owned.get();
         root->add_overlay(std::move(owned), st::ui::UiRoot::OverlayLayout::FillViewport);
         dialog->set_directory(dir);
         root->layout(true);

         const auto read = [dialog]() -> float {
           const auto value = dialog->get_property("scroll");
           return value.has_value() ? std::strtof(value->c_str(), nullptr) : -1.0F;
         };
         const st::ui::RenderContext context = root->render_context();
         WheelProbe probe;
         st::ui::Event down = wheel(-1.0F);
         down.position = dialog->list_rect().center();
         (void)dialog->on_event(context, down);
         probe.after_down = read();
         st::ui::Event up = wheel(1.0F);
         up.position = dialog->list_rect().center();
         (void)dialog->on_event(context, up);
         probe.after_up = read();
         probe.usable = true;  // 300 个条目必然溢出；**不按 after_down 判**（见 MergeView 的说明）
         // 收尾清理（本用例的版本不做目录删除——临时目录由系统临时区回收）。
         return probe;
       }},

      {"CompletionPopup",
       [] {
         auto root = std::make_shared<st::ui::UiRoot>();
         root->set_viewport(st::math::Size{400.0F, 300.0F});
         root->set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
         auto owned = std::make_unique<st::ui::CompletionPopup>();
         st::ui::CompletionPopup* popup = owned.get();
         popup->set_visible(true);
         root->add_overlay(std::move(owned), st::ui::UiRoot::OverlayLayout::FillViewport);
         std::vector<st::ui::CompletionItemView> items;
         for (int index = 0; index < 80; ++index) {
           st::ui::CompletionItemView view{};
           view.label = "item_" + std::to_string(index);
           view.insert_text = view.label;
           items.push_back(std::move(view));
         }
         popup->set_items(items);
         popup->set_anchor(st::math::Rect{20.0F, 20.0F, 2.0F, 16.0F});
         root->layout(true);

         const auto read = [popup]() -> float {
           const auto value = popup->get_property("scroll");
           return value.has_value() ? std::strtof(value->c_str(), nullptr) : -1.0F;
         };
         const st::ui::RenderContext context = root->render_context();
         WheelProbe probe;
         st::ui::Event down = wheel(-1.0F);
         (void)popup->on_event(context, down);
         probe.after_down = read();
         st::ui::Event up = wheel(1.0F);
         (void)popup->on_event(context, up);
         probe.after_up = read();
         probe.usable = true;  // 80 条候选必然溢出；**不按 after_down 判**
         return probe;
       }},
  };

  int verified = 0;
  int skipped = 0;
  for (const WheelCase& entry : cases) {
    const WheelProbe probe = entry.probe();
    if (!probe.usable) {
      ++skipped;
      st::print("[wheel-contract] {}：夹具不可滚动，跳过\n", entry.name);
      continue;
    }
    // 契约本体：向下滚必须让偏移变大、向上滚必须让它变小。
    ST_CHECK(probe.after_down > 0.0F);
    ST_CHECK(probe.after_up < probe.after_down);
    ++verified;
    st::print("[wheel-contract] {}：向下滚 → {:.1f}；向上滚 → {:.1f}（期望 0 < 后者 < 前者）\n",
              entry.name, static_cast<double>(probe.after_down),
              static_cast<double>(probe.after_up));
  }

  // 表里至少要有一半真的验过——防止"全被跳过"这种假绿。
  ST_CHECK(verified >= 5);

  // **属性命名统一性**：全库只应有 `scroll` 这一个纵向偏移属性名。
  // 存在的理由：`markdown_view`/`input`/`table` 曾经叫 `scroll_offset`，
  // 而多数组件叫 `scroll`——同一件事两个名字，外部（控制通道/自动化/智能体）
  // 无法用同一句话问"你滚到哪了"。这条断言把"统一"钉成可检查的事。
  {
    const st::ui::MarkdownView markdown;
    const st::ui::Table table;
    const st::ui::TextArea text_area;
    for (const st::ui::Element* element : {static_cast<const st::ui::Element*>(&markdown),
                                          static_cast<const st::ui::Element*>(&table),
                                          static_cast<const st::ui::Element*>(&text_area)}) {
      const auto names = element->property_names();
      const bool has_scroll =
          std::find(names.begin(), names.end(), std::string_view{"scroll"}) != names.end();
      const bool has_legacy =
          std::find(names.begin(), names.end(), std::string_view{"scroll_offset"}) != names.end();
      ST_CHECK(has_scroll);
      ST_CHECK(!has_legacy);
    }
  }
  st::print("[wheel-contract] 验过 {} 条，跳过 {} 条（共 {} 条）\n", verified, skipped,
            cases.size());
}
