#include "st/math/quaternion.hpp"

#include <algorithm>

namespace st::math {

auto Quat::from_axis_angle(Vec3 axis, float radians) noexcept -> Quat {
  const Vec3 unit = axis.normalized();
  if (unit.length() < 0.5f) return Quat{};
  const float half = radians * 0.5f;
  const float s = std::sin(half);
  return Quat{unit.x * s, unit.y * s, unit.z * s, std::cos(half)};
}

auto Quat::from_euler(Vec3 euler_radians) noexcept -> Quat {
  // 依次绕 X、Y、Z：`q = qz * qy * qx`（右乘先施加）
  Quat result{};
  if (euler_radians.x != 0.0f) {
    result = from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, euler_radians.x) * result;
  }
  if (euler_radians.y != 0.0f) {
    result = from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, euler_radians.y) * result;
  }
  if (euler_radians.z != 0.0f) {
    result = from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, euler_radians.z) * result;
  }
  return result;
}

auto Quat::to_euler() const noexcept -> Vec3 {
  // 直接用四元数分量反解（XYZ 顺序），不走矩阵——少一次往返、也少一处可能写错的正负号。
  //
  // 推导用的是 `R = Rz·Ry·Rx` 展开后的三项：
  //   R[2][0] = -sin(y)
  //   R[2][1] =  sin(x)·cos(y)
  //   R[2][2] =  cos(x)·cos(y)
  //   R[1][0] =  cos(y)·sin(z)   （万向锁之外可解）
  //   R[0][0] =  cos(y)·cos(z)
  const Quat q = normalized();
  const float sin_y = 2.0f * (q.w * q.y - q.z * q.x);
  const float clamped = std::clamp(sin_y, -1.0f, 1.0f);
  Vec3 out{};
  out.y = std::asin(clamped);
  if (std::abs(clamped) < 0.9999f) {
    out.x = std::atan2(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    out.z = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
  } else {
    // ## 万向锁：y = ±π/2 时 x 与 z 退化成同一个自由度
    //
    // 此时 `R[0][0]` 与 `R[1][0]` 都趋零，无法分离 roll 与 yaw。
    // 约定 `z = 0`、让 `x` 承担全部偏航——这是 Blender/Unity 的通行取舍。
    // 注意它是**有损**的：同一姿态的等价欧拉写法有无穷多组，
    // 所以这条路径只在"要显示给人看"时用，动画中间态一律走四元数。
    out.x = std::atan2(2.0f * (q.w * q.x - q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    out.z = 0.0f;
  }
  return out;
}

auto Quat::to_matrix() const noexcept -> Mat4 {
  const Quat q = normalized();
  const float xx = q.x * q.x;
  const float yy = q.y * q.y;
  const float zz = q.z * q.z;
  const float xy = q.x * q.y;
  const float xz = q.x * q.z;
  const float yz = q.y * q.z;
  const float wx = q.w * q.x;
  const float wy = q.w * q.y;
  const float wz = q.w * q.z;
  Mat4 out = Mat4::identity();
  out.set(0, 0, 1.0f - 2.0f * (yy + zz));
  out.set(0, 1, 2.0f * (xy - wz));
  out.set(0, 2, 2.0f * (xz + wy));
  out.set(1, 0, 2.0f * (xy + wz));
  out.set(1, 1, 1.0f - 2.0f * (xx + zz));
  out.set(1, 2, 2.0f * (yz - wx));
  out.set(2, 0, 2.0f * (xz - wy));
  out.set(2, 1, 2.0f * (yz + wx));
  out.set(2, 2, 1.0f - 2.0f * (xx + yy));
  return out;
}

auto Quat::operator*(const Quat& other) const noexcept -> Quat {
  // Hamilton 积：`this` 后施加、`other` 先施加（与矩阵乘法同序）
  return Quat{w * other.x + x * other.w + y * other.z - z * other.y,
              w * other.y - x * other.z + y * other.w + z * other.x,
              w * other.z + x * other.y - y * other.x + z * other.w,
              w * other.w - x * other.x - y * other.y - z * other.z};
}

auto Quat::length() const noexcept -> float {
  return std::sqrt(x * x + y * y + z * z + w * w);
}

auto Quat::normalized() const noexcept -> Quat {
  const float len = length();
  if (len < 1.0e-6f) return Quat{};
  return Quat{x / len, y / len, z / len, w / len};
}

auto Quat::conjugate() const noexcept -> Quat { return Quat{-x, -y, -z, w}; }

auto slerp(const Quat& a, const Quat& b, float t) noexcept -> Quat {
  const Quat qa = a.normalized();
  Quat qb = b.normalized();
  float dot = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
  // ## 最短路径：点积为负说明两个四元数在球面的对跖侧
  //
  // `q` 与 `-q` 是**同一个旋转**，但插值走的是球面上的大圆——
  // 不翻的话会转过 360°-θ（绕远路）。实测症状：短动画看着正常，
  // 某些关键帧对之间突然"反着猛转一圈"。
  if (dot < 0.0f) {
    qb = Quat{-qb.x, -qb.y, -qb.z, -qb.w};
    dot = -dot;
  }
  // 夹角很小时退回线性插值（sin 除零会出 NaN；且此时两者几乎同向，观感无差）
  if (dot > 0.9995f) return nlerp(qa, qb, t);
  const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
  const float sin_theta = std::sin(theta);
  if (std::abs(sin_theta) < 1.0e-6f) return nlerp(qa, qb, t);
  const float wa = std::sin((1.0f - t) * theta) / sin_theta;
  const float wb = std::sin(t * theta) / sin_theta;
  return Quat{qa.x * wa + qb.x * wb, qa.y * wa + qb.y * wb, qa.z * wa + qb.z * wb,
              qa.w * wa + qb.w * wb};
}

auto nlerp(const Quat& a, const Quat& b, float t) noexcept -> Quat {
  const Quat qa = a.normalized();
  Quat qb = b.normalized();
  const float dot = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
  if (dot < 0.0f) qb = Quat{-qb.x, -qb.y, -qb.z, -qb.w};
  return Quat{qa.x + (qb.x - qa.x) * t, qa.y + (qb.y - qa.y) * t, qa.z + (qb.z - qa.z) * t,
              qa.w + (qb.w - qa.w) * t}
      .normalized();
}

}  // namespace st::math
