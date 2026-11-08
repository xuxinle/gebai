#pragma once

/// @file all.hpp
/// @brief **全部组件**的聚合头：一个 include 拿到整套组件（开发期最省事）。
///
/// ## 什么时候用它
///
/// - **开发期**：省得逐个找头，`#include "st/ui/components/all.hpp"` 就全有了；
/// - **XML 驱动的小应用**：界面写在 `main.xml` 里，标签是运行期才决定的，
///   编译期无法枚举要哪些组件——这时全量头是对的选择（XML 应用的另一条路是
///   自己按需 include 用到的组件头，见 `docs/xml-ui.md`）。
///
/// ## 交付前为什么应该改回按需
///
/// 本头把**全部组件**链进产物。实测（2026-11-08，册府改造前）：二进制里有
/// `Terminal` / `CodeEditor` / `DiffView` 这些应用根本不用的组件。
/// 改成按需 include 之后它们不再进产物——这是"按需导入"的全部意义。
///
/// ## 它同时负责"导入"
///
/// 只 include 头**不够**：全量头里列出的组件不一定都被 `new` 过，而**没人引用的
/// `.o` 不进产物**（静态库里按需拉取的机制）。`import_components<...>()` 对每个
/// 组件的导入锚点产生真实引用，于是它们都被拉进产物、登记在 `main` 之前完成。
///
/// （注：如果应用**真的 `new` 了**某个组件，那个 `.o` 自己就进了产物，静态登记随之
/// 生效——那种情况下不需要 import。细节见 `st/ui/components/README.md` 第二节。）
///
/// 按需使用时，抄这几行即可（把不用的去掉）：
///
/// ```cpp
/// #include "st/ui/components/button.hpp"
/// // …某个初始化函数里：
/// st::ui::import_components<st::ui::Button>();
/// ```

// ── 基础 ──
#include "st/ui/components/avatar.hpp"
#include "st/ui/components/badge.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/card.hpp"
#include "st/ui/components/chip.hpp"
#include "st/ui/components/divider.hpp"
#include "st/ui/components/heading.hpp"
#include "st/ui/components/icon_view.hpp"
#include "st/ui/components/key_value_row.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/components/progress_bar.hpp"
#include "st/ui/components/spinner.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/components/tooltip.hpp"

// ── 输入与开关 ──
#include "st/ui/components/checkbox.hpp"
#include "st/ui/components/input_box.hpp"
#include "st/ui/components/radio.hpp"
#include "st/ui/components/select_box.hpp"
#include "st/ui/components/select_panel.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/switch.hpp"
#include "st/ui/components/text_area.hpp"

// ── 容器与列表 ──
#include "st/ui/components/list_item.hpp"
#include "st/ui/components/list_view.hpp"
#include "st/ui/components/scroll_bar.hpp"
#include "st/ui/components/scroll_view.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/components/viewport.hpp"

// ── 浮层与反馈 ──
#include "st/ui/components/dialog.hpp"
#include "st/ui/components/toast.hpp"

// ── 复合控件 ──
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/color_picker.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/completion_popup.hpp"
#include "st/ui/components/diff_view.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/components/slow_content.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/components/theme_picker.hpp"
#include "st/ui/components/timeline_view.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"

// ── 三维视图（需要真实三维后端；无头环境下不渲染）──
#include "st/ui/components/graph_view.hpp"
#include "st/ui/components/label_overlay.hpp"
#include "st/ui/components/scene_view.hpp"
#include "st/ui/components/three_view.hpp"

// ── 导入（登记）────────────────────────────────────────────────────────────
//
// ⚠ include 头只让类型可见；要让组件**进产物并能被按名构造**，必须导入。
//
// 本头提供两档：
//   - `import_all_components()`：一次导入全部（开发期 / XML 应用的推荐做法）
//   - 自己按需：`st::ui::import_components<st::ui::Button, st::ui::Text>();`
//
// 为何是 `inline` 函数而不是静态对象：`import_all_components` 被调用时才取
// 每个组件导入锚点的地址，产生真实引用 → 那些 `.o` 被拉进产物。
// （写成静态对象则只在“本 TU 被包含并初始化”时生效，而没人引用本文件时
//   那个静态对象本身也可能被丢弃——那是另一回事，与组件内 `.cpp` 的静态登记
//   不同。详见 `st/ui/components/README.md` 第二节。）
//
// 新增组件时：上面的 include 区加一行，下面的类型列表也加一行。
// 少改一处会表现为"这个组件没注册"（一致性单测会报）。
namespace st::ui {

/// 导入**全部**组件（`all.hpp` 的配套动作）。
inline void import_all_components() {
  import_components<
      // 基础
      Text, Heading, IconView, Button, Card, Divider, KeyValueRow, Panel, Spacer,
      ProgressBar, Spinner, Badge, Chip, Avatar, Tooltip,
      // 输入与开关
      Input, TextArea, Checkbox, Radio, Switch, Slider, Select, SelectPanel,
      // 容器与列表
      List, ListItem, ScrollView, ScrollBar, SplitView, Table, Tabs, Tree, Viewport,
      // 浮层与反馈
      Dialog, Toast,
      // 复合控件
      CodeEditor, ColorPicker, CommandPalette, CompletionPopup, DiffView, FileDialog,
      MarkdownView, MenuBar, MenuPanel, ContextMenu, MergeView, SlowContent, Terminal,
      ThemePicker, TimelineView, TitleBar, WindowFrame>();
  // 三维族（ThreeView / LabelOverlay / SceneView / GraphView）**不在**导入列表里：
  // 它们不进声明式注册表（理由见各自头文件末尾）。它们的头仍被上面的 include 引入，
  // 所以想直接 `new` 它们的代码只需要 `all.hpp` 就够了。
}

}  // namespace st::ui
