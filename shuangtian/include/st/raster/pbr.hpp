/// @file pbr.hpp
/// @brief **本框架唯一的着色内核**：基于物理的 BRDF，与 three.js r160 的
///        `MeshStandardMaterial`/`MeshPhysicalMaterial` 逐式同构。
///
/// ## 为什么不叫 `three_parity`（改名的理由）
///
/// 这一层最初是为了"与浏览器像素级对标"而建的，所以叫 `three_parity`。
/// 但它其实是**三条输入路径共用的唯一内核**：
///
/// | 输入 | 谁负责翻译 | 内核做什么 |
/// |---|---|---|
/// | three.js 脚本 | `ThreeRuntime` 抽 `Scene`/`Mesh`/`Light` | 只认公式，不问来源 |
/// | bpy 脚本 | `BpySceneExtractor`（Blender 语义 → 本框架语义） | 同上 |
/// | JSON 剧本 | `scene3d_script`（声明式 → 结构体） | 同上 |
///
/// 名字里带 `three` 会让人误以为"只有 three 路径走它"，而那正是本仓反复
/// 出现过的错误（两条腿、三条路径各自长一副样子）。改叫 `pbr` 之后，
/// **"内核只有一套公式"这件事在命名上就是显然的**。
///
/// ## 为什么内核必须是 three 的公式（而不是 Blender 的）
///
/// 两者数学上是同一族（Cook-Torrance + GGX + Smith + Schlick），差别只在
/// **细节近似**：
///
/// | 项 | three.js r160 | Blender 4.x EEVEE | 谁是"标准" |
/// |---|---|---|---|
/// | 遮蔽函数 | Smith **height-correlated** 的解析形 `V` | Smith 分离形 | 两者都是文献标准，数值差 <2% |
/// | 菲涅耳 | `exp2` 二次拟合（Spherical Gaussian） | Schlick `(1-c)^5` | 差 <1%，掠射角略不同 |
/// | 粗糙度下限 | `max(r, 0.0525) + geometryRoughness` | `max(r, 0.001)` | three 更保守 |
/// | 漫反射 | Lambert `albedo/π` | Lambert `albedo/π` | **相同** |
/// | 能量守恒 | 单次散射 | 单次散射 + 部分多次散射补偿 | Blender 更精细 |
///
/// 结论：**选 three 的公式是因为它能被逐像素验证**（浏览器真值通道可复算），
/// 而 Blender 的完整差异要么在兼容层抽取不到（transmission/coat/sheen），
/// 要么需要 OCIO + LUT（AgX）。Blender 侧的差异由 `docs/render_parity.md`
/// 的量化对照记录在案，不假装相同。
///
/// ## 公式（逐字取自 `third_party/three/three.cjs` 的 ShaderChunk）
///
/// ```
/// diffuseColor   = color × (1 − metalness)
/// F0             = mix(vec3(0.04), color, metalness)
/// alpha          = max(roughness, 0.0525)²          // + geometryRoughness（本实现取 0）
/// D = a²/(π·((n·h)²(a²−1)+1)²)
/// V = 0.5 / max(n·l·√(a²+(1−a²)(n·v)²) + n·v·√(a²+(1−a²)(n·l)²), ε)
/// F = F0·(1−Fc) + F90·Fc,   Fc = exp2((−5.55473·v·h − 6.98316)·v·h)
///
/// 直接光：irradiance = saturate(n·l) × lightColor × intensity × attenuation
///         directDiffuse  += irradiance · diffuseColor/π
///         directSpecular += irradiance · D·V·F
/// 环境光：indirectDiffuse += ambientIrradiance · diffuseColor/π
/// IBL    ：indirectDiffuse += iblDiffuse · diffuseColor/π
///         indirectSpecular += iblSpecular(roughness) · (F0·A + B)     // 见 §IBL
/// 自发光：outgoing = saturate(上面之和) + emissive
/// ```
///
/// ## 灯光类型（三种都走同一个 BRDF，只差 `irradiance` 的算法）
///
/// | 类型 | 方向 | 强度 |
/// |---|---|---|
/// | 平行光 | 固定 | `intensity`（无衰减） |
/// | 点光 | `normalize(position − p)` | `intensity / max(d, ε)²` |
/// | 聚光 | 同点光 | 点光 × 锥形衰减（内锥→外锥平滑过渡） |
///
/// 这是 bpy 兼容层的前提：`extract.py` 会导出 `SUN`/`POINT`/`SPOT` 三种，
/// 内核必须都能算，而旧 Legacy 模型是通过"点光/聚光当成附加漫反射项"糊过去的。
///
/// ## IBL（环境光照）——本框架的"环境"是**方向探针**，不是 cubemap
///
/// three 需要一张 PMREM 环境贴图（几十 MB，还要 mip 链与预计算）。本框架的
/// 渲染对象是产品演示/信息图，要的是"上亮下暗 + 有方向结构"这个**型制**。
///
/// 所以 IBL 用 N 个方向探针（方向 + 颜色）表达，运行时逐像素算：
///
/// ```
/// 漫反射：E(n) = Σ_i w_i · c_i · max(0, dot(n, dir_i)) / Σ w_i     // 余弦加权
/// 高光  ：R = reflect(−v, n)
///         S(R) = Σ_i w_i · c_i · max(0, dot(R, dir_i))^sharpness(roughness) / Σ w_i
/// ```
///
/// `sharpness` 随 roughness 下降（光滑面看到锐利的方向结构，粗糙面看到均值），
/// 这是对 PMREM mip 链的**解析近似**——代价是"粗糙面的高光不够模糊连续"，
/// 收益是不需要贴图与预计算，两条腿（软件/GPU）能算出逐像素相同的结果。
///
/// ## 已知残余（写在这里而不是掩盖）
///
/// 1. **`geometryRoughness`**：three 用屏幕空间导数 `max(|dFdx(n)|, |dFdy(n)|)`
///    补偿面片法线，本实现取 0。低模硬边面片角上会比 three 更锐。
/// 2. **`envMap` 精度**：three 的 PMREM 有完整 mip 链；本实现是解析近似
///    （见上），粗糙度高时高光分布形状不同。
/// 3. **阴影**：three 是 PCF 采样；本框架是平面投影阴影，语义不同。
/// 4. **AgX**：three 是 3D LUT；本框架是解析拟合（见 `color_pipeline.hpp`）。
///
/// ## 与两条腿的关系
///
/// 本层的函数是**纯函数**（无状态、无全局），所以软件腿与 D3D11 腿各自逐字
/// 翻译同一组公式，一致性由 `raster_pbr_test` 逐项断言。

