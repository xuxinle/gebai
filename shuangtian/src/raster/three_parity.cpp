/// three.js 同构着色的实现。
///
/// 每个函数的注释标出它逐字对应 `third_party/three/three.cjs` 里的哪一段
/// ShaderChunk——改这里之前先去那边读原文，不要凭"标准 GGX"的记忆改。

#include "st/raster/three_parity.hpp"

#include <algorithm>
#include <cmath>

namespace st::raster {

namespace {

constexpr float kReciprocalPi = 0.3183098861837907f;   // 1/π

/// three 的 `RECIPROCAL_PI` 与 `EPSILON`（ShaderChunk 里是 `0.0000001`）。
constexpr float kThreeEpsilon = 1e-7f;

/// three 的 `pow2(x)`。
[[nodiscard]] auto pow2(float v) noexcept -> float { return v * v; }

const auto clamp01 = [](float v) noexcept -> float {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
};

}  // namespace

auto three_alpha(float roughness) noexcept -> float {
  // lights_physical_fragment:
  //   material.roughness = max( roughnessFactor, 0.0525 );
  //   material.roughness += geometryRoughness;      ← 本实现取 0（见头文件"已知残余"1）
  //   material.roughness = min( material.roughness, 1.0 );
  // BRDF_GGX:  float alpha = pow2( material.roughness );
  const float r = std::clamp(std::max(roughness, 0.0525f), 0.0f, 1.0f);
  return pow2(r);
}

auto three_d_ggx(float alpha, float n_dot_h) noexcept -> float {
  // float D_GGX( const in float alpha, const in float dotNH ) {
  //   float a2 = pow2( alpha );
  //   float denom = pow2( dotNH ) * ( a2 - 1.0 ) + 1.0;
  //   return RECIPROCAL_PI * a2 / pow2( denom );
  // }
  const float a2 = pow2(alpha);
  const float nh2 = pow2(clamp01(n_dot_h));
  const float denom = nh2 * (a2 - 1.0f) + 1.0f;
  return kReciprocalPi * a2 / pow2(denom);
}

auto three_v_ggx(float alpha, float n_dot_l, float n_dot_v) noexcept -> float {
  // float V_GGX_SmithCorrelated( const in float alpha, const in float dotNL, const in float dotNV ) {
  //   float a2 = pow2( alpha );
  //   float gv = dotNL * sqrt( a2 + ( 1.0 - a2 ) * pow2( dotNV ) );
  //   float gl = dotNV * sqrt( a2 + ( 1.0 - a2 ) * pow2( dotNL ) );
  //   return 0.5 / max( gv + gl, EPSILON );
  // }
  const float a2 = pow2(alpha);
  const float nl = clamp01(n_dot_l);
  const float nv = clamp01(n_dot_v);
  const float gv = nl * std::sqrt(a2 + (1.0f - a2) * pow2(nv));
  const float gl = nv * std::sqrt(a2 + (1.0f - a2) * pow2(nl));
  return 0.5f / std::max(gv + gl, kThreeEpsilon);
}

auto three_f_schlick(math::Vec3 f0, float f90, float v_dot_h) noexcept -> math::Vec3 {
  // vec3 F_Schlick( const in vec3 f0, const in float f90, const in float dotVH ) {
  //   float fresnel = exp2( ( - 5.55473 * dotVH - 6.98316 ) * dotVH );
  //   return f0 * ( 1.0 - fresnel ) + ( f90 * fresnel );
  // }
  const float c = clamp01(v_dot_h);
  const float fresnel = std::exp2((-5.55473f * c - 6.98316f) * c);
  return math::Vec3{f0.x * (1.0f - fresnel) + f90 * fresnel,
                    f0.y * (1.0f - fresnel) + f90 * fresnel,
                    f0.z * (1.0f - fresnel) + f90 * fresnel};
}

auto three_direct_light(math::Vec3 normal, math::Vec3 view_dir, math::Vec3 light_dir,
                        math::Vec3 light_color, float light_intensity, math::Vec3 base_color,
                        float metallic, float roughness) noexcept -> math::Vec3 {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 v = view_dir.normalized();
  const math::Vec3 l = light_dir.normalized();

  // lights_physical_fragment:
  //   material.diffuseColor  = diffuseColor.rgb * ( 1.0 - metalnessFactor );
  //   material.specularColor = mix( vec3( 0.04 ), diffuseColor.rgb, metalnessFactor );
  //   material.specularF90   = 1.0;
  const float one_minus_metal = 1.0f - std::clamp(metallic, 0.0f, 1.0f);
  const math::Vec3 diffuse_color{base_color.x * one_minus_metal, base_color.y * one_minus_metal,
                                 base_color.z * one_minus_metal};
  const float m = std::clamp(metallic, 0.0f, 1.0f);
  const math::Vec3 f0{0.04f + (base_color.x - 0.04f) * m, 0.04f + (base_color.y - 0.04f) * m,
                      0.04f + (base_color.z - 0.04f) * m};

  // RE_Direct_Physical:
  //   float dotNL = saturate( dot( geometryNormal, directLight.direction ) );
  //   vec3 irradiance = dotNL * directLight.color;
  //
  // ⚠ `directLight.color` 已含 intensity（WebGLLights：`color * intensity * scaleFactor`，
  //   r160 的 scaleFactor = 1）。所以这里乘 intensity 而不是乘 π。
  const float n_dot_l = clamp01(n.dot(l));

  // 背面：three 的 saturate 让 irradiance = 0，直接光的两个分量都为 0。
  if (n_dot_l <= 0.0f || light_intensity <= 0.0f) return math::Vec3{0.0f, 0.0f, 0.0f};
  const math::Vec3 irradiance{light_color.x * light_intensity * n_dot_l,
                              light_color.y * light_intensity * n_dot_l,
                              light_color.z * light_intensity * n_dot_l};

  // BRDF_Lambert: RECIPROCAL_PI * diffuseColor
  const math::Vec3 direct_diffuse{irradiance.x * diffuse_color.x * kReciprocalPi,
                                  irradiance.y * diffuse_color.y * kReciprocalPi,
                                  irradiance.z * diffuse_color.z * kReciprocalPi};

  // BRDF_GGX
  const math::Vec3 h{(v.x + l.x), (v.y + l.y), (v.z + l.z)};
  const math::Vec3 half_dir = h.normalized();
  const float n_dot_v = clamp01(n.dot(v));
  const float n_dot_h = clamp01(n.dot(half_dir));
  const float v_dot_h = clamp01(v.dot(half_dir));
  const float alpha = three_alpha(roughness);
  const float d = three_d_ggx(alpha, n_dot_h);
  const float vis = three_v_ggx(alpha, n_dot_l, n_dot_v);
  const math::Vec3 f = three_f_schlick(f0, 1.0f, v_dot_h);

  const math::Vec3 spec{irradiance.x * d * vis * f.x, irradiance.y * d * vis * f.y,
                        irradiance.z * d * vis * f.z};

  return math::Vec3{direct_diffuse.x + spec.x, direct_diffuse.y + spec.y,
                    direct_diffuse.z + spec.z};
}

auto three_ambient_light(math::Vec3 base_color, float metallic,
                         math::Vec3 ambient_irradiance) noexcept -> math::Vec3 {
  // RE_IndirectDiffuse_Physical: reflectedLight.indirectDiffuse += irradiance * BRDF_Lambert(material.diffuseColor)
  //   而 BRDF_Lambert = RECIPROCAL_PI * diffuseColor
  const float one_minus_metal = 1.0f - std::clamp(metallic, 0.0f, 1.0f);
  return math::Vec3{ambient_irradiance.x * base_color.x * one_minus_metal * kReciprocalPi,
                    ambient_irradiance.y * base_color.y * one_minus_metal * kReciprocalPi,
                    ambient_irradiance.z * base_color.z * one_minus_metal * kReciprocalPi};
}

}  // namespace st::raster
