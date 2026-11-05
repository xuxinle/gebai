#include "st/ui/components/three_view.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/raster/bpy_runtime.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh_ops.hpp"
#include "st/raster/pick.hpp"
#include "st/ui/text_port.hpp"

namespace st::ui {

namespace {

constexpr float kPi = 3.14159265358979f;
/// 俯仰的硬限（±89°）。再往上 `up` 与视线共线，`look_at` 会退化、画面翻转。
constexpr float kMaxPitch = 89.0f * kPi / 180.0f;
/// 距离下限：必须明显大于 near_z，否则近裁剪面会把物体切掉。
constexpr float kMinDistance = 0.15f;
/// 判定"拖拽"而不是"点击"的位移阈值（逻辑像素）。
constexpr float kDragThreshold = 3.0f;

/// 世界包围盒（用于 `frame_scene`）。
auto scene_bounds(const raster::ThreeScene& scene) -> raster::Bounds {
  raster::Bounds box = raster::Bounds::empty();
  for (const auto& object : scene.objects) {
    if (object.mesh.is_empty()) continue;
    // ## 跳過声明了 `noFrame` 的物体
    //
    // 背景板/地面板这类“氛围件”通常比主体大一圈，把它们算进包围盒
    // 会让取景距离白白拉远——实测：表的直径 40，而一块半径 22.4 的
    // 背景盘把距离从 86 拉到 120，主体在画面里只剩一半大。
    //
    // 场景里写 `mesh.userData.noFrame = true` 即可（与 spin/oscillate 同一套习惯）。
    if (object.no_frame) continue;
    const auto pick = raster::make_pick_object(object.name, object.mesh, object.world);
    if (pick.world_bounds.is_empty()) continue;
    box = box.union_with(pick.world_bounds);
  }
  return box;
}

}  // namespace

// ───────────────────────────── OrbitCameraState ─────────────────────────────

auto OrbitCameraState::to_camera() const noexcept -> raster::Camera {
  raster::Camera camera;
  // 球坐标 → 直角坐标（yaw=0 时相机在 +Z 侧，pitch>0 在目标点上方）
  const float cos_pitch = std::cos(pitch);
  const float offset_x = distance * cos_pitch * std::sin(yaw);
  const float offset_y = distance * std::sin(pitch);
  const float offset_z = distance * cos_pitch * std::cos(yaw);
  camera.eye = math::Vec3{target.x + offset_x, target.y + offset_y, target.z + offset_z};
  camera.target = target;
  camera.up = math::Vec3{0.0f, 1.0f, 0.0f};
  camera.fov_y_degrees = fov_y_degrees;
  camera.near_z = near_z;

  // ## 远平面必须包住“距离 + 场景深度”
  //
  // 这是本会话最刺眼的一个缺陷（花了很久才找到）：`far_z` 默认 100，
  // 而 `frame_scene()` 对一个大场景算出的距离可能远超过 100——
  // 于是**整个场景落在远裁剪面之外，全部被裁掉**。
  //
  // 症状极具欺骗性：物体数/网格（51.8 万顶点）/矩阵/绘制次数全部正常，
  // `three_check`（它用场景自带的相机）渲染同一份场景也完全正常，
  // 只有视口里一片背景色。相机参数不报出来，就永远想不到往这看。
  //
  // 做法：`far` 至少给“距离 + 一定余量”，且不低于用户设定的 far（保留其语义）。
  float far = far_z;
  const float needed = distance * 1.6f + 8.0f;
  if (far < needed) far = needed;
  camera.far_z = far;
  return camera;
}

void OrbitCameraState::clamp() noexcept {
  pitch = std::clamp(pitch, -kMaxPitch, kMaxPitch);
  distance = std::max(distance, kMinDistance);
  fov_y_degrees = std::clamp(fov_y_degrees, 1.0f, 179.0f);
  // near 必须 > 0（否则投影矩阵退化），far 必须 > near
  near_z = std::max(near_z, 0.001f);
  far_z = std::max(far_z, near_z * 10.0f);
}

// ──────────────────────────────── ThreeView ─────────────────────────────────

ThreeView::ThreeView() = default;
ThreeView::~ThreeView() = default;

auto ThreeView::load_script(std::string_view script, std::string_view source_name) -> bool {
  auto loaded = raster::ThreeRuntime::instance().evaluate(script, source_name);
  return adopt_scene(std::move(loaded), source_name);
}

auto ThreeView::load_script_file_kind(std::string_view script, std::string_view source_name)
    -> bool {
  // 扩展名只看最后一段（`a/b/c.py` → `py`），且大小写不敏感。
  std::string_view extension;
  if (const std::size_t dot = source_name.find_last_of('.'); dot != std::string_view::npos) {
    if (source_name.find_first_of("/\\", dot) == std::string_view::npos) {
      extension = source_name.substr(dot + 1);
    }
  }
  std::string lowered;
  lowered.reserve(extension.size());
  for (const char ch : extension) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (lowered == "py") {
    return adopt_scene(raster::BpySceneExtractor::scene_from_script(script), source_name);
  }
  // 其余（`js` 与无扩展名）都走 three.js：这是历史行为，不能因为多了 bpy 而改。
  return load_script(script, source_name);
}

auto ThreeView::load_mixed(std::string_view bpy_script, std::string_view three_script,
                            std::string_view source_name) -> bool {
  // 退化：任一侧为空 → 就是纯脚本场景（与 SceneSpec 的叠加语义一致）。
  if (three_script.empty()) return load_script_file_kind(bpy_script, std::string(source_name) + ".py");
  if (bpy_script.empty()) return load_script(three_script, std::string(source_name) + ".js");

  // 先跑 bpy：它的物体网格成为 three 可用的几何资产。
  auto assets_scene = raster::BpySceneExtractor::extract(bpy_script);
  if (!assets_scene.has_value()) {
    // 失败语义与 `load_script` 一致：**不清空**已有场景，错误给 `last_error`。
    // 混模式下这条尤其重要：几何跑挂了而 three 那侧还是好的，
    // 清空会让用户看到“俩都坏了”（实际上只错了一个）。
    load_error_ = std::format("bpy（资产）失败：{}", assets_scene.error().message);
    source_name_ = std::string(source_name);
    mark_dirty();
    return false;
  }
  raster::ThreeAssets assets;
  assets.items.reserve(assets_scene->objects.size());
  for (auto& object : assets_scene->objects) {
    if (object.mesh.vertices.empty()) continue;
    std::string name = object.name;
    if (name.empty()) name = std::format("asset_{}", assets.items.size());
    assets.items.push_back(raster::BpyAsset{.name = std::move(name),
                                            .mesh = std::move(object.mesh)});
  }
  return adopt_scene(
      raster::ThreeRuntime::instance().evaluate_with_assets(three_script, assets,
                                                          std::string(source_name) + ".js"),
      source_name);
}

auto ThreeView::adopt_scene(Result<raster::ThreeScene> loaded,
                            std::string_view source_name) -> bool {
  if (!loaded.has_value()) {
    // ## 失败**不清空**已有场景
    //
    // 热重载时用户改了半行脚本（语法错误）是常态。若此时把场景清掉，
    // 画面会突然变空——用户的判断会是"程序坏了"，而不是"我脚本写错了"。
    // 保留旧场景 + 在角标上显示错误，才是"改错了但还看得见上一步"的体验。
    load_error_ = loaded.error().message;
    source_name_ = std::string(source_name);
    mark_dirty();
    return false;
  }

  const bool first_load = scene_.objects.empty();
  // 先存旧名，否则下面比较时两边都是新值（之前就是这么写的，
  // 于是切场景永远不会重新取景——而这一行本身看起来完全合理）。
  const std::string previous_source = source_name_;
  scene_ = std::move(*loaded);
  source_name_ = std::string(source_name);
  load_error_.clear();
  hovered_.reset();
  selected_.reset();

  // 拾取缓存：世界 AABB 在建这里算一次（装载后网格与矩阵都不再变）
  pick_cache_.clear();
  pick_cache_.reserve(scene_.objects.size());
  for (std::size_t index = 0; index < scene_.objects.size(); ++index) {
    const auto& object = scene_.objects[index];
    const auto pick = raster::make_pick_object(object.name, object.mesh, object.world);
    pick_cache_.push_back(PickCache{object.name, pick.world_bounds, index});
  }

  // ## 什么时候重新取景
  //
  // | 情形 | 行为 |
  // |---|---|
  // | 首次装载 | 取景 |
  // | 同一个场景文件改了内容（热重载） | **保持**用户当前视角 |
  // | 切到**另一个场景文件** | 重新取景 |
  //
  // 第三条是实测补的：切场景后相机停在上一份场景算出的距离（88.9，而新场景
  // 只需 30 多）——画面里东西缩成一小块，看着像“新场景没加载”。
  // 而热重载时不能重算：“改一下颜色结果镜头跳回去了”很恼人。
  const bool switched_scene =
      !previous_source.empty() && !source_name.empty() && previous_source != source_name;
  if (first_load || switched_scene) {
    frame_scene();
    home_ = orbit_;
    has_home_ = true;
  }
  mark_dirty();
  return true;
}

void ThreeView::set_animation_enabled(bool enabled) noexcept {
  animation_enabled_ = enabled;
  mark_dirty();
}

auto ThreeView::tick_animation() -> bool {
  if (!animation_enabled_) return false;
  bool animated = false;
  for (const auto& object : scene_.objects) {
    if (object.visible && object.animated()) {
      animated = true;
      break;
    }
  }
  if (!animated) return false;
  // 登记损坏区：没有这一步，下一帧会被视口剔除跳过（见头文件里那段死锁说明）。
  mark_dirty();
  return true;
}

void ThreeView::set_camera_state(const OrbitCameraState& state) {
  orbit_ = state;
  orbit_.clamp();
  mark_dirty();
}

void ThreeView::frame_scene(float margin) {
  const auto box = scene_bounds(scene_);
  if (box.is_empty()) {
    // 空场景：给一个能看见原点的默认视角
    orbit_.target = math::Vec3{0.0f, 0.0f, 0.0f};
    orbit_.distance = 4.5f;
    orbit_.clamp();
    mark_dirty();
    return;
  }

  // ## 先尊重**场景自己的相机**（如果它写了）
  //
  // 实测踩到：场景脚本里写 `camera.position.set(10,16,84)` + `camera.lookAt(0,0.4,1.4)`，
  // 而视口里看到的却是另一回事——因为 `frame_scene` 只按包围盒算距离，
  // yaw/pitch/fov 全用 `OrbitCameraState` 的默认值，**把作者写的视角丢了**。
  //
  // 这很隐蔽：脚本里的相机明明是对的（`three_check` 用它能出好图），
  // 而视口里“就是不对”——很容易误判成“抽取坏了”（我当时就这么想了）。
  //
  // 做法：把场景 camema 的 eye 方向换算成球坐标；fov 也一并沿用。
  // 距离仍由包围盒决定（场景相机可能是为静态单帧调的，直接用它
  // 可能让物体出框）。
  {
    const math::Vec3 eye = scene_.camera.eye;
    const math::Vec3 delta{eye.x - box.center().x, eye.y - box.center().y,
                           eye.z - box.center().z};
    const float horizontal = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    // 方向退化（相机在目标点上）时不动，保持默认值
    if (horizontal > 1e-3f || std::abs(delta.y) > 1e-3f) {
      orbit_.yaw = std::atan2(delta.x, delta.z);
      orbit_.pitch = std::atan2(delta.y, std::max(horizontal, 1e-3f));
    }
    if (scene_.camera.fov_y_degrees > 1.0f) orbit_.fov_y_degrees = scene_.camera.fov_y_degrees;
  }
  orbit_.target = box.center();
  const math::Vec3 size = box.size();
  // ## 距离：按**相机坐标系下的横纵跨度**分别适配
  //
  // 不用包围球半径 —— 它对长条模型很浪费：涡轮发动机轴向跨 21、径向只 20，
  // 球半径取对角线 ≈ 15，而实际该适配的是“横向 21 / 纵向 20”。
  // 用球半径算出来的距离偏大约 40%，画面四周大片空白（实测）。
  //
  // 做法：把包围盒的尺寸投影到相机的右/上两个轴上，得到“画面里的宽与高”，
  // 再分别按水平/垂直 fov 反解距离，取**更严**的那个。
  //
  // 投影用世界轴近似即可（不真算相机基向量）：沿 yaw 的水平方向就是
  // 横轴，竖直方向就是纵轴。对轨道相机来说这已经足够准确，
  // 且避免在 frame_scene 里再做一次三角函数推导（那是出错的高发地）。
  const float cos_yaw = std::abs(std::cos(orbit_.yaw));
  const float sin_yaw = std::abs(std::sin(orbit_.yaw));
  // 水平跨度：包围盒在“与视线垂直的水平方向”上的投影
  const float span_h = size.x * cos_yaw + size.z * sin_yaw;
  // 垂直跨度：高度 + 由俯仰带来的深度投影
  const float sin_pitch = std::abs(std::sin(orbit_.pitch));
  const float span_v = size.y + (size.x * sin_yaw + size.z * cos_yaw) * sin_pitch;
  const float half_span_h = std::max(span_h * 0.5f, 0.05f);
  const float half_span_v = std::max(span_v * 0.5f, 0.05f);
  // ## 距离要同时满足**垂直**与**水平**两个方向
  //
  // ## 两个方向都要满足（宽高比陷阱）
  //
  // 所以横纵分别反解，取**更严**的那个（距离取大值）：
  //
  //   垂直方向：d_v = half_span_v / tan(half_fov_v)
  //   水平方向：d_h = half_span_h / tan(half_fov_h)
  //
  // 圆形物体两个约束接近，行为与旧版一致；长条形（发动机）才明显受益。
  const float aspect = bounds_.height > 0.5f ? bounds_.width / bounds_.height : 1.0f;
  const float half_fov_v = std::max(orbit_.fov_y_degrees, 1.0f) * 0.5f * kPi / 180.0f;
  const float half_fov_h = std::atan(std::tan(half_fov_v) * std::max(aspect, 0.05f));
  const float m = std::max(margin, 1.0f);
  const float d_v = half_span_v * m / std::max(std::tan(half_fov_v), 0.05f);
  const float d_h = half_span_h * m / std::max(std::tan(half_fov_h), 0.05f);
  orbit_.distance = std::max(std::max(d_v, d_h), 0.1f);
  orbit_.clamp();
  mark_dirty();
}

void ThreeView::reset_view() {
  if (!has_home_) {
    frame_scene();
    home_ = orbit_;
    has_home_ = true;
    return;
  }
  orbit_ = home_;
  orbit_.clamp();
  mark_dirty();
}

auto ThreeView::highlight_color(const RenderContext& context) const noexcept -> math::Color {
  if (highlight_override_.has_value()) return *highlight_override_;
  // 主题没给 accent 时退到一个可见的暖色（不是灰色——灰高亮在深色背景上看不出）
  const math::Color accent = context.theme.colors().accent;
  if (accent.r == 0 && accent.g == 0 && accent.b == 0) return math::Color{255, 190, 90, 255};
  return accent;
}

auto ThreeView::ray_from_point(math::Point local) const noexcept -> math::Ray {
  math::Ray ray;
  ray.origin = orbit_.to_camera().eye;
  const math::Rect box = bounds().inset(style_.padding);
  const float width = std::max(box.width, 1.0f);
  const float height = std::max(box.height, 1.0f);

  // NDC：x 向右为 +1，y 向**上**为 +1（屏幕 y 向下，故取反）
  const float ndc_x = (local.x - box.x) / width * 2.0f - 1.0f;
  const float ndc_y = 1.0f - (local.y - box.y) / height * 2.0f;

  const raster::Camera camera = orbit_.to_camera();
  const math::Mat4 view = math::Mat4::look_at(camera.eye, camera.target, camera.up);
  // 视线的基向量（从视图矩阵取出）：right = 第 0 行，up = 第 1 行，back = 第 2 行
  // ## 从视图矩阵取基向量：取**行**，不是列
  //
  // `Mat4::look_at` 把相机的三个基向量写进 **行**：
  // 行0 = right、行1 = up、行2 = −forward（见 `matrix.hpp` 的 `set(0,*)/set(1,*)/set(2,*)`）。
  //
  // 第一版按列取（`value(0,0)/value(1,0)/value(2,0)`），得到的是“世界轴在视图空间的像”，
  // 与想要的恰好是转置关系。而转置在 yaw=0 时**看起来是对的**（单位向量仍对），
  // 一旦转镜头就逐渐偏——症状是“悬浮/点击大部分时候不命中”，最难查的那一类。
  const math::Vec3 right{view.value(0, 0), view.value(0, 1), view.value(0, 2)};
  const math::Vec3 up{view.value(1, 0), view.value(1, 1), view.value(1, 2)};
  const math::Vec3 forward{-view.value(2, 0), -view.value(2, 1), -view.value(2, 2)};

  const float aspect = width / height;
  const float tan_half = std::tan(std::max(camera.fov_y_degrees, 1.0f) * 0.5f * kPi / 180.0f);
  const math::Vec3 direction{forward.x + right.x * ndc_x * tan_half * aspect +
                                 up.x * ndc_y * tan_half,
                             forward.y + right.y * ndc_x * tan_half * aspect +
                                 up.y * ndc_y * tan_half,
                             forward.z + right.z * ndc_x * tan_half * aspect +
                                 up.z * ndc_y * tan_half};
  ray.direction = direction.normalized();
  return ray;
}

auto ThreeView::pick(math::Point point) const -> std::optional<ThreeHit> {
  if (pick_cache_.empty()) return std::nullopt;
  const math::Ray ray = ray_from_point(point);

  // 把缓存换成 `PickObject`（`raycast` 的入参形态）。
  //
  // 这里仍然要建一个 vector：`raycast` 的接口吃 `vector<PickObject>`。
  // 但**昂贵的部分（世界 AABB）已经在装载时算好**，这里只是拷结构。
  std::vector<raster::PickObject> objects;
  objects.reserve(pick_cache_.size());
  for (const auto& entry : pick_cache_) {
    if (entry.index >= scene_.objects.size()) continue;
    raster::PickObject object;
    object.name = entry.name;
    object.mesh = &scene_.objects[entry.index].mesh;
    object.world = scene_.objects[entry.index].world;
    object.world_bounds = entry.world_bounds;
    objects.push_back(std::move(object));
  }
  const auto hit = raster::raycast(objects, ray);
  if (!hit.has_value()) return std::nullopt;
  ThreeHit out;
  out.name = hit->name;
  out.point = hit->point;
  out.normal = hit->normal;
  out.distance = hit->distance;
  return out;
}

// ─────────────────────────────── Element ────────────────────────────────────

void ThreeView::measure(const RenderContext& context, const Constraints& constraints) {
  // ## 不能用 `max_*`（列布局里它可能是无界巨值）
  //
  // 第一版写的是 `max_height` 直接当高度用，结果在无头启动时三维后端拿到一个
  // **天文数字尺寸**的帧缓冲，直接 `std::bad_alloc`——而且崩在启动后第一帧，
  // 堆栈里看到的全是分配器，与“三维组件”毫无关联。
  //
  // 用 `fill_*`：它们会在无界时退回 `available_*`，再退回 fallback。
  const float width = fill_width(constraints, style_.width, 480.0f);
  // 高度：
  // · 默认用 `preferred_height_`（不吃满，以便与同级共存）；
  // · `fill_height_` 为真时用 **`available_height`**——它才是父级实际给的空间，
  //   而 `max_height` 在无界约束下是一个巨大值（拿它当高度会让后端分配天文数字）。
  float height = preferred_height_;
  if (fill_height_) {
    const float available = constraints.available_height < kUnbounded
                                ? constraints.available_height
                                : constraints.max_height;
    if (available > 0.0f && available < kUnbounded) height = available;
  }
  if (style_.height >= 0.0f && style_.height != kAuto) height = style_.height;
  measured_ = math::Size{width > 0.0f ? width : 480.0f, height > 0.0f ? height : 320.0f};
  (void)context;
}

void ThreeView::arrange(const RenderContext& context, math::Rect rect) {
  bounds_ = rect;
  (void)context;
}

void ThreeView::render_into(const RenderContext& context, raster::Surface& canvas,
                            math::Rect box) const {
  // ── 动画时钟 ──
  //
  // 用 `RenderContext::time_seconds`（而不是墙钟）：它是宿主给的帧时间，
  // 无头截图时是可控的，因此“同一帧号 = 同一姿态”。
  //
  // ## 为何用增量而不是绝对值
  //
  // 自转要的是累计角度，而 `time_seconds` 可能从非零开始（首帧）。
  // 首帧只记基准时间、不推进，避免“第一帧就跳了一大步”。
  if (animation_enabled_) {
    if (last_frame_time_ >= 0.0 && context.time_seconds > last_frame_time_) {
      animation_time_ += context.time_seconds - last_frame_time_;
    }
    last_frame_time_ = context.time_seconds;
  }

  // ## 有动画的物体必须**主动续帧 + 报损坏区**
  //
  // 实测踩过的两个坑，缺一个都不行：
  //
  // 1. **只靠宿主的 `request_repaint()` → 帧数停在 3**。应用侧的“需要重绘”
  //    是脏标记驱动的，而三维视口的内容在框架看来没变化（它又不知道图形里
  //    有齿轮在转）。正确协议是 `request_animation()`：声明"我还在连续动"。
  //
  // 2. **只调 `request_animation()` → 帧号在涨但画面不动**。因为帧循环里
  //    “损坏区为空”的帧会**跳过绘制**（见 `UiRoot::render_frame` 的 `use_full`
  //    判定）——续帧只保证了“帧来了”，没保证“这一帧要画像素”。
  //    所以还须报一块损坏区，告诉宿主“这里真的变了”。
  //
  // 两条合起来才是完整的“我在动”（这个组合 bug 极难查：物体数、后端、
  // 帧号全部正常，只有像素永远停在首帧）。
  if (animation_enabled_) {
    for (const auto& object : scene_.objects) {
      if (object.visible && object.animated()) {
        request_animation();
        break;
      }
    }
  }
  // ## 尺寸硬上限（防崩护栏，不是“优化”）
  //
  // 实测踩过：布局约束异常时 `box` 可能是一个天文数字（无界值漏传），
  // 于是 `Scene3D::create` 去分配一个几 GB 的帧缓冲 → `std::bad_alloc` 直接 terminate。
  // 而那个堆栈里全是分配器，**看不到“三维组件”的影子**，极难定位。
  //
  // 所以在这里夹一道：超过 8192 就夹到 8192。真实视口不可能需要更大
  //（4K 屏 3840 宽，乘 DPI 2 也只 7680）。
  constexpr float kMaxRenderEdge = 8192.0f;
  const int width = std::clamp(
      static_cast<int>(std::lround(std::min(box.width, kMaxRenderEdge) * canvas.device_scale())), 1,
      static_cast<int>(kMaxRenderEdge));
  const int height = std::clamp(
      static_cast<int>(std::lround(std::min(box.height, kMaxRenderEdge) * canvas.device_scale())),
      1, static_cast<int>(kMaxRenderEdge));

  if (!raster::Scene3D::available()) {
    canvas.fill_rect(box, raster::Paint::solid(math::Color{48, 20, 26, 255}), style_.radius);
    return;
  }

  if (backend_ == nullptr || backend_width_ != width || backend_height_ != height) {
    // ## 后端偏好用 `putenv` 表达（框架的 `ST_SCENE3D_BACKEND` 是进程环境）
    //
    // 注：这是**进程级**设置，同一个进程里的其它三维场景会受影响。
    // 交互式应用里通常只有一个视口，所以可以接受；多视口需要各自的腿时
    // 得改框架接口（把后端选择从环境变量改成 `create` 参数）。
    std::unique_ptr<raster::Scene3D> created;
    std::string failure;
    const bool force = !backend_pref_.empty();
    if (force) {
      const std::scoped_lock lock(raster::Scene3D::backend_mutex());
      const char* previous = std::getenv("ST_SCENE3D_BACKEND");
      const std::string saved = previous != nullptr ? previous : std::string();
#if defined(_WIN32)
      (void)_putenv_s("ST_SCENE3D_BACKEND", backend_pref_.c_str());
#else
      (void)setenv("ST_SCENE3D_BACKEND", backend_pref_.c_str(), 1);
#endif
      auto result = raster::Scene3D::create(width, height);
#if defined(_WIN32)
      (void)_putenv_s("ST_SCENE3D_BACKEND", saved.c_str());
#else
      if (saved.empty()) (void)unsetenv("ST_SCENE3D_BACKEND");
      else (void)setenv("ST_SCENE3D_BACKEND", saved.c_str(), 1);
#endif
      if (result.has_value()) created = std::move(*result);
      else failure = result.error().message;
    } else {
      auto result = raster::Scene3D::create(width, height);
      if (result.has_value()) created = std::move(*result);
      else failure = result.error().message;
    }
    if (created == nullptr) {
      ST_LOG_WARN("ThreeView：创建三维场景失败（偏好 {}，尺寸 {}x{}）：{}",
                  force ? backend_pref_ : "auto", width, height, failure);
      backend_.reset();
      canvas.fill_rect(box, raster::Paint::solid(math::Color{48, 20, 26, 255}), style_.radius);
      return;
    }
    backend_ = std::move(created);
    backend_width_ = width;
    backend_height_ = height;
    ++backend_rebuilds_;
  }

  raster::Camera camera = orbit_.to_camera();
  backend_->begin_frame(background_);
  backend_->set_camera(camera);
  // 光照**必须传**：三维场景的灯光是从 three 抽出来的（金属/粗糙度需要它），
  // 不传就回到默认光——观感完全不同（本场景的金属件会变得平淡发灰）。
  backend_->set_lighting(scene_.lighting);

  for (const auto& object : scene_.objects) {
    if (!object.visible) continue;
    raster::Material material = object.material;

    // ## 高亮怎么实现（为什么改材质而不是加描边几何）
    //
    // 描边要额外一遍绘制（放大/壳层法），在两条腿上各自实现一遍很容易不同；
    // 而"把基色往高亮色推"是纯材质操作，两条腿走同一个公式——**降级就不是降级**。
    //
    // 悬浮：轻推（35%）；选中：重推（70%）且抬高自发光，使它在暗处也看得出被选中。
    const bool is_hovered = hovered_.has_value() && !hovered_->name.empty() &&
                            hovered_->name == object.name;
    const bool is_selected = selected_.has_value() && !selected_->name.empty() &&
                             selected_->name == object.name;
    if (is_hovered || is_selected) {
      // ## 高亮靠**自发光**，不靠推高基色（实测调出来的结论）
      //
      // 第一版把基色往主题 accent（偏白）混 35%/70%。结果是金属球
      // **整块变成近白色**：固有色被白吃掉，金属反射与明暗层次全看不见——
      // 用户看到的是“选中就变白块”，而不是“它被标记了”。
      //
      // 换成加自发光后：物体本色保留，高亮表现为“它自己亮一点”。
      //   悬浮：叠一层很淡的暖色自发光（画面看得出的下限）
      //   选中：更亮、并且颜色向高亮色偏（但不抹掉本色）
      //
      // 数值是调出来的：0.06 在暗背景上刚好能看出、0.16 一眼就知道选中，
      // 而两者都不会让物体失去材质感。
      const math::Vec3 base = material.base_color_set ? material.base_color
                                                      : math::Vec3{1.0f, 1.0f, 1.0f};

      // ## 高亮 = **温度偏移**，不是“变亮”
      //
      // 试过三种写法，前两种都会把物体洗成白块（金属尤其明显）：
      //
      // | 写法 | 结果 |
      // |---|---|
      // | 基色往主题 accent（偏白）混 35%/70% | 固有色被白吃掉 → 白块 |
      // | 再加 `emission`（即使只有 0.06） | 自发光叠在受光结果上，金属反射本来就接近 1 → 冲白 |
      //
      // 能保住材质的做法：把基色往**暖色**（而不是白）偏一点，且**不改亮度**。
      // 颜色变了、材质感不变——这正是“被标记”该有的样子。
      //
      // 选中与悬浮的差别用**偏移量**表达（悬浮轻、选中重），而不是靠叠加亮度。
      const float shift = is_selected ? 0.34f : 0.16f;
      // 暖色目标：偏橙（R 抬、B 压）。不动 G 以免变绿。
      const math::Vec3 warm{std::min(base.x * 1.35f + 0.10f, 1.0f), base.y * 1.02f,
                            base.z * 0.55f};
      material.base_color = math::Vec3{base.x + (warm.x - base.x) * shift,
                                       base.y + (warm.y - base.y) * shift,
                                       base.z + (warm.z - base.z) * shift};
      material.base_color_set = true;
      // 自发光一律不加（理由见上）；如果原材质本来有，保留它。
    }

    // ## 把材质与剔除交给后端（**不能省**）
    //
    // 排查期间为了 A/B 对比把这两行去掉过，结果整个视口变成一片白——
    // 因为不调 `set_material` 时后端用默认材质（白、无金属度/粗糙度区分），
    // 所有零件长得一模一样，金属与宝石完全分不出来。
    //
    // 当时的误判：“画面有东西但全白”看起来像“光照没传”或“材质抽取坏了”，
    // 而真实原因就是这两行不在。教训：A/B 实验**必须成对回滚**。
    backend_->set_cull_mode(object.cull);
    backend_->set_material(material);
    // ── 运动：把自转/振荡乘在烘焙好的世界矩阵后面 ──
    //
    // `object.world` 是从 three 抽出来的**静态**矩阵（含父子链）。
    // 运动是纯旋转，所以 `world * R(轴, 角度)` 就得到了“原地转”的效果——
    // 转动中心就是该物体的局部原点（建模时把齿轮中心放在原点上）。
    //
    // 这也是为什么 `userData.spin` 只需要轴与速度：绕感不需要四个参数。
    math::Mat4 motion = math::Mat4::identity();
    if (animation_enabled_ && object.animated()) {
      const float seconds = static_cast<float>(animation_time_);
      if (object.spin_speed != 0.0f) {
        motion = math::Mat4::rotation(object.spin_axis, object.spin_speed * seconds);
      }
      if (object.oscillate_amplitude != 0.0f) {
        const float angle = object.oscillate_amplitude *
                            std::sin(object.oscillate_frequency * seconds * 2.0f * kPi +
                                     object.oscillate_phase);
        motion = motion * math::Mat4::rotation(object.oscillate_axis, angle);
      }
    }
    backend_->draw_mesh(object.mesh, object.world * motion);
  }

  // 画完把材质/剔除恢复默认，不让它泄漏到后续绘制
  backend_->set_cull_mode(raster::CullMode::Back);
  backend_->set_material(raster::Material{});
  backend_->end_frame(canvas, box);
}

void ThreeView::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  const math::Rect box = bounds_.inset(style_.padding);
  if (box.is_empty()) return;

