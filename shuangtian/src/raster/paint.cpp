#include "st/raster/paint.hpp"

#include <algorithm>
#include <cmath>

namespace st::raster {
namespace {

inline constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] auto sorted_stops(std::vector<GradientStop> stops) -> std::vector<GradientStop> {
  std::ranges::sort(stops, {}, &GradientStop::offset);
  for (auto& stop : stops) {
    stop.offset = stop.offset < 0.0f ? 0.0f : (stop.offset > 1.0f ? 1.0f : stop.offset);
  }
  return stops;
}

}  // namespace

auto Gradient::linear(math::Point from, math::Point to, std::vector<GradientStop> stops) -> Gradient {
  Gradient gradient;
  gradient.kind_ = Kind::Linear;
  gradient.start_ = from;
  gradient.end_ = to;
  gradient.stops_ = sorted_stops(std::move(stops));
  gradient.rebuild();
  return gradient;
}

auto Gradient::radial(math::Point center, float radius, std::vector<GradientStop> stops) -> Gradient {
  Gradient gradient;
  gradient.kind_ = Kind::Radial;
  gradient.start_ = center;
  gradient.radius_ = radius;
  gradient.stops_ = sorted_stops(std::move(stops));
  gradient.rebuild();
  return gradient;
}

auto Gradient::sweep(math::Point center, std::vector<GradientStop> stops) -> Gradient {
  Gradient gradient;
  gradient.kind_ = Kind::Sweep;
  gradient.start_ = center;
  gradient.stops_ = sorted_stops(std::move(stops));
  gradient.rebuild();
  return gradient;
}

void Gradient::rebuild() {
  if (stops_.empty()) {
    lut_.fill(math::Color{0, 0, 0, 0});
    return;
  }
  std::vector<GradientStop> effective = stops_;
  if (effective.front().offset > 0.0f) {
    effective.insert(effective.begin(), GradientStop{0.0f, effective.front().color});
  }
  if (effective.back().offset < 1.0f) {
    effective.push_back(GradientStop{1.0f, effective.back().color});
  }
  for (std::size_t index = 0; index < lut_.size(); ++index) {
    const float position = static_cast<float>(index) / static_cast<float>(lut_.size() - 1);
    std::size_t upper = 1;
    while (upper < effective.size() && effective[upper].offset < position) ++upper;
    if (upper >= effective.size()) {
      lut_[index] = effective.back().color;
      continue;
    }
    const GradientStop& low = effective[upper - 1];
    const GradientStop& high = effective[upper];
    const float span = high.offset - low.offset;
    const float ratio = span <= 0.0001f ? 0.0f : (position - low.offset) / span;
    lut_[index] = math::lerp(low.color, high.color, ratio);
  }
}

auto Gradient::sample(math::Point point) const noexcept -> math::Color {
  float position = 0.0f;
  switch (kind_) {
    case Kind::Linear: {
      const float dx = end_.x - start_.x;
      const float dy = end_.y - start_.y;
      const float length_squared = dx * dx + dy * dy;
      if (length_squared <= 0.0001f) {
        position = 0.0f;
        break;
      }
      position = ((point.x - start_.x) * dx + (point.y - start_.y) * dy) / length_squared;
      break;
    }
    case Kind::Radial: {
      if (radius_ <= 0.0001f) {
        position = 0.0f;
        break;
      }
      const float dx = point.x - start_.x;
      const float dy = point.y - start_.y;
      position = std::sqrt(dx * dx + dy * dy) / radius_;
      break;
    }
    case Kind::Sweep: {
      const float angle = std::atan2(point.y - start_.y, point.x - start_.x);
      position = (angle + kPi) / (2.0f * kPi);
      break;
    }
  }
  const float clamped = math::clamp01(position);
  const float scaled = clamped * static_cast<float>(lut_.size() - 1);
  const auto index = static_cast<std::size_t>(scaled);
  const std::size_t next = index + 1 < lut_.size() ? index + 1 : lut_.size() - 1;
  const float fraction = scaled - static_cast<float>(index);
  return math::lerp(lut_[index], lut_[next], fraction);
}

auto Paint::solid(math::Color color) -> Paint {
  Paint paint;
  paint.color_ = color;
  paint.gradient_ = nullptr;
  return paint;
}

auto Paint::with_gradient(Gradient gradient) -> Paint {
  Paint paint;
  paint.gradient_ = std::make_shared<const Gradient>(std::move(gradient));
  const auto& stops = paint.gradient_->stops();
  paint.color_ = stops.empty() ? math::Color{0, 0, 0, 255} : stops.front().color;
  return paint;
}

auto Paint::sample(math::Point point) const noexcept -> math::Color {
  if (gradient_ != nullptr) return gradient_->sample(point);
  return color_;
}

}  // namespace st::raster
