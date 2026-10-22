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
    case LayerKind::Model3d: return "model3d";
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
  if (name == "model3d") return LayerKind::Model3d;
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

/// 阻尼弹跳的竖直位移（像素；正值 = 向下，与 `offset_y` 同向）。
/// 定义在文件末尾（`evaluate_layer` 要用它）。
[[nodiscard]] auto bounce_offset(const Layer& layer, float local_time) -> float;

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
  if (layer.enter == 0) {
    // 注意：`enter == 0` 只是**不加入场动画**，弹跳仍可以单独生效
    //（“从第一帧就在那里跳”是真实需求：图标本来就浮在桌面上）。
    state.offset_y += bounce_offset(layer, local_time);
    return state;
  }
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
  // 弹跳叠在入场位移之上（两者互不干扰：入场管“淡入/上浮”，弹跳管“反复跳”）。
  state.offset_y += bounce_offset(layer, local_time);
  return state;
}

/// 阻尼弹跳的竖直位移（像素；正值 = 向下，与 `offset_y` 同向）。
///
/// ## 公式与为何这样选
///
/// `offset = amp * e^(-damping * t) * |sin(π * hz * t)|`
///
/// - `|sin|`：取绝对值才能表达“弹起——落地——再弹起”。直接用 `sin`
///   会让位移变成负值（图标沉到基线以下，看起来像掉进地板）。
/// - `e^(-damping*t)`：能量衰减。没有它就是一个永动机，也就无法“最后定住”。
/// - 取负号（在调用处）：位移向上为弹起。这里返回**正位移 = 向下**，
///   与 `offset_y` 的语义一致（画布 y 向下）。
///
/// ## 为什么不是物理模拟（重力 + 弹性系数）
///
/// 真实弹跳要解微分方程，参数是“重力加速度 + 回弹系数 + 摩擦”，
/// 而写脚本的人（和 AI）想的是“跳三下停住”——那是**观感**参数不是**物理**参数。
/// 一个衰减正弦只需三个数就能精确控制“跳几下 / 多快停”，且**严格可复现**
/// （纯函数，无状态累积——导出并行渲染时不会因线程顺序产生差异）。
/// ## 为何用「逐跳抛物线」而不是单个 |sin|
///
/// 第一版用 `|sin(π·hz·t)|`，被用户判“僵硬/假”。根因是**速度分布反了**：
/// 正弦在顶点与地面速度都**最快**，而真实抛体是 **顶点最慢、落地最快**
/// （这正是 Disney 12 原则里的 “slow in & slow out”）。
/// 实测：改为逐跳抛物线后，同一段弹跳立刻“有重量”。
///
/// 所以现在是：把时间切成半周期（每跳一个），在每跳内部用
/// `4·h·s(1-s)`（s 为 0..1 的进度）—— 它是**标准抛物线**，顶点速度为 0。
/// 幅度仍按 `e^(-damping·t)` 衰减，于是“跳几下就停”。
[[nodiscard]] auto bounce_offset(const Layer& layer, float local_time) -> float {
  if (layer.bounce <= 0.0f) return 0.0f;
  const float t = local_time - layer.bounce_at;
  if (t <= 0.0f) return 0.0f;
  const float hz = std::max(0.05f, layer.bounce_hz);
  const float damping = std::max(0.0f, layer.bounce_damping);
  // 每跳的持续时间（秒）与当前处于第几跳
  const float hop_duration = 1.0f / (2.0f * hz);
  const float hop_index = std::floor(t / hop_duration);
  const float s = (t - hop_index * hop_duration) / hop_duration;   // 0..1
  // 抛物线：`4·s·(1-s)` 在 s=0/1 时为 0（落地）、s=0.5 时达最大 1（顶点）
  const float arc = 4.0f * s * (1.0f - s);
  const float envelope = std::exp(-damping * t);
  return -layer.bounce * envelope * arc;
}

}  // namespace caiyun
