#include "st/raster/paint.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "st/core/hash.hpp"

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

/// LUT 的进程级共享存储。
///
/// ## 为什么可以共享
///
/// `rebuild()` 的 256 项查表**只依赖色标序列**（颜色 + 偏移），与渐变的几何
/// （`start_`/`end_`/`radius_`）完全无关——几何参与的是 `sample()` 里算 `position`
/// 那一步，之后才查表。所以「同一组色标」的任何两个渐变，无论画在哪个位置、
/// 多大尺寸，查表内容**逐位相同**。
///
/// ## 为什么要共享
///
/// 填一张表是 256 次「找段 + 插值」，实测 **1772 ns**（`docs/PAINT_DIAGNOSIS.md`
/// 的百分比探针）。而 `Element::paint_box` 的 `paint_highlight_edges` 每帧为
/// 每张卡片现场构造 4 个渐变（上下左右各一），96 卡片场景 = 384 次/帧 ≈ 0.68 ms。
/// 这 4 个渐变的色标在帧间**完全不变**（只随主题/圆角/尺寸变，而它们在一帧内是常量），
/// 于是这张表被反复重算。
///
/// ## 为什么安全
///
/// 表内容由色标指纹唯一决定，命中即逐位复用——**不改变任何像素**。
/// 表只增不减（条目数受色标组合数上界约束：真实界面里是个位数到几十），
/// 单条 1KB（256 × 4B），不做淘汰是正确的取舍（淘汰反而让下一帧重算）。
class LutPool {
 public:
  /// 取（或建立）该色标序列对应的 LUT。
  [[nodiscard]] static auto acquire(std::span<const GradientStop> stops)
      -> const std::array<math::Color, 256>& {
    const std::uint64_t key = fingerprint(stops);
    // 快路径：命中时只读共享表，不建临时 vector。
    {
      const std::scoped_lock lock(mutex_);
      const auto found = entries_.find(key);
      if (found != entries_.end()) return found->second;
    }
    // 未命中：在锁外算（256 次插值不持锁），再插入。
    auto built = std::make_unique<std::array<math::Color, 256>>();
    fill_lut(*built, stops);
    const std::scoped_lock lock(mutex_);
    // 双检：两个线程可能同时算同一份（值相同，后者丢弃自己的）。
    const auto inserted = entries_.try_emplace(key, std::move(*built));
    return inserted.first->second;
  }

 private:
  /// 色标指纹：逐字段 FNV-1a 混合（与字形缓存键同一手法，见 `text.cpp`）。
  /// 偏移量化到 1/65536 —— 远细于任何可感知的渐变台阶，同时保证浮点噪声
  /// 不会让「同一组色标」判成两组（那会让缓存永不命中）。
  [[nodiscard]] static auto fingerprint(std::span<const GradientStop> stops) -> std::uint64_t {
    std::uint64_t seed = 1469598103934665603ULL;
    const auto mix = [&seed](std::uint64_t value) {
      seed ^= value;
      seed *= 1099511628211ULL;
    };
    mix(stops.size());
    for (const GradientStop& stop : stops) {
      mix(static_cast<std::uint64_t>(std::lround(stop.offset * 65536.0f)));
      mix((static_cast<std::uint64_t>(stop.color.r) << 24U) |
          (static_cast<std::uint64_t>(stop.color.g) << 16U) |
          (static_cast<std::uint64_t>(stop.color.b) << 8U) |
          static_cast<std::uint64_t>(stop.color.a));
    }
    return seed;
  }

  /// 与 `Gradient::rebuild` **逐字相同**的填表逻辑（两处必须同口径，否则
  /// 走缓存与不走缓存会画出不同的字）。改动其一必须同步另一处。
  static void fill_lut(std::array<math::Color, 256>& lut,
                       std::span<const GradientStop> stops) {
    if (stops.empty()) {
      lut.fill(math::Color{0, 0, 0, 0});
      return;
    }
    std::vector<GradientStop> effective(stops.begin(), stops.end());
    if (effective.front().offset > 0.0f) {
      effective.insert(effective.begin(), GradientStop{0.0f, effective.front().color});
    }
    if (effective.back().offset < 1.0f) {
      effective.push_back(GradientStop{1.0f, effective.back().color});
    }
    for (std::size_t index = 0; index < lut.size(); ++index) {
      const float position = static_cast<float>(index) / static_cast<float>(lut.size() - 1);
      std::size_t upper = 1;
      while (upper < effective.size() && effective[upper].offset < position) ++upper;
      if (upper >= effective.size()) {
        lut[index] = effective.back().color;
        continue;
      }
      const GradientStop& low = effective[upper - 1];
      const GradientStop& high = effective[upper];
      const float span = high.offset - low.offset;
      const float ratio = span <= 0.0001f ? 0.0f : (position - low.offset) / span;
      lut[index] = math::lerp(low.color, high.color, ratio);
    }
  }

  static inline std::mutex mutex_{};
  static inline std::unordered_map<std::uint64_t, std::array<math::Color, 256>> entries_{};
};

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
  // 查表**只取决于色标**（几何不参与），而真实界面里同一组色标被反复结算
  // （每张卡片每帧 4 次高光渐变）。走进程级共享表，填充逻辑仍留在
  // `LutPool::fill_lut`（与这里曾经的实现逐字相同）。
  lut_ = LutPool::acquire(stops_);
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
  return sample_position(position);
}

auto Gradient::sample_position(float position) const noexcept -> math::Color {
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
