#pragma once

/// 三维数学：向量与 4×4 矩阵（列主序，OpenGL 约定）。
///
/// 为什么放在 `math` 而不是 `raster/gl`：这些是**纯数学**，与谁在渲染无关——
/// 放进 GL 后端会让"算变换"这件事绑死在某个图形 API 上（CPU 端做视锥判断、
/// 导出工具算包围盒都要用它）。矩阵约定只有一种，写清楚比分散三处实现强。

#include <cmath>
#include <cstddef>

namespace st::math {

struct Vec3 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};

  [[nodiscard]] constexpr auto operator+(const Vec3& other) const noexcept -> Vec3 {
    return Vec3{x + other.x, y + other.y, z + other.z};
  }
  [[nodiscard]] constexpr auto operator-(const Vec3& other) const noexcept -> Vec3 {
    return Vec3{x - other.x, y - other.y, z - other.z};
  }
  [[nodiscard]] constexpr auto operator*(float factor) const noexcept -> Vec3 {
    return Vec3{x * factor, y * factor, z * factor};
  }
  [[nodiscard]] constexpr auto dot(const Vec3& other) const noexcept -> float {
    return x * other.x + y * other.y + z * other.z;
  }
  [[nodiscard]] constexpr auto cross(const Vec3& other) const noexcept -> Vec3 {
    return Vec3{y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x};
  }
  [[nodiscard]] auto length() const noexcept -> float {
    return std::sqrt(x * x + y * y + z * z);
  }
  /// 归一化；零向量返回 `(0,0,0)`（而不是 NaN——NaN 会在图形管线里静默扩散成整屏黑）。
  [[nodiscard]] auto normalized() const noexcept -> Vec3 {
    const float len = length();
    return len > 1e-6f ? Vec3{x / len, y / len, z / len} : Vec3{};
  }
};

/// 4×4 矩阵，**列主序**（`m[column * 4 + row]`）——与 OpenGL/GLSL 的 `mat4` 内存布局一致，
/// 因此可以直接上传，不需要转置（转置写错是 3D 里最经典的"画面全乱"来源）。
struct Mat4 {
  float m[16]{};

  [[nodiscard]] static constexpr auto identity() noexcept -> Mat4 {
    Mat4 result;
    result.m[0] = 1.0f;
    result.m[5] = 1.0f;
    result.m[10] = 1.0f;
    result.m[15] = 1.0f;
    return result;
  }

  [[nodiscard]] constexpr auto value(int row, int column) const noexcept -> float {
    return m[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)];
  }
  constexpr void set(int row, int column, float value) noexcept {
    m[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)] = value;
  }

  /// 矩阵乘法：`a * b` 表示"先施加 b，再施加 a"（与数学书写一致）。
  [[nodiscard]] constexpr auto operator*(const Mat4& other) const noexcept -> Mat4 {
    Mat4 result;
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k) sum += value(row, k) * other.value(k, column);
        result.set(row, column, sum);
      }
    }
    return result;
  }

  [[nodiscard]] static constexpr auto translation(Vec3 offset) noexcept -> Mat4 {
    Mat4 result = identity();
    result.set(0, 3, offset.x);
    result.set(1, 3, offset.y);
    result.set(2, 3, offset.z);
    return result;
  }

  [[nodiscard]] static constexpr auto scaling(Vec3 factor) noexcept -> Mat4 {
    Mat4 result = identity();
    result.set(0, 0, factor.x);
    result.set(1, 1, factor.y);
    result.set(2, 2, factor.z);
    return result;
  }

  /// 绕任意轴旋转（罗德里格斯公式）。轴为零向量时返回单位阵（不产生 NaN）。
  [[nodiscard]] static auto rotation(Vec3 axis, float radians) noexcept -> Mat4 {
    const Vec3 unit = axis.normalized();
    if (unit.length() < 1e-6f) return identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const float t = 1.0f - c;
    const float x = unit.x;
    const float y = unit.y;
    const float z = unit.z;
    Mat4 result = identity();
    result.set(0, 0, t * x * x + c);
    result.set(0, 1, t * x * y - s * z);
    result.set(0, 2, t * x * z + s * y);
    result.set(1, 0, t * x * y + s * z);
    result.set(1, 1, t * y * y + c);
    result.set(1, 2, t * y * z - s * x);
    result.set(2, 0, t * x * z - s * y);
    result.set(2, 1, t * y * z + s * x);
    result.set(2, 2, t * z * z + c);
    return result;
  }

  /// 透视投影。`fov_y` 为**弧度**；`near_z`/`far_z` 必须为正且 `near < far`。
  /// 参数非法时返回单位阵（宁可见到明显错误，也不要 NaN 导致的整屏黑）。
  [[nodiscard]] static auto perspective(float fov_y, float aspect, float near_z,
                                        float far_z) noexcept -> Mat4 {
    if (aspect <= 0.0f || near_z <= 0.0f || far_z <= near_z || fov_y <= 0.0f) return identity();
    const float f = 1.0f / std::tan(fov_y * 0.5f);
    Mat4 result;
    result.set(0, 0, f / aspect);
    result.set(1, 1, f);
    result.set(2, 2, (far_z + near_z) / (near_z - far_z));
    result.set(2, 3, (2.0f * far_z * near_z) / (near_z - far_z));
    result.set(3, 2, -1.0f);
    return result;
  }

  /// 视图矩阵：相机在 `eye`，看向 `target`，`up` 为上方向。
  /// `eye == target` 或 `up` 与视线平行时返回单位阵（退化输入不该产出 NaN）。
  [[nodiscard]] static auto look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept -> Mat4 {
    const Vec3 forward = (target - eye).normalized();
    if (forward.length() < 1e-6f) return identity();
    Vec3 right = forward.cross(up);
    if (right.length() < 1e-6f) return identity();
    right = right.normalized();
    const Vec3 true_up = right.cross(forward);
    Mat4 result = identity();
    result.set(0, 0, right.x);
    result.set(0, 1, right.y);
    result.set(0, 2, right.z);
    result.set(1, 0, true_up.x);
    result.set(1, 1, true_up.y);
    result.set(1, 2, true_up.z);
    result.set(2, 0, -forward.x);
    result.set(2, 1, -forward.y);
    result.set(2, 2, -forward.z);
    result.set(0, 3, -right.dot(eye));
    result.set(1, 3, -true_up.dot(eye));
    result.set(2, 3, forward.dot(eye));
    return result;
  }
};

}  // namespace st::math
