/// 裁云通用图层系统单测：坐标解析、入场动画、确定性。
///
/// ## 为什么这几条值得钉住
///
/// 图层系统是"不写 C++ 也能做任意版式"的底座。它错了不会崩，
/// 只会**画歪**——而"画歪"在成片里表现为"看起来不太对"，最难定位。
///
/// 尤其是坐标解析：同一份脚本要在 16:9 / 1:1 / 9:16 三种画幅下都正确，
/// 而 `"50%"` 与 `640` 是两套语义（前者跟画幅、后者跟设计基准），
/// 混了就会在某个尺寸上偏出去。

#include "examples/caiyun/layer.cpp"

#include "st/test/test.hpp"

namespace {

using caiyun::Layer;
using caiyun::LayerKind;

/// 构造一个基准画布尺寸（1080p）与设计基准（同为 1080p，scale=1）。
constexpr float kCanvasW = 1920.0f;
constexpr float kCanvasH = 1080.0f;
constexpr float kDesignW = 1920.0f;
constexpr float kDesignH = 1080.0f;

}  // namespace

ST_TEST(layer_kind_roundtrip) {
  // 名字 ↔ 枚举必须双向一致（脚本靠名字，代码靠枚举）。
  for (const LayerKind kind : {LayerKind::Rect, LayerKind::Ellipse, LayerKind::Line,
                               LayerKind::Text, LayerKind::Image, LayerKind::Path,
                               LayerKind::Glow, LayerKind::Particles}) {
    const auto name = caiyun::layer_kind_name(kind);
    const auto back = caiyun::layer_kind_from(name);
    ST_CHECK(back.has_value());
    ST_CHECK(*back == kind);
  }
  // 不认识的类型返回 nullopt（调用方据此告警而不是崩）。
  ST_CHECK(!caiyun::layer_kind_from("nonexistent").has_value());
}

