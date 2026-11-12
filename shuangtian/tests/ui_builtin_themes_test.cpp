#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/test/test.hpp"
#include "st/ui/builtin_themes.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::ui::builtin_theme;
using st::ui::builtin_theme_names;
using st::ui::builtin_theme_names_by_popularity;
using st::ui::sort_theme_names_by_popularity;

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

/// **常用性序**：默认两档必须打头，且集合与字母序完全一致（只是换了次序）。
///
/// 为何钌：主题选择器的色卡墙是 5 列 × 2 行。字母序时 `light` 排在第 6 位
/// （`cny`/`cyberpunk`/`ink`/`matrix` 把它顶到右半边）——而绝大多数人只在这两档之间切，
/// “默认项要第一眼看到”是选择器的基本形状。
ST_TEST(builtin_theme_names_by_popularity_puts_light_and_dark_first) {
  const auto sorted = builtin_theme_names_by_popularity();
  ST_REQUIRE(sorted.size() >= 10U);
  ST_CHECK_EQ(std::string(sorted[0]), std::string("light"));
  ST_CHECK_EQ(std::string(sorted[1]), std::string("dark"));
  // 集合必须与字母序那份**完全一致**（只是重排，不多不少）：
  // 两边一旦分家，新增主题就会从选择器里消失（看着像“主题加载失败”）。
  auto a = sorted;
  auto b = builtin_theme_names();
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  ST_CHECK_EQ(a.size(), b.size());
  ST_CHECK(a == b);
  ST_CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());   // 无重复
}

/// 未登记在优先级表里的名字必须**接在末尾**（不能静默丢掉）。
///
/// 为何不拿真实主题集验：表里目前恰好包含全部 10 个名字，于是“尾部兼容循环”
/// 什么也不加——删了它也测不出来（**实测踩过：第一版就是这种恒绿的护栏**）。
/// 必须构造一份**含未登记名字**的输入，才能真验那条保底规则。
ST_TEST(sort_theme_names_keeps_unlisted_entries_at_tail) {
  // 输入是字母序（含三个表里没有的名字）：`future-theme` / `newone` / `zzz`。
  const std::vector<std::string_view> input{"cny",   "cyberpunk", "dark",     "future-theme",
                                            "ink",   "light",     "matrix",   "nebula",
                                            "newone", "qinhan",   "synthwave", "tokyo-night",
                                            "zzz"};
  const std::vector<std::string_view> sorted = sort_theme_names_by_popularity(input);
  // 集合必须完全一致（不多少、不丢）——这条是“新增主题不会从选择器里消失”的本体。
  ST_CHECK_EQ(sorted.size(), input.size());
  for (const std::string_view name : input) {
    ST_CHECK(std::find(sorted.begin(), sorted.end(), name) != sorted.end());
  }
  ST_CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
  // 已登记的名字按表排在前 10 位（light/dark 打头），未登记的接在后面。
  ST_REQUIRE(sorted.size() >= 10U);
  ST_CHECK_EQ(std::string(sorted[0]), std::string("light"));
  ST_CHECK_EQ(std::string(sorted[1]), std::string("dark"));
  const std::vector<std::string_view> tail(sorted.begin() + 10, sorted.end());
  std::vector<std::string_view> expected_tail{"future-theme", "newone", "zzz"};   // 保持输入次序
  ST_CHECK(tail == expected_tail);
}

