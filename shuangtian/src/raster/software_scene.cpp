/// 三维场景的**软件实现**：自带 z-buffer 与逐像素光照，不依赖任何图形 API。
///
/// 定位（用户定的架构原则）：**软件渲染是保证跨平台可用的那条腿**，
/// 系统高阶 API（D3D11/OpenGL/Metal）是锦上添花。
/// 因此三维能力**必须**有一条软路径——否则"删掉 OpenGL"就等于"非 Windows 上三维消失"，
/// 那与"软件保底"直接矛盾。而且软路径顺带是 GPU 路径的**对照基准**：
/// 两条腿画同一个场景，差异才可测。
///
/// 性能取舍：这里为**界面里一块区域**（如 200px 高）优化，不是为全屏 3D 游戏。
/// 实测参考：2D 整帧（1280×800 ≈ 100 万像素）12.8ms；本实现只处理网格覆盖的像素
/// （一个 200×960 的视图 ≈ 19 万像素，且用包围盒裁剪），完全够用。

#include "st/raster/scene3d.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"

namespace st::raster {
namespace {

/// 齐次坐标（裁剪空间用）。
struct Vec4 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
  float w{0.0f};
};

/// 列主序矩阵 × 齐次点（与 `Mat4::value(row, col)` 的约定一致）。
[[nodiscard]] auto transform(const math::Mat4& m, math::Vec3 p, float w) noexcept -> Vec4 {
  return Vec4{m.value(0, 0) * p.x + m.value(0, 1) * p.y + m.value(0, 2) * p.z + m.value(0, 3) * w,
              m.value(1, 0) * p.x + m.value(1, 1) * p.y + m.value(1, 2) * p.z + m.value(1, 3) * w,
              m.value(2, 0) * p.x + m.value(2, 1) * p.y + m.value(2, 2) * p.z + m.value(2, 3) * w,
              m.value(3, 0) * p.x + m.value(3, 1) * p.y + m.value(3, 2) * p.z + m.value(3, 3) * w};
}

/// 变换后的顶点：裁剪空间位置 + 世界法线 + 顶点色 + UV。
struct Vertex {
  Vec4 clip{};
  math::Vec3 normal{};
  math::Vec3 color{};
  /// **世界空间**位置：算逐像素视向用（`eye - world`）。
  ///
  /// 存世界位置而不是相机空间：插值后算 `eye - world` 最直接，与模型变换无关
  /// （物体自转时视向自动跟对）。
  math::Vec3 world{};
  float u{0.0f};
  float v{0.0f};
  /// 本三角形是否带 UV（纹理采样开关，避免“无 UV 网格误用残留纹理”）。
  bool has_uv{false};
  [[nodiscard]] auto position() const noexcept -> math::Vec3 { return {clip.x, clip.y, clip.z}; }
};

/// 近裁剪面（`w > kNearEpsilon` 才算在相机前方）。
/// 用 `w` 而不是 `z`：透视投影里 `w = -z_view`，判它最直接，且对正交投影同样成立。
constexpr float kNearEpsilon = 1e-4f;

[[nodiscard]] auto lerp_vertex(const Vertex& a, const Vertex& b, float t) noexcept -> Vertex {
  Vertex out;
  out.clip = Vec4{a.clip.x + (b.clip.x - a.clip.x) * t, a.clip.y + (b.clip.y - a.clip.y) * t,
                  a.clip.z + (b.clip.z - a.clip.z) * t, a.clip.w + (b.clip.w - a.clip.w) * t};
  out.normal = a.normal + (b.normal - a.normal) * t;
  out.color = a.color + (b.color - a.color) * t;
  out.world = a.world + (b.world - a.world) * t;
  // 裁剪产生的顶点也要插值 UV（否则裁过的面会采样到错误位置）
  out.u = a.u + (b.u - a.u) * t;
  out.v = a.v + (b.v - a.v) * t;
  out.has_uv = a.has_uv;
  return out;
}

/// 单平面（`w > eps`）Sutherland–Hodgman 裁剪，输出 0~4 个顶点。
///
/// 为什么必须裁：相机贴在模型上时三角形会跨越相机平面，此时 `w` 为负，
/// 透视除法会把顶点"翻"到画面另一侧——不裁就会出现**穿透模型的乱线**。
/// 只裁近面（不做完整视锥裁剪）：超出左右/上下边界的像素本来就被包围盒与裁剪区挡掉，
/// 多裁三个面只增加代码与出错面。
[[nodiscard]] auto clip_near(const Vertex& a, const Vertex& b, const Vertex& c,
                             Vertex out[4]) noexcept -> int {
  const Vertex* input[3] = {&a, &b, &c};
  Vertex buffer[8];
  int buffer_count = 0;
  Vertex working[8];
  for (int i = 0; i < 3; ++i) {
    working[i] = *input[i];
  }
  int working_count = 3;
  for (int i = 0; i < working_count; ++i) {
    const Vertex& current = working[i];
    const Vertex& next = working[(i + 1) % working_count];
    const bool current_in = current.clip.w > kNearEpsilon;
    const bool next_in = next.clip.w > kNearEpsilon;
    if (current_in) buffer[buffer_count++] = current;
    if (current_in != next_in) {
      const float t = (kNearEpsilon - current.clip.w) / (next.clip.w - current.clip.w);
      buffer[buffer_count++] = lerp_vertex(current, next, t);
    }
  }
  const int produced = std::min(buffer_count, 4);
  for (int i = 0; i < produced; ++i) out[i] = buffer[i];
  return produced;
}
}  // namespace

/// 与 GPU 路径**同一套光照公式**（Lambert + 环境 + 边缘光）。
///
/// 两条腿（软件/GPU）必须在同一场景上给出接近的画面，否则"软件保底"
/// 就只是"另一种长相"。**公式只在这里定义一次**：GPU 侧的 HLSL（`scene3d_d3d11.cpp`
/// 的 `ps_scene3d`）是这段的逐字翻译，改动必须两处同时改。
/// 能逐字节对齐靠的是：两侧都只做乘加、不调 `pow`/`smoothstep` 这类
/// 实现自由度大的函数（见下方“为何把幂改成乘法”）。
///
/// ## 为何把 `pow(rim, 2)` 换成乘法
///
/// 原先写 `std::pow(1 - nz, 2.0f)`——那是**逐像素**调用（1080p 一次 200 万次）。
/// `pow` 的浮点版本在多数工具链上走 `exp/log` 路径，而这里底数是 `1-nz`、
/// 指数恒为 2，直接 `r*r` 在语义上**逐位等价**（乘法比幂函数精确）。
/// 实测收益见 `tools/scene3d_cost_probe.cpp` 的对照数据。
/// 一个光源在某点产生的**方向与强度**（世界空间，方向是「从表面指向光源」）。
///
/// 三种类型的统一表示：着色端只需要「方向 + 强度」，不必关心类型。
/// 这样新增类型只改这一个函数，`shade_surface` 保持单一路径。
struct LightSample {
  math::Vec3 direction{0.0f, 1.0f, 0.0f};   ///< 从表面指向光源（单位向量）
  float intensity{0.0f};                    ///< 已含距离衰减与锥形
};

/// 把任意类型的光源采样成「方向 + 强度」。
///
/// ## 距离衰减为何是 `1/d²` 而不是 `1/d`
///
/// 辐照度按平方反比衰减（能量摊在球面上）。用 `1/d` 会让近处不够亮、
/// 远处过亮——观感上像“整个房间被均匀照”，正是点光要解决的那个问题。
///
/// ## `radius` 的处理
///
/// 纯 `1/d²` 在远处永远有微小贡献，几盏灯加起来会让暗部永远不够暗。
/// 给有限半径后，超出即归零（真实灯泡也有有效范围），
/// “关掉远处那盏灯”才可预测。
[[nodiscard]] auto sample_light(const Light& light, math::Vec3 world_position) -> LightSample {
  LightSample out{};
  if (light.intensity <= 0.0f) return out;

  if (light.kind == LightKind::Directional) {
    out.direction = light.direction.normalized();
    out.intensity = light.intensity;
    return out;
  }

  // 点光 / 聚光：方向由位置决定
  const math::Vec3 delta = light.position - world_position;
  const float distance = delta.length();
  if (distance <= 1e-6f) return out;   // 退化：点在光源上
  const float inv = 1.0f / distance;
  out.direction = math::Vec3{delta.x * inv, delta.y * inv, delta.z * inv};

  // 平方反比；`+1` 避免极近距离爆炸（真实灯泡也有灯丝尺寸）
  float attenuation = 1.0f / (1.0f + distance * distance);

  if (light.radius > 0.0f) {
    // 平滑截断：半径处归零，避免“硬边界”（照出一圈可见的圆）
    const float t = distance / light.radius;
    if (t >= 1.0f) return out;
    const float fade = 1.0f - t * t;
    attenuation *= fade * fade;
  }

  if (light.kind == LightKind::Spot) {
    // 锥形。
    //
    // ## 方向约定（这里踩过一次，代价是把整块画面变黑）
    //
    // `out.direction` 是「从表面指向光源」。所以「从光源指向表面」是它的反向，
    // 而聚光的锥轴就是后者——两者**同向**时点积接近 1（表面在锥内）。
    //
    // 因此：`cos_angle = (-out.direction) · axis_hat`。
    // 踩过的写法是只把轴取反、没把表面方向也取反（或反之），点积恒为 -1，
    // 锥外判定永远成立 → 聚光照不出任何东西（全黑，看起来像“功能没生效”）。
    const math::Vec3 axis_raw = light.direction;
    const math::Vec3 axis = math::Vec3{axis_raw.x, axis_raw.y, axis_raw.z}.normalized();
    const float cos_angle = -(axis.dot(out.direction));
    const float inner = std::cos(light.spot_inner_degrees * 0.017453292519943295f);
    const float outer = std::cos(light.spot_outer_degrees * 0.017453292519943295f);
    if (cos_angle <= outer) return out;
    if (cos_angle < inner && inner > outer) {
      attenuation *= (cos_angle - outer) / (inner - outer);
    }
  }

  out.intensity = light.intensity * attenuation;
  return out;
}

