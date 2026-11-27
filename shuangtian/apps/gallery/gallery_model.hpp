#pragma once

/// 画廊的**目录模型**：页面清单（含分区）与组件目录（分类 + 展示位索引）。
///
/// ## 为什么要把清单抽出来（重设计的第一性改动）
///
/// 旧画廊的三份事实各写一遍：页面清单在 `pages.cpp` 的 `page_specs()`，
/// 组件展示位散在 56 个 `make_card(...)` 调用点上，"哪些组件被展示了"还要靠
/// `tests/support/gallery_exhibits.hpp` 手工对账。于是：
///
/// - 新增组件 → 要记得补卡、补登记、可能补豁免（漏一处就静默漂移）；
/// - 想知道"这个组件在哪一页" → 只能 grep 代码；
/// - 想在界面上做"组件浏览器"（分类 + 搜索）→ 没有数据可筛。
///
/// 本头把**导航与分类**变成数据（页面清单 + 分类清单 + 展示位索引）。
/// 组件卡本身仍在 `pages.cpp` 手搭——它们各自要不同的构造与接线，自动生成
/// 没有意义；但每张卡在分类视图里的归属必须在 `kExhibitIndex` 里有一条，
/// 且由 `tests/gallery_component_parity_test.cpp` 与启动树自检双向核对。

#include <array>
#include <cstddef>
#include <string_view>

