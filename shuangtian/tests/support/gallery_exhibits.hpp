#pragma once

/// gallery 展示集与豁免清单（组件页对账的「展示侧」权威源，tests 与 gallery 共用）。
///
/// ## 这份清单守的是什么（BACKLOG「gallery 组件页对账机制」）
///
/// gallery 自称「控件全集」，但展示卡是**手搭**的：新组件进了 `ST_COMPONENT_LIST`
/// （dsl.cpp 注册表，组件全集的唯一权威清单）却没人补卡时——画廊不说、编译器不说、
/// 任何机制都不说。v0.1.5 专项「补齐 28 组件」之后新增组件零跟进，缺口静默长大，
/// 才被反推发现。治法与「33 轮快捷键卡」同族：**表与代码要么同源、要么有对账**。
/// 本头文件是对账的展示侧：`tests/gallery_component_parity_test.cpp` 断言
/// **注册表 − 豁免清单 == 展示集**（差集为空）——组件表加项不补卡即红。
///
/// ## 登记口径
///
/// - **kGalleryExhibits**：gallery 代码（examples/gallery/{pages,main}.cpp）**直接
///   实例化**的内置注册组件，按注册名登记（dsl.cpp `ST_COMPONENT_LIST` 的第二列）。
///   `dsl::custom<T>` 形态的使用同样算直接使用（gallery 当前未用，别的示例有）。
/// - **kGalleryExempt**：注册表里确实不该有展示卡的组件，逐条写理由。两条铁律：
///   ①豁免必须给出「为什么」——写不出理由的缺卡是缺口，不是豁免；
///   ②理由以「【待补卡】」开头的条目是**缺口登记**：补卡当轮移出本清单、
///   同时登记进 kGalleryExhibits，否则豁免清单会烂成缺卡的坟场（正是本机制要防的病）。
///
/// ## 不在对账域内的树上类型（启动树自检的放行清单）
///
/// gallery 启动时遍历 UI 真值树（main.cpp `audit_gallery_tree`）收集实际出现的
/// 组件 `type()`，与登记集比对（见下）。运行时真值树上还会出现这些**非注册名**
/// 或**注入型**类型，自检时需放行：
/// - 父组件内建注入：ListItem（List）、ScrollBar（ScrollView）——结构性豁免同款；
///   MenuPanel 同属注入型（菜单卡触发后的面板），有 MenuBar/ContextMenu 卡即随之；
///   TitleBar：WindowFrame 构造内建（window_frame.cpp:27）+ gallery 另有直接实例化
///   的标题栏卡（pages.cpp:776）——注入实例与直接卡同 type()，登记在展示集；
/// - 非注册的框架组件：SceneView（gallery 直接 new，但不在 `ST_COMPONENT_LIST`，
///   无展示义务；且它未 override `type()`，树上以基类默认「Element」露面——
///   放行名单按树上真名登记，不按类名想当然）；
/// - gallery 自绘演示类：ProgressRow / IconCell / SvgIconCell（pages.cpp 本地类）；
///   SwatchCell 是已无实例化点的死类（不上树），不需放行；
/// - 交互时懒创建：Dialog/Toast/ContextMenu/FileDialog/CompletionPopup 见
///   kGalleryLazy；SelectPanel（Select 开面板）与 Tooltip（UiRoot 悬停 hint-bubble）
///   是**框架懒注入**的非登记实例——组件页另有直接实例化的 Tooltip 卡（同 type
///   不同实例，已入展示集），自检按来源放行：它们的 type 出现在树上不红，
///   因为触发条件是交互而非登记缺口。
///
/// 放行的准确含义：这些 type **不上树才红**——例如哪天 List 不再内建 ListItem、
/// 或标题栏卡被删，自检会点名缺失（防「放行清单烂成坟场」，与豁免两条铁律同族）。
/// 全展示断言（每个非懒 exhibit 在启动树上至少出现一次）则把「登记了却没真卡」
/// 的头号缺口形态当场钉红——从「人工 grep 提取」到「运行时实证」的升级点。

