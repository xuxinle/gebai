/// **边缘高光形态**（`HighlightEdges`）的像素判据。
///
/// ## 为什么需要这组用例
///
/// 四种形态的差别全在**哪几条边有光、亮暗怎么分布**上——这是纯形态问题：
/// `tree`/`get` 读回来的属性全部正确（`highlight_edges` = 枚举值），
/// 而画面上是“侧边无光”还是“侧边一道暗线”完全看不出来。属性面钉不住它，
/// 只能量像素。
///
/// | 形态 | 上 | 下 | 左 | 右 |
/// |---|---|---|---|---|
/// | `All` | 亮 | 稍暗 | 有光（渐变中段） | 有光（偏暗） |
/// | `TopBottom` | 亮 | 稍暗 | **无光**（裁掉） | **无光** |
/// | `Top` | 亮 | 无 | 无 | 无 |
/// | `TopBottomShaded` | 亮 | 稍暗 | **暗线** | **暗线** |
///
/// 判据不取绝对值而取**相对量**（各边相对卡内底色），因为底色由主题与玻璃
/// tint 共同决定，写死阈值会在换主题时失效。
///
/// ## 量法
///
/// 在每条边的环带中心（`ring/2` 处）沿边取若干采样点，与同一位置的**卡内深处**
/// （向内 `ring + 4px`）比亮度差。差值即“这条边亮了多少”。
///
/// 取样避开圆角：四个角各内缩 `radius + 2px`——圆角处是两条边的过渡带，
/// 混进来会把“侧边无光”测成“侧边有点光”（实测把 `TopBottom` 测成 3.2，而真值是 0）。
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/panel.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::raster::Canvas;
using st::ui::HighlightEdges;
using st::ui::Panel;
using st::ui::UiRoot;

constexpr int kCanvasW = 320;
constexpr int kCanvasH = 240;
constexpr float kBoxX = 40.0f;
constexpr float kBoxY = 40.0f;
constexpr float kBoxW = 240.0f;
constexpr float kBoxH = 160.0f;
constexpr float kRadius = 14.0f;
constexpr float kRing = 1.5f;

/// 亮度的**相对增量**：`边上的采样值 - 同位置内侧的底色`。
///
/// 取相对量而不是绝对值：底色由主题 `surface` + 玻璃 tint 共同决定，
/// 写死阈值（“亮于 200 算有光”）在暗色主题下全盘失效。
struct EdgeProfile {
  float top{0.0f};
  float bottom{0.0f};
  float left{0.0f};
  float right{0.0f};

  [[nodiscard]] auto str() const -> std::string {
    return std::format("top{:.1f} bottom{:.1f} left{:.1f} right{:.1f}",
                       static_cast<double>(top), static_cast<double>(bottom),
                       static_cast<double>(left), static_cast<double>(right));
  }
};

[[nodiscard]] auto lum_at(const Canvas& canvas, float x, float y) -> float {
  const Color c = canvas.pixel_at(static_cast<int>(std::lround(x)),
                                  static_cast<int>(std::lround(y)));
  return (static_cast<float>(c.r) + static_cast<float>(c.g) + static_cast<float>(c.b)) / 3.0f;
}

/// 沿一条边取多个采样点求中位数（抗抗锯齿噪点）。
[[nodiscard]] auto median_of(std::vector<float> values) -> float {
  if (values.empty()) return 0.0f;
  std::ranges::sort(values);
  return values[values.size() / 2];
}

/// 环带内**偏离卡身底色最远**的那一像素（带符号：正=更亮，负=更暗）。
///
/// ## 为何不分开写“取最亮”与“取最暗”两个函数
///
/// 量法必须同时服务亮边（`All`/`TopBottom`）与暗线（`TopBottomShaded` 的侧边）。
/// 若固定取最亮，暗线会直接取到内侧卡身底色（120）——于是“侧边有暗线”与
/// “侧边完全没画”量出同一个值，那条用例带着真实错误仍然报绿。
///
/// 取“偏离最大”的极值，两个方向都能看到，且不需要为每种形态写一套量法。
[[nodiscard]] auto peak_dev(const Canvas& canvas, int x_lo, int x_hi, int y,
                            int ref_x) -> float {
  const float ref = lum_at(canvas, static_cast<float>(ref_x), static_cast<float>(y));
  float best = 0.0f;
  for (int x = x_lo; x <= x_hi; ++x) {
    const float dev = lum_at(canvas, static_cast<float>(x), static_cast<float>(y)) - ref;
    if (std::fabs(dev) > std::fabs(best)) best = dev;
  }
  return best;
}