namespace gallery {

// ————————————————————————————————————————————————————————————————————————————
// 页面清单
// ————————————————————————————————————————————————————————————————————————————

/// 侧栏分区：同分区的页面挨着排，并有一条分区标题。
///
/// 为何分区而不是平铺 12 个按钮：导航要回答"我想干什么"（看组件 / 查质检 /
/// 演练能力），而不是"框架有哪 12 页"。分区让 12 项读起来是 5 组。
enum class PageSection : std::size_t {
  Start = 0,   ///< 概览
  Catalog,     ///< 组件目录
  Quality,     ///< 框架质检（属性面 / 自检台）
  Capability,  ///< 能力演练（动效 / 数据 / 声明式 / 控制通道 / 嵌入 / 能力矩阵）
  Meta,        ///< 关于
  Count,
};

[[nodiscard]] constexpr auto section_label(PageSection section) noexcept -> std::string_view {
  switch (section) {
    case PageSection::Start: return "开始";
    case PageSection::Catalog: return "组件目录";
    case PageSection::Quality: return "框架质检";
    case PageSection::Capability: return "能力演练";
    case PageSection::Meta: return "关于";
    case PageSection::Count: break;
  }
  return {};
}

/// 一页的规格。
struct PageSpec {
  /// 页面容器 id：`page-<id>`（语义树/选择器据此判断当前页）。
  std::string_view id{};
  /// 导航图标名（内置图标集里的名字）。
  std::string_view icon{};
  /// 导航文字与页标题。
  std::string_view label{};
  /// 页副标题（页内标题下方那行说明）。
  std::string_view subtitle{};
  /// 侧栏分区。
  PageSection section{PageSection::Start};
  /// 导航项的悬停提示（一句话说这一页"验什么"）。
  std::string_view hint{};
};

/// 页面清单（导航、切页、自检、e2e 共用同一份）。
///
/// 顺序即导航顺序；`nav-<id>` 是**自动化契约**（历史 e2e 脚本按它寻址），
/// 所以既有 id 一律不改，新增页追加。
inline constexpr std::array<PageSpec, 12> kPages{{
    {.id = "overview", .icon = "grid", .label = "概览",
     .subtitle = "一眼看全框架：实时指标 · 能力速览 · 质检结论",
     .section = PageSection::Start, .hint = "实时指标与各页入口"},

    {.id = "components", .icon = "layers", .label = "组件",
     .subtitle = "组件浏览器：按分类筛选 + 搜索，逐个看交互与属性面",
     .section = PageSection::Catalog, .hint = "全部注册组件，按分类筛选"},

    {.id = "properties", .icon = "sliders", .label = "属性面",
     .subtitle = "由 PropertyDesc 自动生成面板：改一项，看它真的落到元素上",
     .section = PageSection::Quality, .hint = "属性自省与自动面板"},
    {.id = "audit", .icon = "check", .label = "自检台",
     .subtitle = "把框架的运行时不变式变成界面上的 PASS / FAIL 表",
     .section = PageSection::Quality, .hint = "注册表/树/属性面/主题/像素"},

    {.id = "motion", .icon = "sparkles", .label = "动效",
     .subtitle = "CSS 式动画：transition 补间 · keyframes 预设 · 重播",
     .section = PageSection::Capability, .hint = "动画与过渡"},
    {.id = "data", .icon = "database", .label = "数据",
     .subtitle = "表格 / 列表 / 键值 / Mermaid / 思维导图 / 时间轴",
     .section = PageSection::Capability, .hint = "数据展示组件族"},
    {.id = "declarative", .icon = "sparkles", .label = "声明式",
     .subtitle = "描述 |状态 → 界面|，重组由框架做（对标 Compose / ArkTS）",
     .section = PageSection::Capability, .hint = "状态驱动重组"},
    {.id = "control", .icon = "cpu", .label = "控制通道",
     .subtitle = "远程可驱动：协议方法 / 操作靶区 / 端点",
     .section = PageSection::Capability, .hint = "st-control/1 协议"},
    {.id = "native-host", .icon = "layers", .label = "原生嵌入",
     .subtitle = "外部程序窗口嵌进界面（不进 framebuffer）",
     .section = PageSection::Capability, .hint = "子窗口嵌入"},
    {.id = "webview", .icon = "globe", .label = "嵌入浏览器",
     .subtitle = "可编程控制：URL / 内嵌资源包 / 执行 JS",
     .section = PageSection::Capability, .hint = "WebView 通路"},
    {.id = "capabilities", .icon = "cube", .label = "能力矩阵",
     .subtitle = "三维 / 关系图 / 标签层 / 慢内容 / XML / 脚本 / 热重载",
     .section = PageSection::Capability, .hint = "进阶能力与可用性标注"},

    {.id = "about", .icon = "info", .label = "关于",
     .subtitle = "版本与分层",
     .section = PageSection::Meta, .hint = "版本、分层、Markdown 全语法"},
}};

inline constexpr std::size_t kPageCount = kPages.size();

/// 按 id 找页面下标（找不到返回 `kPageCount`）。
[[nodiscard]] constexpr auto page_index(std::string_view id) noexcept -> std::size_t {
  for (std::size_t index = 0; index < kPageCount; ++index) {
    if (kPages[index].id == id) return index;
  }
  return kPageCount;
}

/// 侧栏里某分区包含哪些页下标（按导航顺序）。
[[nodiscard]] constexpr auto pages_in_section(PageSection section)
    -> std::array<std::size_t, kPageCount> {
  std::array<std::size_t, kPageCount> out{};
  for (std::size_t index = 0; index < kPageCount; ++index) out[index] = kPageCount;
  std::size_t write = 0;
  for (std::size_t index = 0; index < kPageCount; ++index) {
    if (kPages[index].section == section) out[write++] = index;
  }
  return out;
}

// ————————————————————————————————————————————————————————————————————————————
// 组件目录（组件浏览器左栏）
// ————————————————————————————————————————————————————————————————————————————

/// 一个分类：名字 + 一句话（"这一类是干什么的"）。
///
/// 为何分类是**固定的 8 组**而不是按注册表自动聚类：分类是**给人读的导航**，
/// 它表达的是"我要找的是哪一类东西"（开关 vs 输入 vs 展示），而注册表只有名字。
/// 自动聚类永远会有一两个勉强归类的桶，读的人反而更慢。
struct ExhibitSection {
  std::string_view id{};
  std::string_view title{};
  std::string_view hint{};
};

inline constexpr std::array<ExhibitSection, 9> kExhibitSections{{
    {.id = "basic", .title = "基础与文字", .hint = "任何界面的地基：按钮 / 图标 / 文字 / 容器"},
    {.id = "toggle", .title = "开关与勾选", .hint = "布尔值：立即生效（Switch）还是提交生效（Checkbox）"},
    {.id = "input", .title = "输入与取值", .hint = "连续量 / 离散量 / 文本 / 表单行"},
    {.id = "select", .title = "下拉与浮层", .hint = "选项放不下时；模态、轻提示、右键菜单"},
    {.id = "nav", .title = "导航与结构", .hint = "层级、分步、页签、工具条、容器与布局"},
    {.id = "feedback", .title = "反馈与状态", .hint = "进度、空态、通知、徽标、骨架"},
    {.id = "data", .title = "数据与图形", .hint = "表格、列表、图、时间轴、连线"},
    {.id = "editor", .title = "编辑器与文档", .hint = "代码、差异、合并、终端、Markdown、HTML"},
    {.id = "advanced", .title = "进阶与嵌入", .hint = "三维、视口、主题、窗框、外部窗口 / 浏览器"},
}};

/// 按 id 找分类下标（找不到返回 `kExhibitSections.size()`）。
[[nodiscard]] constexpr auto section_index(std::string_view id) noexcept -> std::size_t {
  for (std::size_t index = 0; index < kExhibitSections.size(); ++index) {
    if (kExhibitSections[index].id == id) return index;
  }
  return kExhibitSections.size();
}

}  // namespace gallery
