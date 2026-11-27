/// 主题选择器（`ThemePicker`）色卡墙的**项次序**。
///
/// ## 为什么这值得一条回归
///
/// 色卡墙是 5 列 × 2 行的网格，而它按 `names_` 的**先后**填格。旧实现直接取
/// `builtin_theme_names()`（字母序），于是：
///
/// | 位 | 字母序 | 用户看到 |
/// |---|---|---|
/// | 1-4 | cny / cyberpunk / ink / matrix | 全是风格化主题 |
/// | 5-6 | **dark / light** | 默认两档被挤到第 1 行末尾 |
///
/// 而绝大多数人只在 `light`/`dark` 之间切——“默认项要第一眼看到”是选择器的基本形状。
///
/// ## 为什么断言 `options` 属性而不是截图
///
/// `options` 是 `names_` 的**直接投影**（同一份数据的对外出口），而色卡位置完全由它决定，
/// 所以它错位时截图必然错位，反过来不成立的情况不存在。属性面断言可以在任何机器上跑，
/// 不需要真窗口，也不需要挑一张能被「肉眼判定顺序」的截图（10 张色卡的缩略图
/// 彼此差异很小，人眼比对本身就是不稳的量尺）。

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/ui/builtin_themes.hpp"
#include "st/ui/components/theme_picker.hpp"
#include "st/ui/theme.hpp"

namespace {

/// 把 `options` 属性（`a|b|c`）拆成名字表。
[[nodiscard]] auto split_options(const std::string& joined) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string current;
  for (const char ch : joined) {
    if (ch == '|') {
      out.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

}  // namespace

/// 选择器枚举出的主题顺序必须与「常用性序」**逐项一致**。
///
/// 反向验证：把 `populate_builtins` 改回 `builtin_theme_names()`（字母序），
/// 本用例在第一条 `ST_CHECK` 上立刻变红（`cny` ≠ `light`）。
ST_TEST(theme_picker_options_follow_popularity_order) {
  st::ui::ThemePicker picker;
  const auto value = picker.get_property("options");
  ST_REQUIRE(value.has_value());
  const std::vector<std::string> shown = split_options(*value);
  ST_REQUIRE(!shown.empty());

  const auto expected = st::ui::builtin_theme_names_by_popularity();
  ST_REQUIRE(shown.size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (shown[index] == std::string(expected[index])) continue;
    st::log::out("[theme-picker] 第 {} 位是 {}，期望 {}（顺序被改坏）\n", index, shown[index],
                 expected[index]);
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    ST_CHECK_EQ(shown[index], std::string(expected[index]));
  }
}

/// **默认两档必须占据头两格**——这条是与用户约定本身，值得独立断言
/// （上一条会因为任何一位错位而红，红了也说不清“是不是默认项被挪走了”）。
ST_TEST(theme_picker_first_two_cards_are_light_and_dark) {
  st::ui::ThemePicker picker;
  const auto value = picker.get_property("options");
  ST_REQUIRE(value.has_value());
  const std::vector<std::string> shown = split_options(*value);
  ST_REQUIRE(shown.size() >= 2U);
  ST_CHECK_EQ(shown[0], std::string("light"));
  ST_CHECK_EQ(shown[1], std::string("dark"));
}

/// `current` 与 `options` 必须自洽：当前主题名一定在列表里。
///
/// 为何钉它：色卡墙用 `selected_ = names_.index(name)` 定位打勾。
/// 若 `current_` 是 `populate_builtins` 之后才设进来的外部名字、又不在 `names_` 里，
/// 界面上就会**一张卡都不打勾**（看着像“没选中任何主题”）。
ST_TEST(theme_picker_current_is_present_in_options) {
  for (const std::string_view name : st::ui::builtin_theme_names()) {
    st::ui::ThemePicker picker;
    picker.populate_builtins(name);
    const auto value = picker.get_property("options");
    ST_REQUIRE(value.has_value());
    const std::vector<std::string> shown = split_options(*value);
    ST_REQUIRE(!shown.empty());
    ST_CHECK(std::find(shown.begin(), shown.end(), std::string(name)) != shown.end());
    ST_CHECK_EQ(std::string(picker.current()), std::string(name));
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 与真实主题的联动（2026-11-27 修：应用是暗色、选择器勾着亮色）
// ————————————————————————————————————————————————————————————————————————————

/// **非默认主题必须自己报告自己的名字**。
///
/// 这条钉的是一个“显示错、上报也错”的双重缺陷：`Theme::name()` 的文档说它
/// “作标识与上报用”，而旧实现只有 `theme_io.cpp` 从**主题文件**加载时写过它——
/// **内置主题一条路径都没写**，于是它恒为默认值 `"light"`。
///
/// 两个真实受害者：
/// 1. 标题栏的 `ThemePicker` 拿不到真实当前值 ⇒ 应用暗色、选择器勾亮色；
/// 2. 控制通道的 `theme` 命令读 `root.theme().name()` ⇒ 对内建主题恒报 `light`。
///
/// 断言用**多个主题**而不是只测 dark：只测一档时，“恰好返回 hardcode 的 light”
/// 与“真正报告自己”对 dark 都不成立，但换成 light 档就无从区分——
/// 而 `light` 正是缺陷版本“永远说对的”那一档。
ST_TEST(builtin_theme_reports_its_own_name) {
  for (const std::string_view name : st::ui::builtin_theme_names()) {
    const auto theme = st::ui::builtin_theme(name);
    ST_REQUIRE(theme.has_value());
    if (std::string(theme->name()) != std::string(name)) {
      st::log::out("[theme-name] {} 自报为 {}（与选择器/控制通道看到的不一致）\n", name,
                   theme->name());
    }
    ST_CHECK_EQ(theme->name(), std::string(name));
  }
}

/// `set_current` 同值必须**不标脏**（`TitleBar::apply_theme` 每帧会调它一次）。
///
/// 为何必须钉：它是那个“每帧同步”方案的**唯一代价点**。
/// 不逃逸就是每帧把整条祖先链标脏——退化成持续全帧重绘，而
/// **功能看起来完全正常**（只是 CPU 烧着）。
ST_TEST(theme_picker_set_current_same_value_does_not_dirty) {
  st::ui::ThemePicker picker;
  picker.populate_builtins("dark");
  picker.clear_dirty();
  picker.set_current("dark");   // 同值
  ST_CHECK(!picker.dirty());
  picker.set_current("light");  // 真变了
  ST_CHECK(picker.dirty());
}
