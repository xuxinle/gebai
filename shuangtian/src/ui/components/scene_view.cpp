#include "st/ui/components/scene_view.hpp"

#include <cmath>
#include <cstdio>
#include <format>

#include "st/core/log.hpp"

namespace st::ui {

namespace {

/// 网格只在进程里生成一次并共享：生成是纯计算、结果不可变，
/// 每个视图各建一份纯属浪费（单是球体 28 段就有几百个顶点）。
[[nodiscard]] auto shared_shape_mesh(SceneShape shape) -> const raster::Mesh& {
  static const raster::Mesh cube = raster::Mesh::cube(1.3f);
  static const raster::Mesh sphere = raster::Mesh::sphere(0.85f, 28);
  return shape == SceneShape::Sphere ? sphere : cube;
}

}  // namespace

SceneView::SceneView(SceneShape shape) : shape_(shape) {
  // 三维视图是内容型组件：不参与焦点，但需要自己的绘制区域
  style_.radius = 10.0f;
  style_.background = math::Color{0, 0, 0, 0};
}

SceneView::~SceneView() = default;

auto SceneView::rendering_ready() noexcept -> bool { return raster::Scene3D::available(); }

void SceneView::set_mesh(std::shared_ptr<const raster::Mesh> mesh) {
  mesh_ = std::move(mesh);
  mark_dirty();
}

void SceneView::set_shape(SceneShape shape) {
  if (shape_ == shape) return;
  shape_ = shape;
  mark_dirty();
}

void SceneView::measure(const RenderContext& context, const Constraints& constraints) {
  (void)context;
  const float width = constraints.max_width > 0.0f ? constraints.max_width : 320.0f;
  measured_ = math::Size{width, preferred_height_};
}

void SceneView::arrange(const RenderContext& context, math::Rect rect) {
  (void)context;
  bounds_ = rect;
}

void SceneView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const math::Rect box = bounds_.inset(style_.padding);
  if (box.is_empty()) return;

  // GL 不可用：画一块**说明性占位**，而不是静默空白。
  // "什么都没显示"是最难排查的状态——用户会以为是布局问题，实际是能力缺失。
  if (!raster::Scene3D::available()) {
    canvas.fill_rect(box, raster::Paint::solid(context.theme.colors().surface_sunken),
                     style_.radius);
    return;
  }

  const int width = std::max(1, static_cast<int>(std::lround(box.width * canvas.device_scale())));
  const int height = std::max(1, static_cast<int>(std::lround(box.height * canvas.device_scale())));

  // 场景按需创建；尺寸变了就重建（GL 的 FBO 尺寸不可原地改）
  if (scene_ == nullptr || scene_->width() != width || scene_->height() != height) {
    auto created = raster::Scene3D::create(width, height);
    if (!created.has_value()) {
      log::warn("SceneView：创建 3D 场景失败（{}）", created.error().message);
      scene_.reset();
      canvas.fill_rect(box, raster::Paint::solid(context.theme.colors().surface_sunken),
                       style_.radius);
      return;
    }
    scene_ = std::move(*created);
  }

  // 时间推进：静态帧（首帧/离屏单帧/测试截图）不转，避免"截图每次角度都不同"导致回归不稳
  const double now = context.time_seconds;
  const bool advancing = last_time_ >= 0.0 && now > last_time_;
  if (spin_ && advancing) {
    angle_ += speed_ * static_cast<float>(now - last_time_);
    if (angle_ > 6.2831853f) angle_ -= 6.2831853f;
    request_animation();   // 走统一的续帧协议（由 UiRoot 汇总决定帧预算）
  }
  last_time_ = now;

  raster::Camera camera;
  camera.eye = math::Vec3{2.0f, 1.6f, 2.4f};
  camera.target = math::Vec3{0.0f, 0.0f, 0.0f};
  camera.fov_y_degrees = 42.0f;

  scene_->begin_frame(background_);
  scene_->set_camera(camera);
  // 外部网格优先于内置形状：调用方给了模型就画模型
  static const raster::Mesh kEmpty{};
  const raster::Mesh& mesh = mesh_ != nullptr ? *mesh_ : shared_shape_mesh(shape_);
  if (!mesh.is_empty()) {
    scene_->draw_mesh(mesh, math::Mat4::rotation(math::Vec3{0.0f, 1.0f, 0.0f}, angle_));
  }
  (void)kEmpty;
  scene_->end_frame(canvas, box);
  ++frames_;  // （调试期遗留的逐帧 fprintf 已移除：三维视图常驻动画，逐帧刷 stderr 会淹日志并拖慢软件腿）
}

auto SceneView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "shape") return shape_ == SceneShape::Sphere ? "sphere" : "cube";
  if (name == "spin") return spin_ ? "true" : "false";
  if (name == "spin_speed") return std::format("{:.3f}", static_cast<double>(speed_));
  if (name == "scene_frames") return std::format("{}", frames_);
  if (name == "mesh_vertices") {
    return std::format("{}", mesh_ != nullptr ? mesh_->vertex_count() : 0U);
  }
  if (name == "scene_ready") {
    return raster::Scene3D::available() ? "true" : "false";
  }
  return std::nullopt;
}

auto SceneView::set_property(std::string_view name, std::string_view value) -> bool {
  if (name == "shape") {
    if (value == "sphere") set_shape(SceneShape::Sphere);
    else if (value == "cube") set_shape(SceneShape::Cube);
    else return false;
    return true;
  }
  if (name == "spin") {
    spin_ = value == "true" || value == "1";
    mark_dirty();
    return true;
  }
  if (name == "spin_speed") {
    try {
      speed_ = std::stof(std::string(value));
    } catch (...) {
      return false;   // 非法输入不静默接受（调用方会看到 changed: []）
    }
    mark_dirty();
    return true;
  }
  return false;
}

auto SceneView::property_names() const -> std::vector<std::string_view> {
  return {"shape", "spin", "spin_speed", "scene_frames", "scene_ready", "mesh_vertices"};
}

}  // namespace st::ui