  render_into(context, canvas, box);
  ++paint_frames_;

  if (!show_hud_) return;

  // ── 角标：把"这份场景是什么状态"写在画面上 ──
  //
  // 为什么值得占用画面：热重载场景下，用户最常问的两件事是
  // "我改的脚本生效了吗""它为什么没变"。把物体数与错误直接写出来，
  // 比让人去翻日志快得多。
  const NullTextPort fallback_port;
  const TextPort& port = context.text != nullptr ? *context.text : fallback_port;
  const float title_size = 12.0f;
  const float line_height = port.line_height(title_size) + 2.0f;

  std::string title;
  if (!load_error_.empty()) {
    title = std::format("脚本错误 · {}", source_name_.empty() ? "<inline>" : source_name_);
  } else {
    title = std::format("{} 个物体 · {}", scene_.objects.size(),
                        backend_ != nullptr ? backend_->backend() : "…");
  }

  const float padding = 6.0f;
  const float text_x = box.x + padding;
  float text_y = box.y + padding + port.shaped_ascent(title, title_size);

  // 背景压条：不压的话浅色场景上白字看不见。
  //
  // ⚠ 宽度必须**两重夹取**：既不超过视口，也不超过文本自身的估算宽度。
  // 第一版只用 `min(box.width - 4, 360)`，在窄视口上会**伸出右沿**——
  // 截图里是一条黑边“悬”在视口外面（既不美观也不像有意设计）。
  const auto draw_chip = [&](float top, float height, float content_width) {
    const float width = std::min({content_width + padding * 2.0f, box.width - 4.0f, 420.0f});
    if (width <= 0.0f) return;
    canvas.fill_rect(math::Rect{box.x + 2.0f, top, width, height},
                     raster::Paint::solid(math::Color{8, 10, 14, 190}), 4.0f);
  };