#pragma once

#include <utility>

#include "st/math/color.hpp"
#include "st/math/matrix.hpp"

namespace st::raster {

/// 圆周率的倒数（three 的 `RECIPROCAL_PI`）。
inline constexpr float kReciprocalPi = 0.3183098861837907f;

/// GGX 法线分布项 `D`（three `D_GGX` 逐字）。
[[nodiscard]] auto pbr_d_ggx(float alpha, float n_dot_h) noexcept -> float;

/// GGX 可见性项 `V`（three `V_GGX_SmithCorrelated` 逐字；**已含** 1/(4·n·l·n·v)）。
[[nodiscard]] auto pbr_v_ggx(float alpha, float n_dot_l, float n_dot_v) noexcept -> float;

/// 菲涅耳 `F`（three `F_Schlick` 逐字：`exp2` 二次拟合，不是 `(1−c)^5`）。
[[nodiscard]] auto pbr_f_schlick(math::Vec3 f0, float f90, float v_dot_h) noexcept -> math::Vec3;

/// 粗糙度 → GGX 的 `alpha`：`max(roughness, 0.0525)²`。
///
/// 为何先夹到 0.0525：three 在 `lights_physical_fragment` 里这么做，避免
/// `roughness = 0` 时 `D` 退化成 delta 函数（数值上是 inf）。
[[nodiscard]] auto pbr_alpha(float roughness) noexcept -> float;

/// 一个光源在某个表面点上的**入射辐照度**（已含方向、颜色、强度、衰减）。
///
/// 三种灯光类型都折成这一个结构：BRDF 只看 `direction`/`irradiance`，
/// 所以**新增灯光类型不需要改 BRDF**——这是"内核统一"的关键设计。
struct PbrLight {
  /// 从表面指向光源（单位向量）。
  math::Vec3 direction{0.0f, 1.0f, 0.0f};
  /// 辐照度（线性 HDR）。**未乘** `saturate(n·l)`（那在 BRDF 里乘）。
  math::Vec3 irradiance{0.0f, 0.0f, 0.0f};
};

/// 把「平行光 / 点光 / 聚光」统一折算成一个 `PbrLight`。
///
/// `kind` 与 `LightKind` 枚举同值（0=Directional 1=Point 2=Spot）。
///
/// ## 为何把三种类型收在一个函数里
///
/// Legacy 模型是"主光单独一套 + 点光/聚光当附加漫反射项"，于是**两种灯光
/// 走两条公式**，实测出现过"点光在没设材质的场景里完全不生效"。统一成
/// `PbrLight` 之后，BRDF 只有一个入口，新增灯光类型只改这里。
[[nodiscard]] auto pbr_sample_light(int kind, math::Vec3 direction_toward_light,
                                    math::Vec3 position, math::Vec3 color, float intensity,
                                    float range, float spot_inner_degrees,
                                    float spot_outer_degrees, math::Vec3 surface_position)
    -> PbrLight;

/// 直接光贡献（`RE_Direct_Physical` 的等价式）。返回**线性 HDR**。
///
/// `PbrLight::irradiance` **不含** `n·l`；本函数内部乘 `saturate(n·l)`，
/// 与 three 的 `vec3 irradiance = dotNL * directLight.color` 一致。
[[nodiscard]] auto pbr_direct_light(math::Vec3 normal, math::Vec3 view_dir,
                                    const PbrLight& light, math::Vec3 base_color,
                                    float metallic, float roughness) noexcept -> math::Vec3;

/// 环境光（Blender 的 `world` 均匀发光 / three 的 `AmbientLight`）的漫反射贡献。
///
/// 走 `irradiance × diffuseColor/π`——**不是** `albedo × ambient`。
/// 后者（旧 Legacy 的做法）偏亮 π 倍，实测让 0.5 灰面从 76 变成 84+。
[[nodiscard]] auto pbr_ambient_diffuse(math::Vec3 base_color, float metallic,
                                       math::Vec3 ambient_irradiance) noexcept -> math::Vec3;

/// IBL 漫反射：方向探针的余弦加权辐照度（见头文件 §IBL）。
///
/// `probe_dirs`/`probe_colors` 是等长数组，`count` 为有效个数（0 = 无 IBL）。
/// 返回的是**辐照度**（已归一），调用方再乘 `diffuseColor/π`。
[[nodiscard]] auto pbr_ibl_diffuse(math::Vec3 normal, const math::Vec3* probe_dirs,
                                   const math::Vec3* probe_colors, int count) noexcept
    -> math::Vec3;

/// IBL 高光：用反射方向采样探针，`sharpness` 随 roughness 下降。
///
/// 返回的是**辐亮度**（radiance），要乘 `(F0·A + B)` 才进入着色方程。
[[nodiscard]] auto pbr_ibl_specular(math::Vec3 normal, math::Vec3 view_dir, float roughness,
                                    const math::Vec3* probe_dirs,
                                    const math::Vec3* probe_colors, int count) noexcept
    -> math::Vec3;

/// 环境 BRDF 的两个标量 `(A, B)`：`F0·A + B` 就是 IBL 镜面反射的强度因子。
///
/// 这是 split-sum 的解析拟合（Lazarov/UE4 同族）。本框架用解析式而不是
/// 2D LUT 的硬理由：两条腿必须算出**逐像素相同**的数，而 LUT 采样会引入
/// 过滤方式（双线性/最近）的分歧。
[[nodiscard]] auto pbr_env_brdf(float roughness, float n_dot_v) noexcept
    -> std::pair<float, float>;

/// **完整的一次着色**（内核的唯一对外入口）。
///
/// 之所以提供总装函数而不是让调用方自己拼：着色顺序有硬约束
/// （自发光要加在截断之前、IBL 的两个分量要分开、曝光在最后），
/// 拼错不会报错，只会"看起来不对"。
struct ShadeInput {
  math::Vec3 normal{0.0f, 0.0f, 1.0f};
  math::Vec3 base_color{1.0f, 1.0f, 1.0f};
  math::Vec3 view_dir{0.0f, 0.0f, 1.0f};   ///< 从表面指向相机（未归一化也可）
  math::Vec3 world_position{0.0f, 0.0f, 0.0f};
  float metallic{0.0f};
  float roughness{1.0f};
  /// 主光（`pbr_sample_light` 的结果）。
  PbrLight key_light{};
  /// **额外灯光**（点光 / 聚光 / 第二三盏平行光）。
  ///
  /// `nullptr` = 只有主光（旧调用方完全兼容）。
  ///
  /// ## 为何必须是数组，而不是“主光就够了”
  ///
  /// bpy 兼容层会导出**全部**灯（`bpy_watch.py` 就有 key/fill/rim 三盏），
  /// three 脚本也常写多盏。只算主光就会把其余灯丢掉——而“丢掉且不报错”
  /// 正是本仓最贵的那类缺陷（画面看着对，只是“不够立体”，既难定位也难归因）。
  const PbrLight* extra_lights{nullptr};
  int extra_light_count{0};
  /// 环境光（均匀）：Blender 的 world / three 的 AmbientLight。
  math::Vec3 ambient_irradiance{0.0f, 0.0f, 0.0f};
  /// IBL 方向探针（`nullptr` = 无 IBL）。
  const math::Vec3* ibl_dirs{nullptr};
  const math::Vec3* ibl_colors{nullptr};
  int ibl_count{0};
  /// IBL 强度倍数（0 = 关闭）。
  float ibl_strength{0.0f};
  /// **缝隙遮蔽**（ambient occlusion，0~1；1 = 无遮蔽）。
  ///
  /// ## 它乘在哪里（这个定义很重要）
  ///
  /// **只乘 IBL 与环境光的入射辐照度**，不乘直接光：
  ///
  /// ```
  /// indirectDiffuse  = occlusion * (ambient + iblDiffuse) * diffuseColor/π
  /// indirectSpecular = occlusion * iblSpecular * (F0·A + B)
  /// directDiffuse    = （不受 occlusion 影响）
  /// ```
  ///
  /// 为何不乘直接光：直接光**有它自己的**投影阴影路径（`ShadowMap` /
  /// `PlanarShadow`）。两套遮蔽相乘没有物理意义，而且会让暗部黑得不真实。
  ///
  /// ## 它能替代 GI 吗（不能，但能补上哪一部分）
  ///
  /// Cycles 的路径追踪会算「光线在沟壑/接触处反复弹射衰减」——那是**真 GI**。
  /// AO 是它的便宜近似：只看“半球里被挡了多少”，不反弹也不传色。
  ///
  /// | 项 | 真 GI（Cycles） | AO |
  /// |---|---|---|
  /// | 沟壑/接触处变暗 | ✅ | ✅（主要价值）|
  /// | 物体间色彩传递 | ✅ | ❌ |
  /// | 大范围反弹光 | ✅ | ❌ |
  /// | 算力 | 几十倍 | 几乎为零 |
  ///
  /// 所以开了 AO 之后**暗部会明显接近** Cycles，但永远不会逐像素相同。
  /// 像素级基准（受控平面 / 球面）**不开 AO**。
  float occlusion{1.0f};
  /// 自发光（已含强度）。
  math::Vec3 emissive{0.0f, 0.0f, 0.0f};
  /// 曝光（乘在最后，在色调映射之前）。
  float exposure{1.0f};
};

/// 着色：返回**线性 HDR**（未色调映射）。输出链由调用方负责。
[[nodiscard]] auto pbr_shade(const ShadeInput& in) noexcept -> math::Vec3;

}  // namespace st::raster

