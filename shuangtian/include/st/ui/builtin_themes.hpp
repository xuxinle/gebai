#pragma once

/// **内置主题库**：移植自歌白（GEBAI）前端的 10 套配色主题。
///
/// ## 为什么要做成内置资源而不是外部文件
///
/// 这些主题是产品的观感基线，随二进制一起分发才符合"开箱即用"。
/// 外部 JSON 仍可覆盖/新增（`theme_io.hpp` 的 `--theme-file`），
/// 但那要用户自己带文件；内置库是"一个名字就能切"的那一层。
///
/// > **默认两档（`light`/`dark`）已经是亚克力质感**（色相取歌白
/// > `themes/acrylic.css`，明度按霜天的最小可辨拉开），所以这里**不提供 `acrylic`**——
/// > 它只会与 `dark` 形成两套几乎重叠的深色主题，让人不知道该选哪个。
///
/// ## 与歌白 CSS 的对应关系
///
/// 每份 JSON 是**稀疏覆盖**（`theme_io.hpp` 的格式），`base` 指 `light`/`dark`：
///
/// | 主题 | 基准 | 观感 |
/// |---|---|---|
/// | nebula | dark | 青绿主色 + 紫色辅色，主按钮渐变 + 弹层辉光 |
/// | cny | dark | 丝绒红底 + 中国红 + 鎏金 |
/// | cyberpunk | dark | 近黑紫底 + 霓虹粉×青双主色，极小圆角 |
/// | ink | light | 宣纸暖白 + 墨黑 + 朱砂，唯一固定亮色 |
/// | matrix | dark | 纯黑 + 荧光绿单色，圆角归零 |
/// | qinhan | dark | 玄漆 + 朱砂 + 鎏金 + 石青，矿物颜料色 |
/// | synthwave | dark | 紫底 + 粉×青双色，80 年代霓虹 |
/// | tokyo-night | dark | 深蓝紫 + 紫蓝，程序员配色 |
///
/// ## 移植时做的三处**刻意偏离**（数据本身不够用）
///
/// 1. **`surface_raised` 重算**：歌白的 `--bg-elev-2` 语义是"第二层面板"，
///    不一定比 `--bg-elev` 亮（`ink` 就是反的）。霜天的抬升面必须**更亮**
///    （弹层要压过背后内容），所以按明暗向白混合重算。
/// 2. **补齐霜天有、歌白没有的 token**（`surface_alt` / `border_subtle` /
///    `focus_ring` / `code_bg` / `selection` / `glow` …），规则与内置
///    `Theme::light()/dark()` 同一口径，不是随手编的。
/// 3. **毛玻璃 tint**：歌白只有 `saturate()`，霜天没有饱和度概念，
///    改用"不透明度"近似，并按明暗取 0.88 / 0.82 —— 这是弹层能否浮起的关键
///    （见 `tests/ui_overlay_lift_test.cpp` 的取向说明）。
///
/// ## 用法
///
/// ```cpp
/// if (auto theme = st::ui::builtin_theme("cyberpunk")) { root.set_theme(*theme); }
/// for (const auto& name : st::ui::builtin_theme_names()) { ... }
/// ```

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ui/theme.hpp"

namespace st::ui {

/// 按名字取内置主题；名字不存在返回 `nullopt`（**不猜、不回退**：
/// 静默给一个"差不多的"主题会让"我选的主题没生效"变成谜）。
///
/// `light` / `dark` 也认（等价于 `Theme::light()/dark()`），
/// 这样"统一用名字取主题"的调用方不需要为内置两档写特例。
[[nodiscard]] auto builtin_theme(std::string_view name) -> std::optional<Theme>;

/// 全部内置主题名（含 `light` / `dark`），按字母序。
/// 供 `--list-themes` 一类入口与"画廊逐主题遍历"使用。
///
/// ⚠ 这是**稳定枚举口径**（字典序、与展示无关）：给选择器排序请用
/// `builtin_theme_names_by_popularity()`——两个口径各管一件事，不要混用。
[[nodiscard]] auto builtin_theme_names() -> std::vector<std::string_view>;

/// 内置主题名，按**常用性**排（给主题选择器的色卡墙用）。
///
/// 为什么不直接给字母序：色卡墙是 5×2 网格，字母序把 `light`/`dark` 排在
/// 第 5、第 6 位（前者被 `cny`/`cyberpunk`/`ink`/`matrix` 顶到右半边），
/// 而**绝大多數人只在这两档之间切**——“默认项要第一眼看到”是选择器的基本形状。
///
/// 顺序：`light` → `dark` → 中性/通用 → 风格化。后两段内部按“离日常办公观感的远近”排：
///
/// | 位 | 主题 | 理由 |
/// |---|---|---|
/// | 1-2 | light / dark | 默认两档，切换频率最高 |
/// | 3 | ink | 亮色系里唯一带书卷气的一档（亮色用户的第二选择） |
/// | 4 | tokyo-night | 暗色系里最接近“普通 IDE”的一档 |
/// | 5-6 | nebula / qinhan | 有色偏但仍克制（青/砺与矿物色） |
/// | 7-8 | cny / cyberpunk | 强风格化，已是为特定场合选 |
/// | 9-10 | matrix / synthwave | 极强风格（纯黑荧光 / 霓虹），选它们的人自己知道要什么 |
///
/// **保底规则**：未登记在表里的名字（包括本函数写完之后新增的内置主题）
/// 按字母序接在末尾。否则“新加了一个主题、选择器里却没有它”会被当成
/// “主题加载失败”去查。
///
/// 注：这条规则的可测形式是 `sort_theme_names_by_popularity`——它接受**任意的**
/// 名字表，所以测试能传一个“表里没有的名字”进来真验一遍。
/// 直接在真实主题集上验是无效的：当前表恰好包含全部 10 个名字，
/// 尾部那段循环什么也不加，删了它也测不出来（实测踩过）。
[[nodiscard]] auto builtin_theme_names_by_popularity() -> std::vector<std::string_view>;

/// 把**任意一份**名字表按常用性重排（`builtin_theme_names_by_popularity` 的实现本体）。
///
/// 单独暴露是为了可测：保底规则（未登记的接在末尾）只有在输入里**真的存在**
/// 未登记名字时才会生效，而真实主题集里目前一个都没有——
/// 不把这一步抽成接受输入的纯函数，那条断言就永远量不到东西。
///
/// 保证：输出的元素集合与输入**完全相同**（不多不少、不重复）；
/// 已登记名字按表排先后，其余按输入原有次序（调用方传的是字母序时即字母序）。
[[nodiscard]] auto sort_theme_names_by_popularity(std::vector<std::string_view> names)
    -> std::vector<std::string_view>;

/// 内置主题的**中文显示名**（选择器 UI 用；英文名仍是稳定 id）。
/// 名字取自各主题的观感意象（见上表）：不存在的名字返回原名——
/// 外部 `--theme-file` 注册的主题没有中文登记，原名即最好的显示名。
[[nodiscard]] auto builtin_theme_display_name(std::string_view name) -> std::string_view;

/// 内置主题的**预览色**（选择器 UI 的色点用）：`bg` 与 `primary` 两色最能一眼认主题。
/// � `nullopt` 表示没登记（调用方用当前主题的色作占位，而非报错——
/// 选择器不能因为一个主题缺预览就整页画不出来）。
struct ThemePreviewColors {
  math::Color bg{};       ///< 主题底色
  math::Color primary{};  ///< 主题主色
};
[[nodiscard]] auto builtin_theme_preview_colors(std::string_view name)
    -> std::optional<ThemePreviewColors>;

}  // namespace st::ui