  const float chip_height = line_height + padding;
  draw_chip(box.y + 2.0f, chip_height, port.measure_width(title, title_size));

  const math::Color title_color = load_error_.empty() ? math::Color{210, 226, 245, 255}
                                                      : math::Color{255, 138, 128, 255};
  port.draw(canvas, title, math::Point{text_x, text_y}, title_size, title_color);

  // 错误详情：多行，最多 3 行（再多会把视口盖住）
  if (!load_error_.empty()) {
    std::string remaining = load_error_;
    int lines = 0;
    float y = text_y + line_height;
    while (!remaining.empty() && lines < 3) {
      const std::size_t newline = remaining.find('\n');
      const std::string line =
          newline == std::string::npos ? remaining : remaining.substr(0, newline);
      remaining = newline == std::string::npos ? std::string() : remaining.substr(newline + 1);
      draw_chip(y - port.shaped_ascent(line, title_size * 0.92f) - 2.0f, line_height,
                port.measure_width(line, title_size * 0.92f));
      port.draw(canvas, line, math::Point{text_x, y}, title_size * 0.92f,
                math::Color{255, 190, 180, 255});
      y += line_height;
      ++lines;
    }
  }

  // 悬浮/选中标签（跟随指针位置不好做，改为左下角常显）
  const auto& label_hit = selected_.has_value() ? selected_ : hovered_;
  if (label_hit.has_value()) {
    const std::string name = label_hit->name.empty() ? "<未命名>" : label_hit->name;
    const std::string label =
        std::format("{}{}", selected_.has_value() ? "选中 · " : "悬浮 · ", name);
    const float label_size = 12.0f;
    const float y = box.bottom() - padding;
    const float label_height = port.line_height(label_size) + padding;
    const float label_width = port.measure_width(label, label_size) + padding * 2.0f;
    canvas.fill_rect(
        math::Rect{box.x + 2.0f, y - label_height, std::min(label_width, box.width - 4.0f),
                   label_height},
        raster::Paint::solid(math::Color{8, 10, 14, 190}), 4.0f);
    port.draw(canvas, label, math::Point{text_x, y - padding * 0.5f}, label_size,
              math::Color{230, 238, 250, 255});
  }
}

