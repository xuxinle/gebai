/// gbcode 快捷键**单源表**（BACKLOG「`UiRoot` 快捷键注册表无可枚举面」，2026-10-16）。
///
/// ## 这份表是什么
///
/// 全部根级快捷键的**唯一权威登记处**：每条 = 登记 id + 键 + 修饰键 + 展示文案 +
/// 卡片分组 + 注册形态。`main.cpp` 的注册块、`page_dialogs.cpp` 的帮助卡正文、
/// `tests/gbcode_shortcut_card_test.cpp` 的对账，三处都从这一份表读——
/// 表与代码同源，第 33 轮手补 14 条、又再漂 15+ 的「两份数据必分叉」从机制上消灭。
///
/// ## 依赖方向（为什么注册也走这张表）
///
/// 仅让**帮助卡**从 `UiRoot::shortcuts()` 生成还不够：注册表条目没有
/// 「这条叫什么、属于哪组」的语义，卡片只能按登记名裸列，可读性不如手写卡——
/// 于是没人真用，机制空转。让注册本身也从这张表来（`register_shortcut_by_id`），
/// 「文案/分组」才与「真实注册」物理同源；对账测试再反向锁死两边的口径
///（表里的键位 ⇄ 注册表里的 mods，见 `tests/gbcode_shortcut_card_test.cpp`）。
///
/// ## 三类注册形态（`reg` 字段，忠实保留原 main.cpp 注册块的差异）
///
/// - `LetOverlayDecide`：浮层开着时不消费、放行下沉（原 `bind` 助手 + 显式
///   `overlay_open` 守卫那批：Ctrl+S/W/O/T/B/J/`/K/G/D/E/F/P/H/Tab…）；
/// - `Consume`：无外层守卫，消费与否由动作自己的返回值表达（浮层/条件语义
///   自己的键：Ctrl+Space、F2、F7 族（无差异视图时让路）、F12 族、F8、Esc
///   （无事可关时让路）——给它们装 `overlay_open` 守卫反而会挡住浮层自己，
///   见 main.cpp 原注释）；
/// - `Never`：**不注册**，只登记进卡片（Ctrl+Z/Y、Ctrl+A、Ctrl+/——由
///   `CodeEditor::handle_key` 在编辑器聚焦时处理，不走根级表）。卡片如实
///   标注「（编辑器内）」；对账测试把它们从注册面期待集里剔除。
///
/// ## 卡片口径（`show` / `alias_of`）
///
/// 注册粒度 ≠ 展示粒度：一张卡行可以覆盖多条注册（原手写卡如此，如
/// 「Ctrl+F/Ctrl+H 查找 / 替换」一行盖两条、「F7 / Shift+F7」一行盖两条）。
/// - `show=false`：注册但不出卡（它的键位出现在另一条的 label 里）；
/// - `alias_of` 非空：**别名注册**（不同键盘布局的同义登记名，如 Backquote），
///   不出卡、无自己的文案，动作由调用方用主条同款 lambda 注册。
///
/// ## 已知历史形态（如实迁移，不改行为）
///
/// - `Ctrl+\``（spec）：登记名同时收 `` ` `` 与 `Backquote` 两种键盘布局
///   （原注释：`` ` `` 在不同布局上键名不一）——两条注册、一张卡。
/// - `Alt+↑/↓`（spec）：根级登记名 "alt-up"/"alt-down" 与后端键名
///   （"ArrowUp"/"ArrowDown"）不匹配——真移行是 `CodeEditor` 自己的
///   Alt+方向键分支，根级条目从未被派发命中过（对账按登记名走，不受影响）。
///   原样保留（键位归一是另一个单的事，不在本单扩散）。
/// - `Ctrl+Tab`：登记名 "tab"（win32 发 "Tab"，小写归一后匹配）。
/// - `Esc`：登记名 "escape"（win32 发 "Escape"，小写归一后匹配）。

#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/ui_root.hpp"