[[nodiscard]] auto peak_dev_v(const Canvas& canvas, int y_lo, int y_hi, int x,
                              int ref_y) -> float {
  const float ref = lum_at(canvas, static_cast<float>(x), static_cast<float>(ref_y));
  float best = 0.0f;
  for (int y = y_lo; y <= y_hi; ++y) {
    const float dev = lum_at(canvas, static_cast<float>(x), static_cast<float>(y)) - ref;
    if (std::fabs(dev) > std::fabs(best)) best = dev;
  }
  return best;
}

/// 沿圆角弧线取样，看“亮→暗”是不是连续过渡。
struct CornerProfile {
  float max_step{0.0f};    ///< 相邻两点的最大跳变（仅诊断，不用于判据）
  float max_rebound{0.0f}; ///< **反向回弹**的最大幅度（硬切断口的签名）
  float total{0.0f};       ///< 两端总落差（确认确实发生了亮→暗的转变）

  [[nodiscard]] auto str() const -> std::string {
    return std::format("total={:.1f} max_rebound={:.1f} max_step={:.1f}",
                       static_cast<double>(total), static_cast<double>(max_rebound),
                       static_cast<double>(max_step));
  }
};

/// 沿**左上圆角外弧**从顶边走到左边，取环带中心的亮度序列。
///
/// 为何量外弧而不量内侧：环带厚度只有 `ring`，内侧采样会掺到卡身。
/// 外弧上的点都在环带里，且从“上边（亮）”连续走到“左边（暗）”——
/// 中间的过渡是否平滑，看相邻点的差就够了。
[[nodiscard]] auto corner_profile_for(st::ui::HighlightEdges edges) -> CornerProfile {
  UiRoot root;
  root.set_viewport({static_cast<float>(kCanvasW), static_cast<float>(kCanvasH)});
  root.set_theme(st::ui::Theme::light());

  auto card = std::make_unique<Panel>();
  auto* raw = card.get();
  raw->set_id("probe-card");
  root.set_content(std::move(card));

  raw->style().set_solid_background(Color{120, 120, 120, 255});
  raw->style().radius = kRadius;
  raw->style().shadow = {};
  raw->style().border_width = 0.0f;
  raw->style().top_highlight = Color{255, 255, 255, 230};
  raw->style().highlight_width = kRing;
  raw->style().highlight_edges = edges;

  st::ui::RenderContext context{root.theme(), nullptr, 1.0f};
  const Rect box{kBoxX, kBoxY, kBoxW, kBoxH};
  raw->measure(context, st::ui::Constraints{});
  raw->arrange(context, box);

  Canvas canvas{kCanvasW, kCanvasH, 1.0f};
  canvas.clear(Color{120, 120, 120, 255});
  raw->paint(context, canvas);

  // 左上圆角的圆心与弧：圆心 (x+r, y+r)
  const float cx = kBoxX + kRadius;
  const float cy = kBoxY + kRadius;

  // 每个角度上沿**径向**扫过整个环带，取偏离卡身底色最远的那一点。
  //
  // 为何不能只采一个点：环带只有 `ring`（1.5px）厚，单像素采样会随机落到
  // 环带峰值、内缘抗锯齿或卡身上——实测沿弧线扫出的序列是
  // `180 189 206 159 158 216 127 217`（锯齿），把刻度噪声当成了阶跃。
  // 径向扫一圈再取极值，每个角度得到的是一个稳定代表值。
  const float ref = lum_at(canvas, cx, cy);   // 卡身底色（圆心在内侧）

  std::vector<float> samples;
  constexpr int kSteps = 12;
  for (int i = 0; i <= kSteps; ++i) {
    // 从顶边（角度 270°）到左边（180°）
    const float t = static_cast<float>(i) / static_cast<float>(kSteps);
    const float deg = 270.0f - 90.0f * t;
    const float rad = deg * 3.14159265358979f / 180.0f;
    const float ux = std::cos(rad);
    const float uy = std::sin(rad);
    float best = 0.0f;
    // 半径从 `radius - 2*ring` 扫到 `radius + 2*ring`：完全覆盖环带及其两侧。
    for (float rr = kRadius - kRing * 2.0f; rr <= kRadius + kRing * 2.0f; rr += 0.25f) {
      const float dev = lum_at(canvas, cx + rr * ux, cy + rr * uy) - ref;
      if (std::fabs(dev) > std::fabs(best)) best = dev;
    }
    samples.push_back(best);
  }

  CornerProfile out;
  out.total = std::fabs(samples.front() - samples.back());
  // 前三个点跳过：弧线**起点**紧接顶边亮带的末端，那里是“直边→弧”的过渡，
  // 采样点跨过它必然出现一个凹坑（实测 `119 82 113`，其中 82 就是坑底）。
  // 那是几何接缝，不是断口；要看的是弧线中段有没有断。
  //
  // 从 `i = 3` 开始比较（而不是 2）：差值 `samples[i] - samples[i-1]`，
  // 取 i=2 时前面那个 82 仍然参与，回弹照样被计入（实测踩到）。
  for (std::size_t i = 3; i < samples.size(); ++i) {
    const float step = samples[i] - samples[i - 1];
    out.max_step = std::max(out.max_step, std::fabs(step));
    // 主趋势是从亮到暗（step < 0），所以“回弹”就是 step > 0 的幅度。
    if (step > 0.0f) out.max_rebound = std::max(out.max_rebound, step);
  }
  return out;
}

