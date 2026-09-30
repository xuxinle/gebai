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
#include <vector>

#include "st/core/log.hpp"
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

/// 变换后的顶点：裁剪空间位置 + 世界法线 + 顶点色。
struct Vertex {
  Vec4 clip{};
  math::Vec3 normal{};
  math::Vec3 color{};
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

/// 与 GPU 路径**同一套光照公式**（Lambert + 环境 + 边缘光）。
///
/// 两条腿（软件/GPU）必须在同一场景上给出接近的画面，否则"软件保底"
/// 就只是"另一种长相"。公式写在这里与 GPU 侧着色器（`platform_d3d11.cpp`）保持一致。
[[nodiscard]] auto shade(math::Vec3 normal, math::Vec3 color) noexcept -> math::Vec3 {
  const math::Vec3 n = normal.normalized();
  const math::Vec3 light = math::Vec3{0.4f, 0.8f, 0.6f}.normalized();
  const float diffuse = std::max(n.dot(light), 0.0f);
  const float ambient = 0.35f;
  const float rim = std::pow(1.0f - std::max(n.z, 0.0f), 2.0f) * 0.25f;
  const float scale = ambient + diffuse * 0.75f;
  return math::Vec3{std::min(std::max(color.x * scale + rim, 0.0f), 1.0f),
                    std::min(std::max(color.y * scale + rim, 0.0f), 1.0f),
                    std::min(std::max(color.z * scale + rim, 0.0f), 1.0f)};
}

class SoftwareScene final : public Scene3D {
 public:
  SoftwareScene(int width, int height)
      : width_(std::max(1, width)), height_(std::max(1, height)),
        canvas_(width_, height_), depth_(static_cast<std::size_t>(width_) *
                                         static_cast<std::size_t>(height_), 0.0f) {}

  void begin_frame(math::Color clear_color) override {
    canvas_.clear(clear_color);
    std::fill(depth_.begin(), depth_.end(), 1.0f);   // 1.0 = 远平面
    draw_calls_ = 0;
  }

  void set_camera(const Camera& camera) override {
    const float aspect = height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_)
                                     : 1.0f;
    const float fov = camera.fov_y_degrees * 3.14159265358979f / 180.0f;
    view_ = math::Mat4::look_at(camera.eye, camera.target, camera.up);
    projection_ = math::Mat4::perspective(fov, aspect, camera.near_z, camera.far_z);
    mvp_ready_ = true;
  }

  void draw_mesh(const Mesh& mesh, const math::Mat4& model) override {
    if (mesh.is_empty() || !mvp_ready_) return;
    const math::Mat4 mvp = projection_ * view_ * model;
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
      }
      if (!ok) continue;

      Vertex clipped[4];
      const int produced = clip_near(tri[0], tri[1], tri[2], clipped);
      // 裁剪后是凸多边形，按扇形拆回三角形
      for (int k = 1; k + 1 < produced; ++k) {
        rasterize(clipped[0], clipped[k], clipped[k + 1]);
      }
    }
    ++draw_calls_;
  }

  void end_frame(Surface& target, math::Rect destination, float opacity = 1.0f) override {
    // 结果作为**一张图**交给目标画布（与 2D 原语同一条合成路径）。
    // 相比原先 GL 路径的 `glReadPixels` + 逐像素转换，这里没有跨 API 往返。
    (void)opacity;
    target.draw_canvas(canvas_, destination);
  }

  [[nodiscard]] auto width() const noexcept -> int override { return width_; }
  [[nodiscard]] auto height() const noexcept -> int override { return height_; }
  [[nodiscard]] auto draw_calls() const noexcept -> std::uint64_t override { return draw_calls_; }

 private:
  /// 屏幕空间三角形光栅化：重心坐标 + z-buffer + 透视校正插值 + 逐像素光照。
  void rasterize(const Vertex& a, const Vertex& b, const Vertex& c) {
    // 透视除法 → NDC → 屏幕（y 向下，与画布一致）
    const auto to_screen = [&](const Vertex& v) {
      const float inv_w = 1.0f / v.clip.w;
      return Vec4{(v.clip.x * inv_w * 0.5f + 0.5f) * static_cast<float>(width_),
                  (0.5f - v.clip.y * inv_w * 0.5f) * static_cast<float>(height_),
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
    const int max_x = std::min(width_ - 1, static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
    const int max_y = std::min(height_ - 1, static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));
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
        const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                                   static_cast<std::size_t>(x);
        if (z >= depth_[offset]) continue;    // z-buffer：比已画的更远 → 丢弃
        depth_[offset] = z;

        // 透视校正：顶点属性按 1/w 加权插值（不做校正会让纹理/光照在斜面上"歪"）
        const float sum = w0 * p0.w + w1 * p1.w + w2 * p2.w;
        const float i0 = (sum > 1e-9f) ? (w0 * p0.w) / sum : w0;
        const float i1 = (sum > 1e-9f) ? (w1 * p1.w) / sum : w1;
        const float i2 = 1.0f - i0 - i1;
        const math::Vec3 normal = a.normal * i0 + b.normal * i1 + c.normal * i2;
        const math::Vec3 color = a.color * i0 + b.color * i1 + c.color * i2;
        const math::Vec3 lit = shade(normal, color);
        canvas_.set_pixel(x, y, math::Color{static_cast<std::uint8_t>(std::lround(lit.x * 255.0f)),
                                            static_cast<std::uint8_t>(std::lround(lit.y * 255.0f)),
                                            static_cast<std::uint8_t>(std::lround(lit.z * 255.0f)),
                                            255});
      }
    }
  }

  int width_{1};
  int height_{1};
  Canvas canvas_;
  std::vector<float> depth_{};
  math::Mat4 view_{math::Mat4::identity()};
  math::Mat4 projection_{math::Mat4::identity()};
  bool mvp_ready_{false};
  std::uint64_t draw_calls_{0};
};

}  // namespace

auto Scene3D::available() noexcept -> bool { return true; }   // 软件腿：永远可用

auto Scene3D::create(int width, int height) -> Result<std::unique_ptr<Scene3D>> {
  if (width <= 0 || height <= 0) return unexpected(ErrorCode::Invalid, "渲染尺寸必须为正");
  return std::unique_ptr<Scene3D>(std::make_unique<SoftwareScene>(width, height));
}

}  // namespace st::raster