auto shade_terms_at(math::Vec3 normal, math::Vec3 color, const Lighting& lighting,
                    math::Vec3 world_position) -> math::Vec3 {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 light = lighting.key_direction.normalized();
  const float diffuse = std::max(n.dot(light), 0.0f);
  // 边缘光：视线沿 -Z（相机朝 -Z 看），所以 `1 - n.z` 就是“偏离正对相机”的程度。
  const float rim_base = 1.0f - std::max(n.z, 0.0f);
  const float rim = rim_base * rim_base * lighting.rim;
  // ⚠ 边缘光 **乘在固有色上**，不是加到最终色上。
  //
  // 实测踩过（用户一眼就看出来的“边缘锯启”其实是这个）：早先写 `color*scale + rim`，
  // 于是盒子的**侧壁**（n.z=0）拿到 rim 满值，亮度被抬到 0.905——**比顶面 0.969
  // 只暗一点点**，而它本该是最暗的面之一。观感上就是“盒子上贴了一片白边”，
  // 那片假面的两条硬边被误认为是抗锯齿失败。
  //
  // 改成乘法后：侧壁 = color × 1.115、顶面 = color × 1.35（自身受光多），
  // 明暗关系（顶 > 侧 > 正）得以保持，且不会出现“比亮面还亮”的反相。
  const float scale = lighting.ambient + diffuse * lighting.diffuse + rim;
  const auto clamp01 = [](float value) noexcept -> float {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
  };
  const float gain = scale * lighting.exposure;
  math::Vec3 result{color.x * gain, color.y * gain, color.z * gain};

  // ## 附加光源（本次新增）
  //
  // 关键约束：`extra_lights` 全为空时结果必须与上面**逐位相同**。
  // 所以这里是“追加”，而不是把主光也重构成循环。
  //
  // 为何不在 `shade_surface` 里做：`shade_terms` 是两条腿的唯一参考实现，
  // 主光 + 环境 + 边缘光的语义已经在它里面钉住了。附加光源作为**后加的
  // 漫反射项**追加，两边都好维护。
  // 附加光源：走 `sample_light`，把平行光/点光/聚光统一折算成「方向 + 强度」。
  //
  // ## 为何两条路径（本函数与 `shade_surface`）都必须用 `sample_light`
  //
  // 材质默认值（`specular=0, emission=0, metallic=0`）时走的就是**本函数**。
  // 只改 `shade_surface` 会让“没设材质的场景里点光完全不生效”——
  // 实测踩过：探针里点光/聚光四项全红，数值一字不变。
  for (const Light& extra_light : lighting.extra_lights) {
    if (extra_light.intensity <= 0.0f) continue;
    const LightSample sample = sample_light(extra_light, world_position);
    if (sample.intensity <= 0.0f) continue;
    const float lambert = std::max(n.dot(sample.direction), 0.0f);
    if (lambert <= 0.0f) continue;
    const float weight = lambert * sample.intensity;
    result.x += color.x * extra_light.color.x * weight;
    result.y += color.y * extra_light.color.y * weight;
    result.z += color.z * extra_light.color.z * weight;
  }

  return math::Vec3{clamp01(result.x), clamp01(result.y), clamp01(result.z)};
}

// ── GGX 高光与 Principled 着色 ──

/// `alpha` 的下限。
///
/// 为何要夹：`roughness → 0` 时 GGX 的 D 项分母趋于 0，解析式会溢出成 inf/NaN。
/// 实测：不夹时 `roughness=0` 会输出 NaN，整幅变黑（NaN 在色调映射里传插）。
constexpr float kMinAlpha = 1e-3f;

auto ggx_distribution(float n_dot_h, float roughness) noexcept -> float {
  const float alpha = std::max(roughness, kMinAlpha);
  const float a2 = alpha * alpha;
  const float nh = std::max(n_dot_h, 0.0f);
  const float nh2 = nh * nh;
  const float denom = nh2 * (a2 - 1.0f) + 1.0f;
  if (denom <= 1e-12f) return 0.0f;
  return a2 / (3.14159265358979f * denom * denom);
}

auto ggx_geometry(float n_dot_v, float n_dot_l, float roughness) noexcept -> float {
  // Smith 的 Schlick-GGX 近似：k = (alpha+1)²/8（直接光照用这个，IBL 用 alpha²/2）
  const float alpha = std::max(roughness, kMinAlpha);
  const float k = (alpha + 1.0f) * (alpha + 1.0f) * 0.125f;
  const float nv = std::max(n_dot_v, 0.0f);
  const float nl = std::max(n_dot_l, 0.0f);
  const float gv = nv / (nv * (1.0f - k) + k);
  const float gl = nl / (nl * (1.0f - k) + k);
  return gv * gl;
}

auto ggx_specular(math::Vec3 normal, math::Vec3 view_dir, math::Vec3 light_dir,
                  float roughness, float metallic, math::Vec3 base_color,
                  math::Vec3 specular_tint) noexcept -> math::Vec3 {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 v = view_dir.normalized();
  const math::Vec3 l = light_dir.normalized();
  const math::Vec3 h = (v + l).normalized();

  const float n_dot_v = std::max(n.dot(v), 0.0f);
  const float n_dot_l = std::max(n.dot(l), 0.0f);
  if (n_dot_v <= 0.0f || n_dot_l <= 0.0f) return math::Vec3{0.0f, 0.0f, 0.0f};
  const float n_dot_h = std::max(n.dot(h), 0.0f);
  const float v_dot_h = std::max(v.dot(h), 0.0f);

  // Fresnel（Schlick）：掠射角下反射率趋于 1
  // 非金属的 F0 = 0.04（常见绝缘体的平均值）；金属的 F0 = 固有色。
  const math::Vec3 f0 = math::Vec3{0.04f + (base_color.x - 0.04f) * metallic,
                                   0.04f + (base_color.y - 0.04f) * metallic,
                                   0.04f + (base_color.z - 0.04f) * metallic};
  const float one_minus_vh = 1.0f - v_dot_h;
  const float fresnel_scalar = one_minus_vh * one_minus_vh * one_minus_vh * one_minus_vh *
                               one_minus_vh;   // ^5，写成乘法避开 pow

  const float d = ggx_distribution(n_dot_h, roughness);
  const float g = ggx_geometry(n_dot_v, n_dot_l, roughness);
  // 分母 4·(n·v)·(n·l)：能量守恒的归一化因子
  const float denom = 4.0f * n_dot_v * n_dot_l;
  if (denom <= 1e-9f) return math::Vec3{0.0f, 0.0f, 0.0f};

  // ## 为何要乘 `n·l`
  //
  // BRDF 是“出射辐亮度 / 入射辐照度”的比值，而真正进入着色方程的是
  // `BRDF · 辐照度`，其中辐照度含 `n·l`（余弦因子）。
  //
  // 漏掉它的后果很具体：实测 `spec` 只有 **0.0003~0.0067**——比漫反射
  // （0.5 量级）小两个数量级，高光在画面上完全看不见，而这不会报任何错。
  const float cosine = n_dot_l;
  const auto channel = [&](float f0_c, float tint_c) -> float {
    const float f = f0_c + (1.0f - f0_c) * fresnel_scalar;
    return d * g * f / denom * tint_c * cosine;
  };
  return math::Vec3{channel(f0.x, specular_tint.x), channel(f0.y, specular_tint.y),
                    channel(f0.z, specular_tint.z)};
}

math::Vec3 shade_surface(math::Vec3 normal, math::Vec3 base_color, math::Vec3 view_dir,
                         const Lighting& lighting, float metallic, float roughness,
                         float specular, float emission,                          math::Vec3 specular_tint,
                         math::Vec3 world_position = math::Vec3{0.0f, 0.0f, 0.0f},
                         math::Vec3 emissive_color = math::Vec3{1.0f, 1.0f, 1.0f},
                         bool emission_unlit = false) {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 l = lighting.key_direction.normalized();

  // ## 渐进采用：高光为 0 时必须与旧 `shade_terms` 逐位相同
  if (specular <= 0.0f && emission <= 0.0f && metallic <= 0.0f) {
    return shade_terms_at(normal, base_color, lighting, world_position);
  }

  const float n_dot_l = std::max(n.dot(l), 0.0f);
  const float rim_base = 1.0f - std::max(n.z, 0.0f);
  const float rim = rim_base * rim_base * lighting.rim;

  // 金属没有漫反射（自由电子吸收透射光）——这是金属与非金属的物理分界线。
  // 漏掉它时，金色球会显得像“黄色塑料”，一眼就假。
  const float diffuse_weight = 1.0f - metallic;
  const float diffuse_scale =
      lighting.ambient * diffuse_weight + n_dot_l * lighting.diffuse * diffuse_weight + rim;

  math::Vec3 result{base_color.x * diffuse_scale, base_color.y * diffuse_scale,
                    base_color.z * diffuse_scale};

  // 附加光源：与 `shade_terms` 同一口径（漫反射项，乘固有色与光色）。
  // 材质路径不能漏这一步，否则“设了材质就少了几盏灯”。
  for (const Light& extra_light : lighting.extra_lights) {
    if (extra_light.intensity <= 0.0f) continue;
    const LightSample sample = sample_light(extra_light, world_position);
    if (sample.intensity <= 0.0f) continue;
    const float lambert = std::max(n.dot(sample.direction), 0.0f);
    if (lambert <= 0.0f) continue;
    const float weight = lambert * sample.intensity * diffuse_weight;
    result.x += base_color.x * extra_light.color.x * weight;
    result.y += base_color.y * extra_light.color.y * weight;
    result.z += base_color.z * extra_light.color.z * weight;
    // 点光/聚光也产生高光——这才是「灯泡在金属上留下亮点」的关键。
    // 平行光那条走下面的主光路径（保持旧数值不变）。
    if (specular > 0.0f && extra_light.kind != LightKind::Directional) {
      const math::Vec3 spike =
          ggx_specular(n, view_dir, sample.direction, roughness, metallic, base_color,
                       specular_tint);
      result.x += spike.x * specular * sample.intensity;
      result.y += spike.y * specular * sample.intensity;
      result.z += spike.z * specular * sample.intensity;
    }
  }

  if (specular > 0.0f) {
    const math::Vec3 spike =
        ggx_specular(n, view_dir, l, roughness, metallic, base_color, specular_tint);
    result.x += spike.x * specular;
    result.y += spike.y * specular;
    result.z += spike.z * specular;
  }

  // 自发光：加在**色调映射之前**（线性 HDR 域），这样才能被色调映射滚降。
  //
  // ## `emission_unlit` 的意义（2026-10-24）
  //
  // Blender 的 Emission 项**不受光照影响**；而本管线旧实现是“加在受光结果上”。
  // 两者在暗场景里差别很大：霓虹灯管应当自己亮着（哪怕环境全黑），
  // 而不是“被环境照亮后再亮一点”。
  // 所以 `emission_unlit = true` 时把前面的受光项**整体丢掉**，只留自发光。
  if (emission > 0.0f) {
    if (emission_unlit) {
      result = math::Vec3{0.0f, 0.0f, 0.0f};
    }
    result.x += emissive_color.x * emission;
    result.y += emissive_color.y * emission;
    result.z += emissive_color.z * emission;
  }
  return math::Vec3{result.x * lighting.exposure, result.y * lighting.exposure,
                    result.z * lighting.exposure};
}