/// 渲染一块指定形态的玻璃卡片，量出四边相对亮度。
[[nodiscard]] auto profile_for(HighlightEdges edges) -> EdgeProfile {
  UiRoot root;
  root.set_viewport({static_cast<float>(kCanvasW), static_cast<float>(kCanvasH)});
  root.set_theme(st::ui::Theme::light());

  auto card = std::make_unique<Panel>();
  auto* raw = card.get();
  raw->set_id("probe-card");
  root.set_content(std::move(card));

  // 底色用**实色**而不是玻璃：本用例量的是高光边，背景越平越好——
  // 玻璃的背板模糊会把底色揉成缓慢渐变，取样噪声盖过 `ring` 那 1.5px 的信号。
  raw->style().set_solid_background(Color{120, 120, 120, 255});
  raw->style().radius = kRadius;
  raw->style().shadow = {};
  raw->style().border_width = 0.0f;
  raw->style().top_highlight = Color{255, 255, 255, 230};
  raw->style().highlight_width = kRing;
  raw->style().highlight_edges = edges;

  st::ui::RenderContext context{root.theme(), nullptr, 1.0f};
  const Rect box{kBoxX, kBoxY, kBoxW, kBoxH};
  raw->measure(context, st::ui::Constraints{});
  raw->arrange(context, box);

  Canvas canvas{kCanvasW, kCanvasH, 1.0f};
  // 底色取**与卡身同一个灰**：
  //
  // 若清成黑（或任何深色），下边外侧那一圈黑会把“环带亮度”整体拉低，而
  // “内侧参照点”在卡身上——两个减数不同底色，量出的差值就不是高光，
  // 而是“卡身 vs 画布底”。实测（2026-11-20）用黑底时 `All` 量出
  // **top 61 / bottom 112**（下边“比上边亮一倍”），而实际渲染上下是对称的。
  // 把画布清成与卡身同色，两侧参照就同源了。
  canvas.clear(Color{120, 120, 120, 255});
  // ⚠ 直接绘**该元素**，不走 `root.paint`：后者会先自己 `layout()`，
  // 把 `set_content(单个面板)` 重新铺满整个视口，手动 `arrange` 被覆盖。
  raw->paint(context, canvas);

  // 取样区间：避开四个圆角（各内缩 `radius + 2`）。
  const float margin = kRadius + 2.0f;
  const float x_lo = kBoxX + margin;
  const float x_hi = kBoxX + kBoxW - margin;
  const float y_lo = kBoxY + margin;
  const float y_hi = kBoxY + kBoxH - margin;
  // 参照点：卡身**深处**（从各侧外缘向内）。
  //
  // 环带只占 `[0, ring]`，取样不看单一像素而是**扫描整个环带厚度**——
  // 环带在逐像素网格上常只占 1~2 像素，固定点极易落到抗锯齿过渡区。
  // 实测（2026-11-20）：同一张完全对称的图，左边取到 242、右边只取到 181，
  // 把“左右对称”量成了“差 61”——那是量法的错，不是被测对象的错。
  const int band_px = std::max(1, static_cast<int>(std::ceil(kRing)));
  const int depth_px = band_px + 6;

  const int x_lo_i = static_cast<int>(std::lround(x_lo));
  const int x_hi_i = static_cast<int>(std::lround(x_hi));
  const int y_lo_i = static_cast<int>(std::lround(y_lo));
  const int y_hi_i = static_cast<int>(std::lround(y_hi));
  const int top_i = static_cast<int>(std::lround(kBoxY));
  const int bot_i = static_cast<int>(std::lround(kBoxY + kBoxH));
  const int left_i = static_cast<int>(std::lround(kBoxX));
  const int right_i = static_cast<int>(std::lround(kBoxX + kBoxW));

  EdgeProfile p;
  // 每条边沿其长度方向取 11 个位置，每个位置在环带厚度内取**偏离卡身底色最远**
  // 的那一像素（无论更亮还是更暗），再对这些位置取中位数（抗局部噪点）。
  //
  // 为何不固定取最亮：`TopBottomShaded` 的侧边是**暗线**，取最亮会直接取到
  // 内侧卡身的底色（120），于是“侧边有暗线”与“侧边完全没画”量出同一个值。
  // 实测（2026-11-20）：那条用例带着真实错误仍然报 0/0。
  // 取“偏离最大”就能同时服务亮边与暗线，且不需要为每种形态写一套量法。
  std::vector<float> top, bottom, left, right;
  for (int k = 0; k <= 10; ++k) {
    const int x = x_lo_i + (x_hi_i - x_lo_i) * k / 10;
    const int y = y_lo_i + (y_hi_i - y_lo_i) * k / 10;
    top.push_back(peak_dev_v(canvas, top_i, top_i + band_px, x, top_i + depth_px));
    bottom.push_back(peak_dev_v(canvas, bot_i - band_px, bot_i, x, bot_i - depth_px));
    left.push_back(peak_dev(canvas, left_i, left_i + band_px, y, left_i + depth_px));
    right.push_back(peak_dev(canvas, right_i - band_px, right_i, y, right_i - depth_px));
  }
  p.top = median_of(top);
  p.bottom = median_of(bottom);
  p.left = median_of(left);
  p.right = median_of(right);
  return p;
}

}  // namespace

