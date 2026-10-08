/// 终端色板（`TerminalPalette`）的主题适配测试。
///
/// ### 为什么需要这一组
///
/// ANSI 前 16 色是**协议固定语义**（`31` 永远是红、`93` 永远是亮黄），
/// 程序按它们选色；但它们的**明度必须跟终端底色走**——为深底调的亮色
/// 放到白底上会**字面看不见**。实测（修复前，写死一套深底调色板 × 亮色底 `#DDDDE4`）：
///
/// | 颜色 | 浅底对比度 | |
/// |---|---|---|
/// | 亮黄 `#E5E510` | **1.00** | 与底色毫无差别 |
/// | 亮白 `#E5E5E5` | **1.07** | 同上 |
/// | 亮绿 `#23D18B` | **1.47** | 同上 |
/// | 青 `#11A8CD` | 2.07 | 偏弱 |
///
/// 16 色里 **12 色低于 3:1** ⇒ 用户报「亮色主题高亮看不清」。
///
/// ### 这里锁住的契约
///
/// ① **两套色板都在**（亮/暗各一组 16 色），不是一套用到底；
/// ② **每一色对**它当属的主题底色 ≥ **4.5:1**（WCAG AA 正文口径）——
///    这一条是**可算的**，所以不靠截图肉眼看，直接断言；
/// ③ 终端底色**不借用** `surface_sunken`（那是通用凹槽语义）；
/// ④ **色相语义不漂**（红仍是红），换色板不等于换语义。
///
/// ### 为何对比度用 `st::math` 的现成实现
///
/// 框架已提供 `relative_luminance` / `contrast_ratio`（WCAG 2.1）——
/// **主题自检与测试必须走同一份实现**，否则“代码造的色”与“测试量的色”
/// 会各自漂移，那正是本轮缺陷能溜进来的土壤。

#include "st/test/test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

#include "st/math/color.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::math::Color;
using st::math::contrast_ratio;
using st::math::relative_luminance;
using st::ui::Theme;

/// 打印用（失败信息里带上颜色值，不用再回代码里翻）。
[[nodiscard]] auto hex_of(const Color& color) -> std::string {
  const auto part = [](std::uint8_t value) {
    constexpr char kDigits[] = "0123456789abcdef";
    return std::string{kDigits[value >> 4U], kDigits[value & 0x0FU]};
  };
  return "#" + part(color.r) + part(color.g) + part(color.b);
}

/// 16 色里的**灰阶轴**（0=black / 8=bright-black）：它们的设计目的就是
/// "贴近底色/表达明暗轴"，不承担正文可读性——**深底上 black 几乎不可见是正确行为**。
/// 只豁免这两档，其余每一色都必须过 4.5 的线。
[[nodiscard]] auto is_grey_ramp(std::size_t index) -> bool { return index == 0 || index == 8; }

/// 逐色检查"对底色可读"，**带回具体数值的失败信息**。
///
/// 为何直接用 `record_failure` 而不是 `ST_CHECK`：断言宏的消息是 `#expr`（源码文本），
/// 而这里要报的是**算出来的数**（哪个索引、什么色、对比度多少）——光看
/// `contrast_ratio(ansi[index], bg) >= 4.5` 不知道是哪一色不达标。
/// 两个计数入口都走（`count_check` + `record_failure`），与宏的行为一致。
void check_palette_readable(const Theme& theme, const char* label) {
  const Color bg = theme.terminal().bg;
  const auto& ansi = theme.terminal().ansi;
  for (std::size_t index = 0; index < ansi.size(); ++index) {
    if (is_grey_ramp(index)) continue;
    ::st::test::Registry::instance().count_check();
    const float ratio = contrast_ratio(ansi[index], bg);
    if (ratio < 4.5f) {
      ::st::test::Registry::instance().record_failure(
          __FILE__, __LINE__,
          std::format("{} 终端 ANSI[{}] {} 对底 {} 对比度 {:.2f} < 4.5", label, index,
                      hex_of(ansi[index]), hex_of(bg), static_cast<double>(ratio)));
    }
  }
}

/// 判定 `color` 的主通道是否为 `dominant`（0=R 1=G 2=B）。
[[nodiscard]] auto dominant_is(const Color& color, int dominant) -> bool {
  const std::array<int, 3> channels{color.r, color.g, color.b};
  for (int i = 0; i < 3; ++i) {
    if (i == dominant) continue;
    if (channels[dominant] <= channels[i] + 20) return false;
  }
  return true;
}

