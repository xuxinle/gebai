#pragma once

/// OpenGL 3D 渲染：把真三维内容画进画布（离屏 FBO → 回读 → 合成）。
///
/// **源码供给**：加载器（glad 单头）不在仓库里——由 `tools/fetch_opengl.{ps1,sh}` 在本机
/// 拉取到 `third_party/opengl/`（已在 `.gitignore`）。接口在仓库、生成物在机器上：
/// 33 万行生成代码进 review 只会让人无从看起，且它随生成器与驱动需求漂移。
/// 没拉取时本模块整块编不进来（`has_opengl()` / `available()` 如实报 false），
/// 框架其余部分照常可用——**缺能力要能被识别，而不是编不过**。
///
/// 与软件光栅器/D3D11 的关系：这不是"又一个 2D 后端"，而是**补上 2D 做不到的事**
/// （透视投影、深度缓冲、逐像素光照、任意网格）。2D 界面仍走 Surface；
/// 需要真三维的地方调这里，渲染结果再合成回画布。

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/path.hpp"

namespace st::raster {

class Surface;

namespace gl {

/// 本机是否有 OpenGL 加载器（编译期）——没有源码时整个模块被编译排除。
[[nodiscard]] auto has_opengl() noexcept -> bool;

/// 本机是否**可用** OpenGL（编译期有源码 + 运行期建成上下文）。
[[nodiscard]] auto available() noexcept -> bool;

/// 驱动/实现信息。如实上报，用于"这一帧到底谁画的"这类问题。
struct DeviceInfo {
  std::string vendor{};    ///< 例如 "NVIDIA Corporation"
  std::string renderer{};  ///< 例如 "GeForce RTX 4080 SUPER/PCIe/SSE2"
  std::string version{};   ///< 例如 "4.6.0 NVIDIA 560.94"
  std::string glsl{};      ///< 例如 "4.60 NVIDIA"
  int max_texture_size{0};
  long long vram_mb{0};    ///< 可能为 0（扩展不支持时）
};

/// 探测（失败给出**具体原因**：没源码 / 没建成上下文 / 没驱动）。
[[nodiscard]] auto probe() -> Result<DeviceInfo>;

/// 相机（右手系，视线朝 -Z）。
struct Camera {
  math::Vec3 eye{0.0f, 0.0f, 4.0f};
  math::Vec3 target{0.0f, 0.0f, 0.0f};
  math::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_y_degrees{45.0f};
  float near_z{0.1f};
  float far_z{100.0f};
};

/// 网格：交错顶点（位置 3 + 法线 3 + 顶点色 3）。索引用 32 位。
///
/// 为什么带顶点色：光照之外还需要"材质感"（每个面不同色才能看出朝向），
/// 而引入贴图会拉进纹理管线与资源管理——当前范围不需要。
struct Mesh {
  std::vector<float> vertices{};   ///< 每顶点 9 个 float
  std::vector<std::uint32_t> indices{};

  [[nodiscard]] auto vertex_count() const noexcept -> std::size_t {
    return vertices.size() / 9U;
  }
  [[nodiscard]] auto is_empty() const noexcept -> bool {
    return vertices.empty() || indices.empty();
  }

  /// 立方体（边长 `size`，各面不同色：一眼能看出朝向）。
  [[nodiscard]] static auto cube(float size = 1.0f) -> Mesh;
  /// 球（经纬网格，`segments` 越大越圆）。
  [[nodiscard]] static auto sphere(float radius = 0.6f, int segments = 24) -> Mesh;
  /// 圆角长方体（"复杂图形"的最小样例：把三轴半径当参数）。
  [[nodiscard]] static auto box(float x, float y, float z) -> Mesh;
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
  auto operator=(const Scene3D&) = delete;

  /// 建离屏场景（尺寸为**物理像素**）。失败给出具体原因（无上下文/FBO 不完整/着色器编译错误）。
  [[nodiscard]] static auto create(int width, int height) -> Result<std::unique_ptr<Scene3D>>;

  /// 开始一帧：设视口、清色与深度。
  virtual void begin_frame(math::Color clear_color) = 0;
  virtual void set_camera(const Camera& camera) = 0;
  /// 画一个网格（可带模型变换）。同一个网格重复绘制会复用已上传的缓冲。
  virtual void draw_mesh(const Mesh& mesh, const math::Mat4& model) = 0;
  /// 填充一条 2D 路径（**屏幕空间**，坐标 = 物理像素，原点左上）。
  ///
  /// 实现是 GPU 上标准的**模板缓冲法**，不是三角化：
  /// 先把每条轮廓以 `INCR_WRAP`(正面) / `DECR_WRAP`(背面) 累加到模板缓冲，
  /// 再画一个覆盖包围盒的四边形、只在模板 ≠ 0 处着色。
  ///
  /// 为什么选它而不是耳切三角化：
  /// - **语义与软件光栅器一致**（非零环绕）：自交、重叠轮廓的结果与 CPU 路径完全相同，
  ///   而耳切只对简单多边形有效，还得额外处理洞与自交；
  /// - 轮廓只需**扇形展开**（无需真正三角化），代码量与出错面都小得多。
  virtual void fill_path(const Path& path, math::Color color) = 0;

  /// 描边一条 2D 路径：先按 `width` 转成轮廓（与软件光栅器同一个 `stroke_to_path`），
  /// 再用同一个模板填充。这样"描边"与"填充"共享一套正确性。
  virtual void stroke_path(const Path& path, float width, math::Color color) = 0;

  /// 结束一帧并把结果**合成**到 `target` 的目的矩形（源像素做预乘、翻 Y）。
  virtual void end_frame(Surface& target, math::Rect destination,
                         float opacity = 1.0f) = 0;

  /// 渲染尺寸（物理像素）。
  [[nodiscard]] virtual auto width() const noexcept -> int = 0;
  [[nodiscard]] virtual auto height() const noexcept -> int = 0;
  /// 本场景的绘制调用计数（用于确认"确实画了东西"而不是静默空帧）。
  [[nodiscard]] virtual auto draw_calls() const noexcept -> std::uint64_t = 0;
};

}  // namespace gl
}  // namespace st::raster