namespace gbcode_ui {

using st::ui::UiRoot;   // 本头自包含（不带 page.hpp 的 using）：全限定名太噪，别名收敛

/// 一条快捷键的登记单元（注册面 + 卡片面共用；`main.cpp` 与帮助卡都从它读）。
struct ShortcutSpec {
  /// 调用方侧的查找 id（`register_shortcut_by_id` / `shortcut_spec` 按它取）。
  std::string_view id{};
  /// **登记名**：`register_shortcut(key, …)` 的第一参（派发比对用，小写归一）。
  std::string_view key{};
  bool ctrl{false};
  bool shift{false};
  bool alt{false};
  bool meta{false};
  /// 帮助卡上的按键文案（如 "Ctrl+Shift+S"）。
  std::string_view label{};
  /// 帮助卡上的动作说明。
  std::string_view description{};
  /// 帮助卡分组标题（卡片按它聚合；组名相同的条目落进同一节）。
  std::string_view group{};
  /// 注册形态（见文件头「三类注册形态」）。
  enum class Reg : std::uint8_t {
    LetOverlayDecide,  // 浮层开着放行（编辑区命令不抢浮层的键）
    Consume,           // 无外层守卫，动作自己的返回值决定消费与否
    Never,             // 不注册（编辑器内处理，卡片照登）
  };
  Reg reg{Reg::LetOverlayDecide};
  /// 出不出卡（false = 注册但键位写在另一条的 label 里，见文件头「卡片口径」）。
  bool show{true};
  /// 非空 = 别名注册（同义登记名）：不出卡、无文案，与主条同动作。
  std::string_view alias_of{};
};

/// 单源表本体（38 条）。顺序即注册顺序：派发对同键位是「后注册优先」，虽然
/// 当前表内无同键同修饰键的重复条目（顺序行为中性），仍按原 main.cpp 注册块
/// 的先后整理成组，改动表序前先确认无同键位冲突。
///
/// ⚠ 新增/改键位的**唯一入口**是这张表：改这里 = 注册 + 帮助卡 + 对账测试三处
/// 同步变。在 main.cpp 里另行 `register_shortcut` 会绕过对账——启动时的
/// `log_shortcut_drift` 会把它点名出来（表外注册进卡片「其他」节兜底可见）。
inline constexpr std::array<ShortcutSpec, 44> kShortcutSpecs{
    // —— 文件 ——
    ShortcutSpec{.id = "save", .key = "s", .ctrl = true,
                 .label = "Ctrl+S", .description = "保存",
                 .group = "文件"},
    ShortcutSpec{.id = "save-as", .key = "s", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+S", .description = "另存为",
                 .group = "文件"},
    ShortcutSpec{.id = "save-all", .key = "s", .ctrl = true, .alt = true,
                 .label = "Ctrl+Alt+S", .description = "全部保存",
                 .group = "文件"},
    ShortcutSpec{.id = "close", .key = "w", .ctrl = true,
                 .label = "Ctrl+W", .description = "关闭当前编辑器",
                 .group = "文件"},
    ShortcutSpec{.id = "open", .key = "o", .ctrl = true,
                 .label = "Ctrl+O", .description = "打开文件",
                 .group = "文件"},
    ShortcutSpec{.id = "open-folder", .key = "o", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+O", .description = "打开文件夹（换工作区）",
                 .group = "文件"},
    ShortcutSpec{.id = "new", .key = "n", .ctrl = true,
                 .label = "Ctrl+N", .description = "新建文件",
                 .group = "文件"},
    // —— 编辑（前三条由 CodeEditor 自理：不注册，卡片标注「编辑器内」）——
    ShortcutSpec{.id = "undo-redo", .key = "z", .ctrl = true,
                 .label = "Ctrl+Z/Y", .description = "撤销 / 重做（编辑器内）",
                 .group = "编辑", .reg = ShortcutSpec::Reg::Never},
    ShortcutSpec{.id = "select-all", .key = "a", .ctrl = true,
                 .label = "Ctrl+A", .description = "全选（编辑器内）",
                 .group = "编辑", .reg = ShortcutSpec::Reg::Never},
    ShortcutSpec{.id = "toggle-comment", .key = "/", .ctrl = true,
                 .label = "Ctrl+/", .description = "切换行注释（编辑器内）",
                 .group = "编辑", .reg = ShortcutSpec::Reg::Never},
    ShortcutSpec{.id = "select-next-occurrence", .key = "d", .ctrl = true,
                 .label = "Ctrl+D", .description = "选中下一处同词",
                 .group = "编辑"},
    ShortcutSpec{.id = "move-line-up", .key = "alt-up", .alt = true,
                 .label = "Alt+↑", .description = "上移当前行",
                 .group = "编辑"},
    ShortcutSpec{.id = "move-line-down", .key = "alt-down", .alt = true,
                 .label = "Alt+↓", .description = "下移当前行",
                 .group = "编辑"},
    ShortcutSpec{.id = "rename", .key = "f2",
                 .label = "F2", .description = "重命名符号",
                 .group = "编辑", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "format", .key = "f", .shift = true, .alt = true,
                 .label = "Shift+Alt+F", .description = "格式化",
                 .group = "编辑", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "completion", .key = "space", .ctrl = true,
                 .label = "Ctrl+Space", .description = "手动触发补全",
                 .group = "编辑", .reg = ShortcutSpec::Reg::Consume},
    // —— 导航 ——
    ShortcutSpec{.id = "find", .key = "f", .ctrl = true,
                 .label = "Ctrl+F/Ctrl+H", .description = "查找 / 替换",
                 .group = "导航"},
    ShortcutSpec{.id = "replace", .key = "h", .ctrl = true,
                 .label = "Ctrl+H", .description = "查找替换",
                 .group = "导航", .show = false},   // 键位已写进 find 的 label
    ShortcutSpec{.id = "goto-line", .key = "g", .ctrl = true,
                 .label = "Ctrl+G", .description = "转到行…",
                 .group = "导航"},
    ShortcutSpec{.id = "quick-open", .key = "p", .ctrl = true,
                 .label = "Ctrl+P", .description = "快速打开文件",
                 .group = "导航"},
    ShortcutSpec{.id = "palette", .key = "p", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+P", .description = "命令面板",
                 .group = "导航"},
    ShortcutSpec{.id = "symbols", .key = "t", .ctrl = true,
                 .label = "Ctrl+T", .description = "工作区符号",
                 .group = "导航"},
    ShortcutSpec{.id = "view-explorer", .key = "e", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+E/F/G", .description = "资源管理器/搜索/变更",
                 .group = "导航"},
    ShortcutSpec{.id = "view-search", .key = "f", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+F", .description = "切到搜索",
                 .group = "导航", .show = false},   // 键位已写进 view-explorer 的 label
    ShortcutSpec{.id = "view-git", .key = "g", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+G", .description = "切到变更",
                 .group = "导航", .show = false},   // 同上
    ShortcutSpec{.id = "next-tab", .key = "tab", .ctrl = true,
                 .label = "Ctrl+Tab", .description = "循环切换标签",
                 .group = "导航"},
    ShortcutSpec{.id = "toggle-sidebar", .key = "b", .ctrl = true,
                 .label = "Ctrl+B", .description = "切换侧栏",
                 .group = "导航"},
    // —— 页面缩放（复用 DPI 缩放链路）——
    //
    // 键位取 VSCode/浏览器惯例：`Ctrl+=` 放大、`Ctrl+-` 缩小、`Ctrl+0` 重置。
    // 注意 `Ctrl+=` 的 key 是 `equal`（等号键），不是 `plus`——后者要按 Shift，
    // 而"放大"是高频动作，不该要求 Shift。`Ctrl+-` 同理用 `minus`。
    ShortcutSpec{.id = "zoom-in", .key = "equal", .ctrl = true,
                 .label = "Ctrl+=", .description = "放大页面",
                 .group = "视图"},
    ShortcutSpec{.id = "zoom-out", .key = "minus", .ctrl = true,
                 .label = "Ctrl+-", .description = "缩小页面",
                 .group = "视图"},
    ShortcutSpec{.id = "zoom-reset", .key = "0", .ctrl = true,
                 .label = "Ctrl+0", .description = "重置页面缩放",
                 .group = "视图"},
    ShortcutSpec{.id = "next-problem", .key = "f8",
                 .label = "F8", .description = "下一处问题",
                 .group = "导航", .reg = ShortcutSpec::Reg::Consume},
    // —— 跳转（F12 家族）——
    ShortcutSpec{.id = "definition", .key = "f12",
                 .label = "F12", .description = "转到定义",
                 .group = "跳转（F12 家族）", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "references", .key = "f12", .shift = true,
                 .label = "Shift+F12", .description = "查找引用",
                 .group = "跳转（F12 家族）", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "declaration", .key = "f12", .alt = true,
                 .label = "Alt+F12", .description = "查看声明",
                 .group = "跳转（F12 家族）", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "implementation", .key = "f12", .ctrl = true,
                 .label = "Ctrl+F12", .description = "转到实现",
                 .group = "跳转（F12 家族）", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "navigate-back", .key = "ArrowLeft", .alt = true,
                 .label = "Alt+←", .description = "返回上一个光标位置",
                 .group = "跳转（F12 家族）", .reg = ShortcutSpec::Reg::Consume},
    // —— 差异视图 ——
    ShortcutSpec{.id = "next-hunk", .key = "f7",
                 .label = "F7 / Shift+F7", .description = "下一个 / 上一个差异",
                 .group = "差异视图", .reg = ShortcutSpec::Reg::Consume},
    ShortcutSpec{.id = "prev-hunk", .key = "f7", .shift = true,
                 .label = "Shift+F7", .description = "上一个差异",
                 .group = "差异视图", .reg = ShortcutSpec::Reg::Consume,
                 .show = false},   // 键位已写进 next-hunk 的 label
    // —— 查看 ——
    ShortcutSpec{.id = "toggle-bottom", .key = "j", .ctrl = true,
                 .label = "Ctrl+J", .description = "切换底部面板",
                 .group = "查看"},
    ShortcutSpec{.id = "help", .key = "k", .ctrl = true,
                 .label = "Ctrl+K", .description = "本帮助",
                 .group = "查看"},
    ShortcutSpec{.id = "toggle-terminal", .key = "`", .ctrl = true,
                 .label = "Ctrl+`", .description = "切换终端（VSCode 同键位）",
                 .group = "查看"},
    ShortcutSpec{.id = "toggle-terminal-alt", .key = "Backquote", .ctrl = true,
                 .alias_of = "toggle-terminal"},   // 别名布局：不出卡、同动作
    ShortcutSpec{.id = "md-preview", .key = "v", .ctrl = true, .shift = true,
                 .label = "Ctrl+Shift+V", .description = "Markdown 预览/源码切换",
                 .group = "查看"},
    ShortcutSpec{.id = "escape", .key = "escape",
                 .label = "Esc", .description = "关闭浮层",
                 .group = "查看", .reg = ShortcutSpec::Reg::Consume},
};

/// 按 id 取表项（不存在返回 nullptr；调用方用它拿动作注册的键位/守卫口径）。
[[nodiscard]] inline auto shortcut_spec(std::string_view id) -> const ShortcutSpec* {
  for (const ShortcutSpec& spec : kShortcutSpecs) {
    if (spec.id == id) return &spec;
  }
  return nullptr;
}

/// 键名的**小写归一**（与 `UiRoot::register_shortcut` 同一口径：登记时比对前统一
/// 小写，枚举面 `shortcuts()` 回的是归一后的名）。对账/卡片匹配必须走它——
/// 表里写 "ArrowLeft"（展示习惯）而注册表里存 "arrowleft"，直接 string 比较会把
/// 自己注册的条目认成「表外」（实测正是 navigate-back 那条被误报）。
[[nodiscard]] inline auto normalized_key(std::string_view key) -> std::string {
  std::string out(key);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

/// 把一条 spec 落成 `UiRoot::Shortcut`（键位/修饰键从表来，不再手抄第二份）。
[[nodiscard]] inline auto spec_mods(const ShortcutSpec& spec) -> UiRoot::Shortcut {
  UiRoot::Shortcut mods{};
  mods.key = normalized_key(spec.key);   // 与注册归一同口径（register 也会归一，双保险）
  mods.ctrl = spec.ctrl;
  mods.shift = spec.shift;
  mods.alt = spec.alt;
  mods.meta = spec.meta;
  return mods;
}

/// 按 id 注册一条快捷键（单源表的注册入口；`main.cpp` 只给动作，不给键位）。
///
/// 动作是 `std::function<bool()>`：返回值 = 消费与否（原注册块里 F7 无差异视图
/// 返回 false 让路、Esc 无事可关返回 false，都由动作自己表达，包装层不越权）。
/// 守卫口径由 spec 的 `reg` 决定（见文件头「三类注册形态」）——`LetOverlayDecide`
/// 才装 `overlay_open` 外层守卫。id 不在表里（或 Never 条）= 调用方写错了，
/// 返回 false：这正是「表外注册」要被抓住的形态（对账测试与启动检查都不认它）。
inline auto register_shortcut_by_id(UiRoot& root, std::string_view id,
                                    std::function<bool()> action,
                                    std::function<bool()> overlay_open = {})
    -> bool {
  const ShortcutSpec* spec = shortcut_spec(id);
  if (spec == nullptr || spec->reg == ShortcutSpec::Reg::Never) return false;
  std::function<bool()> handler;
  if (spec->reg == ShortcutSpec::Reg::LetOverlayDecide) {
    // 浮层开着 = 这次按键属于浮层：**不消费**，让它继续下沉到浮层里的输入框
    //（原 bind 助手与显式守卫注册的同一语义）。
    handler = [action = std::move(action), overlay_open = std::move(overlay_open)]() {
      if (overlay_open && overlay_open()) return false;
      return action();
    };
  } else {
    handler = [action = std::move(action)]() { return action(); };
  }
  return root.register_shortcut(std::string(spec->key), spec_mods(*spec),
                                std::move(handler));
}

/// 对账差异：单源表 ⇄ 注册表快照（三个方向，防两侧各自烂掉）。
///
/// - `unknown`：注册表里有、表里没有（或修饰键对不上）→ **表外注册**：
///   注册块绕过了单源表——帮助卡不认识它，静默漂移复发（33 轮病灶）；
/// - `missing`：表里 `reg != Never`、注册表里没有 → **漏注册**：表改了、
///   注册块没跟上（卡片登了键位但按键没反应）；
/// - `reserved`：`unknown` 里属于 Never 条的键位 → 编辑器内的键被根级抢注
///   （抢了就会「浮层里打字被编辑命令吃掉」）。
/// 匹配口径：登记名 + 四个修饰键位（与覆盖判定/unregister 的口径一致；
/// 「同键位族」（f12 四条）靠修饰键区分，天然多行共存）。
struct ShortcutDrift {
  std::vector<std::string> unknown;   // 表外注册（含修饰键形态）
  std::vector<std::string> missing;   // 漏注册（按 spec id 报）
  std::vector<std::string> reserved;  // Never 键位被抢注（unknown 的子集）
};
[[nodiscard]] inline auto shortcut_drift(const std::vector<UiRoot::ShortcutInfo>& live)
    -> ShortcutDrift {
  ShortcutDrift drift;
  // 注册面期待集：表里全部「应注册」条目（Never 除外，别名在内）。
  std::vector<const ShortcutSpec*> expected;
  for (const ShortcutSpec& spec : kShortcutSpecs) {
    if (spec.reg != ShortcutSpec::Reg::Never) expected.push_back(&spec);
  }
  const auto matches = [](const ShortcutSpec& spec, const UiRoot::ShortcutInfo& info) {
    // 键名比对走归一（表里是展示习惯的大小写，注册表里是归一名）。
    return normalized_key(spec.key) == info.name && spec.ctrl == info.mods.ctrl &&
           spec.shift == info.mods.shift && spec.alt == info.mods.alt &&
           spec.meta == info.mods.meta;
  };
  for (const UiRoot::ShortcutInfo& info : live) {
    bool found = false;
    for (const ShortcutSpec* spec : expected) {
      if (matches(*spec, info)) {
        found = true;
        break;
      }
    }
    if (!found) {
      const std::string name = std::format("{}{}{}{}", info.name,
                                           info.mods.ctrl ? "+ctrl" : "",
                                           info.mods.shift ? "+shift" : "",
                                           info.mods.alt ? "+alt" : "");
      drift.unknown.push_back(name);
      for (const ShortcutSpec& spec : kShortcutSpecs) {
        if (spec.reg == ShortcutSpec::Reg::Never && matches(spec, info)) {
          drift.reserved.push_back(name);
          break;
        }
      }
    }
  }
  for (const ShortcutSpec* spec : expected) {
    bool found = false;
    for (const UiRoot::ShortcutInfo& info : live) {
      if (matches(*spec, info)) {
        found = true;
        break;
      }
    }
    if (!found) drift.missing.push_back(std::string(spec->id));
  }
  return drift;
}

/// 帮助卡正文：从**注册表快照**生成（卡片 ⇄ 注册面同源，不是 ⇄ spec 表：
/// spec 表决定「注册什么」，注册表决定「卡片登什么」，中间由对账锁死）。
///
/// 形态：按 spec 的 group 聚合（组序 = 表序中首见序），每行 `  <label>\t<说明>`；
/// `show=false` 与别名不出行；Never 条照登（它们本来就不注册，卡片是其唯一面）；
/// **应注册条目以 live 为准**——不在注册面就不登（绝不登一条「照着按没反应」
/// 的键位，那正是 33 轮手写卡的病灶；对账测试同时把漏注册钉红）；
/// 注册表里有、表里没有的（表外注册，对账应红）进末尾「其他」节兜底可见。
[[nodiscard]] inline auto shortcuts_card_body(const std::vector<UiRoot::ShortcutInfo>& live)
    -> std::string {
  const auto is_live = [&live](const ShortcutSpec& spec) {
    const std::string key = normalized_key(spec.key);
    for (const UiRoot::ShortcutInfo& info : live) {
      if (key == info.name && info.mods.ctrl == spec.ctrl &&
          info.mods.shift == spec.shift && info.mods.alt == spec.alt &&
          info.mods.meta == spec.meta) {
        return true;
      }
    }
    return false;
  };
  std::vector<std::string_view> groups;                      // 组序（首见序）
  std::vector<std::vector<const ShortcutSpec*>> rows;        // 每组的行
  for (const ShortcutSpec& spec : kShortcutSpecs) {
    if (!spec.alias_of.empty()) continue;                    // 别名不出行
    if (!spec.show) continue;                                // 键位写在别的行里
    if (spec.reg != ShortcutSpec::Reg::Never && !is_live(spec)) continue;  // 漏注册不登
    std::size_t group_index = groups.size();
    for (std::size_t index = 0; index < groups.size(); ++index) {
      if (groups[index] == spec.group) {
        group_index = index;
        break;
      }
    }
    if (group_index == groups.size()) {
      groups.push_back(spec.group);
      rows.emplace_back();
    }
    rows[group_index].push_back(&spec);
  }
  std::string body;
  for (std::size_t index = 0; index < groups.size(); ++index) {
    body += std::string(groups[index]);
    body += '\n';
    for (const ShortcutSpec* spec : rows[index]) {
      body += std::format("  {}\t{}\n", spec->label, spec->description);
    }
    body += '\n';
  }
  const ShortcutDrift drift = shortcut_drift(live);
  if (!drift.unknown.empty()) {
    body += "其他（未登记进快捷键表——请反馈）\n";
    for (const std::string& name : drift.unknown) {
      body += std::format("  {}\t（表外注册）\n", name);
    }
    body += '\n';
  }
  return body;
}

}  // namespace gbcode_ui
