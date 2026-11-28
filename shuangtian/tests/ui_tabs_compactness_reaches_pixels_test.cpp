/// **`Tabs` 的紧凑度 setter 必须真的改变画面**（不只是在成员上生效）。
///
/// ## 为什么这条用例必须存在
///
/// `theme_style_input_setters_declare_refresh` 是**源码扫描**：它证明 `set_tab_*` 里
/// 出现了 `request_theme_refresh()`，但它证明不了"画面真的变了"——那是静态判据的
/// 固有上限（扫描器只看文本，运行期行为它看不见）。
///
/// 而这里的缺陷形态恰恰是"看起来都对"：`set tab_padding=32` → `get` 回读 32 →
/// 但 `UiRoot` 只在「主题版本变了」或「元素请求了重算」时才跑 `apply_theme`
///（见 `src/ui/ui_root.cpp` 的 `apply_theme_tree`）。少了 `request_theme_refresh()`
/// 就等于**只改了成员、样式没重算、绘制读的还是旧 style_**——用户看到"设了没反应"。
///
/// 所以本用例走**真渲染**：同一 `UiRoot`、同一主题（版本不变、
/// 从而排除了"主题换了所以重算"这条旁路），只调 setter，然后比对像素。
///
/// ## 判据为什么是"像素必须变"而不是"某个具体宽度"
///
/// 内边距/项间距/圆角的具体像素由主题与字体度量共同决定，钉死数值会在换主题或换字体时
/// 变成假红。这里要钉的是一条**因果关系**：setter 一变 → 画面必变。所以取"改前/改后
/// 两次绘制的像素差异数 > 0"，并额外要求 `get` 回读确实变了（防止 setter 自己没生效
/// 时用例因两个错误一起沉默而绿）。
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::raster::Canvas;
using st::ui::Tabs;
using st::ui::Theme;
using st::ui::UiRoot;

/// 本文件自带的断言口（与 `ui_theme_reaches_component_test.cpp` 同一写法：
/// 仓库里的 `expect` 不是共享符号，每个文件自己定义一份）。
void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

/// 两次绘制的差异像素数（按 RGBA 逐位比）。
auto changed_pixels(const Canvas& left, const Canvas& right) -> std::size_t {
  const auto lhs = left.pixels();
  const auto rhs = right.pixels();
  if (lhs.size() != rhs.size()) return lhs.size() + rhs.size();
  std::size_t changed = 0;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index] != rhs[index]) ++changed;
  }
  return changed;
}

}  // namespace

ST_TEST(tabs_compactness_setters_reach_pixels) {
  // 同一主题全程不变：这样任何"画面变了"都只能来自 setter 自己请求的重算，
  // 排除了"主题版本变化触发 apply_theme"这条旁路（否则缺陷会被掩盖）。
  UiRoot root{};
  root.set_theme(Theme::dark());

  auto owned = std::make_unique<Tabs>();
  Tabs* tabs = owned.get();
  tabs->set_id("tabs-compact");
  tabs->add_tab("alpha");
  tabs->add_tab("beta");
  tabs->add_tab("gamma");
  // ⚠ **活动标签必须是非首项**（实测踩到，理由见下）。
  //
  // 裸 `UiRoot` 里没有启动应用 → 字体没加载 → `TextPort::measure_width` 返回 0，
  // 于是每个标签宽 = `k_min_tab_width(36) + pad*2`（与文字长度无关），**文字也画不出来**。
  // 此时画面上唯一随 `tab_gap` 移动的东西是**活动胶囊**；而标签 0 的 `offsets_` 恒为 0，
  // 改 gap 不会移它。若活动项留在默认的 0，`set_tab_gap` 就真的一像素不动，
  // 用例会报成"setter 没生效"——那是量具的局限，不是缺陷（第一版就是这么假红的）。
  // 把活动项改到 1：胶囊随 gap 平移，三个 setter 的变化便都能落到像素上。
  tabs->set_active(1);
  root.set_content(std::move(owned));
  root.set_viewport(st::math::Size{1400.0F, 200.0F});

  Canvas canvas{1400, 200, 1.0F};

  // ## 为何每次绘制前都调 `mark_dirty_all()`
  //
  // `paint_frame` 内部有早退：`if (!force && !dirty_ && !tree_layout_dirty()) return;`
  // （见 `ui_root.cpp`）。本用例刚刚 `clear_dirty()` 过，不重新标脏就会**整个跳过绘制**，
  // 于是两次拿到的都是"上一帧留下的未清画布"——差异像素恒为 0，看着像"setter 没生效"。
  // 这一条实测踩过（第一版就是这个假红，而 setter 其实是好的）。
  // `app_frame_budget_test.cpp` 测最坏稳态帧时也是先 `mark_dirty_all()` 再 `paint_frame`。
  const auto paint_into = [&](Canvas& target, double time) {
    root.set_time(time);
    root.mark_dirty_all();
    root.layout(true);
    target.clear(st::math::Color{0, 0, 0, 0xFF});
    (void)root.paint_frame(target);
    root.clear_dirty();
  };

  // ## 先自查量具：“基线必须真的画了东西且不是一整块纯色”
  //
  // 不查这一条的数列：若 Tabs 根本没画出来，则“改前/改后都黖黑”也会得到差异像素 0——
  // 用例会报成“setter 没生效”（假红），而那与 setter 毫无关系。
  paint_into(canvas, 1.0);
  const auto distinct_colors = [](const Canvas& shot) {
    std::vector<std::uint32_t> seen;
    for (const std::uint32_t pixel : shot.pixels()) {
      bool found = false;
      for (const std::uint32_t value : seen) {
        if (value == pixel) {
          found = true;
          break;
        }
      }
      if (!found) {
        seen.push_back(pixel);
        if (seen.size() > 64) break;   // 足够证明"有内容"，不必数完
      }
    }
    return seen.size();
  };
  st::log::out("[tabs] 基线颜色数 {}\n", distinct_colors(canvas));
  expect(distinct_colors(canvas) > 1,
         "基线只有一种颜色 —— Tabs 没画出标签（量具坏了，后面的像素比对无意义）");

  // ## 三组 setter 逐一验：改前绘制 → setter → 再绘制 → 像素必须变
  double clock = 1.1;
  const auto expect_changes_pixels = [&](const char* name, auto&& apply) {
    Canvas baseline{1400, 200, 1.0F};
    paint_into(baseline, clock);
    clock += 0.1;

    apply();   // 只调 setter：主题没变、版本没变

    Canvas after{1400, 200, 1.0F};
    paint_into(after, clock);
    clock += 0.1;

    const std::size_t changed = changed_pixels(baseline, after);
    st::log::out("[tabs] {} → 差异像素 {}\n", name, changed);
    expect(changed > 0,
           std::format("`{}` 改了但画面一个像素都没变 —— setter 没请求主题重算"
                       "（`apply_theme` 读的是成员，而 `UiRoot` 只在主题换过或元素请求时才跑它）",
                       name));
  };

  expect_changes_pixels("set_tab_padding(28)", [&] { tabs->set_tab_padding(28.0F); });
  expect(tabs->tab_padding() > 27.0F, "`get tab_padding` 没回读到新值 —— setter 自身就没生效");

  expect_changes_pixels("set_tab_gap(18)", [&] { tabs->set_tab_gap(18.0F); });
  expect(tabs->tab_gap() > 17.0F, "`get tab_gap` 没回读到新值");

  expect_changes_pixels("set_tab_radius(14)", [&] { tabs->set_tab_radius(14.0F); });
  expect(tabs->tab_radius() > 13.0F, "`get tab_radius` 没回读到新值");
}