auto ThreeView::on_event(const RenderContext& context, Event& event) -> bool {
  (void)context;
  const math::Rect box = bounds().inset(style_.padding);

  switch (event.kind) {
    case EventKind::MouseMove: {
      if (drag_mode_ != DragMode::None) {
        const float dx = event.position.x - drag_last_.x;
        const float dy = event.position.y - drag_last_.y;
        if (std::abs(event.position.x - press_origin_.x) > kDragThreshold ||
            std::abs(event.position.y - press_origin_.y) > kDragThreshold) {
          press_moved_ = true;
        }
        drag_last_ = event.position;
        if (drag_mode_ == DragMode::Orbit) {
          // 0.008 rad/px：实测手感（拖满 400px 约 183°，正好半圈出头）
          orbit_.yaw -= dx * 0.008f;
          orbit_.pitch += dy * 0.008f;
          orbit_.clamp();
        } else if (drag_mode_ == DragMode::Pan) {
          // 平移量按当前距离缩放：远看时拖同样像素走的世界距离更大，才符合直觉
          const raster::Camera camera = orbit_.to_camera();
          const math::Mat4 view = math::Mat4::look_at(camera.eye, camera.target, camera.up);
          // 取**行**（与 `ray_from_point` 同一口径；取错的症状是平移方向不对）
          const math::Vec3 right{view.value(0, 0), view.value(0, 1), view.value(0, 2)};
          const math::Vec3 up{view.value(1, 0), view.value(1, 1), view.value(1, 2)};
          const float per_pixel =
              orbit_.distance * std::tan(orbit_.fov_y_degrees * 0.5f * kPi / 180.0f) * 2.0f /
              std::max(box.height, 1.0f);
          orbit_.target.x -= (right.x * dx - up.x * dy) * per_pixel;
          orbit_.target.y -= (right.y * dx - up.y * dy) * per_pixel;
          orbit_.target.z -= (right.z * dx - up.z * dy) * per_pixel;
        }
        mark_dirty();
        event.handled = true;
        return true;
      }

      // 悬浮拾取：指针在视口内才做
      if (pick_enabled_ && box.contains(event.position)) {
        auto hit = pick(event.position);
        // ## `hover_events` 统计的是“命中了多少次”，不是“换了几个物体”
        //
        // 第一版只在**命中目标发生变化**时才递增。那个语义看似更“干净”，
        // 实际让观测变得不可用：指针一直悬在同一个物体上、移出去再移回来，
        // 计数不变——于是自动化无法区分“拾取生效但没换目标”与“拾取压根没跑”。
        //
        // 真正的“换了目标”由 `picked` 的名字变化反映，不需要第二个计数器。
        if (hit.has_value()) ++hover_events_;
        const bool changed = hit.has_value() != hovered_.has_value() ||
                             (hit.has_value() && hovered_.has_value() &&
                              hit->name != hovered_->name);
        hovered_ = std::move(hit);
        if (changed) mark_dirty();
      } else if (hovered_.has_value()) {
        hovered_.reset();
        mark_dirty();
      }
      // 鼠标在本元素范围内但**在视口外**（例如在状态栏上）时也要清掉——
      // 视口只占页面的一块，指针滑出去就应该没有高亮。
      if (!box.contains(event.position) && hovered_.has_value()) {
        hovered_.reset();
        mark_dirty();
      }
      return false;   // 不吞事件：父级仍可处理（如滚动容器）
    }

    case EventKind::MouseDown: {
      if (!box.contains(event.position)) return false;
      press_origin_ = event.position;
      drag_last_ = event.position;
      press_moved_ = false;
      if (event.button == 0 && orbit_enabled_) {
        drag_mode_ = DragMode::Orbit;
      } else if ((event.button == 1 || event.button == 2) && pan_enabled_) {
        drag_mode_ = DragMode::Pan;
      } else {
        drag_mode_ = DragMode::None;
      }
      event.handled = true;
      return true;
    }

    case EventKind::MouseUp: {
      if (drag_mode_ == DragMode::None) return false;
      const bool was_click = !press_moved_ && event.button == 0 && pick_enabled_;
      drag_mode_ = DragMode::None;
      if (was_click && box.contains(event.position)) {
        // 点击拾取：命中就选中，点空白处取消选中
        selected_ = pick(event.position);
        ++click_events_;
        mark_dirty();
      }
      event.handled = true;
      return true;
    }

    case EventKind::Wheel: {
      if (!zoom_enabled_ || !box.contains(event.position)) return false;
      // 等比缩放：每格 1.12 倍。用乘法而不是加法，才能在近处也保持手感一致。
      const float factor = event.wheel_delta > 0.0f ? 1.12f : (1.0f / 1.12f);
      orbit_.distance *= factor;
      orbit_.clamp();
      mark_dirty();
      event.handled = true;
      return true;
    }

    case EventKind::WindowLeave: {
      // 窗口级指针离开：清掉悬停态与光标残留。
      //
      // ## 为何本组件也要处理它
      //
      // `UiRoot` 会在窗口级离开时清自己的 `hovered_`，但**三维视口自己的**
      // “悬浮到哪个物体”是组件内部状态，宿主不知道。不处理的话，指针出窗后
      // 高束会一直停留在最后一个命中的物体上——用户看到的是“鼠标走了高亮还在”。
      if (hovered_.has_value()) {
        hovered_.reset();
        mark_dirty();
      }
      return false;
    }

    default:
      break;
  }
  return false;
}

