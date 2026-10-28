#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/builtin_themes.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::builtin_theme;
using st::ui::builtin_theme_names;

/// 歌白移植过来的 10 个主题必须**全部**能加载。
///
/// 这条测试的由来（两次真实事故）：
///   ① 生成脚本写了 `metrics.shadow_blur`——霜天没有这个 token，
///      解析直接失败 → `builtin_theme` 返回 nullopt → 应用**静默回退**到内置 dark。
///      症状是"界面能开、主题不生效"，靠看截图很难发现（深色主题都长得像）。
///   ② `--theme cyberpunk` 不生效且不报错，因为构造期绕过了 `compose_theme`。
/// 所以这里对每个名字都做**强断言**（不是"能返回就行"，还要检查调色板真的换了）。
ST_TEST(builtin_themes_all_load) {
  static const std::vector<std::string_view> expected{
      "cny",     "cyberpunk", "ink",       "matrix", "nebula",
      "qinhan",  "synthwave", "tokyo-night",
  };
  for (const std::string_view name : expected) {
    const auto theme = builtin_theme(name);
    ST_CHECK(theme.has_value());
    if (!theme.has_value()) {
      st::log::out("[builtin-theme] {} 加载失败（nullopt）\n", name);
      continue;
    }
    // 名字必须回填（`theme` 命令与日志都靠它，也用于断言"取到的就是请求的那个"）。
    ST_CHECK_EQ(theme->name(), std::string(name));
    // 调色板必须**真的不是**内置基准的默认值：否则说明解析成功但什么都没应用。
    const st::ui::Theme base = st::ui::Theme::by_mode(theme->mode());
    const bool same_bg = theme->colors().bg.to_hex() == base.colors().bg.to_hex();
    const bool same_primary =
        theme->colors().primary.to_hex() == base.colors().primary.to_hex();
    ST_CHECK(!(same_bg && same_primary));
  }
}

/// `light` / `dark` 也必须能从同一个入口取到（调用方不用为两档写特例）。
ST_TEST(builtin_themes_include_base_modes) {
  const auto light = builtin_theme("light");
  ST_CHECK(light.has_value());
  ST_CHECK(light.has_value() && light->mode() == st::ui::ThemeMode::Light);
  const auto dark = builtin_theme("dark");
  ST_CHECK(dark.has_value());
  ST_CHECK(dark.has_value() && dark->mode() == st::ui::ThemeMode::Dark);
}

/// 不存在的名字必须返回 nullopt（**不回退**）：
/// 静默给一个"差不多的"主题会让"我选的主题没生效"变成一个谜。
ST_TEST(builtin_themes_unknown_name_returns_nullopt) {
  ST_CHECK(!builtin_theme("nosuchtheme").has_value());
  ST_CHECK(!builtin_theme("").has_value());
  ST_CHECK(!builtin_theme("Cybezpunk").has_value());   // 大小写敏感
}

/// 名字表必须覆盖全部主题，且有序、无重复（供 `--list-themes` 类入口直接用）。
ST_TEST(builtin_theme_names_are_complete_and_sorted) {
  const auto names = builtin_theme_names();
  ST_CHECK(names.size() >= 10U);   // 8 个移植主题 + light + dark
  ST_CHECK(std::is_sorted(names.begin(), names.end()));
  ST_CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());   // 无重复
  for (const std::string_view must : {"light", "dark", "cyberpunk", "ink"}) {
    ST_CHECK(std::find(names.begin(), names.end(), must) != names.end());
  }
}

/// **层级不变式**：每套主题的 `surface_raised` 都必须比 `surface` 更亮。
///
/// 霜天靠"抬升面更亮"表达浮起（弹层压在内容之上）。歌白的 `--bg-elev-2`
/// 语义是"第二层面板"，不保证更亮——`ink` 就是反的（`#faf7ef` → `#f1ece0`）。
/// 移植时按明暗向白重算，这条断言锁住那个决定。
ST_TEST(builtin_themes_surface_hierarchy_is_monotonic) {
  const auto names = builtin_theme_names();
  for (const std::string_view name : names) {
    const auto theme = builtin_theme(name);
    if (!theme.has_value()) continue;
    const auto& c = theme->colors();
    ST_CHECK(c.surface_raised.to_hex() != c.surface.to_hex());
    // 用整数通道和做单调性判据：足够抓"反了"，又不受 gamma 差异干扰。
    const int surf = static_cast<int>(c.surface.r) + static_cast<int>(c.surface.g) +
                     static_cast<int>(c.surface.b);
    const int raised = static_cast<int>(c.surface_raised.r) +
                       static_cast<int>(c.surface_raised.g) +
                       static_cast<int>(c.surface_raised.b);
    if (raised <= surf) {
      st::log::out("[builtin-theme] {} 层级反了：surface={:06X} raised={:06X}\n", name,
                   c.surface.to_hex(), c.surface_raised.to_hex());
    }
    ST_CHECK(raised > surf);
  }
}

}  // namespace
