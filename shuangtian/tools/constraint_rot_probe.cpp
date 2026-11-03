// rotation_to_point_y 隔离验证：给定方向，检查解出的欧拉角经引擎
// from_euler 的四元数旋转后是否把 +Y 指回该方向（互逆性）。
#include <cstdio>
#include <cmath>

#include "st/math/quaternion.hpp"
#include "st/raster/scene_constraints.hpp"

namespace {
/// q v q*（手写，避免依赖不存在的成员）：v' = v + 2w(u×v) + 2(u×(u×v))。
auto rotate_vector(const st::math::Quat& q, st::math::Vec3 v) -> st::math::Vec3 {
  const st::math::Vec3 u{q.x, q.y, q.z};
  const st::math::Vec3 c1 = u.cross(v);
  const st::math::Vec3 c2 = u.cross(c1);
  return v + c1 * (2.0f * q.w) + c2 * 2.0f;
}
}  // namespace

auto main() -> int {
  struct Case {
    float x, y, z;
    const char* label;
  };
  const Case cases[] = {
      {-0.132972f, -0.886484f, -0.443242f, "cable A->B"},
      {1.0f, 0.0f, 0.0f, "+X"},
      {0.0f, 0.0f, 1.0f, "+Z"},
      {0.7071f, 0.7071f, 0.0f, "45 XY"},
      {0.0f, -1.0f, 0.0f, "-Y（反向）"},
  };
  int failures = 0;
  for (const Case& c : cases) {
    const st::math::Vec3 d{c.x, c.y, c.z};
    const st::math::Vec3 euler_deg = st::raster::rotation_to_point_y(d);
    // 引擎路径：度→弧度→from_euler→旋转 +Y
    const float rad = 3.14159265358979f / 180.0f;
    const st::math::Quat q = st::math::Quat::from_euler(
        st::math::Vec3{euler_deg.x * rad, euler_deg.y * rad, euler_deg.z * rad});
    const st::math::Vec3 up{0.0f, 1.0f, 0.0f};
    const st::math::Vec3 moved = rotate_vector(q, up);
    const st::math::Vec3 want = d.normalized();
    const float err = (moved - want).length();
    std::printf("%-14s euler=(%.2f, %.2f, %.2f) -> +Y 到 (%.4f, %.4f, %.4f) 期望 (%.4f, %.4f, %.4f) 误差 %.5f %s\n",
                c.label, double(euler_deg.x), double(euler_deg.y), double(euler_deg.z),
                double(moved.x), double(moved.y), double(moved.z),
                double(want.x), double(want.y), double(want.z), double(err),
                err < 1e-3f ? "[OK]" : "[FAIL]");
    if (err >= 1e-3f) ++failures;
  }
  return failures == 0 ? 0 : 1;
}