ST_TEST(highlight_all_lights_every_edge) {
  // `All`：四边都有光。这是基线——若本项都红，说明量法本身坏了。
  const EdgeProfile p = profile_for(HighlightEdges::All);
  const std::string msg = p.str();
  if (!(p.top > 8.0f)) ST_FAIL(std::format("上边应有光：{}", msg));
  if (!(p.bottom > 2.0f)) ST_FAIL(std::format("下边应有光：{}", msg));
  if (!(p.left > 2.0f)) ST_FAIL(std::format("左边应有光：{}", msg));
  if (!(p.right > 2.0f)) ST_FAIL(std::format("右边应有光：{}", msg));
}

ST_TEST(highlight_topbottom_leaves_sides_dark) {
  // `TopBottom`：上下有光，**左右完全无光**（被裁掉）。
  //
  // 这条与下面 `TopBottomShaded` 的用例互为对照：两者的差别正是
  // “侧边是完全没有”还是“侧边有一道暗线”——属性面看不出，只有像素能分。
  const EdgeProfile p = profile_for(HighlightEdges::TopBottom);
  const std::string msg = p.str();
  if (!(p.top > 8.0f)) ST_FAIL(std::format("上边应有光：{}", msg));
  // 侧边判据取 **< 1.0**：环带被完全裁掉，理论值 0；
  // 留 1.0 吸收圆角黄线与重采样的亚像素误差。
  if (!(std::fabs(p.left) < 1.0f)) ST_FAIL(std::format("左边应无光：{}", msg));
  if (!(std::fabs(p.right) < 1.0f)) ST_FAIL(std::format("右边应无光：{}", msg));
}

