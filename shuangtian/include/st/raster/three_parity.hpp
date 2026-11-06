/// @file three_parity.hpp
/// @brief **过渡层**：把旧的三参数签名接到统一内核 `pbr.hpp` 上。
///
/// ## 为什么还留着这个名字
///
/// `docs/render_parity.md` 与 `tests/raster_three_parity_test.cpp` 里有一批
/// 已固化的**实测基线**（受控平面 76 / 强度 2.0 时 106 / ACESFilmic 141）。
/// 那些基线是对标成果的账本，改名会让文档里的路径失效。
///
/// 所以这一步不直接删，而是让它**转调 `pbr` 内核**：
///
/// | 旧函数 | 现在 |
/// |---|---|
/// | `three_d_ggx` | `pbr_d_ggx` |
/// | `three_v_ggx` | `pbr_v_ggx` |
/// | `three_f_schlick` | `pbr_f_schlick` |
/// | `three_alpha` | `pbr_alpha` |
/// | `three_direct_light` | `pbr_direct_light`（包装一层 PbrLight） |
/// | `three_ambient_light` | `pbr_ambient_diffuse` |
///
/// 等所有调用方与测试都迁到 `pbr` 之后，本文件删除（见计划第 5 步）。

#pragma once

#include "st/math/color.hpp"
#include "st/raster/pbr.hpp"

namespace st::raster {

/// @deprecated 用 `pbr_d_ggx`。
[[nodiscard]] inline auto three_d_ggx(float alpha, float n_dot_h) noexcept -> float {
  return pbr_d_ggx(alpha, n_dot_h);
}

/// @deprecated 用 `pbr_v_ggx`。
[[nodiscard]] inline auto three_v_ggx(float alpha, float n_dot_l, float n_dot_v) noexcept
    -> float {
  return pbr_v_ggx(alpha, n_dot_l, n_dot_v);
}

/// @deprecated 用 `pbr_f_schlick`。
[[nodiscard]] inline auto three_f_schlick(math::Vec3 f0, float f90, float v_dot_h) noexcept
    -> math::Vec3 {
  return pbr_f_schlick(f0, f90, v_dot_h);
}

/// @deprecated 用 `pbr_alpha`。
[[nodiscard]] inline auto three_alpha(float roughness) noexcept -> float {
  return pbr_alpha(roughness);
}

/// @deprecated 用 `pbr_direct_light`（它接受统一的 `PbrLight`）。
[[nodiscard]] inline auto three_direct_light(math::Vec3 normal, math::Vec3 view_dir,
                                             math::Vec3 light_dir, math::Vec3 light_color,
                                             float light_intensity, math::Vec3 base_color,
                                             float metallic, float roughness) noexcept
    -> math::Vec3 {
  PbrLight light{};
  light.direction = light_dir.normalized();
  light.irradiance = math::Vec3{light_color.x * light_intensity,
                                light_color.y * light_intensity,
                                light_color.z * light_intensity};
  return pbr_direct_light(normal, view_dir, light, base_color, metallic, roughness);
}

/// @deprecated 用 `pbr_ambient_diffuse`。
[[nodiscard]] inline auto three_ambient_light(math::Vec3 base_color, float metallic,
                                              math::Vec3 ambient_irradiance) noexcept
    -> math::Vec3 {
  return pbr_ambient_diffuse(base_color, metallic, ambient_irradiance);
}

}  // namespace st::raster