namespace {

class SoftwareScene final : public Scene3D {
 public:
  SoftwareScene(int width, int height)
      : width_(std::max(1, width)), height_(std::max(1, height)),
        // **超采样缓冲**：每个像素 `S²` 个样本，解析（resolve）后才是真正的抗锯齿。
        //
        // 为何需要它（实测）：单点采样下三维模型的斜边是一排排像素阶梯（成片上
        // 非常明显，用户一眼就看到了）。二维那边本来就抗锯齿（扫描线覆盖率），
        // 所以反差更刺眼：文字平滑、模型崩齿。
        //
        // 为何用**超采样**而不是“覆盖率估算”：三维的锯齿主要在**轮廓边**，
        // 而区分轮廓边与内部边的代价并不比干脆多画几个样本低；而超采样对
        // “细长三角形”、“边缘光rim”这些情形都是天然正确的。
        //
        // 代价：光栅像素数 ×S²（S=2 是 4 倍）。实测三维光栅本身很便宜
        //（720p 千面软件腿 ~60ms 中的大头在像素），所以 S=2 是本轮选的档位；
        // 要更高质量把 `kBake` 调到 3/4（成本 S²）。
        canvas_(width_ * kBake, height_ * kBake),
        depth_(static_cast<std::size_t>(width_) * kBake *
                   static_cast<std::size_t>(height_) * kBake,
               0.0f) {}

  void begin_frame(math::Color clear_color) override {
    // ## 顺序：先重置算子，再编码清屏色
    //
    // 若先清屏再重置，用的是**上一帧**的 `tone_map_`（首帧用默认值）——
    // 表现是“切了色调映射后背景慢一帧才跟上”。这类延迟在批量出片里很难查。
    lighting_ = Lighting{};          // 每帧回到默认：不调 `set_lighting` 的调用方行为完全不变
    tone_map_ = lighting_.tone_map;
    cull_mode_ = CullMode::Back;     // 同上：不重置会让上一帧的设置泄漏
    material_ = Material{};
    shadow_ = PlanarShadow{};
    shadow_tris_.clear();
    entity_mask_.assign(depth_.size(), 0);
    mask_entities_ = false;
    transparent_.clear();
    // ## 清屏色也必须走**同一条输出链**
    //
    // 实测踩到：原先这里直接 `canvas_.clear(clear_color)`，于是 background
    // 是“线性值当 sRGB 存”（旧行为），而实体走新链（ACES+sRGB）。
    // 结果是深灰底渲染出来一片白——两种色彩空间混在一帧里。
    //
    // 输入约定：`clear_color` 是**线性**值（与 `Lighting`/顶点色同一口径），
    // 输出到画布前统一经 `encode_pixel`。
    const math::Vec3 bg_linear{static_cast<float>(clear_color.r) / 255.0f,
                               static_cast<float>(clear_color.g) / 255.0f,
                               static_cast<float>(clear_color.b) / 255.0f};
    canvas_.clear(encode_pixel(bg_linear, 1.0f, tone_map_));
    std::fill(depth_.begin(), depth_.end(), 1.0f);   // 1.0 = 远平面
    draw_calls_ = 0;
  }

  void set_camera(const Camera& camera) override {
    // ⚠ 纵横比用**逻辑尺寸**，不是超采样缓冲尺寸。
    // 两者比例相同，但直接写 `kWidth*kBake / kHeight*kBake` 在 kBake 改变时
    // 容易漏改一处，而纵横比错了会让模型整体变形（很难一眼看出是哪里的错）。
    const float aspect = height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_)
                                     : 1.0f;
    const float fov = camera.fov_y_degrees * 3.14159265358979f / 180.0f;
    eye_ = camera.eye;
    view_ = math::Mat4::look_at(camera.eye, camera.target, camera.up);
    // 投影：透视/正交由相机决定。正交下 `fov` 无效，由 `ortho_scale`（垂直可视高度）决定。
    projection_ = camera.projection == ProjectionKind::Orthographic
                      ? math::Mat4::orthographic(camera.ortho_scale, aspect, camera.near_z,
                                                 camera.far_z)
                      : math::Mat4::perspective(fov, aspect, camera.near_z, camera.far_z);
    mvp_ready_ = true;
  }

  void set_lighting(const Lighting& lighting) override {
    lighting_ = lighting;
    tone_map_ = lighting.tone_map;
  }

  void set_cull_mode(CullMode mode) override { cull_mode_ = mode; }

  void set_shadow_map(const ShadowMap& shadow) override { shadow_map_ = shadow; }

  /// 构阴影贴图：用**光源视角**把世界三角形光栅化到一张深度图。
  ///
  /// ## 为何用正交投影
  ///
  /// 平行光没有“原点”，只能用正交。聚光理论上可用透视（锥体就是视锥），
  /// 但正交在小锥角下差别很小，而两套路径的代码量与出错面会翻倍。
  /// 所以统一用正交，`extent` 给包络半径。
  ///
  /// 这个取舍的代价写在文档里：大锥角聚光的阴影会偏小。
  void build_shadow_map() {
    shadow_ready_ = false;
    if (shadow_map_.strength <= 0.0f) return;
    if (shadow_geometry_.empty()) return;

    const int index = std::clamp(shadow_map_.light_index, 0, kMaxExtraLights - 1);
    const Light& light = lighting_.extra_lights[static_cast<std::size_t>(index)];
    if (light.intensity <= 0.0f) return;
    // 点光不能用单张阴影贴图（需要立方体贴图），静默跳过。
    if (light.kind == LightKind::Point) return;

    const int res = std::clamp(shadow_map_.resolution, 64, 4096);
    shadow_depth_.assign(static_cast<std::size_t>(res) * static_cast<std::size_t>(res), 1.0f);

    // 光源朝向：平行光用 `direction`，聚光同。`direction` 是**照射方向**。
    math::Vec3 dir = light.direction;
    const float dir_len = dir.length();
    if (dir_len <= 1e-6f) return;
    dir = math::Vec3{dir.x / dir_len, dir.y / dir_len, dir.z / dir_len};

    // 光源相机：从“逆向远处”看向原点（平行光无位置）。
    // 聚光有位置，但从它的位置看与从远处看（同方向）在小锥角下几乎一致。
    const float dist = std::max(shadow_map_.extent * 2.0f, 4.0f);
    const math::Vec3 eye{light.kind == LightKind::Spot ? light.position.x - dir.x * dist
                                                      : -dir.x * dist,
                         light.kind == LightKind::Spot ? light.position.y - dir.y * dist
                                                      : -dir.y * dist,
                         light.kind == LightKind::Spot ? light.position.z - dir.z * dist
                                                      : -dir.z * dist};
    const math::Vec3 target{eye.x + dir.x, eye.y + dir.y, eye.z + dir.z};
    // up 不能与 dir 平行：|dir.y| 接近 1 时改用 X 轴做 up
    const math::Vec3 up = std::abs(dir.y) > 0.99f ? math::Vec3{1.0f, 0.0f, 0.0f}
                                                 : math::Vec3{0.0f, 1.0f, 0.0f};
    const math::Mat4 view = math::Mat4::look_at(eye, target, up);
    const math::Mat4 proj =
        math::Mat4::orthographic(shadow_map_.extent * 2.0f, 1.0f, 0.01f, dist * 2.0f);
    shadow_vp_ = proj * view;

    // 光栅化：每个三角形投到贴图上，写最小深度
    const float half = static_cast<float>(res) * 0.5f;
    for (const ShadowTriangle& tri : shadow_geometry_) {
      Vec4 clip[3];
      float sx[3];
      float sy[3];
      float sz[3];
      bool ok = true;
      for (int k = 0; k < 3; ++k) {
        clip[k] = transform(shadow_vp_, tri.v[k], 1.0f);
        if (clip[k].w == 0.0f) {
          ok = false;
          break;
        }
        sx[k] = (clip[k].x / clip[k].w * 0.5f + 0.5f) * static_cast<float>(res);
        sy[k] = (0.5f - clip[k].y / clip[k].w * 0.5f) * static_cast<float>(res);
        sz[k] = clip[k].z / clip[k].w;
      }
      if (!ok) continue;
      // 包围盒（夹到贴图内）
      const float min_x = std::max(std::min({sx[0], sx[1], sx[2]}), 0.0f);
      const float max_x = std::min(std::max({sx[0], sx[1], sx[2]}), static_cast<float>(res - 1));
      const float min_y = std::max(std::min({sy[0], sy[1], sy[2]}), 0.0f);
      const float max_y = std::min(std::max({sy[0], sy[1], sy[2]}), static_cast<float>(res - 1));
      if (min_x > max_x || min_y > max_y) continue;

      const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
      if (std::abs(area) < 1e-9f) continue;
      // ## 两面都要写（裁剪后的面积可能为负）
      //
      // 阴影贴图不关心面朝向（背向光源的面也会投影），而地面网格的两个三角面
      // 绕序相反。若直接用 `1/area` 归一，一半三角形的重心坐标符号会翻转，
      // 于是被 `w0 < 0` 判成“外部”而跳过——实测覆盖率只有 0.77%，
      // 大片地面根本没进贴图，表现为“阴影只在极小区域生效”或整片发黑。
      //
      // 取 `abs` 归一后，两种绕序的内部判定一致。
      const float inv_area = 1.0f / std::abs(area);
      const bool flipped = area < 0.0f;
      for (int y = static_cast<int>(min_y); y <= static_cast<int>(max_y); ++y) {
        for (int x = static_cast<int>(min_x); x <= static_cast<int>(max_x); ++x) {
          const float px = static_cast<float>(x) + 0.5f;
          const float py = static_cast<float>(y) + 0.5f;
          float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv_area;
          float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv_area;
          if (flipped) {
            w0 = -w0;
            w1 = -w1;
          }
          const float w2 = 1.0f - w0 - w1;
          if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
          const float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
          const std::size_t offset =
              static_cast<std::size_t>(y) * static_cast<std::size_t>(res) +
              static_cast<std::size_t>(x);
          if (z < shadow_depth_[offset]) shadow_depth_[offset] = z;
        }
      }
    }
    (void)half;
    shadow_ready_ = true;
  }