ST_TEST(highlight_topbottom_shaded_draws_side_shade) {
  // `TopBottomShaded`：上下亮、**左右各一道暗线**。
  //
  // 判据取“侧边比卡内底色**暗**”（负值）。实现里暗线是黑色叠 30% alpha，
  // 在 120 灰底上实测偏离 **-32**（2026-11-20）。
  //
  // 这条与上个用例（`TopBottom` 侧边 = 0）**互为对照**：两者在属性面上
  // 只差一个枚举值，而在画面上差着“没有侧边”与“一道暗线”。
  // 若两者都放宽成“側边不太亮就打绿”，它们就可以互相代替——那正是本组
  // 用例要防的事。
  const EdgeProfile p = profile_for(HighlightEdges::TopBottomShaded);
  const std::string msg = p.str();
  if (!(p.top > 8.0f)) ST_FAIL(std::format("上边应亮：{}", msg));
  // 侧边是**负值**（比卡身暗）。阈值取 -8：实测 -32，留大余量吸收主题变化。
  if (!(p.left < -8.0f)) ST_FAIL(std::format("左边应为暗线（负值）：{}", msg));
  if (!(p.right < -8.0f)) ST_FAIL(std::format("右边应为暗线（负值）：{}", msg));
}

ST_TEST(highlight_topbottom_shaded_top_brighter_than_bottom) {
  // 上下都是亮带，但**上边缘必须比下边缘亮**——那是“光从上来”的唯一线索。
  //
  // 若两带等亮，卡片读作“上下各镶一道银线”（无体积感），
  // 而这不是任何一种形态想要的效果。
  const EdgeProfile p = profile_for(HighlightEdges::TopBottomShaded);
  const std::string msg = p.str();
  if (!(p.top > p.bottom + 2.0f)) {
    ST_FAIL(std::format("上边应明显亮于下边：{}", msg));
  }
}

ST_TEST(highlight_topbottom_shaded_corner_is_gradual) {
  // 圆角处亮度应当**单调过渡**（亮带 → 暗线），不应出现断口。
  //
  // ## 这条测的到底是什么（逆向验证后的诚实标注）
  //
  // 逆向验证（把亮带改回 `solid` 硬切）**仍然绿**：因为侧边暗线现在画在
  // 亮带**上层**，它自己的纵向渐变把硬切的边界抹平了。也就是说——
  // **这条用例钉不住“亮带是否用渐变”的实现细节**，只钉得住
  // “弧上亮度是单调的、确实从亮到暗”。
  //
  // 保留它的理由：它是“圆角读起来是连续的”这个**用户可见结果**的回归护栏；
  // 若将来改成“只在圆角处出现台阶”的写法，它会红。
  // 但它**不能**用来证明“亮带用的是渐变”——那个由 8x 放大图人工确认
  //（`tools/highlight_corner_probe.cpp` 的 `hl-tbs-corner8x.png`）。
  const CornerProfile c = corner_profile_for(HighlightEdges::TopBottomShaded);
  const std::string msg = c.str();
  if (!(c.total > 60.0f)) {
    ST_FAIL(std::format("圆角两端应有明显亮→暗落差（否则没过渡）：{}", msg));
  }
  // 回弹容忍 14 级（抗锯齿 + 分层叠加的量化噪声）。
  if (!(c.max_rebound < 14.0f)) {
    ST_FAIL(std::format("圆角处有亮度回弹（断口）：{}", msg));
  }
}
