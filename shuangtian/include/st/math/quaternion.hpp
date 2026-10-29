#pragma once

/// @file quaternion.hpp
/// @brief 四元数：旋转的**可插值**表示（欧拉角不能插值，见下）。
///
/// ## 为什么动画必须用四元数
///
/// 欧拉角有三个致命的插值问题，全都表现为"动画看起来不对"：
///
/// 1. **绕远路**：从 350° 到 10° 线性插值会（错误地）转过 340°，而正确是 20°。
///    实际表现是"物体先反向猛转一圈再到位"。
/// 2. **万向锁**：两轴对齐时丢失一个自由度，插值中姿态会突然跳变。
/// 3. **分量不独立**：单独插值 x/y/z 三个角，得到的中间姿态**不在**起点与终点
///    的"最短旋转路径"上，观感是"甩"而不是"摆"。
///
/// 四元数的 `slerp`（球面线性插值）在单位球面的大圆上取值，天生解决这三条。
/// 这也是 glTF 用四元数存旋转（`rotation: [x,y,z,w]`）的原因——格式与引擎都这么做。
///
/// ## 与欧拉角的关系
///
/// 脚本层仍写欧拉角（人更容易算），进动画前用 `from_euler` 转成四元数；
/// 采样后再用 `to_matrix` 变成矩阵。**转换点只有这两处**，中间一律四元数。
///
/// ## 约定
///
/// - 右手系，与 `math::Mat4::rotation` 同一套（绕轴正转 = 逆时针，从轴正向看）
/// - `w` 是标量分量，存最后（与 glTF 的 `[x,y,z,w]` 顺序一致，避免读写时倒序出错）
/// - 乘法 `a * b` 表示"先施加 b，再施加 a"（与矩阵 `A * B` 的语义一致）

#include <cmath>

#include "st/math/matrix.hpp"

namespace st::math {

struct Quat {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  float w{1.0f};   ///< 标量分量（单位四元数 = 无旋转）

  [[nodiscard]] static constexpr auto identity() noexcept -> Quat { return Quat{}; }

  /// 绕**单位轴**转 `radians`（罗德里格斯公式的四元数形式）。
  /// 轴为零向量时返回单位四元数（不产生 NaN——NaN 会在管线里静默扩散）。
  [[nodiscard]] static auto from_axis_angle(Vec3 axis, float radians) noexcept -> Quat;

  /// 欧拉角 → 四元数，顺序 **X → Y → Z**（与 `TransformTRS::rotation` 同一口径）。
  ///
  /// 为什么必须钉死顺序：同一姿态有 6 种欧拉写法，顺序不同结果不同。
  /// 这里与 Blender 的 `rotation_euler`（XYZ 模式）对齐，数值可以直接搬。
  [[nodiscard]] static auto from_euler(Vec3 euler_radians) noexcept -> Quat;

  /// 四元数 → 欧拉角（XYZ 顺序，与 `from_euler` 互逆）。
  ///
  /// 万向锁时 `y` 被夹在 ±π/2，`z` 取 0——这是**有损**的（三个自由度在锁死处
  /// 只剩两个），所以只在"要显示给人看/写回欧拉字段"时用，不要在动画中间态用。
  [[nodiscard]] auto to_euler() const noexcept -> Vec3;

  /// 四元数 → 旋转矩阵（含归一化：非单位四元数会先归一化，避免缩放混入）。
  [[nodiscard]] auto to_matrix() const noexcept -> Mat4;

  [[nodiscard]] auto operator*(const Quat& other) const noexcept -> Quat;
  [[nodiscard]] auto length() const noexcept -> float;
  /// 归一化；长度接近 0 时返回单位四元数。
  [[nodiscard]] auto normalized() const noexcept -> Quat;
  [[nodiscard]] auto conjugate() const noexcept -> Quat;
};

/// 球面线性插值（**最短路径**）：`t=0` 得 `a`，`t=1` 得 `b`。
///
/// 实现要点：两个四元数 `q` 与 `-q` 表示**同一个旋转**，所以先取点积；
/// 为负就翻转 `b`，否则会走"绕远路"（转过 360°-θ 而不是 θ）。
/// 这一条是最容易漏、且症状最奇怪的一个细节：不翻时短动画看起来正常，
/// 长动画会在某些关键帧对之间突然反向猛转。
[[nodiscard]] auto slerp(const Quat& a, const Quat& b, float t) noexcept -> Quat;

/// 线性插值 + 归一化（`nlerp`）：比 `slerp` 便宜，角速度略不均匀。
/// 关键帧密集时（每帧都有）观感无差别，`slerp` 用在稀疏关键帧上。
[[nodiscard]] auto nlerp(const Quat& a, const Quat& b, float t) noexcept -> Quat;

}  // namespace st::math