/// 断言 `ok`，失败时把 `message` 当详情（同上：消息要带算出来的数）。
void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// ① 两套色板都在，且底色区分亮暗
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_palette_background_differs_between_light_and_dark) {
  // 防「两套色板但底色相同」的半吊子修复——那样 16 色其实无法同时适配。
  const Theme light = Theme::light();
  const Theme dark = Theme::dark();
  const Color light_bg = light.terminal().bg;
  const Color dark_bg = dark.terminal().bg;

  ST_CHECK(light_bg != dark_bg);
  // 亮主题的终端底必须**真的亮**、暗主题必须**真的暗**（而不是"两个都不亮的灰"）。
  expect(relative_luminance(light_bg) > 0.5f, std::format("亮色终端底不够亮: {}", hex_of(light_bg)));
  expect(relative_luminance(dark_bg) < 0.05f, std::format("暗色终端底不够暗: {}", hex_of(dark_bg)));

  // 默认前景（`SGR 39` 回到它）也要过 AA。
  expect(contrast_ratio(light.terminal().fg, light_bg) >= 4.5f,
         std::format("亮色终端前景 {} 对底 {} 对比度 {:.2f}", hex_of(light.terminal().fg),
                     hex_of(light_bg),
                     static_cast<double>(contrast_ratio(light.terminal().fg, light_bg))));
  expect(contrast_ratio(dark.terminal().fg, dark_bg) >= 4.5f,
         std::format("暗色终端前景 {} 对底 {} 对比度 {:.2f}", hex_of(dark.terminal().fg),
                     hex_of(dark_bg),
                     static_cast<double>(contrast_ratio(dark.terminal().fg, dark_bg))));
}

ST_TEST(terminal_background_is_not_borrowed_from_surface_sunken) {
  // 终端底有自己的语义（"一块屏幕"）；`surface_sunken` 是通用凹槽（输入框/代码底共用）。
  // 借用的直接后果：亮色下发灰、暗色下奇黑，且 16 色的对比度无从上标。
  const Theme light = Theme::light();
  const Theme dark = Theme::dark();
  ST_CHECK(light.terminal().bg != light.colors().surface_sunken);
  ST_CHECK(dark.terminal().bg != dark.colors().surface_sunken);
}

// ————————————————————————————————————————————————————————————————————————————
// ② 核心契约：每色对底色 ≥4.5:1
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(terminal_ansi_palette_is_readable_on_light_background) {
  // **本轮缺陷的直接判据**：写死的那套深底调色板在亮底上 12/16 色低于 3:1
  //（亮黄 1.00、亮白 1.07 —— 字面看不见）。
  check_palette_readable(Theme::light(), "亮色");
}

ST_TEST(terminal_ansi_palette_is_readable_on_dark_background) {
  check_palette_readable(Theme::dark(), "暗色");
}

ST_TEST(terminal_ansi_bright_variants_are_distinguishable_from_normal) {
  // 亮色档（8-15）必须与对应常规档（0-7）**看得出区别**——
  // 否则「加粗 = 提亮」（`SGR 1` 走的正是这条映射）形同虚设。
  const auto check = [](const Theme& theme, const char* label) {
    for (std::size_t index = 0; index < 8; ++index) {
      const Color normal = theme.terminal().ansi[index];
      const Color bright = theme.terminal().ansi[index + 8];
      expect(normal != bright,
             std::format("{} ANSI[{}] 与亮色档同为 {}（加粗提亮将无从体现）", label, index,
                         hex_of(normal)));
    }
  };
  check(Theme::light(), "亮色");
  check(Theme::dark(), "暗色");
}

ST_TEST(terminal_ansi_palette_keeps_hue_semantics_in_both_themes) {
  // 换色板**不能换语义**：红/绿/黄/蓝/青/品红必须仍是那个色相
  //（程序指望 `31` 看起来像红——否则 `git diff` 的加删色会反直觉）。
  //
  // 判据只做"主通道明确领先"这一档：不做完整色彩空间换算，只要挡住
  // "红变成蓝"这类语义漂移即可（过度精确的色相断言会让调色板无法微调）。
  const auto check = [&](const Theme& theme, const char* label) {
    const auto& ansi = theme.terminal().ansi;
    const auto report = [&](std::size_t index, const char* what) {
      return std::format("{} ANSI[{}] 不是{}: {}", label, index, what, hex_of(ansi[index]));
    };
    expect(dominant_is(ansi[1], 0), report(1, "红"));
    expect(dominant_is(ansi[2], 1), report(2, "绿"));
    // 黄 = 红、绿同时高（蓝最低）
    expect(ansi[3].r > ansi[3].b + 20 && ansi[3].g > ansi[3].b + 20, report(3, "黄"));
    expect(dominant_is(ansi[4], 2), report(4, "蓝"));
    // 品红 = 红蓝双高、**绿最低**（哪个高不一定：`#861f9b` 蓝略高、`#ce63ce` 红蓝相等）。
    // 先写成“红蓝都占主”是错的——那把合法的品红判成不合格。
    expect(ansi[5].g + 20 < ansi[5].r && ansi[5].g + 20 < ansi[5].b, report(5, "品红（绿最低）"));
    // 青 = 蓝绿双高、**红最低**（同理，蓝绿哪个高不一定）。
    expect(ansi[6].r + 20 < ansi[6].g && ansi[6].r + 20 < ansi[6].b, report(6, "青（红最低）"));
  };
  check(Theme::light(), "亮色");
  check(Theme::dark(), "暗色");
}
