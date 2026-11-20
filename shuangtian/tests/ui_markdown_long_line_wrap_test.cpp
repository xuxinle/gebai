/// 超长行（中文混长 ASCII 标识符）必须自动折行，**不能溢出可用宽**。
///
/// ## 缺陷现场（用户附截图：“超长内容没有自动换行”）
///
/// 思考块里的一整段列表（3 空格缩进的续行）**整块没有折行**：
/// 中文长句从卡片左边一直排到屏幕右缘（实测排到 x=2183，而卡片右缘只有 1828），
/// 溢出 355px。同一块里代码块、列表项全部如此。
///
/// ## 为什么这个文件用 `ProportionalTextPort`
///
/// 本目录的 fixture 注释写得很清楚：Markdown 该用 `ProportionalTextPort`
/// （ASCII ≈ 0.55·size、非 ASCII = size），它**按真实字形宽度差**模拟排版。
/// 而 `FixedAdvanceTextPort` 是每个码点恒定 8px——用它测这个问题会**把缺陷抹平**：
/// 中文与 ASCII 等宽时，任何一行都"恰好"能折，永远测不出真实的折行失败。
///
/// ## 判据
///
/// 两个正交量同时立住才算过：
/// 1. **宽度**：视图不许超过可用宽（收缩逻辑不能拿"最宽碎片"当自然宽）。
/// 2. **高度**：内容高必须远超单行——排成一行时只有 ~21px。
#include <string>

#include "st/test/test.hpp"

#include "st/ui/components/markdown_view.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::ui::Constraints;
using st::ui::MarkdownView;
using st::ui::RenderContext;
using st::ui::Theme;

// 实测口径：思考块里 MarkdownView 的内容宽 1752、字号 12。
constexpr float kWidth = 1752.0f;
constexpr float kBase = 12.0f;

struct Fixture {
  Theme theme{Theme::light()};
  st::test::ProportionalTextPort port{};
  [[nodiscard]] auto context() -> RenderContext { return RenderContext{theme, &port, 0.0}; }
};

[[nodiscard]] auto constraints() -> Constraints {
  Constraints c;
  c.max_width = kWidth;
  c.available_width = kWidth;
  return c;
}

/// 真实原文（从缺陷会话里原样取出的那段思考内容）。
[[nodiscard]] auto real_segment() -> std::string {
  return "关键几何构造：\n\n"
         "1. 外壳（ExtrudeGeometry 带孔）：\n"
         "   Shape：外圈圆（半径 R，用 absarc）→ 但要带倒角。用 Shape.absarc(0,0,R,0,2π,false)。\n"
         "   holes：3 个扇形路径（Path，内弧 + 外弧 + 边）。要挖出窗口，扇形角度范围 ±30° 左右，即 width 60°，间隔 60°。\n"
         "   \n"
         "   注意：在 three里，Shape的 hole 需要 winding 与 shape 相反（顺时针才是 hole？）。three.js 的 ExtrudeGeometry 对 holes 有内置处理，通常 path 顺时针即可。参考 04-watch 的 hole 用法：hole 用 lineTo 沿角度从 start 到 end（逆时针），然后内弧从 end 回 start。没有显式考虑 winding，three 的 ShapeUtils 会自动判断。这里用同样风格。\n\n"
         "   扇形孔路径：外弧从 a0 到 a1（沿 r_out），然后直线到内弧 (r_in, a1)，内弧从 a1 回到 a0，直线回外弧起点。\n\n"
         "2. 厚度方向：ExtrudeGeometry 沿 +Z 挤出。板面在 XY 平面。相机从 +Z 方向看。天顶视角时，盘面在 XY，Z 是厚度——这跟参考图接近（俯视）。但为了立体感，我从 z 上方看。\n";
}

/// 一行超长中文（不带任何 Markdown 结构），最纯粹的长行。
[[nodiscard]] auto plain_long_line() -> std::string {
  return "TorusGeometry 的 arc 从 0 起（+X 方向开始，逆时针）。要放到中心角 aC 处："
         "rotation.z = aC - arcAngle/2。torus 在 XY 平面 √（TorusGeometry 的交线在 XY 平面，绕 Z 轴）。"
         "径向条：box(长, 宽, 厚)，长 = rOut - rIn，放在极角 a 中间半径处，rotation.z = a，"
         "若长边沿 X，则旋转 a 后指向 a 方向 √。位置 = 中点 (cos a * rMid, sin a * rMid)。";
}

}  // namespace

ST_TEST(md_real_thinking_segment_wraps) {
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(kBase);
  view.set_markdown(real_segment());
  view.measure(fixture.context(), constraints());

  // ① 宽度不许超过可用宽。
  ST_CHECK(view.measured_size().width <= kWidth + 1.0f);
  // ② 必须折成多行：这段 ~600 字符，单行只有 ~21px 高。
  ST_CHECK(view.content_height() > kBase * 1.6f * 5.0f);
  // ③ 折行时铺满可用宽（不许被"最宽碎片"收缩成窄柱）。
  ST_CHECK(view.measured_size().width >= kWidth - 1.0f);
}

ST_TEST(md_plain_long_line_wraps) {
  // 最纯粹的长行：一个段落、没有列表/缩进结构。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(kBase);
  view.set_markdown(plain_long_line());
  view.measure(fixture.context(), constraints());

  ST_CHECK(view.measured_size().width <= kWidth + 1.0f);
  ST_CHECK(view.content_height() > kBase * 1.6f * 3.0f);
}

ST_TEST(md_overlong_line_respects_narrow_width) {
  // 窗口变窄时同样必须折行（宽度只受容器约束）。
  Fixture fixture;
  MarkdownView view;
  view.set_shrink_to_fit(true);
  view.set_base_font_size(kBase);
  view.set_markdown(plain_long_line());

  Constraints narrow;
  narrow.max_width = 400.0f;
  narrow.available_width = 400.0f;
  view.measure(fixture.context(), narrow);

  ST_CHECK(view.measured_size().width <= 401.0f);
  ST_CHECK(view.content_height() > kBase * 1.6f * 6.0f);   // 400px 里要折更多行
}