#include <array>
#include <string_view>
#include <utility>

namespace gallery {

/// gallery 展示集：gallery 代码直接实例化的内置注册组件（注册名口径）。
/// 与 examples/gallery/{pages,main}.cpp 的实例化点一一对应。
/// 「直接实例化」含两种形态：建卡时实例化，以及懒创建组件经按钮触发构造后挂
/// 叠加层（后者另列 kGalleryLazy；树自检时两类都在场断言里放行）。
inline constexpr std::array<std::string_view, 44> kGalleryExhibits{
    "Panel",       "Text",       "Heading",     "Icon",        "Button",
    "Card",        "Divider",    "KeyValueRow", "Input",       "TextArea",
    "Checkbox",    "Radio",      "Switch",      "Slider",      "Select",
    "List",        "Table",      "ScrollView",  "SplitView",   "Tabs",
    "ProgressBar", "Spinner",    "Badge",       "Chip",        "Avatar",
    "Dialog",      "Toast",      "MarkdownView", "TitleBar",   "WindowFrame",
    "Tree",        "Tooltip",    "ColorPicker", "Viewport",
    "CodeEditor",  "DiffView",   "CompletionPopup", "CommandPalette",
    "TimelineView", "MergeView", "MenuBar",     "ContextMenu", "FileDialog",
    "Terminal",
};

/// gallery 懒创建展示集：交互时才构造、挂 UiRoot 叠加层的展示卡组件。
/// 启动树上不可见是它们的存在形态（按钮触发，见括号触发钮 id）；树自检的
/// 「全展示断言」只管非懒 exhibits，本表中的名字另经触发钮覆盖验证。
inline constexpr std::array<std::string_view, 7> kGalleryLazy{
    // Dialog（btn-open-dialog）/ Toast（btn-toast-ok）/ Tooltip（btn-tooltip）/
    // ContextMenu（btn-context-open）/ FileDialog（btn-filedialog-open）/
    // CompletionPopup（btn-completion-open）/ CommandPalette（btn-palette-open）
    "Dialog", "Toast", "Tooltip", "ContextMenu", "FileDialog", "CompletionPopup",
    "CommandPalette",
};

/// gallery 豁免清单：注册表里不该有展示卡的组件（注册名 + 理由）。
/// 结构性豁免 4 条，与展示集 44 条合起来正好覆盖注册表 48 项（48 = 44 + 4，
/// 【待补卡】清单已随三包补卡全部清零：首包 Tree/Tooltip/ColorPicker/Viewport，
/// 二包 CodeEditor/DiffView/CompletionPopup/CommandPalette，
/// 三包 TimelineView/MergeView/MenuBar/ContextMenu/FileDialog/Terminal）。
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4>
    kGalleryExempt{
        std::pair{"Spacer", "布局占位：无可视内容，无展示价值"},
        std::pair{"ListItem", "父组件注入：List::add_item 内建创建（list.cpp:291），列表卡随之展示，不单独立卡"},
        std::pair{"ScrollBar", "父组件注入：ScrollView 构造内建（scroll.cpp:156），页面滚动随之展示，不单独立卡"},
        std::pair{"MenuPanel", "父组件注入：MenuBar::make_panel 内建创建（menu.cpp:386），有 MenuBar 卡即随之展示（菜单栏/右键菜单两卡的面板都是它）"},
    };

}  // namespace gallery

