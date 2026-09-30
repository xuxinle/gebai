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

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {

class Surface;

/// 相机（右手系，视线朝 -Z）。
struct Camera {
  math::Vec3 eye{0.0f, 0.0f, 4.0f};
  math::Vec3 target{0.0f, 0.0f, 0.0f};
  math::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_y_degrees{45.0f};
  float near_z{0.1f};
  float far_z{100.0f};
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

  /// 开始一帧：设视口、清色与深度。
  virtual void begin_frame(math::Color clear_color) = 0;
  virtual void set_camera(const Camera& camera) = 0;
  /// 画一个网格（可带模型变换）。同一个网格重复绘制会复用已上传的缓冲。
  virtual void draw_mesh(const Mesh& mesh, const math::Mat4& model) = 0;
  /// 结束一帧并把结果**合成**到 `target` 的目的矩形（逻辑坐标）。
  virtual void end_frame(Surface& target, math::Rect destination, float opacity = 1.0f) = 0;

  /// 渲染尺寸（物理像素）。
  [[nodiscard]] virtual auto width() const noexcept -> int = 0;
  [[nodiscard]] virtual auto height() const noexcept -> int = 0;
  /// 本场景的绘制调用计数（用于确认"确实画了东西"而不是静默空帧）。
  [[nodiscard]] virtual auto draw_calls() const noexcept -> std::uint64_t = 0;
};

}  // namespace st::raster