auto ThreeView::cursor_at(math::Point point) const noexcept -> MouseCursor {
  if (!bounds().inset(style_.padding).contains(point)) return MouseCursor::Arrow;
  if (drag_mode_ == DragMode::Orbit || drag_mode_ == DragMode::Pan) return MouseCursor::Move;
  // 悬浮在可点物体上给手型（这是"这里能点"的唯一非视觉提示）
  if (hovered_.has_value()) return MouseCursor::Hand;
  return MouseCursor::Arrow;
}

auto ThreeView::semantics_text() const -> std::string {
  if (!load_error_.empty()) return "3D 视口（脚本错误）";
  return std::format("3D 视口（{} 个物体）", scene_.objects.size());
}

auto ThreeView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "objects") return std::to_string(scene_.objects.size());
  if (name == "distance") return std::format("{:.4f}", orbit_.distance);
  if (name == "yaw") return std::format("{:.4f}", orbit_.yaw);
  if (name == "pitch") return std::format("{:.4f}", orbit_.pitch);
  // ⚠ 名字用 `picked` / `selected_name` 而不是 `hovered` / `selected`：
  //
  // `hovered` 是**框架级**属性（`element_snapshot` 会写入布尔值“指针是否悬停在本元素上”），
  // 同名会互相覆盖——实测就是框架那个布尔值赢了，于是“悬浮到哪个物体”读出来永远是 false。
  //
  // `selected` 暂时没被框架占用，但一并避开：将来的多选/焦点语义很可能用到它。
  if (name == "picked") return hovered_.has_value() ? hovered_->name : std::string();
  if (name == "selected_name") return selected_.has_value() ? selected_->name : std::string();
  if (name == "pick_distance") {
    return hovered_.has_value() ? std::format("{:.4f}", hovered_->distance) : std::string();
  }
  if (name == "error") return load_error_;
  // 后端：未渲染过时如实报 `uninitialized`（而不是空串——空串会被当成“属性不存在”）
  if (name == "backend") {
    return backend_ != nullptr ? backend_->backend() : std::string("uninitialized");
  }
  if (name == "hover_events") return std::to_string(hover_events_);
  if (name == "click_events") return std::to_string(click_events_);
  // 绘制帧数 + 动画时钟：两者一起才能区分“没画”与“画了但时间没走”
  if (name == "paint_frames") return std::to_string(paint_frames_);
  if (name == "anim_time") return std::format("{:.3f}", animation_time_);
  // 后端重建次数：若它每帧都在涨，画面就永远停在初始时刻
  if (name == "backend_rebuilds") return std::to_string(backend_rebuilds_);
  if (name == "render_size") return std::format("{}x{}", backend_width_, backend_height_);
  // 取景用的包围盒（诊断：看 noFrame 有没有被排除）
  if (name == "frame_bounds") {
    const auto box = scene_bounds(scene_);
    if (box.is_empty()) return std::string("empty");
    const auto size = box.size();
    return std::format("size=({:.2f},{:.2f},{:.2f})", static_cast<double>(size.x),
                       static_cast<double>(size.y), static_cast<double>(size.z));
  }
  if (name == "backend_pref") return backend_pref_.empty() ? std::string("auto") : backend_pref_;
  // 实际送给后端的相机（证明它不是一个退化的默认值）
  if (name == "camera_state") {
    const raster::Camera camera = orbit_.to_camera();
    return std::format("eye=({:.2f},{:.2f},{:.2f}) target=({:.2f},{:.2f},{:.2f}) fov={:.1f}",
                       static_cast<double>(camera.eye.x),
                       static_cast<double>(camera.eye.y),
                       static_cast<double>(camera.eye.z),
                       static_cast<double>(camera.target.x),
                       static_cast<double>(camera.target.y),
                       static_cast<double>(camera.target.z),
                       static_cast<double>(camera.fov_y_degrees));
  }
  // 网格诊断：物体数正常但全部 mesh 为空时，画面上什么都不会有
  if (name == "mesh_stats") {
    std::size_t empty = 0;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    for (const auto& object : scene_.objects) {
      if (object.mesh.is_empty()) ++empty;
      vertices += object.mesh.vertices.size();
      triangles += object.mesh.indices.size() / 3;
    }
    return std::format("empty={}/{} verts={} tris={}", empty, scene_.objects.size(), vertices,
                       triangles);
  }
  // 首个动画物体的实际旋转角（度）：证明“时间→矩阵”这一步真的接上了
  if (name == "first_anim_deg") {
    for (const auto& object : scene_.objects) {
      if (object.spin_speed != 0.0f) {
        const double degrees =
            static_cast<double>(object.spin_speed) * animation_time_ * 180.0 / 3.14159265358979;
        return std::format("{:.2f}", std::fmod(degrees, 360.0));
      }
    }
    return std::string("none");
  }
  return std::nullopt;
}

