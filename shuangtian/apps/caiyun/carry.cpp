/// 裁云跨场承接层的实现（见 `carry.hpp`）。

#include "carry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>

namespace caiyun {
namespace {

/// 解析「设计像素」或「画面比例」。
///
/// 与 `layer.cpp` 的 `resolve_dim` 同一口径：纯数字 / 带 `%` 的字符串。
/// 但这里**独立实现一份**，因为两处失败语义不同——图层的失败是"画到原点"，
/// 承接的失败会让跨场断裂（比画错更贵），所以这里对非法输入退到 `fallback`
/// 而不是 0。
[[nodiscard]] auto parse_dim_or_ratio(std::string_view raw, float base, float fallback) -> float {
  if (raw.empty()) return fallback;
  // 去掉前后空白
  std::size_t begin = 0;
  std::size_t end = raw.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) --end;
  if (begin >= end) return fallback;
  const std::string_view text = raw.substr(begin, end - begin);

  const bool percent = text.back() == '%';
  std::string_view number = percent ? text.substr(0, text.size() - 1) : text;
  // 再去一次尾部空白：`" 100 %"` 去掉 `%` 后尾部还有个空格，
  // 而 `stod` 只消费到数字，于是会被下面的 `consumed != size` 判成"尾部有杂物"
  // 而回退——一个纯空格造成的静默失效（实测踩过）。
  while (!number.empty() &&
         std::isspace(static_cast<unsigned char>(number.back())) != 0) {
    number.remove_suffix(1);
  }
  if (number.empty()) return fallback;
  double value = 0.0;
  std::size_t consumed = 0;
  try {
    value = std::stod(std::string(number), &consumed);
  } catch (const std::exception&) {
    return fallback;
  }
  if (consumed != number.size()) return fallback;  // 尾部有杂物："50px" 之类
  const double scaled =
      percent ? value * 0.01 * static_cast<double>(base) : value;
  return static_cast<float>(scaled);
}

// —— 缓动（只收承接真正用得上的几种；名字与 onetake 的动作库同名，便于对照）——
auto linear(float u) -> float { return u; }
auto smooth(float u) -> float { return u * u * (3.0f - 2.0f * u); }
auto smoother(float u) -> float { return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f); }
auto cubic_out(float u) -> float { const float v = 1.0f - u; return 1.0f - v * v * v; }
auto quart_out(float u) -> float { const float v = 1.0f - u; return 1.0f - v * v * v * v; }
auto expo_out(float u) -> float {
  if (u <= 0.0f) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  return (1.0f - std::pow(2.0f, -10.0f * u)) / (1.0f - std::pow(2.0f, -10.0f));
}
auto expo_in_out(float u) -> float {
  if (u <= 0.0f) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  // 与 onetake 的 expoInOut 同形：前一半加速，后一半减速
  const auto expo_in = [](float v) -> float {
    return (std::pow(2.0f, 10.0f * v) - 1.0f) / 1023.0f;
  };
  return u < 0.5f ? expo_in(2.0f * u) * 0.5f : 1.0f - expo_in(2.0f - 2.0f * u) * 0.5f;
}

}  // namespace

auto resolve_carry_dim(std::string_view raw, float base, float fallback) -> float {
  return parse_dim_or_ratio(raw, base, fallback);
}

auto carry_curve_by_name(std::string_view name) -> CarryCurve {
  if (name.empty() || name == "expo_out" || name == "expoOut") return &expo_out;
  if (name == "linear") return &linear;
  if (name == "smooth") return &smooth;
  if (name == "smoother") return &smoother;
  if (name == "cubic_out" || name == "cubicOut") return &cubic_out;
  if (name == "quart_out" || name == "quartOut") return &quart_out;
  if (name == "expo_in_out" || name == "expoInOut") return &expo_in_out;
  return nullptr;
}

auto evaluate_carry(const CarryOut& carry, double time, float design_w, float design_h)
    -> CarryPose {
  CarryPose pose{};
  if (carry.name.empty()) return pose;  // 没名字 = 不算承接（见 CarryOut 注释）
  const double span = carry.dur > 1e-4 ? carry.dur : 1e-4;
  const double into_span = time - carry.at;
  // 活动区间含端点：起点那一帧与终点那一帧都要画（否则边界两侧会各缺一帧，
  // 而那正是验收器取样的位置）。
  if (into_span < 0.0 || into_span > span) return pose;

  const float raw_k = static_cast<float>(std::clamp(into_span / span, 0.0, 1.0));
  const CarryCurve curve = carry_curve_by_name(carry.curve);
  const float k = curve != nullptr ? curve(raw_k) : expo_out(raw_k);

  const auto lerp = [](float a, float b, float t) -> float { return a + (b - a) * t; };
  const float fx = resolve_carry_dim(carry.from.x, design_w, 0.0f);
  const float fy = resolve_carry_dim(carry.from.y, design_h, 0.0f);
  const float fw = resolve_carry_dim(carry.from.w, design_w, 0.0f);
  const float fh = resolve_carry_dim(carry.from.h, design_h, 0.0f);
  const float tx = resolve_carry_dim(carry.to.x, design_w, 0.0f);
  const float ty = resolve_carry_dim(carry.to.y, design_h, 0.0f);
  const float tw = resolve_carry_dim(carry.to.w, design_w, 0.0f);
  const float th = resolve_carry_dim(carry.to.h, design_h, 0.0f);

  pose.active = true;
  pose.x = lerp(fx, tx, k);
  pose.y = lerp(fy, ty, k);
  pose.w = lerp(fw, tw, k);
  pose.h = lerp(fh, th, k);
  pose.radius = lerp(carry.from.radius, carry.to.radius, k);
  pose.k = k;
  // 淡出：在**未缓动的**进度（`raw_k`）上算——`fade_out` 是“最后占多长一段”的
  // 时间比例，不该被缓动曲线扭曲（否则不同曲线下淡入的“感觉长度”不一致）。
  if (carry.fade_out > 0.001f) {
    const float window = std::clamp(carry.fade_out, 0.0f, 1.0f);
    const float fade_start = 1.0f - window;
    pose.alpha = raw_k <= fade_start
                     ? 1.0f
                     : std::clamp(1.0f - (raw_k - fade_start) / window, 0.0f, 1.0f);
  }
  pose.name = carry.name.c_str();
  return pose;
}

auto collect_active_carries(const Script& script, double time, float design_w, float design_h)
    -> std::vector<CarryTrack> {
  std::vector<CarryTrack> out;
  for (const Scene& scene : script.scenes) {
    if (scene.carry.name.empty()) continue;
    const CarryPose pose = evaluate_carry(scene.carry, time, design_w, design_h);
    if (!pose.active) continue;
    out.push_back(CarryTrack{scene.carry.name, pose.x, pose.y, pose.w, pose.h});
  }
  return out;
}

}  // namespace caiyun
