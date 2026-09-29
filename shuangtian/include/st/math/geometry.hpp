#pragma once

/// 几何基础：点/尺寸/矩形/内边距/二维仿射变换。全部为值类型，`constexpr` 可用。
/// 约定：坐标单位是**逻辑像素**（左上原点，y 向下）；缩放由 shell 层 DPR 处理。

#include <cmath>
#include <cstdint>
#include <optional>

namespace st::math {

struct Point {
  float x{0.0f};
  float y{0.0f};

  friend constexpr auto operator==(Point lhs, Point rhs) noexcept -> bool {
    return lhs.x == rhs.x && lhs.y == rhs.y;
  }
};

struct Size {
  float width{0.0f};
  float height{0.0f};
};

/// 整数像素矩形（渲染缓冲/脏区用）。
struct IntRect {
  int x{0};
  int y{0};
  int width{0};
  int height{0};

  [[nodiscard]] constexpr auto right() const noexcept -> int { return x + width; }
  [[nodiscard]] constexpr auto bottom() const noexcept -> int { return y + height; }
  [[nodiscard]] constexpr auto is_empty() const noexcept -> bool { return width <= 0 || height <= 0; }

  [[nodiscard]] constexpr auto inflate(int delta) const noexcept -> IntRect {
    return IntRect{x - delta, y - delta, width + delta * 2, height + delta * 2};
  }

  [[nodiscard]] constexpr auto offset(int dx, int dy) const noexcept -> IntRect {
    return IntRect{x + dx, y + dy, width, height};
  }

  [[nodiscard]] constexpr auto intersect(const IntRect& other) const noexcept -> IntRect {
    const int left = x > other.x ? x : other.x;
    const int top = y > other.y ? y : other.y;
    const int right_edge = right() < other.right() ? right() : other.right();
    const int bottom_edge = bottom() < other.bottom() ? bottom() : other.bottom();
    return IntRect{left, top, right_edge - left > 0 ? right_edge - left : 0,
                   bottom_edge - top > 0 ? bottom_edge - top : 0};
  }

  [[nodiscard]] constexpr auto contains(int px, int py) const noexcept -> bool {
    return px >= x && py >= y && px < right() && py < bottom();
  }
};

struct Insets {
  float left{0.0f};
  float top{0.0f};
  float right{0.0f};
  float bottom{0.0f};

  [[nodiscard]] static constexpr auto all(float value) noexcept -> Insets {
    return Insets{value, value, value, value};
  }
  [[nodiscard]] static constexpr auto symmetric(float horizontal, float vertical) noexcept -> Insets {
    return Insets{horizontal, vertical, horizontal, vertical};
  }
  [[nodiscard]] constexpr auto horizontal() const noexcept -> float { return left + right; }
  [[nodiscard]] constexpr auto vertical() const noexcept -> float { return top + bottom; }
};

struct Rect {
  float x{0.0f};
  float y{0.0f};
  float width{0.0f};
  float height{0.0f};

  [[nodiscard]] static constexpr auto from_ltrb(float left, float top, float right, float bottom)
      -> Rect {
    return Rect{left, top, right - left, bottom - top};
  }
  [[nodiscard]] static constexpr auto from_size(Point origin, Size size) noexcept -> Rect {
    return Rect{origin.x, origin.y, size.width, size.height};
  }

  [[nodiscard]] constexpr auto left() const noexcept -> float { return x; }
  [[nodiscard]] constexpr auto top() const noexcept -> float { return y; }
  [[nodiscard]] constexpr auto right() const noexcept -> float { return x + width; }
  [[nodiscard]] constexpr auto bottom() const noexcept -> float { return y + height; }
  [[nodiscard]] constexpr auto size() const noexcept -> Size { return Size{width, height}; }
  [[nodiscard]] constexpr auto center() const noexcept -> Point {
    return Point{x + width * 0.5f, y + height * 0.5f};
  }
  [[nodiscard]] constexpr auto is_empty() const noexcept -> bool { return width <= 0.0f || height <= 0.0f; }

  [[nodiscard]] constexpr auto contains(Point value) const noexcept -> bool {
    return value.x >= x && value.y >= y && value.x < right() && value.y < bottom();
  }

  [[nodiscard]] constexpr auto offset(float dx, float dy) const noexcept -> Rect {
    return Rect{x + dx, y + dy, width, height};
  }

  [[nodiscard]] constexpr auto inset(Insets insets) const noexcept -> Rect {
    return Rect{x + insets.left, y + insets.top, width - insets.horizontal(),
                height - insets.vertical()};
  }

  [[nodiscard]] constexpr auto inflate(float delta) const noexcept -> Rect {
    return Rect{x - delta, y - delta, width + delta * 2.0f, height + delta * 2.0f};
  }