auto ThreeView::set_property(std::string_view name, std::string_view value) -> bool {
  const auto as_float = [&]() -> std::optional<float> {
    try {
      return std::stof(std::string(value));
    } catch (...) {
      return std::nullopt;
    }
  };
  if (name == "distance") {
    const auto parsed = as_float();
    if (!parsed.has_value()) return false;
    orbit_.distance = *parsed;
    orbit_.clamp();
    mark_dirty();
    return true;
  }
  if (name == "yaw") {
    const auto parsed = as_float();
    if (!parsed.has_value()) return false;
    orbit_.yaw = *parsed;
    orbit_.clamp();
    mark_dirty();
    return true;
  }
  if (name == "pitch") {
    const auto parsed = as_float();
    if (!parsed.has_value()) return false;
    orbit_.pitch = *parsed;
    orbit_.clamp();
    mark_dirty();
    return true;
  }
  if (name == "selected_name") {
    // 允许按名字选中（脚本/控制通道用）：空串 = 取消
    if (value.empty()) {
      selected_.reset();
    } else {
      for (const auto& object : scene_.objects) {
        if (object.name == value) {
          ThreeHit hit;
          hit.name = object.name;
          selected_ = hit;
          break;
        }
      }
    }
    mark_dirty();
    return true;
  }
  return false;
}

