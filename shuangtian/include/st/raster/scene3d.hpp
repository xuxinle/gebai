#pragma once

/// 三维场景：**平台中立的接口**，由当前平台的最佳图形 API 实现。
///
/// 为什么接口在这里，而不是在某个具体后端里（例如原先的 `raster::gl`）：
/// "把三维画进一块区域"是框架能力，**与用哪个 API 无关**。它原先定义在 `gl.hpp` 里，
/// 于是这个能力被绑死在 OpenGL 上——换后端要重写调用方，非 Windows 平台直接没有。
/// 现在：接口中立、实现按平台选（Windows 用 D3D11），调用方（`ui::GlView`）不感知后端。
///
/// 与 2D 的关系：这不是"另一个 2D 后端"，而是**补上 2D 做不到的事**
/// （透视投影、深度缓冲、逐像素光照、任意网格）。2D 界面仍走 `Surface`；
/// 需要真三维的地方调这里，渲染结果再合成回画布。

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/color_pipeline.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {

class Surface;

/// 面朝向剔除模式。
///
/// ## 为何需要（实测）
///
/// 不加剔除时，闭合物体的**背向面也会画**。对不透明封闭体它们会被深度测试
/// 挡掉（结果碰巧正确），但两种场合会直接穿帮：
/// - 相机钻进物体内部时，看到的是内壁（本应一片黑）；
/// - **透明物体**：背面不剔除时，后面与前面两个面都会被混合，
///   颜色叠加两次——“玻璃变浓”就是这么来的。
///
/// 默认值 `Back` 与 Blender/游戏引擎一致。
/// 单面物体（如 `Mesh::plane`）从背面看会消失，这也是标准行为。
enum class CullMode : std::uint8_t {
  None = 0,   ///< 不剔除（双面可见；给“薄片双面”场景用）
  Back = 1,   ///< 剔除背向面（**默认**，封闭物体的正确行为）
  Front = 2,  ///< 剔除正向面（看内壁时用；不常见）
};

/// 材质参数。与 `set_texture` 同一设计：**材质是场景的一个状态**，
/// 不是网格的一部分（原因见 `set_texture` 的说明——换材质不应重建网格缓存）。
struct Material {
  /// 不透明度（1 = 完全不透明，0 = 完全透明）。
  ///
  /// 小于 1 时，网格的三角形会被推到**透明队列**：
  /// 先画完全部不透明物体，再按“远→近”排序后混合。
  /// 这是在没有完整 BSP 排序的前提下能做到的正确做法。
  float opacity{1.0f};
  /// 是否写入深度。透明物体必须关（否则后面的透明面会被它挡住）。
  /// 一般不需要手动改：`opacity < 1` 时自动为 false。
  bool depth_write{true};

  // ── Principled 材质（对标 Blender Principled BSDF 的子集）──
  //
  // ## 为何是这五个参数
  //
  // Blender 的 Principled BSDF 有二十多个输入，但其中真正**决定材质类别**的是：
  //   metallic（金属/非金属）、roughness（光滑/粗糙）、
  //   base_color（固有色）、emission（自发光）、opacity（透明）。
  // 这五个就能把塑料 / 金属 / 陶瓷 / 橡胶 / 发光体分开，其余（sheen、
  // clearcoat、subsurface）属于细节修饰，等有真实需求再加。
  //
  // ## 默认值 = 现有外观（渐进采用的底线）
  //
  // `metallic=0` + `roughness=1` + `specular=0` 时，光照公式退化为
  // 原来的「Lambert + 环境 + 边缘光」——老场景逐像素不变。

  /// 金属度（0 = 绝缘体如塑料/陶瓷，1 = 纯金属如金/铁）。
  ///
  /// 物理含义：金属**没有漫反射**（自由电子吸收透射光），
  /// 反射光被固有色染色；非金属有漫反射，反射光保持白色。
  float metallic{0.0f};
  /// 粗糙度（0 = 镜面，1 = 完全漫反射）。
  ///
  /// 直接作为 GGX 的 `alpha` 用（Blender 也是这么做的：
  /// `rbsdf->alpha_x = roughness`，不再平方）。
  float roughness{1.0f};
  /// 高光强度（0 = 无高光，1 = 物理量）。
  ///
  /// 默认为 0 而不是 1：高光是**新增能力**，默认开启会改变所有已出片子的外观。
  /// 要用时显式设 1。
  float specular{0.0f};
  /// 自发光强度（0 = 不自发光）。
  ///
  /// 乘在固有色上：`emission=2` 表示“以两倍固有色亮度自发光”，
  /// 超过 1 时会被色调映射滚降（这正是 ACES 存在的意义）。
  float emission{0.0f};
  /// 高光颜色（非金属时通常为白；金属时应当跟随固有色）。
  /// 留空（默认）时按 metallic 自动选：金属用固有色，非金属用白。
  math::Vec3 specular_tint{0.0f, 0.0f, 0.0f};
  /// `specular_tint` 是否已显式设定（全零 + 本标记 = 自动）。
  bool specular_tint_set{false};

  // ── 固有色与自发光色（2026-10-24，对齐 Blender Principled 的 Base/Emissive）──
  //
  // ## 为什么必须有（此前是个硬限制）
  //
  // 之前颜色**只能来自网格顶点色**：同一份 `Mesh` 想换颜色就得重建网格，
  // 且多个物体共用同一份网格时无法各自上色。
  //
  // 有了 `base_color` 之后：网格只描述**形状与 UV**，颜色由材质决定。
  // 这与 Blender 的行为一致（物体数据 vs 材质数据分离），也让
  // 「同一个盒子演示十种材质」零成本——不用生成十份网格。
  //
  // ## 默认值怎么定的（渐进采用）
  //
  // `base_color_set = false` 时**完全沿用顶点色**（老场景逐像素不变）。
  // 与 `specular` 默认 0 同一个理由：新能力默认不改变既有外观。

  /// 固有色（线性空间）。仅在 `base_color_set` 为真时生效。
  math::Vec3 base_color{1.0f, 1.0f, 1.0f};
  /// `base_color` 是否已显式设定；false = 用网格顶点色。
  bool base_color_set{false};

  /// 自发光颜色（线性空间）。仅在 `emissive_color_set` 为真时生效。
  math::Vec3 emissive_color{1.0f, 1.0f, 1.0f};
  /// `emissive_color` 是否已显式设定；false = 用固有色（旧行为）。
  bool emissive_color_set{false};

  /// 自发光是否**不受光照影响**（true = 纯发光体，如霓虹灯管/屏幕）。
  ///
  /// Blender 的 Emission 项本身不受光照影响。旧实现里 `emission` 乘的是
  /// 固有色、且与光照项相加，语义更接近“被打亮的表面”。
  /// 这个开关让两种都能表达。
  bool emission_unlit{false};
};

/// 相机投影类型。
///
/// 放在 `raster` 而不是 `math`：`Mat4` 只关心矩阵形状（已经有了两个构造函数），
/// “相机用哪种”是场景语义。
enum class ProjectionKind : std::uint8_t {
  Perspective = 0,   ///< 透视：近大远小（默认，历史行为）。
  Orthographic = 1,  ///< 正交：远近同尺寸（工程图 / 等距展示）。
};

/// 相机（右手系，视线朝 -Z）。
struct Camera {
  math::Vec3 eye{0.0f, 0.0f, 4.0f};
  math::Vec3 target{0.0f, 0.0f, 0.0f};
  math::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_y_degrees{45.0f};
  float near_z{0.1f};
  float far_z{100.0f};

  // ── 正交投影（2026-10-24）──
  //
  // ## 为什么需要
  //
  // 透视投影下近大远小，所以「等距展示一组物体」「工程图/技术插图」
  // 「UI 里的小图标三维预览」都会变形（离相机远的那个显得小）。
  // 正交投影让远处与近处同尺寸，这两类用途才能做对。
  //
  // Blender 的相机也有 Perspective/Orthographic 两种类型，语义相同：
  // 正交时 `fov_y_degrees` **失效**，由 `ortho_scale` 决定可视宽度。
  ProjectionKind projection{ProjectionKind::Perspective};
  /// 正交视图的**垂直可视高度**（世界单位）；`projection = Orthographic` 时生效。
  ///
  /// 为何用“垂直高度”而不是“视野角”：正交没有投影中心，角度无意义；
  /// 直接用场景尺度（如 2.0 = 屏幕高度对应 2 个世界单位）最直观。
  float ortho_scale{2.0f};
};

/// ## 色彩空间口径（**读一次能省一堆调试**）
///
/// 三维管线默认在**线性空间**算光照、最后经色调映射 + sRGB 编码输出。
/// 于是不同入口的输入口径**不一样**，传错就会“画面亮一档但看不出原因”：
///
/// | 入口 | 该传什么 | 为什么 |
/// |---|---|---|
/// | `Mesh::colored_sphere` / `colored_box` / `plane` / `textured_*` 的 `color` | **sRGB 字节** | 与设计工具里看到的色一致；内部解码 |
/// | `Mesh::vertices` 直接写 | **线性值** | 那是已经解码后的工作空间 |
/// | `set_texture()` 的 RGBA8 | **sRGB 字节** | 那是 PNG/图片文件里的字节 |
/// | `begin_frame(clear_color)` | **线性值** | 与 `Lighting` 同一口径 |
/// | `Lighting` 里的颜色（隐含于 `light_from` 等） | **线性值** | — |
/// | `Lighting::exposure` / `tone_map` | 曝光 / 算子 | 作用于**线性** HDR 值 |
///
/// 一句话：**从图片/设计工具来的 → sRGB；在代码里手算的 → 线性**。

/// 光照参数。**默认值就是历史行为**（键光方向、环境 0.35、边缘光 0.25、漫反射 0.75）。
///
/// 为何要参数化：软件腿原先把这三组数字写死在 `shade()` 里，于是"换一个打光"
/// 成了改代码重编译。裁云要把三维当画面主体用，打光就是**构图的一部分**——
/// 同一个模型，正面平光与侧逆光完全是两张片子。
///
/// 两条腿（软件 / GPU）必须消费**同一组**参数与**同一个公式**（见 `shade_terms`），
/// 否则"降级"就不是降级，而是换了一种长相。
/// 附加光源（除主光之外）。
///
/// ## 为什么是“附加”而不是“列表里的一条”
///
/// 现有的 `light_from` / `ambient` / `diffuse` 已经被大量场景引用。
/// 把它们改成“光源列表的第一个”会让**所有已出的片子的数值语义**都变
/// （比如 `ambient` 不是光源而是全局项，放进列表就很难再表达）。
///
/// 所以多光源做成**可选的附加项**：`extra_lights` 为空时，光照公式与现在
/// **逐位相同**（渐进采用的同一条底线）。
/// 光源类型（对齐 Blender 的 Light Type；面积光未做，理由见 `Light`）。
enum class LightKind : std::uint8_t {
  Directional = 0,  ///< 平行光（太阳）：无距离衰减，只有方向。
  Point = 1,        ///< 点光（灯泡）：向四周发射，1/d² 衰减。
  Spot = 2,         ///< 聚光（追光）：点光 × 锥形限制。
};

struct Light {
  /// 方向（世界空间，**指向光源**；不需要预先归一化）。这是**平行光**语义。
  ///
  /// `kind != Directional` 时本字段**不用**（点光没有方向，由 `position` 决定）。
  math::Vec3 direction{0.0f, 1.0f, 0.0f};
  /// 颜色（线性空间）。乘在光强上：暖色补光用 `{1.0, 0.85, 0.7}`。
  math::Vec3 color{1.0f, 1.0f, 1.0f};
  /// 强度（乘在漫反射项上；0 = 关闭）。
  float intensity{0.0f};

  // ── 光源类型与位置（2026-10-24，对齐 Blender 的 Light 类型）──
  //
  // ## 为什么要有类型
  //
  // 只有平行光时，一个场景里所有光都是“太阳”：**没有距离衰减**，
  // 于是「台灯照亮桌面一角」「房间角落的暖光」这类效果根本表达不了。
  // 平行光会把整个场景均匀照亮——这是观感上最容易被指出的一处“假”。
  //
  // ## 三种类型
  //
  // | 类型 | 衰减 | 用途 |
  // |---|---|---|
  // | Directional | 无 | 太阳/主光（现状，默认） |
  // | Point | 1/d² + 可选半径 | 灯泡、烛光、局部补光 |
  // | Spot | 1/d² × 锥形 | 舞台追光、手电、聚光强调 |
  //
  // 面积光（Area）**没做**：它要求对光源采样（每次着色 N 个样本），
  // 软件腿每像素成本会乘 N；而柔和观感可以用「点光 + 较大半径」近似。
  LightKind kind{LightKind::Directional};
  /// 光源世界位置（`Point` / `Spot` 用；`Directional` 忽略）。
  math::Vec3 position{0.0f, 2.0f, 2.0f};
  /// 作用半径（>0 时超出距离衰减到 0）。
  ///
  /// 为何需要：纯 1/d² 在远距离仍有微小贡献，几盏灯加起来会让
  /// “暗部永远不够暗”。给一个有限半径既有物理性（真实灯泡也有有效范围），
  /// 也让“关掉远处那盏灯”可预测。
  float radius{0.0f};
  /// 聚光内锥半角（度）。锥内为全强度。
  float spot_inner_degrees{20.0f};
  /// 聚光外锥半角（度）。内外之间平滑衰减，之外为 0。
  float spot_outer_degrees{30.0f};
};

/// 光源上限。
///
/// 为何是常量而不是动态：着色器常量缓冲是固定大小的，而“光源数量动态”
/// 需要分支或循环展开——对实时渲染是明确的负优化。四点布光（主 + 补 + 轮廓 + 背景）
/// 已经能表达绝大多数需求。
constexpr int kMaxExtraLights = 3;

/// 平面投影阴影（planar projected shadow）。
///
/// ## 为何先做这个而不是阴影贴图
///
/// 阴影贴图需要：额外的深度通道、两套着色器、PCF 滤波、双岸一致实现——
/// 一个完整子系统，且在任何“地面是平面”的三维展示里（产品台面、
/// 粒子地面、三维字标底部）**阴影贴图给的结果与平面投影几乎一样**。
///
/// 先把平面投影做好，能回答“阴影到底好不好看、值不值得投入”这个先决问题；
/// 真正的阴影贴图（自阴影、多物体互遮）留给“确实需要”的场景。
///
/// ## 原理
///
/// 把每个三角形沿光方向**投影到平面 y = ground_y**，得到一个压扁的几何，
/// 用半透明黑色画一遍。这要求地面是**水平面**（y = 常量）——绝大多数展示
/// 场景正是如此。
/// **阴影贴图**（shadow mapping）：物体之间互相投影。
///
/// ## 与 `PlanarShadow` 的分工
///
/// | | `PlanarShadow` | `ShadowMap`（本结构）|
/// |---|---|---|
/// | 投影到哪 | 只有**一个水平面** | 任意表面（球面、墙面、物体之间）|
/// | 成本 | 几乎为零（二维遮罩）| 一次额外深度渲染 |
/// | 适用 | 产品展示的接触阴影 | 多物体互相遮挡、嵌套结构 |
///
/// 两者不互斥，可以同时开。
///
/// ## 用哪个光源做投影
///
/// `light_index` 指向 `Lighting::extra_lights` 中一个 `Directional` 或 `Spot` 光源。
/// 点光（向四周发射）不能用单张阴影贴图——那需要立方体贴图（6 张），本实现不做。
///
/// ## 为何需要 `extent`
///
/// 阴影贴图是一张固定分辨率的深度图：覆盖范围越大概率的阴影越糊。
/// 让调用方按场景尺度定，比猜一个默认值可靠。
struct ShadowMap {
  /// 阴影强度（0 = 关闭，1 = 全黑阴影）。
  float strength{0.0f};
  /// 用 `extra_lights` 中哪一个光源做投影（仅 `Directional` / `Spot`）。
  int light_index{0};
  /// 阴影贴图分辨率（边长，正方形；512/1024 是常用档）。
  int resolution{512};
  /// 深度偏移（防自阴影条纹 shadow acne）。
  float bias{0.0015f};
  /// 柔化半径（贴图纹素为单位；0 = 硬阴影）。PCF 采样半径。
  float soft_radius{1.0f};
  /// 投影相机包络半径（世界单位）：覆盖场景所需的正交盒半径。
  float extent{6.0f};
};

struct PlanarShadow {
/// 阴影强度（0 = 关闭，1 = 全黑）。建议 0.25~0.55：太黑会看起来像“洞”。
float strength{0.0f};
  /// 投影到哪个水平面（世界坐标 y）。
  float ground_y{0.0f};
  /// 阴影颜色（线性空间）。默认纯黑；暖色调场景可以用深棕。
  math::Vec3 color{0.0f, 0.0f, 0.0f};
  /// 阴影位置的微小抬升（避免与地面 z-fighting）。
  ///
  /// ## 实测：默认值太小会让阴影**完全不出现**
  ///
  /// 阴影与地面共面时，深度值几乎相等，而测试是 `z >= depth → 丢弃`，
  /// 于是**每一个像素都被舍弃**（实测：784 个三角形都收集到了、绘制也执行了，
  /// 但画布**零像素差异**）。
  ///
  /// 0.002 在世界坐标里很小，经过透视投影后差异更小，不足以保证浮点比较成立。
  /// 取 0.01（场景尺度约 1~5 时是可靠的）。
  float lift{0.01f};
};

struct Lighting {
  /// 主光方向（世界空间，**指向光源**，不需要预先归一化）。
  math::Vec3 light_from{0.4f, 0.8f, 0.6f};
  /// 环境项：无光照处的底色亮度（0 = 背光面全黑）。
  float ambient{0.35f};
  /// 漫反射权重（与环境项相加，1.10 ≈ 原值 ambient 0.35 + diffuse 0.75）。
  float diffuse{0.75f};
  /// 边缘光（轮廓光）强度：让模型从背景里"浮"出来。
  float rim{0.25f};
  /// 整体曝光乘子（1 = 不调）。批量出片时统一压/提亮比逐个改顶点色省事。
  float exposure{1.0f};
  /// 色调映射算子（见 `color_pipeline.hpp`）。
  ///
  /// **默认 `Aces`**：它是“正确”的那一档（HDR 滚降、高光不糊），
  /// 而且现有场景全部在 [0,1] 内，ACES 对中调的影响是“略提亮”——
  /// 实测 0.5 → 0.616，画面变得更接近预期。
  ///
  /// 要逐像素回到旧行为（无映射、线性值直接当 sRGB），把它设成 `None`；
  /// 但**不推荐**——那是把“线性当 sRGB”的错误固化下来。
  ///
  /// 注：`color_pipeline.hpp` 里的 `ToneMap` 定义在 `st::raster`，
  /// 本结构也在同一命名空间，所以这里直接写类型名。
  ToneMap tone_map{ToneMap::Aces};

  /// 附加光源（最多 `kMaxExtraLights` 个）。
  ///
  /// ## 为何是数组而不是 `std::vector`
  ///
  /// 这个结构每帧被复制、并逐字镜像到 GPU 常量缓冲。`std::vector` 会带来
  /// 堆分配与额外间接层，而它的容量本身就是常量上上限。用 `std::array`
  /// 把“最多几个”变成编译期事实，两条腿的常量缓冲布局才能逐字节对应。
  std::array<Light, kMaxExtraLights> extra_lights{};
};

/// 一像素的光照结果（两条腿共用同一套算式的**唯一**定义处）。///
/// 存在的理由：软件腿逐像素调它、GPU 腿的 HLSL 是它的**逐字翻译**。
/// 把算式写成一个具名函数，两边的"看起来差不多"才能被测试钉住
/// （`raster_scene3d_lighting_test` 用同一组输入比两边输出）。
[[nodiscard]] auto shade_terms(math::Vec3 normal, math::Vec3 color, const Lighting& lighting)
    -> math::Vec3;

/// GGX 法线分布函数 D（微表面法线指向半角向量的概率密度）。
///
/// ## 为何用 GGX 而不是 Blinn-Phong
///
/// Blinn-Phong 的高光公式是 `(n·h)^shininess`——它是**经验模型**：
/// 高光形状不随角度变化，掠射角下会崩（高光变成一束硬边）。
/// GGX 是**微表面模型**：粗糙度直接决定高光的宽度与拖尾形状，
/// 掠射时自然拉长——这就是金属看起来“像金属”的原因。
///
/// ## 公式（与 Blender `bsdf_D<GGX>` 一致）
///
/// ```
/// D = alpha² / (π · ((n·h)² · (alpha² - 1) + 1)²)
/// ```
/// 其中 `alpha = roughness`（Blender 不再对 roughness 平方）。
///
/// `roughness → 0` 时 D 退化成一个冲击函数——此时解析式会溢出，
/// 所以 `alpha` 走下限夹取（见 `kMinAlpha`）。
[[nodiscard]] auto ggx_distribution(float n_dot_h, float roughness) noexcept -> float;

/// GGX 的几何遮蔽项 G（Smith 近似 + Schlick 的 k）。
///
/// 物理含义：微表面互相遮蔽，掠射角下可见面积变小。
/// 缺了它，物体的**边缘会发亮**（能量不守恒）——高光恰好在边缘最明显，
/// 所以漏掉 G 的破绻一眼就看得出。
[[nodiscard]] auto ggx_geometry(float n_dot_v, float n_dot_l, float roughness) noexcept -> float;

/// 完整的高光项（已含 Fresnel、D、G 与分母）。
///
/// 返回**辐照度量级**的标量：调用方乘固有色/高光色即可。
/// 系数已归一化到“白底白高光 = 1”附近（与 Blender 的量级可比）。
[[nodiscard]] auto ggx_specular(math::Vec3 normal, math::Vec3 view_dir, math::Vec3 light_dir,
                                float roughness, float metallic, math::Vec3 base_color,
                                math::Vec3 specular_tint) noexcept -> math::Vec3;

/// 材质的最终着色（含 Principaled 参数）。
///
/// 与 `shade_terms` 的关系：后者是**只含漫反射**的旧版（保留给老路径与
/// 两腿一致性测试）；本函数是完整版，`specular == 0 && emission == 0` 时
/// 退化为与 `shade_terms` **逐像素相同**的结果（渐进采用）。
[[nodiscard]] auto shade_surface(math::Vec3 normal, math::Vec3 base_color,
                                 math::Vec3 view_dir, const Lighting& lighting,
                                 float metallic, float roughness, float specular,
                                 float emission, math::Vec3 specular_tint) -> math::Vec3;

/// 三维场景交出的**原生纹理句柄**（供同后端的上层零拷贝合成）。
///
/// 定义在 `Scene3D` 上而不是 `Surface`：三维场景**不是**一张画布（它没有
/// fill_rect/文字/裁剪这些能力），只是“一块渲好的纹理”。把两者分开，
/// `Surface` 就不必为一个它多数实现用不上的能力承担接口重量。
struct SceneTexture {
  const void* handle{nullptr};   ///< 后端自有对象（D3D11 上是 `ID3D11ShaderResourceView*`）
  const char* backend{nullptr};  ///< 后端标签（如 `"d3d11"`）；不匹配即不可用
  int width{0};
  int height{0};

  [[nodiscard]] auto valid() const noexcept -> bool { return handle != nullptr; }
};

/// 三维场景：离屏渲染 → 合成进画布。
///
/// 用法：`create` → 每帧 `begin_frame` / `set_camera` / `draw_mesh(…)` / `end_frame(surface, dest)`。
/// 一次 `begin_frame` 内的所有 `draw_mesh` 共享同一深度缓冲（因此**互相遮挡关系正确**）。
class Scene3D {
 public:
  Scene3D() = default;
  virtual ~Scene3D() = default;
  Scene3D(const Scene3D&) = delete;
  auto operator=(const Scene3D&) -> Scene3D& = delete;

  /// 本平台是否有可用的三维实现（编进来 **且** 运行期可用）。
  [[nodiscard]] static auto available() noexcept -> bool;

  /// 建离屏场景（尺寸为**物理像素**）。失败给出具体原因（无设备/资源创建失败/着色器编译错误）。
  [[nodiscard]] static auto create(int width, int height) -> Result<std::unique_ptr<Scene3D>>;

  /// 后端选择用的**进程级互斥**（`ST_SCENE3D_BACKEND` 是进程环境，不是线程局部）。
  ///
  /// ## 用它保护什么
  ///
  /// 「临时设 `ST_SCENE3D_BACKEND` → `create` → 还原」这段必须原子。
  /// 否则两个线程同时改这个变量时，A 建出来的场景可能用的是 B 设的后端。
  ///
  /// ## 为什么放在框架侧而不是各测试自己定义
  ///
  /// 锁必须是**同一把**才有互斥效果。测试分散在多个翻译单元里，
  /// 各自定义一把 static 锁互不相干——实测：只给一个测试文件加锁后，
  /// 另一个不持锁的用例仍然会读到中间值（全量并行跑时偶发失败）。
  ///
  /// 返回值是进程内唯一的同一把锁（函数内 static）。
  [[nodiscard]] static auto backend_mutex() -> std::mutex&;

  /// 开始一帧：设视口、清色与深度。
  virtual void begin_frame(math::Color clear_color) = 0;
  virtual void set_camera(const Camera& camera) = 0;
  /// 设光照（**不调也可**：`Lighting` 的默认值就是历史行为）。每帧重置为默认。
  virtual void set_lighting(const Lighting& lighting) = 0;

  /// 设面朝向剔除模式（**不调也可**：默认就是 `CullMode::Back`）。每帧重置为默认。
  ///
  /// 为何要每帧重置：与 `Lighting` 同理——不重置会让上一帧的设置泄漏到下一帧，
  /// 而那种“某个场景之后全都错了”的 bug 极难定位。
  virtual void set_cull_mode(CullMode /*mode*/) {}

  /// 设材质（**不调也可**：默认完全不透明、写深度）。每帧重置为默认。
  ///
  /// `opacity < 1` 时网格进入**透明队列**（先画不透明、再远→近混合）。
  virtual void set_material(const Material& /*material*/) {}

  /// 设平面投影阴影（**不调也可**：`strength = 0` 即关闭）。每帧重置为默认。
  virtual void set_planar_shadow(const PlanarShadow& /*shadow*/) {}
  /// 设阴影贴图（默认无操作：不支持的后端静默忽略）。
  ///
  /// 为何不报错：与 `set_material` 同一取舍——“能渲出来”比“参数全支持”重要。
  /// 不支持的后端会出“没阴影”的画面，这个差异由探针/文档声明，
  /// 不该让渲染直接失败。
  virtual void set_shadow_map(const ShadowMap& /*shadow*/) {}
  /// 设当前纹理（**可选**）：设了之后，带 UV 的网格会采样它。
  ///
  /// ## 为什么纹理是“场景的一个状态”而不是“网格的一部分”
  ///
  /// 三维里的纹理是**材质**，而材质在真实用法里经常跨网格共用（一圈产品都贴同一个 logo），
  /// 也会在同帧内切换（不同面贴不同图要多次绘制 + 换纹理）。
  /// 把它挂在网格上会逼着每次换图重建网格缓存——而那正是我们最不想动的热路径。
  ///
  /// 不调用就是**无纹理**：网格走顶点色，行为与以前完全一致。
  ///
  /// `pixels` 是 **RGBA8、行优先、非预乘**；`width/height` 必须为正。
  /// 空 span 或非法尺寸 = 清除纹理（回到顶点色）。
  virtual void set_texture(std::span<const std::uint8_t> /*pixels*/, int /*width*/,
                           int /*height*/) {}
  /// 设 UV 变换（**可选**；在采样前作用：`uv' = uv * scale + offset`）。
  ///
  /// ## 为什么 UV 变换是场景状态而不是网格属性
  ///
  /// 与 `set_texture` 同一理由：换平铺密度不该重建网格。
  /// 而且"同一张图在不同零件上以不同密度平铺"是极常见的需求（砖墙、地面、布料），
  /// 烘焙进网格意味着每个零件一份顶点副本（内存 × N、且不再共享）。
  ///
  /// `clamp_edges` 为真时超出 `0..1` 的 UV 夹到边缘（图标/贴图的常见语义），
  /// 为假时按 **repeat 环绕**（平铺的语义）。
  ///
  /// 不调用就是恒等变换（`scale = (1,1)`、`offset = (0,0)`、repeat）——
  /// 与以前完全一致。
  virtual void set_texture_transform(float /*scale_u*/, float /*scale_v*/, float /*offset_u*/,
                                     float /*offset_v*/, bool /*clamp_edges*/) {}
  /// 画一个网格（可带模型变换）。同一个网格重复绘制会复用已上传的缓冲。
  virtual void draw_mesh(const Mesh& mesh, const math::Mat4& model) = 0;
  /// 结束一帧并把结果**合成**到 `target` 的目的矩形（逻辑坐标）。
  ///
  /// **降级链在这里**：`target` 是本进程的 GPU 画布且本场景也是 GPU 腿时，
  /// 三维纹理直接以着色器资源合成（**零回读**）；否则走 `Surface::draw_canvas`
  /// 的普通路径（软件目标 → 逐像素拷贝，GPU 目标 → 一次上传）。
  /// 这使「有显卡用显卡、没显卡降级」不需要任何调用方分支。
  virtual void end_frame(Surface& target, math::Rect destination, float opacity = 1.0f) = 0;

  /// 把迄今为止提交的绘制**真正跑完**（阻塞到 GPU 空闲）。
  ///
  /// 为何接口上需要它：
  /// 1. **计时**。`draw_mesh` 在 GPU 腿上只是把命令塞进缓冲就返回，不等待就量
  ///    只能量到 CPU 侧 API 开销（实测会得出“GPU 光栅 0.03 ms”这种假数据）。
  /// 2. **帧间可重入**。同一份网格缓冲/常量缓冲要在下一帧被覆盖前确保上一帧
  ///    已经用完（驱动层通常也会保护，但从语义上把它说清楚）。
  ///
  /// 软件腿是同步的，所以它是空操作（立即返回）。
  virtual void flush() = 0;

  /// 渲染尺寸（物理像素）。
  [[nodiscard]] virtual auto width() const noexcept -> int = 0;
  [[nodiscard]] virtual auto height() const noexcept -> int = 0;
  /// 本场景的绘制调用计数（用于确认"确实画了东西"而不是静默空帧）。
  [[nodiscard]] virtual auto draw_calls() const noexcept -> std::uint64_t = 0;
  /// 本场景实际跑在哪条腿上：`"gpu"` / `"software"`。
  ///
  /// 为何必须能查：降级是**无声**的——没有这个接口，"这台机器到底用没用上显卡"
  /// 只能靠帧耗时反推，而那种推法在快机器上永远得出"应该用上了"。
  /// 应用把它上报到 metrics，测试据此断言两条腿都被覆盖。
  [[nodiscard]] virtual auto backend() const -> std::string = 0;

  /// 把当前帧的结果交出为**原生纹理**（不读回显存）。
  ///
  /// 存在的理由（实测）：三维光栅只要 0.06 ms，而“取成像素交给软件画布”要
  /// 100 ms（1080p）——不想办法消掉它，“用显卡画三维”就是负优化。
  /// 上层若是同后端的 GPU 画布，就能拿这个句柄直接采样合成（零拷贝）；
  /// 拿不到（默认空）或后端不匹配时，`end_frame` 自动走回读回退路。
  [[nodiscard]] virtual auto texture() const noexcept -> SceneTexture { return {}; }
};

}  // namespace st::raster
