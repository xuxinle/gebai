#pragma once

/// `GlView`：把 OpenGL 三维场景嵌进 2D 界面（一个普通组件）。
///
/// 为什么做成组件而不是"另一个应用"：三维内容在界面里几乎总是**一块区域**
/// （模型预览、数据可视化、产品展示），而不是整屏。做成组件意味着它可以被布局、
/// 被裁剪、跟其它组件叠放——这正是 2D 框架已经解决的事。
///
/// 实现要点：`paint_content` 里把 GL 场景渲染到离屏 FBO 再合成到给定矩形；
/// 旋转动画靠 `context.time_seconds` 推进（与开关/悬浮同一套做法，无需外部 tick）。

#include <memory>
#include <string>
#include <vector>

#include "st/math/matrix.hpp"
#include "st/raster/scene3d.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

/// 展示用网格类型。
enum class GlShape : std::uint8_t { Cube, Sphere };

class GlView : public Element {
 public:
  explicit GlView(GlShape shape = GlShape::Cube);
  ~GlView() override;
  GlView(const GlView&) = delete;
  auto operator=(const GlView&) -> GlView& = delete;

  void set_shape(GlShape shape);
  /// 显示一个**外部网格**（如 OBJ 加载结果）。设为非空后 `shape` 不再生效。
  ///
  /// 为什么用 `shared_ptr<const Mesh>`：网格可能很大且由调用方缓存复用
  /// （同一个模型显示在多处），拷贝一份等于白白复制几万个顶点。
  void set_mesh(std::shared_ptr<const raster::Mesh> mesh);
  [[nodiscard]] auto mesh() const noexcept -> const std::shared_ptr<const raster::Mesh>& {
    return mesh_;
  }
  [[nodiscard]] auto shape() const noexcept -> GlShape { return shape_; }
  /// 自动旋转（默认开）。关掉用于"定格看一个角度"的场合。
  void set_spin(bool enabled) noexcept { spin_ = enabled; }
  /// 每秒旋转弧度。
  void set_spin_speed(float radians_per_second) noexcept { speed_ = radians_per_second; }
  /// 底色（GL 场景的清屏色）。
  void set_background(math::Color color) noexcept { background_ = color; }
  /// 视口高度：三维视图没有"内容高度"，必须显式给（否则会塌成 0 高）。
  void set_preferred_height(float height) noexcept {
    preferred_height_ = height > 0.0f ? height : 1.0f;
  }

  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

  [[nodiscard]] auto semantics_text() const -> std::string override { return "3D 视图"; }
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  /// 三维是否可用（不可用时组件画一块占位提示，而不是静默空白）。
///
/// 注意它**不再是"OpenGL 是否可用"**：三维能力走平台中立接口
/// （`raster::Scene3D`），软件实现永远可用——这正是"软件保底跨平台"的落地。
  [[nodiscard]] static auto rendering_ready() noexcept -> bool;

 private:
  GlShape shape_{GlShape::Cube};
  bool spin_{true};
  float speed_{0.6f};
  math::Color background_{0x11, 0x16, 0x22, 0xFF};
  float preferred_height_{180.0f};

  /// GL 资源按需创建（构造时建会在"没有 GL 的机器"上让整个界面建不起来）
  std::shared_ptr<const raster::Mesh> mesh_{};
  mutable std::unique_ptr<raster::Scene3D> scene_{};
  mutable float angle_{0.0f};
  mutable double last_time_{-1.0};
  mutable std::uint64_t frames_{0};
};

}  // namespace st::ui