/// 真实主题集上的两条断言：集合完整 + 抽样次序。
ST_TEST(builtin_theme_names_by_popularity_keeps_unlisted_names) {
  const auto sorted = builtin_theme_names_by_popularity();
  const auto all = builtin_theme_names();
  for (const std::string_view name : all) {
    ST_CHECK(std::find(sorted.begin(), sorted.end(), name) != sorted.end());
  }
  // 已登记的那几个必须真的按表排（抽样验证表本身生效，而不是恒等于字母序）。
  const std::size_t ink_at = static_cast<std::size_t>(
      std::find(sorted.begin(), sorted.end(), "ink") - sorted.begin());
  const std::size_t cyber_at = static_cast<std::size_t>(
      std::find(sorted.begin(), sorted.end(), "cyberpunk") - sorted.begin());
  const std::size_t tokyo_at = static_cast<std::size_t>(
      std::find(sorted.begin(), sorted.end(), "tokyo-night") - sorted.begin());
  const std::size_t matrix_at = static_cast<std::size_t>(
      std::find(sorted.begin(), sorted.end(), "matrix") - sorted.begin());
  const std::size_t qinhan_at = static_cast<std::size_t>(
      std::find(sorted.begin(), sorted.end(), "qinhan") - sorted.begin());
  const std::size_t synthwave_at = static_cast<std::size_t>(
      std::find(sorted.begin(), sorted.end(), "synthwave") - sorted.begin());
  ST_CHECK(ink_at < cyber_at);
  ST_CHECK(tokyo_at < matrix_at);
  ST_CHECK(qinhan_at < synthwave_at);
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

/// **可读性门槛**：每套内置主题都必须满足霜天的对比度下限。
///
/// 为何要专为**移植主题**加这一条（而 `ui_theme_test` 只覆盖 light/dark）：
/// 8 套主题是从歌白前端 CSS 移植的，而歌白有 `backdrop-filter` 毛玻璃——
/// 它的层次感有一部分是**模糊补出来的**，令牌之间的亮度差可以很小（实测
/// `surface`/`bg-elev-2` 只差 1.01~1.06）。霜天自绘管线没有常驻面板毛玻璃
/// （成本见 `raster_blur_cost_test`：全窗 316% 帧预算），层差必须**显式给足**，
/// 否则侧栏与内容区糊成一片。
///
/// 实测缺口（修正前）：8/8 主题 `surface`/`surface_alt` < 1.08；
/// 6/8 的 `text_faint` < 4.0（ink 最低 2.30，小字几乎看不见）；
/// 5/8 的 `surface`/`surface_sunken` < 1.10。
///
/// 门槛口径与 `ui_theme_test` 一致：正文/辅助文字 4.5（`faint` 也是 4.5 口径，
/// 这里取 4.0 是历史值——`dark` 基准本身按 4.0 锁），描边 "肉眼可辨" 1.35。
/// 半透明令牌先合成到底色再量（见 `theme.hpp` 亚克力口径 §1）。
ST_TEST(builtin_themes_meet_contrast_floor) {
  // `light`/`dark` 两档**不在本用例范围内**：它们是手调基准，各 token 的门槛
  // 已在 `ui_theme_test` 里按各自的设计意图单独锁着（浅色面板与外壳只差 1.061 ——
  // 浅色下“更亮”已到头，靠阴影而非层差表达抬升；描边 1.261 也是浅色下的
  // 有意取值，那里写的是 1.20 而非暗色的 1.35）。把暗色口径套到它们身上是错的。
  // 本用例只管**从歌白移植的 8 套**——它们才是“照搬了靠毛玻璃补足的值”那一批。
  static const std::vector<std::string_view> ported{
      "cny", "cyberpunk", "ink", "matrix", "nebula", "qinhan", "synthwave", "tokyo-night",
  };
  for (const std::string_view name : ported) {
    const auto theme = builtin_theme(name);
    if (!theme.has_value()) continue;
    const auto& c = theme->colors();

    const auto ratio = [](st::math::Color fg, st::math::Color bg) -> double {
      // 半透明前景先合成：直接拿原值算会高估（描边都是半透明的）。
      if (fg.a < 255U) {
        const float alpha = static_cast<float>(fg.a) / 255.0F;
        const auto mix = [alpha](std::uint8_t top, std::uint8_t bottom) -> std::uint8_t {
          const float v = static_cast<float>(top) * alpha +
                          static_cast<float>(bottom) * (1.0F - alpha);
          return static_cast<std::uint8_t>(v + 0.5F);
        };
        fg = st::math::Color{mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b), 255U};
      }
      return static_cast<double>(st::math::contrast_ratio(fg, bg));
    };

    // ① 表面阶梯：外壳与面板、面板与凹槽都要分得开。
    const double alt_gap = ratio(c.surface, c.surface_alt);
    const double sunken_gap = ratio(c.surface, c.surface_sunken);
    if (alt_gap < 1.08) {
      st::log::out("[builtin-theme] {} 外壳/面板层差不足：{:.3f} < 1.08 "
                   "(surface={:06X} alt={:06X})\n",
                   name, alt_gap, c.surface.to_hex(), c.surface_alt.to_hex());
    }
    if (sunken_gap < 1.10) {
      st::log::out("[builtin-theme] {} 凹槽/面板层差不足：{:.3f} < 1.10 "
                   "(surface={:06X} sunken={:06X})\n",
                   name, sunken_gap, c.surface.to_hex(), c.surface_sunken.to_hex());
    }
    ST_CHECK(alt_gap >= 1.08);
    ST_CHECK(sunken_gap >= 1.10);

    // ② 三级文字：对**四个**底色取最差（`sunken` 最严：它最亮/最沉）。
    //    漏掉 `sunken` 会让「注释与行号在编辑器底上看不清」完全逃过护栏
    //    （`ui_theme_test` 里记着这个漏过的实测）。
    const std::array<st::math::Color, 4> backgrounds{c.bg, c.surface, c.surface_alt,
                                                     c.surface_sunken};
    const auto worst = [&](st::math::Color fg) -> double {
      double lowest = 1e9;
      for (const auto& bg : backgrounds) lowest = std::min(lowest, ratio(fg, bg));
      return lowest;
    };
    const double faint = worst(c.text_faint);
    const double muted = worst(c.text_muted);
    if (faint < 4.0) {
      st::log::out("[builtin-theme] {} 次要文字对比不足：{:.3f} < 4.0 (faint={:06X})\n",
                   name, faint, c.text_faint.to_hex());
    }
    ST_CHECK(faint >= 4.0);
    ST_CHECK(muted >= 4.5);

    // ③ 描边必须「看得见」：它是「这里可以交互」的唯一线索之一。
    const double border = ratio(c.border, c.surface);
    if (border < 1.35) {
      st::log::out("[builtin-theme] {} 描边在面板上太糊：{:.3f} < 1.35 (border={:06X})\n",
                   name, border, c.border.to_hex());
    }
    ST_CHECK(border >= 1.35);
  }
}

}  // namespace
