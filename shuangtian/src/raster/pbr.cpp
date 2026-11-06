/// 着色内核的实现。
///
/// 每个函数的注释标出它对应 `third_party/three/three.cjs` 的哪一段 ShaderChunk
/// ——改这里之前先去那边读原文，不要凭"标准 GGX"的记忆改。
///
/// ## 为什么这个文件里没有任何 `if (backend == ...)`
///
/// 内核必须与后端无关。两条腿（软件光栅 / D3D11）各自把**这里的公式**逐字
/// 翻译成自己的语言（C++ / HLSL），而不是各自实现一套。本仓吃过四次亏
/// （环境反射、自发光、纹理标志、`tone_map`），每次都是"只改了一条腿"。

#include "st/raster/pbr.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace st::raster {

namespace {

/// three 的 `EPSILON`（ShaderChunk 里是 `0.0000001`）。
constexpr float kThreeEpsilon = 1e-7f;

/// three 的 `pow2(x)`。
[[nodiscard]] auto pow2(float v) noexcept -> float { return v * v; }

const auto clamp01 = [](float v) noexcept -> float {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
};

/// three 的 `smoothstep(edge0, edge1, x)`（GLSL 语义：edge0 > edge1 时也正确）。
[[nodiscard]] auto smoothstep(float edge0, float edge1, float x) noexcept -> float {
  const float denominator = edge1 - edge0;
  if (std::abs(denominator) < 1e-12f) return x < edge0 ? 0.0f : 1.0f;
  const float t = clamp01((x - edge0) / denominator);
  return t * t * (3.0f - 2.0f * t);
}

/// 方向探针的权重衰减次数：`sharpness` 越大越锐利。
///
/// ## 为何用 `pow(dot, sharp)` 而不是余弦的幂
///
/// 这是对 "PMREM mip 链" 的解析替代：光滑面（roughness→0）应当只看到
/// **一个方向**（接近镜面），粗糙面（roughness→1）应当看到**全部方向的均值**。
/// 幂函数正好是这个过渡：`sharp→∞` 时只留 dot 最大的那个探针，
/// `sharp=1` 时退化成余弦加权（漫反射的形状）。
[[nodiscard]] auto probe_sharpness(float roughness) noexcept -> float {
  // 1 → 64：实测这组上下限在"齿轮金属"场景里与 three 的 PMREM 观感最接近
  // （见 docs/render_parity.md 的分级对照）。
  const float t = std::clamp(roughness, 0.0f, 1.0f);
  return 1.0f + (1.0f - t) * (1.0f - t) * 63.0f;
}

/// 按方向探针求"加权平均颜色"，权重 = `max(0, dot(dir, target))^sharpness`。
///
/// ## 为什么要**归一化**（而不是直接求和）
///
/// 归一化保证两件事：
/// 1. **探针个数不影响亮度**——加第 5 个同色探针不会让画面变亮；
/// 2. **同色环境退化为均匀**——所有探针同色时，任何方向都得到那个颜色，
///    这正是"均匀世界背景"（Blender 的 `world`）应有的行为。
///
/// 不归一化时，第二个性质不成立：法线朝上会拿到比朝下更多的能量，
/// 于是"均匀环境"在物体上表现为"上亮下暗"——那是**环境有结构**，不是均匀。
[[nodiscard]] auto sample_probes(math::Vec3 target, float sharpness, const math::Vec3* dirs,
                                 const math::Vec3* colors, int count) noexcept -> math::Vec3 {
  if (dirs == nullptr || colors == nullptr || count <= 0) {
    return math::Vec3{0.0f, 0.0f, 0.0f};
  }
  float sum_w = 0.0f;
  math::Vec3 sum_c{0.0f, 0.0f, 0.0f};
  const math::Vec3 t = target.normalized();
  for (int i = 0; i < count; ++i) {
    const math::Vec3 dir = dirs[i].normalized();
    const float cos_angle = t.dot(dir);
    if (cos_angle <= 0.0f) continue;
    // 整数幂用乘法（与 GPU 腿的 `pow` 在 sharpness=1/2 时数值一致；
    // 其余情形由 HLSL 侧同样用 `pow`，见 platform_d3d11.cpp 的说明）。
    const float w = std::pow(cos_angle, sharpness);
    sum_w += w;
    sum_c.x += colors[i].x * w;
    sum_c.y += colors[i].y * w;
    sum_c.z += colors[i].z * w;
  }
  if (sum_w <= 1e-9f) {
    // 所有探针都在背面：退化为"全部探针的均值"，而不是黑。
    // 这一步对应 three 的 PMREM 在 mip 最高级时退化成均值的行为。
    math::Vec3 mean{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; ++i) {
      mean.x += colors[i].x;
      mean.y += colors[i].y;
      mean.z += colors[i].z;
    }
    return math::Vec3{mean.x / static_cast<float>(count), mean.y / static_cast<float>(count),
                      mean.z / static_cast<float>(count)};
  }
  return math::Vec3{sum_c.x / sum_w, sum_c.y / sum_w, sum_c.z / sum_w};
}

/// three 的 `getDistanceAttenuation`（非 legacy 分支，`decay = 2`）。
///
/// ```
/// float distanceFalloff = 1.0 / max( pow( lightDistance, decayExponent ), 0.01 );
/// if ( cutoffDistance > 0.0 ) distanceFalloff *= pow2( saturate( 1.0 - pow4( lightDistance / cutoffDistance ) ) );
/// ```
[[nodiscard]] auto distance_attenuation(float distance, float cutoff) noexcept -> float {
  const float d = std::max(distance, 1e-6f);
  float falloff = 1.0f / std::max(pow2(d), 0.01f);
  if (cutoff > 0.0f) {
    const float ratio = d / cutoff;
    const float window = 1.0f - std::pow(clamp01(ratio), 4.0f);
    falloff *= pow2(clamp01(window));
  }
  return falloff;
}

}  // namespace

