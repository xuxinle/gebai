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
auto shade_terms(math::Vec3 normal, math::Vec3 color, const Lighting& lighting) -> math::Vec3 {
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
  return math::Vec3{clamp01(color.x * gain), clamp01(color.y * gain), clamp01(color.z * gain)};
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
    view_ = math::Mat4::look_at(camera.eye, camera.target, camera.up);
    projection_ = math::Mat4::perspective(fov, aspect, camera.near_z, camera.far_z);
    mvp_ready_ = true;
  }

  void set_lighting(const Lighting& lighting) override {
    lighting_ = lighting;
    tone_map_ = lighting.tone_map;
  }

 void set_cull_mode(CullMode mode) override { cull_mode_ = mode; }

 void set_material(const Material& material) override {
    material_ = material;
    // opacity < 1 时必须关深度写：否则后面的透明面会被它挡住，
    // 表现为“里面的东西全消失”（而且只在透明物体上出错，很难联想到深度写）。
    if (material_.opacity < 0.999f) material_.depth_write = false;
    material_.opacity = std::clamp(material_.opacity, 0.0f, 1.0f);
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

  void draw_mesh(const Mesh& mesh, const math::Mat4& model) override {
    if (mesh.is_empty() || !mvp_ready_) return;
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
    ++draw_calls_;
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

        // 透视校正：顶点属性按 1/w 加权插值（不做校正会让纹理/光照在斜面上"歪"）
        const float sum = w0 * p0.w + w1 * p1.w + w2 * p2.w;
        const float i0 = (sum > 1e-9f) ? (w0 * p0.w) / sum : w0;
        const float i1 = (sum > 1e-9f) ? (w1 * p1.w) / sum : w1;
        const float i2 = 1.0f - i0 - i1;
        const math::Vec3 normal = a.normal * i0 + b.normal * i1 + c.normal * i2;
        math::Vec3 color = a.color * i0 + b.color * i1 + c.color * i2;
        // 有 UV 且已设纹理 → 采样（**双线性**，与纹理坐标一起透视校正）。
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
        const math::Vec3 lit = shade_terms(normal, color, lighting_);
        // ## 输出链：线性 → 曝光 → 色调映射 → sRGB → 8bit
        //
        // 修之前这里直接 `lit.x * 255`，两处错（都“看不出哪里错、只觉得不对”）：
        //   1. **线性值当 sRGB 存**：0.5 线性 ≈ 0.735 sRGB，整幅系统性偏暗；
        //   2. **先 clamp 再存**：HDR 被截断在 1，高光/日光糊成一片白。
        //
        // `shade_terms` 仍返回**线性值**（两条腿共用的公式不变），
        // 颜色变换集中在这里——顺序写错很难定位（看起来像“配色不对”）。
        if (opacity >= 0.999f) {
          canvas_.set_pixel(x, y, st::raster::encode_pixel(lit, 1.0f, tone_map_));
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
  bool mvp_ready_{false};
  Lighting lighting_{};
 /// 色调映射算子（从 `Lighting::tone_map` 同步过来）。
 ///
 /// 为何单独存一份：每帧重置为默认光照时（`begin_frame`），码里会
 /// 重建 `lighting_`；把它放在同一个结构里就只靠 `set_lighting` 传递，
 /// 不会两处不同步。默认值必须与 `Lighting::tone_map` 一致。
 ToneMap tone_map_{ToneMap::Aces};
 /// 面朝向剔除（默认剔除背向面）。
 CullMode cull_mode_{CullMode::Back};
 /// 当前材质（opacity < 1 → 三角形进透明队列）。
 Material material_{};
 /// 透明三角形延迟队列：`begin_frame` 清、`draw_mesh` 收集、`end_frame` 前排序并绘制。
 ///
 /// 为何要延迟：正确的透明混合要求**远→近**绘制，而 `draw_mesh` 是按调用顺序
 /// 提交的（与远近无关）。收集后统一排序是唯一能保证正确顺序的做法。
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