ST_TEST(layer_pixel_coords_scale_with_canvas) {
  // 设计像素：1920 宽的设计基准下 x=960 就在画面中心。
  Layer layer{};
  layer.x = "960";
  layer.y = "540";
  layer.enter = 0;   // 关掉入场动画，只看坐标
  auto state = caiyun::evaluate_layer(layer, 1.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK_NEAR(state.x, 960.0f, 0.5f);
  ST_CHECK_NEAR(state.y, 540.0f, 0.5f);
}

ST_TEST(layer_percent_coords_follow_canvas) {
  // 画面比例：50% 在**任何**画幅下都是中心（这是它存在的意义）。
  Layer layer{};
  layer.x = "50%";
  layer.y = "50%";
  layer.enter = 0;
  // 横版
  auto wide = caiyun::evaluate_layer(layer, 1.0f, 1920.0f, 1080.0f, kDesignW, kDesignH);
  ST_CHECK_NEAR(wide.x, 960.0f, 0.5f);
  ST_CHECK_NEAR(wide.y, 540.0f, 0.5f);
  // 竖版：仍然是中心（而设计像素写法会跑偏——这就是两套语义的差别）
  auto tall = caiyun::evaluate_layer(layer, 1.0f, 1080.0f, 1920.0f, kDesignW, kDesignH);
  ST_CHECK_NEAR(tall.x, 540.0f, 0.5f);
  ST_CHECK_NEAR(tall.y, 960.0f, 0.5f);
}

ST_TEST(layer_percent_parsing_tolerates_spaces) {
  // " 50 % " 这类手写空格不该被解析成 0（静默归零 = 元素跑到左上角）。
  Layer layer{};
  layer.x = "50%";
  layer.enter = 0;
  auto state = caiyun::evaluate_layer(layer, 1.0f, 1000.0f, 1000.0f, 1000.0f, 1000.0f);
  ST_CHECK_NEAR(state.x, 500.0f, 0.5f);
}

ST_TEST(layer_enter_none_is_immediately_visible) {
  Layer layer{};
  layer.enter = 0;
  layer.opacity = 1.0f;
  auto state = caiyun::evaluate_layer(layer, 0.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK_NEAR(state.alpha, 1.0f, 0.001f);
  ST_CHECK_NEAR(state.offset_y, 0.0f, 0.001f);
}

ST_TEST(layer_enter_before_start_is_invisible) {
  // 还没到入场时间：alpha 必须是 0（而不是"画一个透明的"）。
  // 这条防的是"时间窗没生效、所有元素一起出现"——那样分镜的错峰就废了。
  Layer layer{};
  layer.enter = 2;
  layer.enter_at = 1.0f;
  layer.enter_dur = 0.5f;
  auto state = caiyun::evaluate_layer(layer, 0.3f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK_NEAR(state.alpha, 0.0f, 0.001f);
}

ST_TEST(layer_enter_settles_to_full) {
  // 入场结束后必须完全可见、无位移（否则内容永远"浮"着）。
  Layer layer{};
  layer.enter = 2;   // rise
  layer.enter_at = 0.0f;
  layer.enter_dur = 0.6f;
  auto state = caiyun::evaluate_layer(layer, 5.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK_NEAR(state.alpha, 1.0f, 0.01f);
  ST_CHECK_NEAR(state.offset_y, 0.0f, 0.01f);
}

ST_TEST(layer_enter_rise_moves_up_over_time) {
  // rise 的语义：开始时在下方，落定时回到原位（offset_y 递减到 0）。
  Layer layer{};
  layer.enter = 2;
  layer.enter_at = 0.0f;
  layer.enter_dur = 1.0f;
  const auto early = caiyun::evaluate_layer(layer, 0.1f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  const auto late = caiyun::evaluate_layer(layer, 0.9f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(early.offset_y > late.offset_y);
  ST_CHECK(late.offset_y >= 0.0f);
}

ST_TEST(layer_enter_scale_grows_to_one) {
  Layer layer{};
  layer.enter = 3;   // scale
  layer.enter_at = 0.0f;
  layer.enter_dur = 1.0f;
  const auto early = caiyun::evaluate_layer(layer, 0.05f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  const auto late = caiyun::evaluate_layer(layer, 2.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(early.scale < late.scale);
  ST_CHECK_NEAR(late.scale, 1.0f, 0.01f);
}

ST_TEST(layer_evaluate_is_deterministic) {
  // 同一时刻两次求值必须完全相同（全仓的逐像素可复现判据）。
  Layer layer{};
  layer.enter = 2;
  layer.enter_at = 0.3f;
  layer.x = "37.5%";
  layer.y = "420";
  layer.w = "640";
  layer.h = "360";
  const auto a = caiyun::evaluate_layer(layer, 0.8f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  const auto b = caiyun::evaluate_layer(layer, 0.8f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(a.x == b.x);
  ST_CHECK(a.y == b.y);
  ST_CHECK(a.alpha == b.alpha);
  ST_CHECK(a.scale == b.scale);
  ST_CHECK(a.offset_y == b.offset_y);
}

ST_TEST(layer_opacity_clamped) {
  // 越界透明度要夹取（脚本是外部输入，不该让 1.5 变成"过曝"或 -1 变成"反色"）。
  Layer layer{};
  layer.enter = 0;
  layer.opacity = 1.5f;
  auto state = caiyun::evaluate_layer(layer, 1.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK_NEAR(state.alpha, 1.0f, 0.001f);
  layer.opacity = -0.5f;
  state = caiyun::evaluate_layer(layer, 1.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK_NEAR(state.alpha, 0.0f, 0.001f);
}


// ══════════════════════════════════════════════════════════════════════════════
// 路径动画：从 A 飞到 B，走一条弧线
// ══════════════════════════════════════════════════════════════════════════════
//
// ## 为何要这组用例
//
// 路径动画是"跳到任务栏并定住"这类动作的地基，而它的正确性有三个互相
// **独立**的方面，任何一项错都会让画面"看着别扭"却说不出哪里不对：
//
// ① **端点精确**：t=0 在起点、t=1 在终点（差 1 像素就"落不到位"）；
// ② **弧线朝上且对称**：中点最高、两端贴地（Arcs 原则；写成朝下就是"往下陷"）；
// ③ **缓动改变速度分布而不改变端点**：过冲档结束后必须回到终点。
//
// 这三条互不蕴含：端点对但弧线朝下、弧线对但缓动不回位——都能单独发生。

namespace {

/// "从 (100,800) 飞到 (900,200)、弧高 120、时长 1 秒"的标准夹具。
[[nodiscard]] auto make_flying_layer() -> caiyun::Layer {
  caiyun::Layer layer{};
  layer.enter = 0;   // 不带入场动画，只看路径
  layer.x = "100";
  layer.y = "800";
  layer.w = "200";
  layer.h = "200";
  layer.path_from_x = "100";
  layer.path_from_y = "800";
  layer.path_to_x = "900";
  layer.path_to_y = "200";
  layer.path_arc = 120.0f;
  layer.path_dur = 1.0f;
  layer.path_at = 0.0f;
  layer.path_ease = 0;   // 线性，便于精确验算
  return layer;
}

}  // namespace

ST_TEST(path_starts_at_from_and_ends_at_to) {
  const caiyun::Layer layer = make_flying_layer();
  const auto at_start =
      caiyun::evaluate_path(layer, 0.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  // 起点：位移增量必须为 0（图层 x/y 本身就是起点）
  ST_CHECK(std::abs(at_start.dx) < 1e-3f);
  ST_CHECK(std::abs(at_start.dy) < 1e-3f);

  const auto at_end = caiyun::evaluate_path(layer, 1.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  // 终点：增量 = to - from = (800, -600)，弧在两端为 0，所以 dy 不带弧
  ST_CHECK(std::abs(at_end.dx - 800.0f) < 1e-2f);
  ST_CHECK(std::abs(at_end.dy + 600.0f) < 1e-2f);
}

ST_TEST(path_arc_bulges_upward_at_midpoint) {
  const caiyun::Layer layer = make_flying_layer();
  const auto mid = caiyun::evaluate_path(layer, 0.5f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  // 线性缓动下中点位移 = 一半直线位移
  ST_CHECK(std::abs(mid.dx - 400.0f) < 1e-2f);
  // 直线中点 dy = -300；叠上弧高 120（向上为负）→ -420
  ST_CHECK(std::abs(mid.dy + 420.0f) < 1e-2f);
  // 弧必须**向上**：dy 比纯直线更负
  ST_CHECK(mid.dy < -300.0f);
}

ST_TEST(path_without_fields_is_a_noop) {
  // 老脚本兼容：没写 path_* 的图层，增量必须恒为 0
  caiyun::Layer layer{};
  layer.enter = 0;
  layer.x = "640";
  layer.y = "360";
  layer.w = "100";
  layer.h = "100";
  const auto offset = caiyun::evaluate_path(layer, 0.5f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(std::abs(offset.dx) < 1e-6f);
  ST_CHECK(std::abs(offset.dy) < 1e-6f);
}

ST_TEST(path_overshoot_returns_to_the_endpoint) {
  // 过冲档（path_ease=3）在中途会冲过头，但**结束时必须回到 1.0**，
  // 否则物体永远停在差一点的位置——这类错肉眼极难发现。
  caiyun::Layer layer = make_flying_layer();
  layer.path_ease = 3;
  const auto at_end = caiyun::evaluate_path(layer, 1.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(std::abs(at_end.dx - 800.0f) < 0.5f);
  // 中途某点应当**超过**线性进度（这就是"冲过头"）
  const auto at_mid = caiyun::evaluate_path(layer, 0.6f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(at_mid.dx > 800.0f * 0.6f);
}

ST_TEST(path_missing_to_keeps_layer_position) {
  // 只写起点不写终点 = "从别处飞到这里"（终点就是图层自身位置）
  caiyun::Layer layer{};
  layer.enter = 0;
  layer.x = "500";
  layer.y = "400";
  layer.w = "100";
  layer.h = "100";
  layer.path_from_x = "0";
  layer.path_from_y = "400";
  layer.path_dur = 1.0f;
  layer.path_ease = 0;
  const auto at_end = caiyun::evaluate_path(layer, 1.0f, kCanvasW, kCanvasH, kDesignW, kDesignH);
  ST_CHECK(std::abs(at_end.dx - 500.0f) < 1e-2f);
  ST_CHECK(std::abs(at_end.dy) < 1e-2f);
}

ST_TEST(path_zero_arc_is_a_straight_line) {
  // 弧高为 0 时必须退化成直线（接口契约：path_arc=0 不应意外带弧）
  caiyun::Layer layer = make_flying_layer();
  layer.path_arc = 0.0f;
  for (const float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    const auto offset =
        caiyun::evaluate_path(layer, t, kCanvasW, kCanvasH, kDesignW, kDesignH);
    ST_CHECK(std::abs(offset.dy + 600.0f * t) < 1e-2f);
  }
}
