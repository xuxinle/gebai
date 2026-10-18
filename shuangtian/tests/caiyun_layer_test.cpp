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