  /// 某世界点是否在阴影里（PCF 柔化）。
  ///
  /// 返回 0（全亮）~ 1（全在阴影）。`soft_radius` 是纹素单位的采样半径。
  [[nodiscard]] auto shadow_factor_at(math::Vec3 world) const -> float {
    if (!shadow_ready_) return 0.0f;
    const int res = std::clamp(shadow_map_.resolution, 64, 4096);
    const Vec4 clip = transform(shadow_vp_, world, 1.0f);
    if (clip.w == 0.0f) return 0.0f;
    const float nx = clip.x / clip.w * 0.5f + 0.5f;
    const float ny = 0.5f - clip.y / clip.w * 0.5f;
    const float nz = clip.z / clip.w;
    // 贴图外 → 无阴影（否则投影相机外的区域会整片变黑）
    if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f || nz < 0.0f || nz > 1.0f) {
      return 0.0f;
    }

    const float bias = shadow_map_.bias;
    const int center_x = static_cast<int>(nx * static_cast<float>(res));
    const int center_y = static_cast<int>(ny * static_cast<float>(res));
    const int radius = static_cast<int>(std::ceil(std::max(shadow_map_.soft_radius, 0.0f)));
    int lit = 0;
    int total = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
      for (int dx = -radius; dx <= radius; ++dx) {
        const int sx = std::clamp(center_x + dx, 0, res - 1);
        const int sy = std::clamp(center_y + dy, 0, res - 1);
        const float stored =
            shadow_depth_[static_cast<std::size_t>(sy) * static_cast<std::size_t>(res) +
                          static_cast<std::size_t>(sx)];
        ++total;
        // 存的是“最近表面深度”；比它更远 → 在阴影里
        if (nz - bias <= stored) ++lit;
      }
    }
    if (total == 0) return 0.0f;
    return 1.0f - static_cast<float>(lit) / static_cast<float>(total);
  }

  void set_material(const Material& material) override {
    material_ = material;
    // opacity < 1 时必须关深度写：否则后面的透明面会被它挡住。
    if (material_.opacity < 0.999f) material_.depth_write = false;
    material_.opacity = std::clamp(material_.opacity, 0.0f, 1.0f);
  }

  void set_planar_shadow(const PlanarShadow& shadow) override {
    shadow_ = shadow;
    // ## 激活遮罩标记
    //
    // 调用约定：先画地面（或其它不投阴影的背景几何），再调 `set_planar_shadow`，
    // 之后画的网格才会投阴影并标记遮罩。
    //
    // 为何用调用顺序而不是靠“哪个是地面”的额外标记：调用方本来就
    // 知道自己在做什么，而给地面加一个新概念（`set_ground(i)`）只会把
    // 接口变复杂。这个约定与 `set_texture` / `set_material` 的设计一致——
    // 都是“先设状态、再画”。
    mask_entities_ = shadow.strength > 0.0f;
  }

  /// 纹理（RGBA8 非预乘，行优先）。空 span / 非法尺寸 = 清除纹理（回到顶点色）。
  void set_texture(std::span<const std::uint8_t> pixels, int width, int height) override {
    const auto clear = [this] {
      texture_.clear();
      texture_width_ = 0;
      texture_height_ = 0;
    };
    if (pixels.empty() || width <= 0 || height <= 0) {
      clear();
      return;
    }
    const std::size_t needed =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (pixels.size() < needed) {
      clear();
      return;
    }
    texture_.assign(pixels.begin(), pixels.begin() + static_cast<std::ptrdiff_t>(needed));
    texture_width_ = width;
    texture_height_ = height;
  }

  /// 把三个世界空间顶点沿光方向投影到 `ground_y`，收集成阴影三角形。
  ///
  /// ## 投影公式
  ///
  /// 给定点 `p`、光方向 `d`（指向光源）、地面高度 `h`，求 `t` 使得：
  /// ```
  /// p.y + t·d.y = h   =>   t = (h - p.y) / d.y
  /// ```
  /// 然后投影点 = `p + t·d`。
  ///
  /// ## `d.y` 接近 0 时要跳过
  ///
  /// 光几乎水平时 `t` 趋于无穷，投影会跑到极远的地方（画面上是一道拖到天际的条）。
  /// 物理上那也确实“不产生可见阴影”，所以直接跳过。
  void collect_shadow_triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    const math::Vec3 dir = lighting_.key_direction.normalized();
    // ## 方向约定（一开始写反了，阴影完全不出）
    //
    // `key_direction` 是**指向光源**的方向。光从上方来时 `dir.y > 0`，
    // 影子落在地面（下方）——所以投射方向是 `-dir`，投射后
    // `t = (h - p.y) / (-dir.y)` 分母为负，结果仍正确（`h < p.y` 时 `t > 0`）。
    //
    // 实测踩到：一开始写成 `if (dir.y >= -1e-4f) return;`（要求光从下方来），
    // 于是默认布光下**永不出影子**，四档强度数值一模一样。
    if (dir.y <= 1e-4f) return;   // 光不从上往下照 → 不会在地面留影

    const auto project = [&](const Vertex& v) -> math::Vec3 {
      // 沿 **-dir** 投射到 y = ground_y
      const float t = (v.world.y - shadow_.ground_y) / dir.y;
      // ## lift 要**向上**（朝相机方向）抬，不是向上抬离地面
      //
      // 相机在物体上方往下看时，y 越大越**远**。实测：把 lift 加在 y 上
      // 得到 `shadow_z=0.9619` vs `ground_z=0.9506`——阴影比地面更远，
      // 于是 `z >= depth` 恒成立，**一个像素也画不出来**。
      // 所以沿投射方向**回退**一点（朝相机）才能稳定胜出。
      return math::Vec3{v.world.x - dir.x * t, shadow_.ground_y - shadow_.lift,
                        v.world.z - dir.z * t};
    };
    const math::Vec3 p0 = project(a);
    const math::Vec3 p1 = project(b);
    const math::Vec3 p2 = project(c);
    // 投影后法线全部朝上（+Y），与地面同向
    const int base = static_cast<int>(shadow_tris_.size()) / 9;
    (void)base;
    shadow_tris_.insert(shadow_tris_.end(),
                        {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z});
  }

  /// 把收集到的阴影三角形画到画布上（叠加一层暗色）。
  void paint_planar_shadow() {
    if (shadow_tris_.empty()) return;
    const float strength = std::clamp(shadow_.strength, 0.0f, 1.0f);
    // 阴影是半透明的：用 alpha 混合到当前画布。
    //
    // 为何不用 `rasterize` 的透明路径：那条路径是“实体表面 + 材质”，
    // 而阴影是**叠加一层暗色**（没有光照、没有材质），语义不同。
    // 这里直接光栅化一个纯色三角形，逐像素与已有像素混合。
    for (std::size_t i = 0; i + 8 < shadow_tris_.size(); i += 9) {
      const math::Vec3 w0{shadow_tris_[i], shadow_tris_[i + 1], shadow_tris_[i + 2]};
      const math::Vec3 w1{shadow_tris_[i + 3], shadow_tris_[i + 4], shadow_tris_[i + 5]};
      const math::Vec3 w2{shadow_tris_[i + 6], shadow_tris_[i + 7], shadow_tris_[i + 8]};
      Vertex v0;
      Vertex v1;
      Vertex v2;
      v0.world = w0;
      v1.world = w1;
      v2.world = w2;
      v0.clip = transform(projection_ * view_, w0, 1.0f);
      v1.clip = transform(projection_ * view_, w1, 1.0f);
      v2.clip = transform(projection_ * view_, w2, 1.0f);
      // 阴影不参与剔面（投影后绕序可能翻转，但那不重要）
      rasterize_shadow(v0, v1, v2, strength);
    }
    shadow_tris_.clear();
  }

  /// 光栅化一个阴影三角形（纯色 + alpha 混合）。
  ///
  /// ## 为何必须做**深度测试**（但不写）
  ///
  /// 阴影在 `end_frame` 里绘制，那时实体已经写入深度。量体在地面**前方**时，
  /// 它的深度更小，阴影（在地面上）测试失败 → 不会被画到量体上。
  /// 这正是“量体遮住自己的影子”的正确效果。
  ///
  /// 不写深度：阴影之间会重叠，写深度会让后画的被前面已经写过的挡掉，
  /// 表现为“几个球只投出一个影子”。而阴影强度靠 alpha 叠加，本来就该叠。
  void rasterize_shadow(const Vertex& a, const Vertex& b, const Vertex& c, float strength) {
    const int bake_width = width_ * kBake;
    const int bake_height = height_ * kBake;
    const auto to_screen = [&](const Vertex& v) {
      const float inv_w = 1.0f / v.clip.w;
      return Vec4{(v.clip.x * inv_w * 0.5f + 0.5f) * static_cast<float>(bake_width),
                  (0.5f - v.clip.y * inv_w * 0.5f) * static_cast<float>(bake_height),
                  v.clip.z * inv_w, inv_w};
    };
    const Vec4 p0 = to_screen(a);
    const Vec4 p1 = to_screen(b);
    const Vec4 p2 = to_screen(c);
    const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::abs(area) < 1e-6f) return;
    const float inv_area = 1.0f / area;
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x}))));
    const int max_x =
        std::min(bake_width - 1, static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
    const int max_y =
        std::min(bake_height - 1, static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));
    if (min_x > max_x || min_y > max_y) return;

    const math::Color shadow_rgb{
        static_cast<std::uint8_t>(std::lround(st::raster::linear_to_srgb8(shadow_.color.x))),
        static_cast<std::uint8_t>(std::lround(st::raster::linear_to_srgb8(shadow_.color.y))),
        static_cast<std::uint8_t>(std::lround(st::raster::linear_to_srgb8(shadow_.color.z))),
        255};
    for (int y = min_y; y <= max_y; ++y) {
      for (int x = min_x; x <= max_x; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        const float py = static_cast<float>(y) + 0.5f;
        const float w0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) * inv_area;
        const float w1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) * inv_area;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
        // ## 为何不做深度测试，而看一份“实体遮罩”
        //
        // 阴影与地面共面，两者的深度差异在透视投影后极小——实测
        // `shadow_z=0.9619` vs `ground_z=0.9502`，阴影**更远**，于是
        // `z >= depth` 恒成立，一个像素也画不出来。调 lift 也靠不住：
        // 那是在与浮点精度搏斗，而不是解决问题。
        //
        // 正确的语义是“阴影不能盖到**实体**上”，而地面不算实体。
        // 所以改用一份独立的遮罩：实体在 `rasterize` 写像素时标记，
        // 阴影只在**未标记**的像素上画。这样与深度无关，确定性更好。
        const std::size_t offset = static_cast<std::size_t>(y) *
                                       static_cast<std::size_t>(bake_width) +
                                   static_cast<std::size_t>(x);
        if (entity_mask_[offset] != 0) continue;   // 被实体占据 → 不画阴影
        const math::Color dst = canvas_.pixel_at(x, y);
        const auto mix = [&](int s, int d) -> int {
          const int out =
              static_cast<int>(std::lround(static_cast<float>(d) +
                                           (static_cast<float>(s) - static_cast<float>(d)) *
                                               strength));
          return std::clamp(out, 0, 255);
        };
        canvas_.set_pixel(x, y,
                          math::Color{static_cast<std::uint8_t>(mix(shadow_rgb.r, dst.r)),
                                static_cast<std::uint8_t>(mix(shadow_rgb.g, dst.g)),
                                static_cast<std::uint8_t>(mix(shadow_rgb.b, dst.b)),
                                dst.a});
      }
    }
  }

  void draw_mesh(const Mesh& mesh, const math::Mat4& model) override {
    if (mesh.is_empty() || !mvp_ready_) return;
    // ## 阴影贴图开启时：延迟到 `end_frame` 统一绘制
    //
    // 为何必须延迟：阴影贴图要**先知道全部几何**（先渲染一次光源视角的深度图），
    // 而像素着色在 `draw_mesh` 里就发生了。先画的网格看不到后画的网格的影子。
    //
    // 这个延迟只在 `shadow_map_.strength > 0` 时启用：关闭时走原来的即时路径，
    // 行为与之前**逐位相同**（老场景不受影响）。
    if (shadow_map_.strength > 0.0f) {
      deferred_.push_back(DeferredMesh{&mesh, model, material_});
      ++draw_calls_;
      return;
    }
    draw_mesh_now(mesh, model);
    ++draw_calls_;
  }

  /// 即时绘制（原有路径，行为不变）。
  void draw_mesh_now(const Mesh& mesh, const math::Mat4& model) {
    const math::Mat4 mvp = projection_ * view_ * model;
    // 本网格是否有可用 UV；有且已设纹理时才采样
    const bool mesh_has_uv = mesh.has_uvs();
    use_texture_ = mesh_has_uv && !texture_.empty() && texture_width_ > 0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      Vertex tri[3];
      bool ok = true;
      for (int k = 0; k < 3; ++k) {
        const std::size_t index = mesh.indices[i + static_cast<std::size_t>(k)];
        const auto vertex = mesh[index];
        if (vertex.size() < Mesh::kVertexFloats) { ok = false; break; }
        const math::Vec3 position{vertex[0], vertex[1], vertex[2]};
        const math::Vec3 normal = math::Vec3{vertex[3], vertex[4], vertex[5]};
        tri[k].clip = transform(mvp, position, 1.0f);
        // 世界位置（含模型变换）：算逐像素视向用
        tri[k].world = math::Vec3{
            model.value(0, 0) * position.x + model.value(0, 1) * position.y +
                model.value(0, 2) * position.z + model.value(0, 3),
            model.value(1, 0) * position.x + model.value(1, 1) * position.y +
                model.value(1, 2) * position.z + model.value(1, 3),
            model.value(2, 0) * position.x + model.value(2, 1) * position.y +
                model.value(2, 2) * position.z + model.value(2, 3)};
        // 法线用模型矩阵变换（与 GPU 侧一致：`mat3(u_model) * in_normal`）
        const math::Vec3 world_normal{
            model.value(0, 0) * normal.x + model.value(0, 1) * normal.y + model.value(0, 2) * normal.z,
            model.value(1, 0) * normal.x + model.value(1, 1) * normal.y + model.value(1, 2) * normal.z,
            model.value(2, 0) * normal.x + model.value(2, 1) * normal.y + model.value(2, 2) * normal.z};
        tri[k].normal = world_normal;
        tri[k].color = math::Vec3{vertex[6], vertex[7], vertex[8]};
        const auto uv = mesh.uv_at(index);
        tri[k].u = uv.first;
        tri[k].v = uv.second;
        tri[k].has_uv = mesh_has_uv;
      }
      if (!ok) continue;

      // ## 平面投影阴影：把未裁剪的三角形投到地面
      //
      // 为何在裁剪**之前**：裁剪是对相机的，而阴影的几何是**世界空间**的
      //（光方向 → 地面）。用裁剪后的顶点投会漏掉“部分在相机外”的三角形，
      // 表现为阴影边缘缺一块。
      if (shadow_.strength > 0.0f) collect_shadow_triangle(tri[0], tri[1], tri[2]);

      // ## 阴影贴图：收集世界空间三角形
      //
      // 为何也在裁剪**之前**：阴影贴图的相机是**光源**，与摄像机无关。
      // 用裁剪后的顶点会漏掉“在相机外、但在光照里”的三角形——它们的影子
      // 本该投到可见区域。
      //
      // 为何只在**不透明**时收：透明物体不投实阴影（否则玻璃后面会多一团黑影）。
      if (shadow_map_.strength > 0.0f && material_.opacity >= 0.999f) {
        shadow_geometry_.push_back(ShadowTriangle{tri[0].world, tri[1].world, tri[2].world});
      }

      Vertex clipped[4];
      const int produced = clip_near(tri[0], tri[1], tri[2], clipped);
      // 裁剪后是凸多边形，按扇形拆回三角形
      for (int k = 1; k + 1 < produced; ++k) {
        // ## 面朝向剔除（在**裁剪后**做）
        //
        // 为何在裁剪后：裁剪会改变顶点，面朝向必须按最终几何判。
        // 而且裁剪前判一次会漏掉“部分在相机后”的情形。
        if (!face_passes_cull(clipped[0], clipped[k], clipped[k + 1])) continue;
        if (material_.opacity < 0.999f) {
          // 透明：先收集，`end_frame` 前按远→近排序再画
          TransparentTri item{};
          item.v[0] = clipped[0];
          item.v[1] = clipped[k];
          item.v[2] = clipped[k + 1];
          item.depth = (clipped[0].clip.w + clipped[k].clip.w + clipped[k + 1].clip.w) / 3.0f;
          transparent_.push_back(item);
        } else {
          rasterize(clipped[0], clipped[k], clipped[k + 1], 1.0f);
        }
      }
    }
  }

  /// 面朝向剔除判定（屏幕空间符号面积）。
  ///
  /// ## 为何用屏幕空间而不是世界空间叉积
  ///
  /// 世界空间叉积需要“顶点绕序约定”（顺时针/逆时针）加模型矩阵行列式符号，
  /// 两个都要对；而屏幕空间符号面积**已经包含**了两者（投影 + 所有变换），
  /// 只看结果符号就行——少一个错源。
  ///
  /// 符号方向：本渲染器 y 向下（`to_screen` 里 `0.5 - y*0.5`），
  /// 所以**逆时针绕序在屏幕上表现为负面积**。正向面 = 面积为负。
  [[nodiscard]] auto face_passes_cull(const Vertex& a, const Vertex& b,
                                       const Vertex& c) const noexcept -> bool {
    if (cull_mode_ == CullMode::None) return true;
    const auto to_xy = [](const Vertex& v) {
      const float inv_w = 1.0f / (v.clip.w > 1e-9f ? v.clip.w : 1e-9f);
      return std::pair<float, float>{v.clip.x * inv_w, v.clip.y * inv_w};
    };
    const auto [ax, ay] = to_xy(a);
    const auto [bx, by] = to_xy(b);
    const auto [cx, cy] = to_xy(c);
    // NDC 下 y 向上（`to_screen` 才翻转），所以面积 > 0 = 逆时针 = 正向面。
    const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (cull_mode_ == CullMode::Back) return area > 0.0f;
    return area <= 0.0f;   // CullMode::Front
  }

  void end_frame(Surface& target, math::Rect destination, float opacity = 1.0f) override {
    // ## 阴影贴图：先构贴图，再把延迟的网格画出来
    //
    // 顺序至关重要：贴图必须在任何像素着色**之前**就绪（着色时要查它），
    // 而几何收集在 `draw_mesh` 时完成。所以这里分两遍：
    //   ① 遍历 `deferred_`，收集世界三角形 → 构光源视角深度图；
    //   ② 再遍历 `deferred_`，真正光栅化（着色时查贴图）。
    //
    // 只在 `shadow_map_.strength > 0` 时走这条；关闭时 `deferred_` 为空，
    // 下面两遍都是空循环，行为与之前逐位相同。
    if (!deferred_.empty()) {
      // ① 收集几何（要带着各自的模型矩阵求世界坐标）
      shadow_geometry_.clear();
      for (const DeferredMesh& item : deferred_) {
        if (item.mesh == nullptr || item.material.opacity < 0.999f) continue;
        const math::Mat4& m = item.model;
        for (std::size_t i = 0; i + 2 < item.mesh->indices.size(); i += 3) {
          math::Vec3 world[3];
          bool ok = true;
          for (int k = 0; k < 3; ++k) {
            const auto vertex = (*item.mesh)[item.mesh->indices[i + static_cast<std::size_t>(k)]];
            if (vertex.size() < Mesh::kVertexFloats) {
              ok = false;
              break;
            }
            const float px = vertex[0];
            const float py = vertex[1];
            const float pz = vertex[2];
            world[k] = math::Vec3{
                m.value(0, 0) * px + m.value(0, 1) * py + m.value(0, 2) * pz + m.value(0, 3),
                m.value(1, 0) * px + m.value(1, 1) * py + m.value(1, 2) * pz + m.value(1, 3),
                m.value(2, 0) * px + m.value(2, 1) * py + m.value(2, 2) * pz + m.value(2, 3)};
          }
          if (!ok) continue;
          shadow_geometry_.push_back(ShadowTriangle{world[0], world[1], world[2]});
        }
      }
      build_shadow_map();
      // ② 真正绘制（逐个网格恢复它自己的材质）
      for (const DeferredMesh& item : deferred_) {
        if (item.mesh == nullptr) continue;
        const Material saved = material_;
        material_ = item.material;
        draw_mesh_now(*item.mesh, item.model);
        material_ = saved;
      }
      deferred_.clear();
      shadow_geometry_.clear();
    }

    // ## 平面投影阴影：先画（它在实体**下方**，被实体遮住的部分本就该被遮）
    //
    // 为何先于透明队列：阴影是“地面上的暗层”，它应该压在背景上、
    // 在实体与透明物体之下。因为阴影不写深度，先画不会影响后续遮挡。
    paint_planar_shadow();
    // ## 透明队列：远 → 近排序后绘制
    //
    // 为何在 `end_frame` 才算：`draw_mesh` 按调用顺序提交，与远近无关。
    // 透明混合的正确性**依赖绘制顺序**（后画的一层覆盖前面），
    // 所以必须收集后统一排序。
    //
    // 排序键用 `clip.w` 的平均值——透视投影里 `w = -z_view`（相机空间距离），
    // 所以 w 越大越远。降序排 = 远先画。
    if (!transparent_.empty()) {
      std::sort(transparent_.begin(), transparent_.end(),
                [](const TransparentTri& l, const TransparentTri& r) {
                  return l.depth > r.depth;   // 远（w 大）在前
                });
      // 透明绘制期间不写深度
      const bool saved_write = material_.depth_write;
      material_.depth_write = false;
      for (const TransparentTri& item : transparent_) {
        rasterize(item.v[0], item.v[1], item.v[2], material_.opacity);
      }
      material_.depth_write = saved_write;
      transparent_.clear();
    }
    // 结果作为**一张图**交给目标画布（与 2D 原语同一条合成路径）。
    //
    // 交付前必须先 **resolve**（超采样缓冲 → 逻辑分辨率）：直接 `draw_canvas`
    // 也能缩对，但那会把“平均”交给双线性采样，而 alpha 的平场需要预乘空间
    // 里的加权平均才能与“其它二维图元”一致。所以自己平均一次。
    const Canvas resolved = resolve_to_logical();
    st::raster::DrawOptions options{};
    options.opacity = opacity;
    target.draw_canvas(resolved, destination, options);
  }

  [[nodiscard]] auto width() const noexcept -> int override { return width_; }
  [[nodiscard]] auto height() const noexcept -> int override { return height_; }
  [[nodiscard]] auto draw_calls() const noexcept -> std::uint64_t override { return draw_calls_; }
  [[nodiscard]] auto backend() const -> std::string override { return "software"; }
  /// 软件腿是同步的：`draw_mesh` 返回时像素已经写完了，没什么可等。
  void flush() override {}

 private:
  /// 屏幕空间三角形光栅化：重心坐标 + z-buffer + 透视校正插值 + 逐像素光照。
  ///
  /// 坐标系是**超采样缓冲**（`width_*kBake` × `height_*kBake`）：透视映射、
  /// 包围盒、深度缓冲都按它算。这样“一个逻辑像素的 `S²` 个子样本”自然落在
  /// 同一块缓冲上，resolve 时平均即得抗锯齿。
  void rasterize(const Vertex& a, const Vertex& b, const Vertex& c, float opacity) {
    const int bake_width = width_ * kBake;
    const int bake_height = height_ * kBake;
    // 透视除法 → NDC → 屏幕（y 向下，与画布一致）
    const auto to_screen = [&](const Vertex& v) {
      const float inv_w = 1.0f / v.clip.w;
      return Vec4{(v.clip.x * inv_w * 0.5f + 0.5f) * static_cast<float>(bake_width),
                  (0.5f - v.clip.y * inv_w * 0.5f) * static_cast<float>(bake_height),
                  v.clip.z * inv_w, inv_w};
    };
    const Vec4 p0 = to_screen(a);
    const Vec4 p1 = to_screen(b);
    const Vec4 p2 = to_screen(c);

    const float area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
    if (std::abs(area) < 1e-6f) return;   // 退化三角形
    const float inv_area = 1.0f / area;

    // 包围盒与画布求交（这一步就是 3D 的"视口剔除"：屏幕外的像素一个都不碰）
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x}))));
    const int max_x = std::min(bake_width - 1, static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
    const int max_y = std::min(bake_height - 1, static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));
    if (min_x > max_x || min_y > max_y) return;

    for (int y = min_y; y <= max_y; ++y) {
      for (int x = min_x; x <= max_x; ++x) {
        const float px = static_cast<float>(x) + 0.5f;
        const float py = static_cast<float>(y) + 0.5f;
        // 重心坐标（面积比）
        float w0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) * inv_area;
        float w1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) * inv_area;
        float w2 = 1.0f - w0 - w1;
        // 允许 1e-5 的边界容差：相邻三角形共享的边要**无缝**（否则会看到裂缝）
        if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;

        // 深度：用 NDC z 线性插值（已含透视除法，插值即正确）
        const float z = w0 * p0.z + w1 * p1.z + w2 * p2.z;
        if (z < 0.0f || z > 1.0f) continue;   // 近/远裁剪面之外
        // ⚠ 深度索引按**超采样宽**算（不是 `width_`）：两者相差 `kBake` 倍，
        // 用错会把深度写入别的像素行——症状是“模型局部穿帮”，极难定位。
        const std::size_t offset = static_cast<std::size_t>(y) *
                                       static_cast<std::size_t>(bake_width) +
                                   static_cast<std::size_t>(x);
        if (z >= depth_[offset]) continue;    // z-buffer：比已画的更远 → 丢弃
        // ## 透明物体**不写深度**
        //
        // 不透明物体写（默认）；透明不写——否则近处的透明面会挡住后面
        // 本应透过它看到的东西，表现为“玻璃后面的物体全消失”。
        // （深度**测试**仍要做：透明面之间的前后关系靠它排。）
        if (material_.depth_write) depth_[offset] = z;
        // 实体遮罩：`set_planar_shadow` 之后画的网格都是“实体”，
        // 阴影不得盖在它们上面（地面在它之前画，不会被标记）。
        if (mask_entities_) entity_mask_[offset] = 1;

        // 透视校正：顶点属性按 1/w 加权插值（不做校正会让纹理/光照在斜面上"歪"）
        const float sum = w0 * p0.w + w1 * p1.w + w2 * p2.w;
        const float i0 = (sum > 1e-9f) ? (w0 * p0.w) / sum : w0;
        const float i1 = (sum > 1e-9f) ? (w1 * p1.w) / sum : w1;
        const float i2 = 1.0f - i0 - i1;
        const math::Vec3 normal = a.normal * i0 + b.normal * i1 + c.normal * i2;
        // 逐像素视向：`eye - world`。
        //
        // 固定方向近似时只有画面正中是对的，越往边缘偏差越大——掠射角高光不拉长，
        // 金属看起来像塑料。而这里插值出的世界位置本来就是逐像素的，只多一次减法。
        const math::Vec3 world = a.world * i0 + b.world * i1 + c.world * i2;
        const math::Vec3 pixel_view{eye_.x - world.x, eye_.y - world.y, eye_.z - world.z};
        math::Vec3 color = a.color * i0 + b.color * i1 + c.color * i2;
        // ## 材质基色（2026-10-24）
        //
        // 设了 `base_color` 就用它**取代**顶点色。这是“网格只描述形状、材质描述外观”
        // 的必要条件：同一份网格能用不同材质渲出不同颜色，不必重建网格。
        //
        // 与纹理的关系：纹理仍然**乘**在基色上（基色当“染色”，纹理提供图案）——
        // 这与 Blender 的 Base Color 接图片节点后仍可用 Mix 调色是同一思路。
        if (material_.base_color_set) color = material_.base_color;        // 有 UV 且已设纹理 → 采样（**双线性**，与纹理坐标一起透视校正）。
        //
        // 为何必须双线性：三维里一个纹理像素常被映射到多个屏幕像素的**斜向**区域，
        // 点采样会让图标边缘重新出现硬眍齿（把刚修好的抗锯齿又抵消掉）。
        if (use_texture_ && a.has_uv) {
          const float u = a.u * i0 + b.u * i1 + c.u * i2;
          const float v = a.v * i0 + b.v * i1 + c.v * i2;
          const math::Vec3 texel = sample_texture(u, v);
          // 纹理与顶点色**相乘**：顶点色充当“基础色/染色”，纹理提供图案。
          color = math::Vec3{color.x * texel.x, color.y * texel.y, color.z * texel.z};
        }
        const math::Vec3 lit =
            shade_surface(normal, color, pixel_view, lighting_, material_.metallic,
                          material_.roughness, material_.specular, material_.emission,
                          material_.specular_tint_set ? material_.specular_tint
                                                       : math::Vec3{1.0f, 1.0f, 1.0f},
                          world,
                          material_.emissive_color_set ? material_.emissive_color : color,
                          material_.emission_unlit);
        // ## 阴影贴图：把受光部分按遮挡因子压暗
        //
        // 为何乘在 `lit` 上而不是在着色公式里：阴影是**几何遮挡**，与材质/BRDF
        // 无关。乘在结果上是把所有光照项统一按遮挡比例衰减——这正是物理上的
        // “这部分表面看不到光源”。
        //
        // 自发光不受影响：`emission_unlit` 的物体（霓虹灯管）不该因为被挡住就变暗
        //（它自己发光，不需要外部光）。
        math::Vec3 shaded = lit;
        if (shadow_ready_ && !material_.emission_unlit) {
          const float occl = shadow_factor_at(world);
          if (occl > 0.0f) {
            const float keep = 1.0f - occl * std::clamp(shadow_map_.strength, 0.0f, 1.0f);
            shaded = math::Vec3{lit.x * keep, lit.y * keep, lit.z * keep};
          }
        }
        // ## 输出链：线性 → 曝光 → 色调映射 → sRGB → 8bit
        //
        // 修之前这里直接 `lit.x * 255`，两处错（都“看不出哪里错、只觉得不对”）：
        //   1. **线性值当 sRGB 存**：0.5 线性 ≈ 0.735 sRGB，整幅系统性偏暗；
        //   2. **先 clamp 再存**：HDR 被截断在 1，高光/日光糊成一片白。
        //
        // `shade_terms` 仍返回**线性值**（两条腿共用的公式不变），
        // 颜色变换集中在这里——顺序写错很难定位（看起来像“配色不对”）。
        if (opacity >= 0.999f) {
          canvas_.set_pixel(x, y, st::raster::encode_pixel(shaded, 1.0f, tone_map_));
        } else {
          // ## 透明混合必须在**线性空间**做（实测很重要）
          //
          // 在 sRGB 编码后混合是错的：sRGB 是非线性的，
          // 不透明度的物理意义是“光量比例”，只在��性域成立。
          // 不过这里有一层实际约束：画布存的是**已编码**的 8bit sRGB 值，
          // 拿不到“已画像素的线性值”除非重新解码。所以分两步：
          //   ① 解码已有像素回线性；
          //   ② 线性混合后再编码回去。
          // 成本比直接混 abgr 高，但正确；透明物体通常只占少部分像素。
          const math::Color dst_encoded = canvas_.pixel_at(x, y);
          const float inv = 1.0f / 255.0f;
          const math::Vec3 dst_linear =
              srgb_to_linear(math::Vec3{static_cast<float>(dst_encoded.r) * inv,
                                        static_cast<float>(dst_encoded.g) * inv,
                                        static_cast<float>(dst_encoded.b) * inv});
          const math::Vec3 mixed{dst_linear.x + (lit.x - dst_linear.x) * opacity,
                                 dst_linear.y + (lit.y - dst_linear.y) * opacity,
                                 dst_linear.z + (lit.z - dst_linear.z) * opacity};
          canvas_.set_pixel(x, y, st::raster::encode_pixel(mixed, 1.0f, tone_map_));
        }
      }
    }
  }

  /// 双线性采样纹理（UV 已 clamp 到 [0,1]）。
  [[nodiscard]] auto sample_texture(float u, float v) const noexcept -> math::Vec3 {
    // clamp：UV 超出 0..1 时取边缘色（比 repeat 安全——图标边缘不应循环）
    const float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    const float cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    // 纹理坐标→像素：减去半个像素才能让采样中心对得上像素中心
    const float fx = cu * static_cast<float>(texture_width_) - 0.5f;
    const float fy = cv * static_cast<float>(texture_height_) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto texel = [this](int x, int y) noexcept -> math::Vec3 {
      const int cx = x < 0 ? 0 : (x >= texture_width_ ? texture_width_ - 1 : x);
      const int cy = y < 0 ? 0 : (y >= texture_height_ ? texture_height_ - 1 : y);
      const std::size_t offset =
          (static_cast<std::size_t>(cy) * static_cast<std::size_t>(texture_width_) +
           static_cast<std::size_t>(cx)) *
          4U;
      // ## 纹理字节是 sRGB，必须解码到线性后才能参与光照
      //
      // `set_texture` 的契约是“RGBA8 非预乘”——那是 PNG/图片文件里的字节，
      // 也就是**显示编码**（sRGB）。直接 /255 当线性用会让纹理整体偏亮，
      // 且与顶点色（已改为解码）口径不一致。
      return srgb_to_linear(
          math::Vec3{static_cast<float>(texture_[offset + 0U]) / 255.0f,
                     static_cast<float>(texture_[offset + 1U]) / 255.0f,
                     static_cast<float>(texture_[offset + 2U]) / 255.0f});
    };
    const math::Vec3 top = texel(x0, y0) * (1.0f - tx) + texel(x0 + 1, y0) * tx;
    const math::Vec3 bottom = texel(x0, y0 + 1) * (1.0f - tx) + texel(x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
  }

  /// 超采样缓冲 → 逻辑分辨率（**均值 resolve**）。
  ///
  /// 为何要自己 average 而不是交给 `draw_canvas` 的双线性缩放：
  /// 画布内部是**预乘**色，而正确的降采样必须在预乘空间里平均（否则透明区
  /// 的边缘会发黑）。同时这里只用原分辨率画布，不给目标增加额外缩放误差。
  [[nodiscard]] auto resolve_to_logical() const -> Canvas {
    Canvas out{width_, height_, 1.0f};
    if (kBake == 1) {
      const std::span<std::uint32_t> pixels = out.pixels();
      const std::span<const std::uint32_t> source = canvas_.pixels();
      std::copy(source.begin(), source.end(), pixels.begin());
      return out;
    }
    const int bake_width = width_ * kBake;
    const int samples = kBake * kBake;
    const std::span<const std::uint32_t> source = canvas_.pixels();
    std::span<std::uint32_t> pixels = out.pixels();
    if (source.empty() || pixels.empty()) return out;
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        // 逐通道累加（预乘值：R/G/B 已乘 alpha，直接平均即正确的面积平均）
        std::uint32_t r = 0;
        std::uint32_t g = 0;
        std::uint32_t b = 0;
        std::uint32_t a = 0;
        for (int sy = 0; sy < kBake; ++sy) {
          const std::size_t row = static_cast<std::size_t>(y) * kBake + sy;
          const std::size_t base = row * static_cast<std::size_t>(bake_width) +
                                   static_cast<std::size_t>(x) * kBake;
          for (int sx = 0; sx < kBake; ++sx) {
            const std::uint32_t pixel = source[base + static_cast<std::size_t>(sx)];
            a += pixel & 0xFFU;
            b += (pixel >> 8U) & 0xFFU;
            g += (pixel >> 16U) & 0xFFU;
            r += (pixel >> 24U) & 0xFFU;
          }
        }
        // 四舍五入而不是截断：截断会让整体系统性偏暗（S²个样本各偏 0.5）
        const auto average = [samples](std::uint32_t sum) -> std::uint32_t {
          return (sum + static_cast<std::uint32_t>(samples) / 2U) /
                 static_cast<std::uint32_t>(samples);
        };
        const auto to_byte = [](std::uint32_t value) -> std::uint32_t {
          return value > 255U ? 255U : value;
        };
        pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
               static_cast<std::size_t>(x)] =
            (to_byte(average(r)) << 24U) | (to_byte(average(g)) << 16U) |
            (to_byte(average(b)) << 8U) | to_byte(average(a));
      }
    }
    return out;
  }

  /// 超采样倍数（每个逻辑像素每轴 `kBake` 个样本，共 `kBake²`）。
  ///
  /// 档位选择：2 = 4 样本（成本 4× 像素）。实测软件腿 720p 千面模型
  /// 光栅本身只要几十毫秒，而**轮廓锯齿**是成片上一眼可见的缺陷，这笔钱值得付。
  /// 3/4 可进一步平滑，但成本是 9/16 倍，且 2× 已经让阶梯基本看不出来。
  static constexpr int kBake = 3;
  int width_{1};
  int height_{1};
  Canvas canvas_;
  std::vector<float> depth_{};
  math::Mat4 view_{math::Mat4::identity()};
  math::Mat4 projection_{math::Mat4::identity()};
  /// 相机位置（算逐像素视向用）。
  ///
  /// GGX 的 Fresnel 依赖**视线与半角的夹角**，固定方向近似时只有画面正中是对的。
  math::Vec3 eye_{0.0f, 0.0f, 4.0f};
  bool mvp_ready_{false};
  Lighting lighting_{};
  /// 色调映射算子（从 `Lighting::tone_map` 同步过来）。
  ///
  /// 为何单独存一份：每帧重置为默认光照时（`begin_frame`）会重建 `lighting_`，
  /// 把它放在同一个结构里就只靠 `set_lighting` 传递，不会两处不同步。
  ToneMap tone_map_{ToneMap::Aces};
  /// 面朝向剔除（默认剔除背向面）。
  CullMode cull_mode_{CullMode::Back};
  /// 当前材质。
  Material material_{};
  /// 平面投影阴影（`strength = 0` = 关闭）。
  PlanarShadow shadow_{};
  /// 阴影三角形的世界坐标（每 9 个 float 一个三角形）。
  ///
  /// 为何先收集再画：阴影必须**在所有实体之后**绘制（它是叠加层），
  /// 而 `draw_mesh` 是按调用顺序来的。
  std::vector<float> shadow_tris_{};
  /// 实体遮罩（超采样分辨率）：1 = 该像素已被实体占据，阴影不能画上去。
  ///
  /// 为何需要它：阴影与地面共面，深度比较分不开“地面”与“实体”，
  /// 而语义上要排除的是**实体**。一个独立遮罩比调深度偏置可靠得多。
  std::vector<std::uint8_t> entity_mask_{};

  // ── 阴影贴图（shadow mapping，2026-10-24）──

  /// 世界空间三角形（阴影贴图的几何源）。
  struct ShadowTriangle {
    math::Vec3 v[3];
  };

  /// 延迟绘制的网格（阴影贴图开启时）。
  ///
  /// 存**网格指针 + 模型矩阵 + 材质快照**：网格指针的生命周期由调用方保证
  /// （`draw_mesh` 的契约就是“调用期间有效”，而延迟到 `end_frame` 仍在同一次
  /// 帧内，所以安全）。材质必须快照，否则后一个网格的 `set_material` 会
  /// 覆盖前一个的。
  struct DeferredMesh {
    const Mesh* mesh{nullptr};
    math::Mat4 model{math::Mat4::identity()};
    Material material{};
  };
  std::vector<DeferredMesh> deferred_{};
  std::vector<ShadowTriangle> shadow_geometry_{};
  /// 阴影贴图配置（`strength = 0` = 关闭）。
  ShadowMap shadow_map_{};
  /// 光源视角的深度图（`resolution²` 个 float；1.0 = 最远/空）。
  std::vector<float> shadow_depth_{};
  /// 光源视角的 view-projection（世界 → 阴影贴图 NDC）。
  math::Mat4 shadow_vp_{math::Mat4::identity()};
  /// 本帧是否已构好阴影贴图。
  bool shadow_ready_{false};
  /// 是否标记实体遮罩（`set_planar_shadow` 之后才为真）。
  bool mask_entities_{false};
  /// 透明三角形延迟队列：`begin_frame` 清、`draw_mesh` 收集、 `end_frame` 前排序并绘制。
  ///
  /// 为何要延迟：正确的透明混合要求**远→近**绘制，而 `draw_mesh` 是按调用
  /// 顺序提交的（与远近无关）。收集后统一排序是唯一能保证正确顺序的做法。
  struct TransparentTri {
    Vertex v[3];
    float depth{0.0f};   ///< 排序键（相机空间距离，越大越远）
  };
  std::vector<TransparentTri> transparent_{};
  /// 纹理（RGBA8 非预乘，行优先）：空 = 无纹理。
  std::vector<std::uint8_t> texture_{};
  int texture_width_{0};
  int texture_height_{0};
  /// 本网格是否正在采样纹理（无 UV / 无纹理时为 false）。
  bool use_texture_{false};
  std::uint64_t draw_calls_{0};
};

}  // namespace

