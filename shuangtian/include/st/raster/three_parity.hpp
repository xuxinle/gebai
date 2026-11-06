/// @file three_parity.hpp
/// @brief **与 three.js 逐式同构的着色**：让「three.js 脚本 → 像素」与浏览器里的
///        three.js 渲染器走同一套公式。
///
/// ## 为什么需要单独一层（而不是改 `shade_surface`）
///
/// `shade_surface` 实现的是本框架自己的模型：
///
///     lit = albedo × (ambient + n·l·diffuse + rim) + 高光 + 环境反射
///
/// 它有意省掉了几样东西（省得对、看起来才"稳"），但对标浏览器时它们全是**硬差**：
///
/// | 项 | 本框架 | three.js（Physical） | 0.5 灰面实测差 |
/// |---|---|---|---|
/// | 漫反射归一化 | `albedo × diffuse` | `albedo/π × irradiance` | **+100/255** |
/// | 常量环境保底 | `ambient = 0.35` 恒在 | 只有显式 `AmbientLight` 才有 | 背光面 0 → 84 |
/// | 边缘光 | `rim = 0.25` 加算 | 不存在这个概念 | 轮廓 +25% |
/// | 环境反射 | 从 `scene.background` **推导** | 只有 `envMap`/`scene.environment` 才有 | 金属凭空多一层 |
/// | G 项 | Schlick-GGX（`k=(α+1)²/8`） | `V_GGX_SmithCorrelated` | 高光形状不同 |
/// | F 项 | `(1-v·h)^5` | `exp2((-5.55473v·h-6.98316)v·h)` | 掠射角不同 |
///
/// 与其把 `shade_surface` 改成"又像又不是"，不如**另立一条明确同构的路**：
/// 它只承诺一件事——与 three.js r160 的 `MeshStandardMaterial`/`MeshPhysicalMaterial`
/// 直接光 + 环境光 + 自发光三部分是同一组公式，逐项可对照。
///
/// ## 口径（全部取自 `third_party/three/three.cjs` 的 ShaderChunk，逐字）
///
/// ```
/// diffuseColor   = color × (1 − metalness)
/// specularColor  = mix(vec3(0.04), color, metalness)          // = F0
/// alpha          = max(roughness, 0.0525)²                     // + geometryRoughness（见下）
/// D = a²/(π·((n·h)²(a²−1)+1)²)
/// V = 0.5 / max(n·l·√(a²+(1−a²)(n·v)²) + n·v·√(a²+(1−a²)(n·l)²), ε)
/// F = f0·(1−Fc) + F90·Fc,  Fc = exp2((−5.55473·v·h − 6.98316)·v·h)
/// BRDF_GGX       = D·V·F            （V 里已含 1/(4·n·l·n·v)）
/// irradiance     = saturate(n·l) × lightColor
/// directSpecular += irradiance · BRDF_GGX
/// directDiffuse  += irradiance · diffuseColor/π
/// indirectDiffuse+= ambientIrradiance · diffuseColor/π
/// outgoing        = saturate(directDiffuse + directSpecular + indirectDiffuse) + emissive
/// ```
///
/// ## 已知残余（写在这里而不是掩盖）
///
/// 1. **`geometryRoughness`**：three 用屏幕空间导数
///    `max(|dFdx(n)|, |dFdy(n)|)` 的最大分量补偿面片法线，本实现取 0。
///    平面与高细分曲面几乎无差；低模的硬边面片角上会比 three **更锐**。
/// 2. **`envMap`/PMREM**：本框架没有环境贴图管线。three 脚本若设了
///    `scene.environment`，本实现**不会**产生对应的间接高光（差异必然很大）。
///    纯色 `scene.background` 在 three 里**不产生 IBL**——这一点本实现与它一致，
///    而这正是旧路径"凭空多一层环境反射"的来源。
/// 3. **阴影**：three 的 shadow 是 PCF 采样；本框架是平面投影阴影，语义不同。
///
/// ## 与两条腿的关系
///
/// 本层的公式是**纯函数**（无状态），所以软件腿与 D3D11 腿可以各自逐字翻译，
/// 一致性由 `raster_three_parity_test` 逐项断言（见 `docs/render_alignment.md`）。

#pragma once

#include "st/math/color.hpp"
#include "st/math/matrix.hpp"

namespace st::raster {

/// three.js 的 GGX 分布项 `D`（逐字）。
[[nodiscard]] auto three_d_ggx(float alpha, float n_dot_h) noexcept -> float;

/// three.js 的可见性项 `V_GGX_SmithCorrelated`（逐字；已含 1/(4·n·l·n·v)）。
[[nodiscard]] auto three_v_ggx(float alpha, float n_dot_l, float n_dot_v) noexcept -> float;

/// three.js 的菲涅耳 `F_Schlick`（逐字：用 `exp2` 的二次拟合，不是 `(1-cos)^5`）。
[[nodiscard]] auto three_f_schlick(math::Vec3 f0, float f90, float v_dot_h) noexcept
    -> math::Vec3;

/// three.js 的 `alpha`：`max(roughness, 0.0525)²`。
///
/// 为何不是 `roughness²`：three 在 `lights_physical_fragment` 里先夹到 0.0525
/// 再平方。少了这一步，`roughness = 0` 的镜面会退化成 delta 函数（本框架用
/// `1e-3` 兜底，观感上是"高光小一圈"）。
[[nodiscard]] auto three_alpha(float roughness) noexcept -> float;

/// 单个平行光的直接光照贡献（`RE_Direct_Physical` 的等价式）。
///
/// 返回**线性 HDR** 值，不含曝光与色调映射（那属于输出链）。
[[nodiscard]] auto three_direct_light(math::Vec3 normal, math::Vec3 view_dir,
                                      math::Vec3 light_dir, math::Vec3 light_color,
                                      float light_intensity, math::Vec3 base_color,
                                      float metallic, float roughness) noexcept -> math::Vec3;

/// 环境光（`AmbientLight`）的贡献：`irradiance × diffuseColor/π`。
///
/// ## 为何这里也要除以 π
///
/// three 的 `getAmbientLightIrradiance` 直接把 `ambientLightColor` 当 irradiance，
/// 然后走 `RE_IndirectDiffuse_Physical` → `BRDF_Lambert` = `diffuseColor/π`。
/// 所以"环境光强度 1.0"的贡献是 `albedo/π`，不是 `albedo`——这正是本框架
/// `ambient = 0.35` 那类常量保底偏亮 3.14 倍的根因。
[[nodiscard]] auto three_ambient_light(math::Vec3 base_color, float metallic,
                                       math::Vec3 ambient_irradiance) noexcept -> math::Vec3;

}  // namespace st::raster
