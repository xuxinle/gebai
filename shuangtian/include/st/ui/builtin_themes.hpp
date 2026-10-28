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
[[nodiscard]] auto builtin_theme_names() -> std::vector<std::string_view>;

}  // namespace st::ui