/// GPU 三维腿（平台实现提供；非 Windows / 无显卡时返错误）。
/// 声明在这里而不在公共头：它返回 D3D 无关的 `Scene3D`，**调用方不需要知道
/// 它背后是哪条腿**——这正是「平台中立接口」的含义。
[[nodiscard]] auto gpu_scene3d_available() -> bool;
[[nodiscard]] auto create_gpu_scene3d(int width, int height)
    -> Result<std::unique_ptr<Scene3D>>;

/// 建一个软件三维场景（始终可用）。
[[nodiscard]] auto create_software_scene3d(int width, int height)
    -> Result<std::unique_ptr<Scene3D>> {
  if (width <= 0 || height <= 0) return unexpected(ErrorCode::Invalid, "渲染尺寸必须为正");
  return std::unique_ptr<Scene3D>(std::make_unique<SoftwareScene>(width, height));
}

}  // namespace st::raster

// ————————————————————————————————————————————————————————————————————————————
// 两条腿的选择（**唯一的决策点**）
// ————————————————————————————————————————————————————————————————————————————
//
// 为何不直接“有 GPU 就用 GPU”：小尺寸（界面里一块 960×200 的卡片）下，GPU 腿
// 每帧多出一次纹理回读 + 上传（`end_frame` 要把结果交回一张 `Surface`），
// 而软件腿在那个面积下只逐个像素算被三角形覆盖的那一小块。二维已经踩过
// 这个坑（`backend_util.cpp` 的 `create_surface` 因此是**实测取快的那条**，
// 而不是硬编码 GPU 优先）。三维沿用同一条立场。
//
// 实测的代价：两条腿各建一次、各画一帧。三维场景建设（深度纹理 + 网格上传）
// 是毫秒级，而 `Scene3D` 的生命周期通常是整部片子（不是每帧），所以只付一次。
namespace st::raster {
namespace {

/// 用同一网格 + 同一相机量一次单帧耗时（纳秒，best-of-N）。
[[nodiscard]] auto probe_leg_ms(Scene3D& scene) -> double {
  Canvas canvas{scene.width(), scene.height(), 1.0f};
  Camera camera;
  camera.eye = math::Vec3{0.0f, 0.0f, 3.0f};
  camera.target = math::Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;
  const Mesh mesh = Mesh::sphere(0.9f, 24);   // ~1000 面：与真实产品模型的量级相当
  const math::Rect destination{0.0f, 0.0f, static_cast<float>(scene.width()),
                               static_cast<float>(scene.height())};
  const auto run = [&] {
    scene.begin_frame(math::Color{0, 0, 0, 255});
    scene.set_camera(camera);
    scene.draw_mesh(mesh, math::Mat4::identity());
    scene.end_frame(canvas, destination);
  };
  run();   // 预热：第一次要建着色器/上传网格/分配暂存纹理，那不算稳态成本
  double best = 1e18;
  for (int round = 0; round < 5; ++round) {
    const std::int64_t start = st::time::now_ns();
    run();
    const double ns = static_cast<double>(st::time::now_ns() - start);
    if (ns < best) best = ns;
  }
  return best / 1.0e6;
}

}  // namespace

auto Scene3D::available() noexcept -> bool { return true; }   // 软件腿：永远可用

auto Scene3D::backend_mutex() -> std::mutex& {
  // 进程内唯一：函数内 static，与 `available()` 一样是「整个进程共享一个」的语义。
  //
  // 这里**必须**是进程级可变状态：互斥锁的作用就是让多个线程共享同一把锁，
  // 注入 Context 无处可注（调用方是整个进程，且它恰好就是用来保护
  // `ST_SCENE3D_BACKEND` 这个进程环境变量的）。
  static std::mutex mutex;   // lint-allow: L8 进程级互斥本身就是共享状态，正是它的用途
  return mutex;
}

auto Scene3D::create(int width, int height) -> Result<std::unique_ptr<Scene3D>> {
  if (width <= 0 || height <= 0) return unexpected(ErrorCode::Invalid, "渲染尺寸必须为正");
  // 强制指定腿：供探针做对照与出问题时的逃生门（与 `ST_GPU_FORCE_WARP` 同一套惯例）。
  // 值：`gpu` / `software`；未设或非法值 = 走实测决策。
  if (const auto forced = st::fs::read_env("ST_SCENE3D_BACKEND"); forced.has_value()) {
    if (*forced == "gpu") {
      if (auto gpu = create_gpu_scene3d(width, height); gpu.has_value()) return gpu;
      if (auto software = create_software_scene3d(width, height); software.has_value()) {
        return software;
      }
    } else if (*forced == "software") {
      return create_software_scene3d(width, height);
    }
  }
  // 先试 GPU（只要平台实现了这条腿且着色器真建起来了）
  if (gpu_scene3d_available()) {
    auto gpu = create_gpu_scene3d(width, height);
    if (gpu.has_value()) {
      // **实测取快的那条**：两条腿各画一帧，软件明显更快就用软件。
      //
      // 为何不硬编码“有显卡就用显卡”：GPU 腿每帧多出一次**纹理回读 + 上传**
      // （`end_frame` 要把结果交回一张 `Surface`），而软件腿在那个面积下只逐像素
      // 算被三角形覆盖的那一小块。小面积（界面里一块卡片）上后者可能真更快，
      // 二维已经踩过这个坑（`backend_util.cpp` 的 `create_surface` 因此也是实测）。
      //
      // 阈值 1.25：GPU 略快但差不多时不折腾（见上方隐藏成本）。反过来说，
      // **正是这一条让“界面小卡片”自然继续走软件腿，而裁云的半屏/全屏三维
      // 走 GPU 腿**——不需要调用方写任何分支。
      if (auto software = create_software_scene3d(width, height); software.has_value()) {
        const double gpu_ms = probe_leg_ms(**gpu);
        const double software_ms = probe_leg_ms(**software);
        if (software_ms > 0.0 && gpu_ms > software_ms * 1.25) {
          ST_LOG_INFO("三维：{}×{} 实测软件 {:.2f} ms vs GPU {:.2f} ms → 选软件腿", width,
                      height, software_ms, gpu_ms);
          return software;
        }
        ST_LOG_DEBUG("三维：{}×{} 实测软件 {:.2f} ms vs GPU {:.2f} ms → 选 GPU 腿", width,
                     height, software_ms, gpu_ms);
      }
      return gpu;
    }
    // GPU 腿建不出来（着色器/深度目标失败）：如实记原因再落软件
    ST_LOG_WARN("三维：GPU 腿不可用（{}），已回退软件光栅器", gpu.error().message);
  }
  return create_software_scene3d(width, height);
}

}  // namespace st::raster
