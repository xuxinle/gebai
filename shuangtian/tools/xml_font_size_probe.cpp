// 探针：XML 里写的 `font_size` 在**主题刷新之后**还在不在。
//
// ## 为什么单独量这一条
//
// HTML→XML 生成器要把 CSS 的 `font-size` 落成某个属性名。属性面有两条通道：
//   - `font_size` → `apply_layout_property` → 直接写 `Style::font_size`
//   - `text_size` → `Element::set_text_size` → 记进 `text_size_override`（主题回放末尾重新落位）
//
// 若前者会被 `Text::apply_theme` 盖回，那生成器写 `font_size` 就是“看起来生效、
// 换主题/重绘后消失”——必须写 `text_size`。判据只能靠**真的跑一次 refresh_theme**，
// 读回样式值：构造后读一次（写后立即）与主题刷新后读一次（真正上屏用的是这个）。

#include <cstdio>
#include <memory>
#include <string>

#include "st/ui/components/basic.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/xml_ui.hpp"

namespace {

using st::ui::UiRoot;

/// 建一棵 XML 树并挂到 root 上（走真实建树路径）。
auto build(UiRoot& root, std::string_view xml) -> void {
  auto tree = st::ui::build_from_xml(xml);
  root.set_content(std::move(tree));
}

auto font_size_of(UiRoot& root, const char* id) -> float {
  auto* element = root.find(id);
  return element != nullptr ? element->style().font_size : -1.0f;
}

}  // namespace

auto main() -> int {
  UiRoot root;
  const st::ui::Theme theme = st::ui::Theme::light();
  root.set_theme(theme);
  root.set_viewport(st::math::Size{800.0f, 600.0f});
  root.layout(true);   // 首次布局：把主题真正下发到每个元素

  // 三个对照组：不写字号（主题默认）/ 写 font_size / 写 text_size（样式串 size 等价）。
  build(root, R"(<Column>
    <Text id="plain">默认</Text>
    <Text id="by-font-size" font_size="28">font_size</Text>
    <Text id="by-text-size" text_size="28">text_size</Text>
    <Text id="by-style" style="size 28">style</Text>
  </Column>)");
  root.set_theme(theme);   // 换肤（版本号 → 下一帧全树重算）
  root.layout();           // 真实上屏走的就是这条：layout 里把主题下发到每个元素

  const float plain = font_size_of(root, "plain");
  std::printf("主题默认 font_base = %.2f\n", static_cast<double>(plain));
  const struct {
    const char* id;
    const char* how;
  } cases[] = {{"by-font-size", "font_size=\"28\""},
               {"by-text-size", "text_size=\"28\""},
               {"by-style", "style=\"size 28\""}};
  int failures = 0;
  for (const auto& item : cases) {
    const float got = font_size_of(root, item.id);
    const bool ok = got > plain + 8.0f;
    std::printf("%-22s → 主题刷新后 %.2f  %s\n", item.how, static_cast<double>(got),
                ok ? "OK" : "被盖回（不可用于生成器）");
    if (!ok) ++failures;
  }
  std::printf("\n结论：%s\n", failures == 0 ? "三条都生效" : "有通道在主题刷新后被盖回");
  return failures;
}