auto pbr_alpha(float roughness) noexcept -> float {
  // lights_physical_fragment:
  //   material.roughness = max( roughnessFactor, 0.0525 );
  //   material.roughness += geometryRoughness;      ← 本实现取 0（见头文件"已知残余"1）
  //   material.roughness = min( material.roughness, 1.0 );
  // BRDF_GGX: float alpha = pow2( material.roughness );
  const float r = std::clamp(std::max(roughness, 0.0525f), 0.0f, 1.0f);
  return pow2(r);
}

auto pbr_d_ggx(float alpha, float n_dot_h) noexcept -> float {
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

auto pbr_v_ggx(float alpha, float n_dot_l, float n_dot_v) noexcept -> float {
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

auto pbr_f_schlick(math::Vec3 f0, float f90, float v_dot_h) noexcept -> math::Vec3 {
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

auto pbr_sample_light(int kind, math::Vec3 direction_toward_light, math::Vec3 position,
                      math::Vec3 color, float intensity, float range, float spot_inner_degrees,
                      float spot_outer_degrees, math::Vec3 surface_position) -> PbrLight {
  PbrLight out{};
  const math::Vec3 tint{color.x * intensity, color.y * intensity, color.z * intensity};

  if (kind == 0) {
    // 平行光：方向固定、无衰减。
    out.direction = direction_toward_light.normalized();
    out.irradiance = tint;
    return out;
  }

  // 点光 / 聚光：方向由位置差决定。
  const math::Vec3 to_light{position.x - surface_position.x, position.y - surface_position.y,
                            position.z - surface_position.z};
  const float distance = to_light.length();
  out.direction = distance > 1e-6f ? math::Vec3{to_light.x / distance, to_light.y / distance,
                                                to_light.z / distance}
                                   : direction_toward_light.normalized();
  float scale = distance_attenuation(distance, range);

  if (kind == 2) {
    // 聚光：锥形衰减。`spot_inner_degrees` = 满强度，`spot_outer_degrees` = 到 0。
    //
    // ## 为何用 `smoothstep` 而不是线性插值
    //
    // 线性过渡在锥边会留一道可见的折线（二阶不连续），实测在金属表面上
    // 表现为一圈硬边。three 用的就是 `smoothstep`。
    //
    // 角度是从**光源指向表面**的方向算的——即 `(surface - position)`。
    const math::Vec3 forward{surface_position.x - position.x, surface_position.y - position.y,
                             surface_position.z - position.z};
    const float forward_len = forward.length();
    if (forward_len <= 1e-6f) {
      out.irradiance = math::Vec3{0.0f, 0.0f, 0.0f};
      return out;
    }
    const math::Vec3 axis{forward.x / forward_len, forward.y / forward_len,
                          forward.z / forward_len};
    const float cos_angle = axis.dot(out.direction);
    const float inner = std::cos(spot_inner_degrees * 0.017453292519943295f);
    const float outer = std::cos(spot_outer_degrees * 0.017453292519943295f);
    // `smoothstep(outer, inner, cos)`：cos 大于内锥余弦 → 1，小于外锥余弦 → 0。
    scale *= smoothstep(outer, inner, cos_angle);
  }

  out.irradiance = math::Vec3{tint.x * scale, tint.y * scale, tint.z * scale};
  return out;
}

auto pbr_direct_light(math::Vec3 normal, math::Vec3 view_dir, const PbrLight& light,
                      math::Vec3 base_color, float metallic, float roughness) noexcept
    -> math::Vec3 {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 v = view_dir.normalized();
  const math::Vec3 l = light.direction.normalized();

  // lights_physical_fragment:
  //   material.diffuseColor  = diffuseColor.rgb * ( 1.0 - metalnessFactor );
  //   material.specularColor = mix( vec3( 0.04 ), diffuseColor.rgb, metalnessFactor );
  //   material.specularF90   = 1.0;
  const float m = std::clamp(metallic, 0.0f, 1.0f);
  const float one_minus_metal = 1.0f - m;
  const math::Vec3 diffuse_color{base_color.x * one_minus_metal, base_color.y * one_minus_metal,
                                 base_color.z * one_minus_metal};
  const math::Vec3 f0{0.04f + (base_color.x - 0.04f) * m, 0.04f + (base_color.y - 0.04f) * m,
                      0.04f + (base_color.z - 0.04f) * m};

  // RE_Direct_Physical:
  //   float dotNL = saturate( dot( geometryNormal, directLight.direction ) );
  //   vec3 irradiance = dotNL * directLight.color;
  const float n_dot_l = clamp01(n.dot(l));
  if (n_dot_l <= 0.0f) return math::Vec3{0.0f, 0.0f, 0.0f};
  const math::Vec3 irradiance{light.irradiance.x * n_dot_l, light.irradiance.y * n_dot_l,
                              light.irradiance.z * n_dot_l};
  if (irradiance.x <= 0.0f && irradiance.y <= 0.0f && irradiance.z <= 0.0f) {
    return math::Vec3{0.0f, 0.0f, 0.0f};
  }

  // BRDF_Lambert: RECIPROCAL_PI * diffuseColor
  const math::Vec3 direct_diffuse{irradiance.x * diffuse_color.x * kReciprocalPi,
                                  irradiance.y * diffuse_color.y * kReciprocalPi,
                                  irradiance.z * diffuse_color.z * kReciprocalPi};

  // BRDF_GGX
  const math::Vec3 h{v.x + l.x, v.y + l.y, v.z + l.z};
  const math::Vec3 half_dir = h.normalized();
  const float n_dot_v = clamp01(n.dot(v));
  const float n_dot_h = clamp01(n.dot(half_dir));
  const float v_dot_h = clamp01(v.dot(half_dir));
  const float alpha = pbr_alpha(roughness);
  const float d = pbr_d_ggx(alpha, n_dot_h);
  const float vis = pbr_v_ggx(alpha, n_dot_l, n_dot_v);
  const math::Vec3 f = pbr_f_schlick(f0, 1.0f, v_dot_h);

  const math::Vec3 spec{irradiance.x * d * vis * f.x, irradiance.y * d * vis * f.y,
                        irradiance.z * d * vis * f.z};

  return math::Vec3{direct_diffuse.x + spec.x, direct_diffuse.y + spec.y,
                    direct_diffuse.z + spec.z};
}

auto pbr_ambient_diffuse(math::Vec3 base_color, float metallic,
                         math::Vec3 ambient_irradiance) noexcept -> math::Vec3 {
  // RE_IndirectDiffuse_Physical: reflectedLight.indirectDiffuse += irradiance * BRDF_Lambert(material.diffuseColor)
  const float one_minus_metal = 1.0f - std::clamp(metallic, 0.0f, 1.0f);
  return math::Vec3{ambient_irradiance.x * base_color.x * one_minus_metal * kReciprocalPi,
                    ambient_irradiance.y * base_color.y * one_minus_metal * kReciprocalPi,
                    ambient_irradiance.z * base_color.z * one_minus_metal * kReciprocalPi};
}

auto pbr_ibl_diffuse(math::Vec3 normal, const math::Vec3* probe_dirs,
                     const math::Vec3* probe_colors, int count) noexcept -> math::Vec3 {
  // 余弦加权（sharpness = 1）：环境辐照度的形状。
  return sample_probes(normal, 1.0f, probe_dirs, probe_colors, count);
}

auto pbr_ibl_specular(math::Vec3 normal, math::Vec3 view_dir, float roughness,
                      const math::Vec3* probe_dirs, const math::Vec3* probe_colors,
                      int count) noexcept -> math::Vec3 {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 v = view_dir.normalized();
  const float n_dot_v = std::max(n.dot(v), 0.0f);
  // reflect(-v, n) = -v + 2·(n·v)·n
  const math::Vec3 r{n.x * (2.0f * n_dot_v) - v.x, n.y * (2.0f * n_dot_v) - v.y,
                     n.z * (2.0f * n_dot_v) - v.z};
  return sample_probes(r, probe_sharpness(roughness), probe_dirs, probe_colors, count);
}

auto pbr_env_brdf(float roughness, float n_dot_v) noexcept -> std::pair<float, float> {
  // Lazarov 的解析拟合（UE4 的 `EnvBRDFApprox`，与 Khronos glTF 采样器同族）。
  //
  // ## 为什么不用数值积分
  //
  // split-sum 的 LUT 要一张 2D 纹理 + 预计算；本框架没有贴图管线，而两条腿
  // 必须算出**逐像素相同**的数——解析式天然满足这一点，LUT 采样会引入
  // 过滤方式（双线性/最近）的分歧。
  //
  // 误差 <2%（对比 128×128 数值积分），远小于本框架的其他近似。
  constexpr float kC0x = -1.0f, kC0y = -0.0275f, kC0z = -0.572f, kC0w = 0.022f;
  constexpr float kC1x = 1.0f, kC1y = 0.0425f, kC1z = 1.04f, kC1w = -0.04f;
  const float r = std::clamp(roughness, 0.0f, 1.0f);
  const float nv = clamp01(n_dot_v);
  const float rx = r * kC0x + kC1x;
  const float ry = r * kC0y + kC1y;
  const float rz = r * kC0z + kC1z;
  const float rw = r * kC0w + kC1w;
  const float a004 = std::min(rx * rx, std::exp2(-9.28f * nv)) * rx + ry;
  const float a = -1.04f * a004 + rz;
  const float b = 1.04f * a004 + rw;
  return {a, b};
}

auto pbr_shade(const ShadeInput& in) noexcept -> math::Vec3 {
  const math::Vec3 n = in.normal.normalized();
  const math::Vec3 v = in.view_dir.normalized();
  const float n_dot_v = std::max(n.dot(v), 0.0f);
  const float m = std::clamp(in.metallic, 0.0f, 1.0f);
  const float one_minus_metal = 1.0f - m;
  const math::Vec3 f0{0.04f + (in.base_color.x - 0.04f) * m,
                      0.04f + (in.base_color.y - 0.04f) * m,
                      0.04f + (in.base_color.z - 0.04f) * m};

  // ① 直接光（主光）
  math::Vec3 result =
      pbr_direct_light(n, v, in.key_light, in.base_color, m, in.roughness);

  // ①b 额外灯光：**走同一个 `pbr_direct_light`**，只是入射光不同。
  //
  // 这是“内核全面”的关键：不做“主光一套 / 附加灯另一套”。旧模型就是那么干的
  // （附加光只乘一个 Lambert 项），于是点光/聚光没有高光、颜色口径也不同。
  // bpy 场景常有 key/fill/rim 三盏灯，只算主光会让画面“不够立体”而不报错。
  if (in.extra_lights != nullptr) {
    for (int i = 0; i < in.extra_light_count; ++i) {
      const PbrLight& extra = in.extra_lights[i];
      if (extra.irradiance.x <= 0.0f && extra.irradiance.y <= 0.0f &&
          extra.irradiance.z <= 0.0f) {
        continue;
      }
      const math::Vec3 contribution =
          pbr_direct_light(n, v, extra, in.base_color, m, in.roughness);
      result.x += contribution.x;
      result.y += contribution.y;
      result.z += contribution.z;
    }
  }

  // ② 均匀环境光（Blender 的 world / three 的 AmbientLight）
  //
  // ## 为何乘 `occlusion`
  //
  // 环境光是**间接光**（它来自四面八方，没有单一方向）。而缝隙遮蔽（AO）
  // 的定义就是“半球里被挡了多少”——所以它应当只乘间接光，**不乘直接光**。
  // 直接光有自己的投影阴影路径（`ShadowMap` / `PlanarShadow`），
  // 两套遮蔽相乘没有物理意义，且会让暗部黑得不真实。
  const float ao = std::clamp(in.occlusion, 0.0f, 1.0f);
  {
    const math::Vec3 ambient = pbr_ambient_diffuse(in.base_color, m, in.ambient_irradiance);
    result.x += ambient.x * ao;
    result.y += ambient.y * ao;
    result.z += ambient.z * ao;
  }

  // ③ IBL（方向探针）：漫反射 + 高光两个分量**必须分开**
  //
  // 合起来会错得很具体：漫反射要的是"法线周围的能量"，高光要的是
  // "反射方向周围的能量"。用一个采样糊两者，结果是金属的高光跟着法线走
  // （看起来像塑料）而不是跟着视角走。
  if (in.ibl_strength > 0.0f && in.ibl_count > 0) {
    const math::Vec3 ibl_diffuse =
        pbr_ibl_diffuse(n, in.ibl_dirs, in.ibl_colors, in.ibl_count);
    const math::Vec3 indirect_diffuse{
        ibl_diffuse.x * in.base_color.x * one_minus_metal * kReciprocalPi * in.ibl_strength * ao,
        ibl_diffuse.y * in.base_color.y * one_minus_metal * kReciprocalPi * in.ibl_strength * ao,
        ibl_diffuse.z * in.base_color.z * one_minus_metal * kReciprocalPi * in.ibl_strength *
            ao};
    result.x += indirect_diffuse.x;
    result.y += indirect_diffuse.y;
    result.z += indirect_diffuse.z;

    const math::Vec3 radiance =
        pbr_ibl_specular(n, v, in.roughness, in.ibl_dirs, in.ibl_colors, in.ibl_count);
    const auto [a, b] = pbr_env_brdf(in.roughness, n_dot_v);
    // `F0·A + B`：split-sum 的强度因子（B 是掠射角补偿）
    const math::Vec3 specular{
        radiance.x * (f0.x * a + b) * in.ibl_strength * ao,
        radiance.y * (f0.y * a + b) * in.ibl_strength * ao,
        radiance.z * (f0.z * a + b) * in.ibl_strength * ao};
    result.x += specular.x;
    result.y += specular.y;
    result.z += specular.z;
  }

  // ④ 自发光：加在 saturate **之前**（这样超过 1 的部分能被色调映射滚降）
  result.x += in.emissive.x;
  result.y += in.emissive.y;
  result.z += in.emissive.z;

  // ⑤ 截断到非负 + 曝光（three 的 `outgoingLight = saturate(...)`，曝光在输出链）
  const auto non_negative = [](float value) noexcept { return value < 0.0f ? 0.0f : value; };
  const float gain = in.exposure > 0.0f ? in.exposure : 0.0f;
  return math::Vec3{non_negative(result.x) * gain, non_negative(result.y) * gain,
                    non_negative(result.z) * gain};
}

}  // namespace st::raster