auto ThreeView::property_names() const -> std::vector<std::string_view> {
  // ## 可写属性
  //
  // 只列“能被外部改动”的那些。只读观测值（物体数/后端/悬浮命中/计数）
  // 走 `readonly_property_names()`。
  return {"distance", "yaw", "pitch", "selected_name"};
}

auto ThreeView::readonly_property_names() const -> std::vector<std::string_view> {
  // ## 为什么这些观测值必须导出成属性
  //
  // 它们是“画面到底怎么了”的**唯一可断言面**。没有它们，验证只能靠截图：
  // “物体数对不对”“click 到底有没有命中”都得看图猜——而看图恰好是不可靠的那一环。
  //
  // ⚠ 实测踩过：只把它们写在 `get_property` 里**不够**。控制通道的属性快照
  // 是遍历 `property_names()` + `readonly_property_names()` 生成的，
  // 没列进去就**永远不会被导出**——而症状是“属性读出来是空的”，
  // 看上去像“组件没实现”。
  return {"objects",      "backend",      "picked",       "selected_name",
          "error",        "hover_events", "click_events", "pick_distance",
          "paint_frames", "anim_time", "backend_rebuilds", "render_size",
          "mesh_stats", "first_anim_deg", "backend_pref", "camera_state", "frame_bounds"};
}

auto ThreeView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  if (action == "reset_view" || action == "home") {
    reset_view();
    return true;
  }
  if (action == "frame_scene" || action == "frame") {
    frame_scene();
    return true;
  }
  if (action == "clear_selection") {
    selected_.reset();
    mark_dirty();
    return true;
  }
  if (action == "select") {
    return set_property("selected_name", argument);
  }
  return false;
}

}  // namespace st::ui
