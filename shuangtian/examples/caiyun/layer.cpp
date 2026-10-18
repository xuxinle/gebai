/// 裁云 · 通用图层系统实现（见 `layer.hpp`）。

#include "layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

namespace caiyun {

auto layer_kind_name(LayerKind kind) -> std::string_view {
  switch (kind) {
    case LayerKind::Rect: return "rect";
    case LayerKind::Ellipse: return "ellipse";
    case LayerKind::Line: return "line";
    case LayerKind::Text: return "text";
    case LayerKind::Image: return "image";
    case LayerKind::Path: return "path";
    case LayerKind::Glow: return "glow";
    case LayerKind::Particles: return "particles";
  }
  return "rect";
}

auto layer_kind_from(std::string_view name) -> std::optional<LayerKind> {
  if (name == "rect") return LayerKind::Rect;
  if (name == "ellipse") return LayerKind::Ellipse;
  if (name == "line") return LayerKind::Line;
  if (name == "text") return LayerKind::Text;
  if (name == "image") return LayerKind::Image;
  if (name == "path") return LayerKind::Path;
  if (name == "glow") return LayerKind::Glow;
  if (name == "particles") return LayerKind::Particles;
  return std::nullopt;
}

namespace {

/// 解析"设计像素或画面比例"的坐标值。
///
/// 两种写法：`"640"`（设计像素）与 `"50%"`（画面比例）。
/// 用字符串而不是数字，是因为 JSON 里两者都能写、而 `double` 存不下百分比语义。
[[nodiscard]] auto parse_dim(const std::string& raw, float canvas, float design) -> float {
  if (raw.empty()) return 0.0f;
  if (raw.back() == '%') {
    const std::string body = raw.substr(0, raw.size() - 1);
    const auto value = std::strtof(body.c_str(), nullptr);
    return canvas * (value / 100.0f);
  }
  const auto value = std::strtof(raw.c_str(), nullptr);
  // 设计像素 → 物理像素（按画布宽 / 设计宽的比例缩放；高度同比例，保证不变形）
  const float scale = design > 0.5f ? canvas / design : 1.0f;
  return value * scale;
}

/// 缓动：与 motion.cpp 同口径（`ease_out_cubic`），但独立实现避免循环依赖。
[[nodiscard]] auto ease_out_cubic(float t) -> float {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const float inverse = 1.0f - clamped;
  return 1.0f - inverse * inverse * inverse;
}

}  // namespace

auto evaluate_layer(const Layer& layer, float local_time, float canvas_w, float canvas_h,
                    float design_w, float design_h) -> LayerState {
  LayerState state{};
  state.x = parse_dim(layer.x, canvas_w, design_w);
  state.y = parse_dim(layer.y, canvas_h, design_h);
  state.w = parse_dim(layer.w, canvas_w, design_w);
  state.h = parse_dim(layer.h, canvas_h, design_h);
  state.alpha = std::clamp(layer.opacity, 0.0f, 1.0f);

  // 入场动画：三种就够（fade / rise / scale）。
  //
  // 为什么不做"每层各自的时间线表达式"：那等于在 JSON 里写一个动画语言，
  // 用户要学的比写 C++ 还多。三种预设覆盖了绝大多数需求，
  // 更复杂的时间关系用场景级的转场与运镜表达。
  if (layer.enter == 0) return state;
  const float elapsed = local_time - layer.enter_at;
  if (elapsed <= 0.0f) {
    state.alpha = 0.0f;   // 还没到入场时间：完全不可见（而不是"画一个透明的"）
    return state;
  }
  const float progress = ease_out_cubic(elapsed / std::max(0.01f, layer.enter_dur));
  switch (layer.enter) {
    case 1:   // fade
      state.alpha *= progress;
      break;
    case 2:   // rise：上浮 + 淡入（最常用的"内容浮现"）
      state.alpha *= progress;
      state.offset_y = (1.0f - progress) * 36.0f;
      break;
    case 3:   // scale：轻微放大落定（适合卡片/图片）
      state.alpha *= progress;
      state.scale = 0.92f + 0.08f * progress;
      break;
    default:
      break;
  }
  return state;
}

}  // namespace caiyun