// ————逆向验证设计（终报验证清单③，控制面批：纯推演落档，未实跑）————————————————
//
// 计数演进：本段推演写于豁免 18 / 展示集 30 时；首包补卡（Tree/Tooltip/ColorPicker/
// Viewport，控制面单 5552903-首包）后为豁免 14 / 展示集 34；第二包补编辑器族四卡
//（CodeEditor/DiffView/CompletionPopup/CommandPalette，控制面单 5552903-二包）后
// 为豁免 10 / 展示集 38；第三包补齐尾六卡（TimelineView/MergeView/MenuBar/
// ContextMenu/FileDialog/Terminal，控制面单 5552903-三包）后【待补卡】清零，
// 现为结构性豁免 4 / 展示集 44（48 = 44 + 4，注册表全覆盖）。下文数字保留当时快照，
// 实验①的举例（Tree）已随补卡移出豁免，【待补卡】也已清零——复现时改删任一
// 结构性豁免条目（如 Spacer）即可，报文形态不变（缺卡名单换 [Spacer]）。
//
// 两处变红实验的预期行为。报文格式依据：test_runner.cpp:146 `"{}:{}: {}"`（文件:行: 消息）；
// ST_FAIL 前缀「显式失败: 」（test.hpp:265）；ST_CHECK 前缀「断言失败: 」（test.hpp:187）；
// 用例级输出 `FAIL <名> <耗时>` + 缩进消息 + `复现 st test <名>`（test_runner.cpp:281-290）。
// 文件路径按 __FILE__ 实际形态（stpm 以绝对路径编译，形如 D:/…/tests/gallery_component_parity_test.cpp，
// 下文简写为 <file>）。
//
// 实验①：注释掉 kGalleryExempt 里的任一【待补卡】条目（例：Tree）。
//   豁免 18→17，Tree 仍在注册表 48、仍不在展示集 30。用例①②⑤不受影响（交集/存在性/
//   理由形态均仍成立），唯主断言红：
//   gallery_parity_registry_minus_exemptions_equals_exhibits  →  FAIL，报文：
//     <file>:129: 显式失败: gallery 对账不平（注册表−豁免 31 项 vs 展示集 30 项）：缺卡 [Tree]；多登 []
//     复现 st test gallery_parity_registry_minus_exemptions_equals_exhibits
//   （missing=[Tree]：豁免被拿掉后 Tree 成了“没卡也没豁免”的裸缺口——这正是本机制
//   要当场暴露的头号形态：任何组件在两张名单里都找不到归处即红，缺口无法静默。）
//
// 实验②：kGalleryExhibits 里加一个不存在的名字（例："Bogus"）。
//   先决细节：std::array 的 size 参数须同步 30→31，漏改则元素过多——编译期就红
//   （测试构建失败，退 125），这本身就是第一道闸。正确加法（size=31）则两用例红：
//   a) gallery_parity_exhibits_all_exist_in_registry →  FAIL，报文：
//     <file>:104: 断言失败: registry.contains(name)
//     复现 st test gallery_parity_exhibits_all_exist_in_registry
//   （注意 ST_CHECK 的 #expr 只回显宏实参文本，不展开循环变量值——Bogus 名字本身
//   不在报文里，需结合下一处的 ST_FAIL 定位；这是框架宏形态的如实呈现，非缺陷。）
//   b) gallery_parity_registry_minus_exemptions_equals_exhibits  →  FAIL，报文：
//     <file>:129: 显式失败: gallery 对账不平（注册表−豁免 30 项 vs 展示集 31 项）：缺卡 []；多登 [Bogus]
//     复现 st test gallery_parity_registry_minus_exemptions_equals_exhibits
//   （stale=[Bogus]：展示集登记了注册表外的名字，登记口径漂移；b 的报文直接点名，
//   可精确补 a 的定位短板。）
//
// 实验覆盖结论：豁免侧被拿走 → 用例④报缺卡名单；展示侧多登 → 用例③④双红、④点名。
// 加上用例①的“补卡后忘移出豁免”（报文「同时在展示集与豁免集」）与用例②对豁免侧拼错
// 的存在性红，五用例把“新增组件必居其一（补卡/豁免）、名单必真、两名单不得互撞”
// 三个不变量全部钉住。实跑验证（正向）已由控制面验证回执批完成：5 用例编译过+全绿。
