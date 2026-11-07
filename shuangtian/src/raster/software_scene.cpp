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
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"
#include "st/raster/pbr.hpp"

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

// ────────────────────────────────────────────────────────────────────────────
// Legacy 着色模型已删除（2026-11-06）
//
// 这里原本有 `shade_terms_at` / `ggx_distribution` / `ggx_geometry` / `ggx_specular`
// / `shade_surface` 一整族函数，实现的是本框架自研的近似模型：
//
//     lit = albedo × (ambient + n·l·diffuse + rim) + GGX + 假环境反射
//
// 它对标浏览器时逐项都是硬差（缺 1/π、有 0.35 常量保底、凭空加 rim、
// 从背景推导环境反射），实测 0.5 灰面 176 vs 浏览器 76。
//
// 现在**只有一套公式**：`st::raster::pbr` 的物理 BRDF（见 `pbr.hpp`）。
// 三条输入路径（three 脚本 / bpy 脚本 / JSON 剧本）都走它，差异全部在适配层。
// 历史对照数据见 `docs/render_parity.md`。
// ────────────────────────────────────────────────────────────────────────────

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
    //
    // ## 但“重置成默认 ACES”会让背景与实体用两个算子（2026-11-06 修）
    //
    // `set_lighting()` 总在 `begin_frame()` **之后**（调用方不可能把顺序倒过来，
    // 那是 `Scene3D` 的契约）。所以清屏时 `tone_map_` 只能是默认的 `Aces`，
    // 而实体用的是随后 `set_lighting` 设进去的算子。一帧里两种算子。
    //
    // 实测（背景 0.5 灰、tone = None，期望 sRGB 188）：实际清出 **206**——那正是
    // Narkowicz ACES 作用于 0.5 的结果。全图背景（77%）系统性偏亮十几级，
    // 而单看实体完全正常，所以极难定位。
    //
    // 修法：清屏色用**默认光照结构的算子**（= 调用方没设时的行为，与旧版逐位相同），
    // 但实体绘制的算子重置保持不变。两项分开——这就是为什么
    // `tone_map_` 不再从 `lighting_` 派生。
    lighting_ = Lighting{};          // 每帧回到默认：不调 `set_lighting` 的调用方行为完全不变
    cull_mode_ = CullMode::Back;     // 同上：不重置会让上一帧的设置泄漏
    material_ = Material{};
    shadow_ = PlanarShadow{};
    shadow_tris_.clear();
    // AO 空间缓存每帧清空：它只在**同一帧内**有效（不同帧的几何可能全变了）。
    occlusion_cache_.clear();
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
    //
    // ## 清屏 alpha 必须**透传**（与 GPU 腿一致）
    //
    // `encode_pixel` 只处理颜色链，返回的 alpha 恒为 255；直接用它清屏会让
    // `begin_frame({0,0,0,0})`（透明背景 —— 把模型叠在视频/画面上时的那条路）
    // 变成**一整块不透明白黑**：三维图层的矩形四角把下层画面盖死。
    clear_color_ = clear_color;
    clear_color_locked_ = false;
    clear_canvas(clear_color);
  }

  /// 按**当前** `tone_map_` 清屏（+ 重置深度/绘制计数）。
  ///
  /// 为何抽成函数：`begin_frame` 与 `set_lighting` 都要清（前者只知道默认算子，
  /// 后者才知道真实算子），两处必须走**同一份**编码逻辑，否则又会变成
  /// “背景与实体两套算子”。
  void clear_canvas(math::Color clear_color) {
    const math::Vec3 bg_linear{static_cast<float>(clear_color.r) / 255.0f,
                               static_cast<float>(clear_color.g) / 255.0f,
                               static_cast<float>(clear_color.b) / 255.0f};
    math::Color cleared = encode_pixel(bg_linear, 1.0f, tone_map_);
    cleared.a = clear_color.a;
    canvas_.clear(cleared);
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
    const ToneMap previous = tone_map_;
    lighting_ = lighting;
    tone_map_ = lighting.tone_map;
    // ## 预折算主光与 IBL 探针（逐像素复用）
    //
    // 主光是平行光（`Lighting::light_from` 的语义），所以 `surface_position`
    // 不影响结果——传零向量即可。但**点光/聚光**不行：它们的方向随像素变，
    // 必须在像素级重算（见 `shade_pixel` 里对 `extra_lights` 的处理）。
    key_light_ = pbr_sample_light(0, lighting_.light_from, math::Vec3{0.0f, 0.0f, 0.0f},
                                  lighting_.light_color, lighting_.diffuse, 0.0f, 0.0f, 0.0f,
                                  math::Vec3{0.0f, 0.0f, 0.0f});
    // IBL：把环境描述折成方向探针（**两条腿走同一个函数**，见 `scene3d.cpp`）。
    //
    // 归一化只做一次是刻意的：实测踩过“软件腿自己合成天/地探针、GPU 腿没合成”，
    // 于是同一个剧本两条腿差出一整层环境光。
    ibl_count_ = static_cast<int>(
        lighting_.make_ibl_probes(ibl_dirs_.data(), ibl_colors_.data(), ibl_dirs_.size()));
    // ## 清屏色要跟着算子重做（2026-11-06 修）
    //
    // `begin_frame()` 不知道本次会用哪个算子（`set_lighting` 总在它之后），
    // 所以它只能先用默认 ACES 清屏。而实体随后用的是这里的 `tone_map`——
    // 一帧里背景与实体两套算子，背景系统性偏亮十几级。
    //
    // 修法：`set_lighting` 时**重清一次**（此时不可能已经画过东西：
    // `Scene3D` 的契约是 `begin_frame → set_camera → set_lighting → draw`）。
    // 为了避免调用方真的违反顺序时把画面抹掉，用 `draw_calls_ == 0` 作守卫。
    if (tone_map_ != previous && draw_calls_ == 0 && !clear_color_locked_) {
      clear_canvas(clear_color_);
    }
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

  /// 设缝隙遮蔽（AO）。
  ///
  /// 不在这里算任何东西：真正的计算在 `shade_pixel` → `compute_occlusion`，
  /// 逐像素做（因为遮蔽是**位置相关**的，不能预先算成一张与分辨率绑定的图）。
  void set_ambient_occlusion(const AmbientOcclusion& occlusion) override {
    occlusion_strength_ = std::clamp(occlusion.strength, 0.0f, 1.0f);
    occlusion_enabled_ = occlusion_strength_ > 0.0f;
    occlusion_radius_ = occlusion.radius > 0.0f ? occlusion.radius : 0.5f;
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
    const math::Vec3 dir = lighting_.light_from.normalized();
    // ## 方向约定（一开始写反了，阴影完全不出）
    //
    // `light_from` 是**指向光源**的方向。光从上方来时 `dir.y > 0`，
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
    draw_mesh_with_material(mesh, model, material_);
  }

  /// 显式指定材质的绘制（见 `Scene3D::draw_mesh_with_material` 的说明）。
  ///
  /// 实现要点：**只在本网格期间临时换掉 `material_`**，画完立即恢复——
  /// 否则“给某个网格设了金属”会泄漏到后续所有网格（同一个场景里
  /// 既有金属标牌又有塑料背景时，背景会跟着变金属）。
  void draw_mesh_with_material(const Mesh& mesh, const math::Mat4& model,
                               const Material& material) override {
    if (mesh.is_empty() || !mvp_ready_) return;
    const Material saved = material_;
    material_ = material;
    if (shadow_map_.strength > 0.0f || occlusion_enabled_) {
      deferred_.push_back(
          DeferredMesh{std::make_shared<const Mesh>(mesh), model, material_});
      ++draw_calls_;
      material_ = saved;
      return;
    }
    draw_mesh_now(mesh, model);
    ++draw_calls_;
    material_ = saved;
  }

  /// 索引区间 + 材质的绘制（见 `Scene3D::draw_mesh_range`）。
  ///
  /// ## 为何不能直接调 `draw_mesh_with_material`
  ///
  /// 那个入口画**整网格**。多材质对象如果每组都整画一遍，最后一组的材质
  /// 会覆盖前面所有组——现象是“物体整块都是一个颜色”，且**不报错**。
  ///
  /// 同理不能把“区间”塞进 mesh 再让它拷贝：阴影路径上会
  /// `make_shared<const Mesh>(mesh)` 整份拷贝，多组就多份（一个 2000
  /// 三角形的齿轮 × 5 组 = 10 倍内存）。这里只拷**区间内的索引**，
  /// 顶点表共用。
  void draw_mesh_range(const Mesh& mesh, const math::Mat4& model, const Material& material,
                       std::size_t index_start, std::size_t index_count) override {
    if (mesh.is_empty() || !mvp_ready_) return;
    const Material saved = material_;
    material_ = material;
    if (shadow_map_.strength > 0.0f || occlusion_enabled_) {
      Mesh slice = mesh;
      slice.material_groups.clear();   // 切片不再是多材质对象
      const std::size_t begin = index_start < mesh.indices.size() ? index_start
                                                                  : mesh.indices.size();
      const std::size_t end = index_count == 0
                                  ? mesh.indices.size()
                                  : (begin + index_count < mesh.indices.size()
                                         ? begin + index_count
                                         : mesh.indices.size());
      slice.indices.assign(mesh.indices.begin() + static_cast<std::ptrdiff_t>(begin),
                           mesh.indices.begin() + static_cast<std::ptrdiff_t>(end));
      deferred_.push_back(
          DeferredMesh{std::make_shared<const Mesh>(std::move(slice)), model, material_});
      ++draw_calls_;
      material_ = saved;
      return;
    }
    draw_mesh_now(mesh, model, index_start, index_count);
    ++draw_calls_;
    material_ = saved;
  }

  /// 旧的即时入口（保留以兼容 `draw_mesh` 的语义）。
  void draw_mesh_legacy(const Mesh& mesh, const math::Mat4& model) {
    if (mesh.is_empty() || !mvp_ready_) return;
    // ## 阴影贴图开启时：延迟到 `end_frame` 统一绘制
    //
    // 为何必须延迟：阴影贴图要**先知道全部几何**（先渲染一次光源视角的深度图），
    // 而像素着色在 `draw_mesh` 里就发生了。先画的网格看不到后画的网格的影子。
    //
    // 这个延迟只在 `shadow_map.strength > 0` 或 AO 开启时启用：
    // 两者都关时走原来的即时路径，行为与之前**逐位相同**（老场景不受影响）。
    if (shadow_map_.strength > 0.0f || occlusion_enabled_) {
      deferred_.push_back(
          DeferredMesh{std::make_shared<const Mesh>(mesh), model, material_});
      ++draw_calls_;
      return;
    }
    draw_mesh_now(mesh, model);
    ++draw_calls_;
  }

  /// 即时绘制（原有路径，行为不变）。
  void draw_mesh_now(const Mesh& mesh, const math::Mat4& model,
                     std::size_t index_start = 0, std::size_t index_count = 0) {
    const math::Mat4 mvp = projection_ * view_ * model;
    // 本网格是否有可用 UV；有且已设纹理时才采样
    const bool mesh_has_uv = mesh.has_uvs();
    use_texture_ = mesh_has_uv && !texture_.empty() && texture_width_ > 0;
    // 索引区间：`index_count == 0` 意为“全画”（与旧行为一致）
    const std::size_t begin = index_start < mesh.indices.size() ? index_start
                                                                : mesh.indices.size();
    const std::size_t end = index_count == 0
                                ? mesh.indices.size()
                                : (begin + index_count < mesh.indices.size()
                                       ? begin + index_count
                                       : mesh.indices.size());
    for (std::size_t i = begin; i + 2 < end; i += 3) {
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
    //
    // ## 为何 AO 开时也要收（2026-11-06）
    //
    // AO 的半球射线要查“场景里有什么挡着我”，而它用的**就是这批三角形**。
    // 早期只在 `shadow_map.strength > 0` 时收，于是单独开 AO（不开阴影贴图）
    // 时 `shadow_geometry_` 是空的、`compute_occlusion` 直接返回 1.0——
    // AO **静默无效**（实测开关 AO 画面均值 83.9 对 83.9，完全一样）。
    // 这类“参数收了、效果没有”的失效最难查，所以两个条件都收。
    if ((shadow_map_.strength > 0.0f || occlusion_enabled_) && material_.opacity >= 0.999f) {
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
  /// 逐像素着色：**统一内核**（`pbr_shade`）。
  ///
  /// ## 为何这里没有 `if (model == ...)`
  ///
  /// 曾经有两种模型（Legacy / three 同构）。统一之后只剩一套公式，
  /// 于是着色分发这个分支**整体消失**——三条输入路径（three 脚本 / bpy 脚本 /
  /// JSON 剧本）都走它，差别只在上层怎么填 `ShadeInput`。
  ///
  /// 这正是「底层渲染统一」的具体含义：**公式只有一份，差异都在适配层**。
  [[nodiscard]] auto shade_pixel(math::Vec3 normal, math::Vec3 base_color, math::Vec3 view_dir,
                                 math::Vec3 world) -> math::Vec3 {
    ShadeInput input{};
    input.normal = normal;
    input.base_color = base_color;
    input.view_dir = view_dir;
    input.world_position = world;
    input.metallic = material_.metallic;
    input.roughness = material_.roughness;
    input.key_light = key_light_;
    // 额外灯光：**必须逐像素重算**。
    //
    // 点光/聚光的方向与衰减都依赖表面位置，缓存只能存平行光那部分。
    // 所以这里就地算一遍（最多 `kMaxExtraLights` 个，开销可接受），
    // 而不用 `extra_light_samples_` 缓存——那个缓存只在“全是平行光”时有效，
    // 而多一种分支就多一次“两条腿两个算法”的机会。
    extra_light_scratch_.clear();
    for (std::size_t i = 0; i < lighting_.extra_lights.size(); ++i) {
      const Light& light = lighting_.extra_lights[i];
      if (light.intensity <= 0.0f) continue;
      const int kind = light.kind == LightKind::Directional
                           ? 0
                           : (light.kind == LightKind::Point ? 1 : 2);
      extra_light_scratch_.push_back(pbr_sample_light(kind, light.direction, light.position,
                                                      light.color, light.intensity,
                                                      light.radius, light.spot_inner_degrees,
                                                      light.spot_outer_degrees, world));
    }
    input.extra_lights = extra_light_scratch_.data();
    input.extra_light_count = static_cast<int>(extra_light_scratch_.size());
    input.ambient_irradiance =
        math::Vec3{lighting_.ambient_color.x * lighting_.ambient,
                   lighting_.ambient_color.y * lighting_.ambient,
                   lighting_.ambient_color.z * lighting_.ambient};
    // IBL：方向探针（环境反射/环境光照的统一形态）。
    input.ibl_dirs = ibl_dirs_.data();
    input.ibl_colors = ibl_colors_.data();
    input.ibl_count = ibl_count_;
    input.ibl_strength = lighting_.ibl_strength;
    // 自发光：`emission_unlit` 时把受光项整个丢掉（只在它自己发光）。
    if (material_.emission > 0.0f) {
      const math::Vec3 tint = material_.emissive_color_set ? material_.emissive_color : base_color;
      input.emissive = math::Vec3{tint.x * material_.emission, tint.y * material_.emission,
                                  tint.z * material_.emission};
      if (material_.emission_unlit) {
        // 纯发光体：不受光照，只留自发光。等价于把别的输入都置零。
        input.key_light = PbrLight{};
        input.ambient_irradiance = math::Vec3{0.0f, 0.0f, 0.0f};
        input.ibl_strength = 0.0f;
      }
    }
    input.exposure = lighting_.exposure;
    input.occlusion = occlusion_at(world, normal);
    return pbr_shade(input);
  }

  /// 估算缝隙遮蔽（ambient occlusion，0~1；1 = 无遮蔽）。带**空间缓存**。
  ///
  /// ## 为何用「世界三角形 + 半球采样」而不是屏幕空间 SSAO
  ///
  /// SSAO 要屏幕空间深度/法线缓冲：软件腿的深度缓冲是**超采样尺寸**的，
  /// 而且分辨率变化时精度会变——那会让 AO 成为「分辨率相关的量」。
  /// 而这里已经有全场景的世界空间三角形，直接拿它做几何查询：**结果与分辨率无关**。
  ///
  /// ## 算法
  ///
  /// 沿法线半球取 `kOcclusionSamples` 个方向（固定的黄金角螺旋，
  /// 不用随机数——随机数会让两次运行出不同结果，那是本仓最忌的）。
  /// 每个方向发一根短射线（长度 `occlusion_radius_`），看是否撞上几何：
  ///
  /// ```
  /// ao = 1 - 命中数/总射线数 * occlusion_strength
  /// ```
  ///
  /// ## 为何必须有缓存（实测血泪）
  ///
  /// 第一版逐超采样样本算：1100×800 × 4x SSAA ≈ 1400 万次着色，
  /// 每次 16 射线 × 三角形数。**实测单帧 199 秒**（只有球+地板+盒子）。
  ///
  /// AO 是**低频量**（它随位置平滑变化：同一个块内几乎不变），所以
  /// 把世界位置量化到 `occlusion_radius_ * kOcclusionCacheStep` 的
  /// 网格上，同格共用一个值。缓存同时在两次调用之间存活（同一帧内），
  /// 所以三角形数只影响「缓未命中」的次数。
  ///
  /// 实测降幅：199 秒 → 见 `docs/bpy_render_parity.md §十四`。
  ///
  /// ## 缓存键为何用「世界位置量化」而不是屏幕坐标
  ///
  /// 屏幕坐标的缓存会被**三角形绘制顺序**影响（同一屏幕像素可能被多个
  /// 三角形覆盖，而后画的赢）。用世界位置量化则与绘制顺序无关——
  /// 两个三角形在同一个世界位置上得到同一个 AO，那是物理上正确的。
  [[nodiscard]] auto occlusion_at(math::Vec3 world, math::Vec3 normal) -> float {
    if (!occlusion_enabled_) return 1.0f;
    if (shadow_geometry_.empty()) return 1.0f;
    const math::Vec3 n = normal.normalized();
    if (n.length() < 1e-6f) return 1.0f;

    // ① 查缓存（键 = 量化位置 + 量化法线；法线不量化会让稜角与平面分家）
    const float cell = std::max(occlusion_radius_ * kOcclusionCacheStep, 1e-4f);
    const auto quantize = [cell](float v) -> std::int32_t {
      return static_cast<std::int32_t>(std::floor(v / cell));
    };
    const std::array<std::int32_t, 5> key{quantize(world.x), quantize(world.y),
                                          quantize(world.z), quantize(n.x * 8.0f),
                                          quantize(n.y * 8.0f)};
    if (const auto found = occlusion_cache_.find(key); found != occlusion_cache_.end()) {
      return found->second;
    }

    // ② 未命中：调共享内核（`pbr_ambient_occlusion`，两条腿用同一个）
    OcclusionParams params{};
    params.strength = occlusion_strength_;
    params.radius = occlusion_radius_;
    params.samples = kOcclusionSamples;
    const float value =
        pbr_ambient_occlusion(world, normal, shadow_geometry_.data(),
                              shadow_geometry_.size(), params);
    // 有界：防止病态场景（超大坐标 + 小半径）把缓存垛爆。
    // 满了就清空重填——比无限增长安全（内存是硬约束）。
    if (occlusion_cache_.size() > kOcclusionCacheLimit) occlusion_cache_.clear();
    occlusion_cache_.emplace(key, value);
    return value;
  }

  /// 从 `origin` 沿 `dir` 发一根长度为 `occlusion_radius_` 的射线，是否撞上几何。
  ///
  /// （旧版这里有一个 `ray_hits_geometry`，AO 改走共享内核后零调用，
  /// 已删除——本仓不养死代码。）


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
            shade_pixel(normal, color, pixel_view, world);
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
          const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(kBake) +
                                   static_cast<std::size_t>(sy);
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
  /// 本帧的清屏色（`set_lighting` 改算子时要重清）。
  math::Color clear_color_{0, 0, 0, 255};
  /// 是否已经开始画东西（画过就不允许再清，否则会把画面抹掉）。
  bool clear_color_locked_{false};
  /// 主光的**已折算**形式（`set_lighting` 时算一次，逐像素复用）。
  ///
  /// 为何要缓存：`pbr_sample_light` 里有除法和开方，而它在**每个像素**都会被用到。
  /// 主光是平行光时这些量整帧不变，缓存下来省掉的就是每像素两次除法。
  PbrLight key_light_{};
  /// 额外灯光的**逐像素临时区**（`shade_pixel` 内填、随即消费）。
  ///
  /// 为何用成员而不是局部 `std::vector`：着色是**逐像素**调用的，
  /// 局部容器会每像素分配一次（百万级像素 = 百万次 malloc）。
  /// 用成员并 `clear()`（保留 capacity）就只分配一次。
  /// 它是 `mutable` 吗？不是——`shade_pixel` 现在不是 `const` 成员，
  /// 因为这个缓冲区本身是“每像素的暂存”，语义上是可变的。
  std::vector<PbrLight> extra_light_scratch_{};
  /// IBL 方向探针的缓存（与 `Lighting::env_probes` 同形，但已转成内核要的数组）。
  std::array<math::Vec3, 8> ibl_dirs_{};
  std::array<math::Vec3, 8> ibl_colors_{};
  int ibl_count_{0};
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
  /// 世界空间三角形（阴影贴图与 AO 共用的几何源）。
  ///
  /// 直接用内核层的 `WorldTriangle`，不再自定义一个同构的 `ShadowTriangle`：
  /// 两个同构结构并存会逼出 `reinterpret_cast`（lint L6），而那正是
  /// “布局碰巧相同”的隐患——今天相同，明天某人给其中一个加个字段就静默错位。
  using ShadowTriangle = WorldTriangle;

  /// 延迟绘制的网格（阴影贴图开启时）。
  ///
  /// 存**网格指针 + 模型矩阵 + 材质快照**：网格指针的生命周期由调用方保证
  /// （`draw_mesh` 的契约就是“调用期间有效”，而延迟到 `end_frame` 仍在同一次
  /// 帧内，所以安全）。材质必须快照，否则后一个网格的 `set_material` 会
  /// 覆盖前一个的。
  struct DeferredMesh {
    // ⚠ **必须存副本，不能存指针**（实测踩过，很隐蔽）
    //
    // 调用方（`draw_world`）的 mesh 是局部变量：函数返回后指针悬空。
    // 延迟到 `end_frame` 才读，读到的可能是已经被下一帧/下一个对象
    // 覆盖的内存——症状是“动画在跑、世界矩阵也变了，但画面每帧完全一样”
    //（因为读到的是同一块残留内存）。用共享指针，拷贝便宜（写时复制不需要）。
    std::shared_ptr<const Mesh> mesh{};
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

  // ── 缝隙遮蔽（AO，2026-11-06）──
  //
  // 见 `compute_occlusion` 的说明：它是真 GI 的便宜近似，只乘间接光。
  /// AO 是否启用（默认**关**：像素级基准不能开它）。
  bool occlusion_enabled_{false};
  /// 遮蔽强度（0 = 无衰减，1 = 全黑）。
  float occlusion_strength_{0.7f};
  /// 射线长度（世界单位）：决定“多大范围的接触会被算成遮蔽”。
  float occlusion_radius_{0.5f};
  /// 半球采样数（确定性黄金角螺旋；越大越平滑也越慢）。
  static constexpr int kOcclusionSamples{16};
  /// 射线起点偏移：避开自相交（否则每个像素都撞到自己所在三角形）。
  static constexpr float kOcclusionBias{0.01f};
  /// 缓存格尺寸相对于 `radius` 的倍数。
  ///
  /// 为何是 0.5：AO 随距离的变化尺度就是 `radius` 本身，所以半个半径的
  /// 格子已经足够细（更大的格子会丢接触阴影的细节）。
  static constexpr float kOcclusionCacheStep{0.5f};
  /// 缓存条目上限（防止病态场景把内存吃光）。
  static constexpr std::size_t kOcclusionCacheLimit{400000};
  /// AO 空间缓存：键 = 量化世界位置 + 量化法线。
  /// 每帧清空（`begin_frame`），所以不同帧不会串。
  std::map<std::array<std::int32_t, 5>, float> occlusion_cache_{};
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
[[nodiscard]] auto gpu_scene3d_unavailable_reason() -> std::string;
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
///
/// ## 轮次为何是 1+2 而不是 1+5（2026-10-31 实测）
///
/// 旧实现是 1 次预热 + 5 次计时 = **6 次全屏渲染**，两条腿各跑一遍就是 12 次。
/// 对软件腿，一次全屏 `end_frame` 在 800×450 上要 **176 ms**，于是选腿探测本身
/// 要花 **2.87 秒**（实测：自动=3.84s vs 强制 gpu=0.97s）。
///
/// 这笔开销对短片段是灾难：2 帧的片子，整次导出 3.84 秒里有 2.9 秒是“为了
/// 决定要不要用显卡而画了 12 帧”。
///
/// 而多轮的意义在这里很小——两条腿的差距是 **10 倍量级**（176 ms vs 17.8 ms），
/// 不是 10%。best-of-2 足以定下方向，再多的轮次只是把噪声抑平到远低于
/// 判定阈值（`software_ms > gpu_ms * 1.25`）。
///
/// 代价（如实记录）：臂环境噪声大时可能选错腿。选错的代价是“慢几倍”，
/// 而旧实现选对的代价是“每次都慢 2.9 秒”——后者在任何长度的片子上都要付。
[[nodiscard]] auto probe_leg_ms(Scene3D& scene) -> double {
  // ⚠ 目标画布的尺寸**不决定**成本：软件腿的 `begin_frame` 清的是自己的
  // `canvas_` 与 `depth_`（`Scene3D::create` 时按场景尺寸分配），
  // 传一个小的 target 进来并不会少算一个像素。要降成本只能缩**场景**本身。
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
  for (int round = 0; round < 2; ++round) {
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
  // ## 整函数串行化（2026-10-31：这是 4 路并行卡死的根因）
  //
  // `backend_mutex()` 本来就存在（声明在头里，注释写着"进程级互斥"），
  // 但**全仓库没有任何调用者**——声明的意图从未接上。
  //
  // 后果：4 个 worker 同时进到这里，并发做下面这些事：
  //   · `create_gpu_scene3d`（建 D3D 资源、编译着色器）
  //   · `probe_leg_ms`（共享 `g_context_mutex()` 的立即上下文）
  //   · `create_software_scene3d`
  //
  // 实测（1920×1080 / 4 worker）：日志停在“选 GPU 腿”之后**再无输出**，
  // 进程挂死，且没有 `0x887A0005`——是死锁不是设备丢失。
  // 而 1/2 worker 都正常，是这个判断的最强旁证（并发才触发）。
  //
  // 锁的代价：只在建场景时拿一次（每 worker 一次，不是每帧）。
  // ## 锁只包"建实例"，**不包"探测选腿"**（2026-11-01：这是并行死锁的真因）
  //
  // 上一版把整个 `create` 罩在 `backend_mutex()` 下，想修"并发建 D3D 资源"。
  // 但那只是把崩溃换成了死锁——实测 4 路并行 8 次里 1~2 次永久挂死，
  // 且**四个 worker 同时卡住**（诊断显示都在 stage1=渲染中，next_index 停住）。
  //
  // 根因是 ABBA：
  //   · 本函数持 `backend_mutex` → 调 `probe_leg_ms` → `begin_frame` → 要 `g_context_mutex`
  //   · 别的 worker 正在渲染、持着 `g_context_mutex`，而它所属线程之后又要 `backend_mutex`
  // 两个方向各拿一把，谁都不放。
  //
  // 实测判定：把这一行注释掉，8/8 全部通过、0 卡死。加回来就复现。
  //
  // 修法：锁的**粒度**缩到只覆盖实例创建；探测（会碰 GPU 上下文）放到锁外。
  // 这样持 `backend_mutex` 期间**绝不**去要 `g_context_mutex`，环就断了。
  // ## 不用 `backend_mutex()`（2026-11-01：它自己就是死锁源）
  //
  // 那把锁的意图是"进程级串行建场景"，但它和 `g_context_mutex()` 构成 ABBA：
  // 本函数持 `backend_mutex` 去探测（要 `g_context_mutex`），
  // 而渲染线程持 `g_context_mutex` 时所属线程又要 `backend_mutex`。
  //
  // 实测三组对照：
  //   · 持锁 + 探测在锁内 → 8 次里 1~2 次死锁（4 个 worker 全卡在渲染中）
  //   · 持锁 + 探测移出锁 → 仍有死锁（还有别的持锁路径要 GPU）
  //   · **完全不持这把锁 → 8/8 全过，0 卡死**
  //
  // 那"并发建 D3D 资源"怎么防？`create_gpu_scene3d` 内部本来就拿
  // `g_context_mutex()`（所有 D3D 调用都要它），串行化已经由那把锁保证。
  // 这把额外的锁是重复保护，而且引入了一个它自己解决不了的环。
  //
  // `backend_mutex()` 保留在头里不删：第三方可能拿它做别的用途，
  // 但本函数不再参与——注释在此，免得以后有人"顺手加回来"。
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
  //
  // ## 为什么"不可用"必须记日志
  //
  // 旧实现在 GPU 腿建不起来时**静默**落到软件腿。后果是：一台 RTX 4080
  // 在跑纯 CPU 光栅化（实测 3.2 秒/帧），而日志里一个字都没有——
  // 没人会想到"显卡明明有，为什么这么慢"。
  //
  // GPU 不可用是**异常状态**（用户买了显卡就是为了用），不是正常的降级路径，
  // 所以这里必须让调用方看得见原因。
  if (!gpu_scene3d_available()) {
    ST_LOG_INFO("三维：GPU 腿不可用（{}）→ 用软件腿", gpu_scene3d_unavailable_reason());
  }
  if (gpu_scene3d_available()) {
    auto gpu = create_gpu_scene3d(width, height);
    if (!gpu.has_value()) {
      ST_LOG_WARN("三维：GPU 腿创建失败（{}）→ 回退软件腿", gpu.error().message);
    }
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
      //
      // ## 探测用**缩小**的场景实例（2026-10-31 实测：这曾占了 2.1 秒）
      //
      // 旧实现直接拿全尺寸实例（`width × height`）去量。而软件腿的
      // `begin_frame` 清的是它自己的 `canvas_`/`depth_`（按场景尺寸分配），
      // 传小目标进来完全不省成本——于是 800×450 上一次探测要 **525 ms**。
      //
      // 1 次预热 + 2 轮 × 两条腿 ≈ 6 次全屏渲染 → `Scene3D::create` 实测
      // **2100 ms**。对 2 帧的短片，整次导出 3.85 秒里有 2.1 秒花在
      // “为了决定用哪条腿而画了 6 帧”。
      //
      // 修法：**探测实例单独建小尺寸**（1/4 边长 = 1/16 像素）。
      // 两条腿的耗时都随像素数线性变化（实测：面积 100%/50%/25% → 107/44/14 ms），
      // 所以比值在降采样后保持不变，而成本降到约 1/16。
      //
      // 注意：返回给调用方的仍是**全尺寸**实例，探测实例用完即弃。
      const int probe_w = std::max(64, width / 4);
      const int probe_h = std::max(36, height / 4);
      auto gpu_probe = create_gpu_scene3d(probe_w, probe_h);
      auto software_probe = create_software_scene3d(probe_w, probe_h);
      if (gpu_probe.has_value() && software_probe.has_value()) {
        const double gpu_ms = probe_leg_ms(**gpu_probe);
        const double software_ms = probe_leg_ms(**software_probe);
        if (software_ms > 0.0 && gpu_ms > software_ms * 1.25) {
          ST_LOG_INFO("三维：{}×{} 实测软件 {:.2f} ms vs GPU {:.2f} ms → 选软件腿（探测于 {}×{}）",
                      width, height, software_ms, gpu_ms, probe_w, probe_h);
          return create_software_scene3d(width, height);
        }
        ST_LOG_INFO("三维：{}×{} 实测软件 {:.2f} ms vs GPU {:.2f} ms → 选 GPU 腿（探测于 {}×{}）",
                    width, height, software_ms, gpu_ms, probe_w, probe_h);
      }
      return gpu;
    }
    // GPU 腿建不出来（着色器/深度目标失败）：如实记原因再落软件
    ST_LOG_WARN("三维：GPU 腿不可用（{}），已回退软件光栅器", gpu.error().message);
  }
  return create_software_scene3d(width, height);
}

}  // namespace st::raster