  [[nodiscard]] constexpr auto intersect(const Rect& other) const noexcept -> Rect {
    const float left_edge = x > other.x ? x : other.x;
    const float top_edge = y > other.y ? y : other.y;
    const float right_edge = right() < other.right() ? right() : other.right();
    const float bottom_edge = bottom() < other.bottom() ? bottom() : other.bottom();
    const float out_width = right_edge - left_edge;
    const float out_height = bottom_edge - top_edge;
    return Rect{left_edge, top_edge, out_width > 0.0f ? out_width : 0.0f,
                out_height > 0.0f ? out_height : 0.0f};
  }

  [[nodiscard]] constexpr auto union_with(const Rect& other) const noexcept -> Rect {
    if (is_empty()) return other;
    if (other.is_empty()) return *this;
    const float left_edge = x < other.x ? x : other.x;
    const float top_edge = y < other.y ? y : other.y;
    const float right_edge = right() > other.right() ? right() : other.right();
    const float bottom_edge = bottom() > other.bottom() ? bottom() : other.bottom();
    return Rect::from_ltrb(left_edge, top_edge, right_edge, bottom_edge);
  }

  /// 向外取整到整数像素（覆盖整个矩形的最小整数框）。
  [[nodiscard]] auto round_out() const noexcept -> IntRect {
    const int left_edge = static_cast<int>(std::floor(x));
    const int top_edge = static_cast<int>(std::floor(y));
    const int right_edge = static_cast<int>(std::ceil(right()));
    const int bottom_edge = static_cast<int>(std::ceil(bottom()));
    return IntRect{left_edge, top_edge, right_edge - left_edge, bottom_edge - top_edge};
  }

  /// 向内取整（完全落在矩形内的最大整数框）。
  [[nodiscard]] auto round_in() const noexcept -> IntRect {
    const int left_edge = static_cast<int>(std::ceil(x));
    const int top_edge = static_cast<int>(std::ceil(y));
    const int right_edge = static_cast<int>(std::floor(right()));
    const int bottom_edge = static_cast<int>(std::floor(bottom()));
    return IntRect{left_edge, top_edge, right_edge - left_edge, bottom_edge - top_edge};
  }
};

/// 二维仿射变换：`| a c e |` / `| b d f |`（与 Canvas 2D 的 setTransform 同序）。
struct Transform {
  float a{1.0f};
  float b{0.0f};
  float c{0.0f};
  float d{1.0f};
  float e{0.0f};
  float f{0.0f};

  [[nodiscard]] static constexpr auto identity() noexcept -> Transform { return Transform{}; }

  [[nodiscard]] static constexpr auto translate(float dx, float dy) noexcept -> Transform {
    return Transform{1.0f, 0.0f, 0.0f, 1.0f, dx, dy};
  }

  [[nodiscard]] static constexpr auto scale(float sx, float sy) noexcept -> Transform {
    return Transform{sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
  }

  [[nodiscard]] static auto rotate(float radians) noexcept -> Transform {
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return Transform{cosine, sine, -sine, cosine, 0.0f, 0.0f};
  }

  /// `this * other`（先施加 other，再施加 this）。
  [[nodiscard]] constexpr auto concat(const Transform& other) const noexcept -> Transform {
    return Transform{a * other.a + c * other.b,
                     b * other.a + d * other.b,
                     a * other.c + c * other.d,
                     b * other.c + d * other.d,
                     a * other.e + c * other.f + e,
                     b * other.e + d * other.f + f};
  }

  [[nodiscard]] constexpr auto apply(Point value) const noexcept -> Point {
    return Point{a * value.x + c * value.y + e, b * value.x + d * value.y + f};
  }

  [[nodiscard]] constexpr auto apply_vector(Point value) const noexcept -> Point {
    return Point{a * value.x + c * value.y, b * value.x + d * value.y};
  }

  /// 逆变换（奇异矩阵返回空）。
  [[nodiscard]] constexpr auto invert() const noexcept -> std::optional<Transform> {
    const float determinant = a * d - b * c;
    if (determinant == 0.0f) return std::nullopt;
    const float inverse = 1.0f / determinant;
    return Transform{d * inverse, -b * inverse, -c * inverse, a * inverse,
                     (c * f - d * e) * inverse, (b * e - a * f) * inverse};
  }
};

/// 线性插值（几何量；颜色插值见 color.hpp）。
[[nodiscard]] constexpr auto lerp(float from, float to, float t) noexcept -> float {
  return from + (to - from) * t;
}

[[nodiscard]] constexpr auto clamp01(float value) noexcept -> float {
  return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

[[nodiscard]] constexpr auto clampf(float value, float low, float high) noexcept -> float {
  return value < low ? low : (value > high ? high : value);
}

}  // namespace st::math
