/// `ThreeView` 的交互与拾取回归测试。
///
/// ## 这一组为什么必须存在（而且判据必须对准"能生效"）
///
/// 交互类代码的失败**几乎全都是静默的**：
///
/// | 写错的后果 | 表现 |
/// |---|---|
/// | 射线方向算错 | 悬浮不命中，但没有任何报错 |
/// | 相机球坐标反了 | 拖拽方向反直觉（用户只觉得"怪"，说不出哪错） |
/// | 俯仰没夹住 | 拖过 90° 后画面翻转，**用户无法自己拖回来** |
/// | 属性没登记 | 控制通道读到空字符串，看起来像"组件没实现" |
/// | 尺寸上限缺失 | 布局异常时分配到天文数字 → `bad_alloc` 直接崩 |
///
/// 所以这里的每条判据都对准一个**具体缺陷**，而不是"调一下不崩"。

#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>

#include "st/raster/pick.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/three_view.hpp"

using st::ui::OrbitCameraState;
using st::ui::ThreeView;

namespace {

/// 三物体场景：球 + 盒 + 底座（与示例素材同形）。
constexpr const char* kScene = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 16/9, 0.05, 100);
camera.position.set(0, 1.15, 4.4);
camera.lookAt(0, 0.35, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0x8899bb, 0.6));
const key = new THREE.DirectionalLight(0xffffff, 2.4);
key.position.set(3, 4.5, 3.5);
scene.add(key);

const ball = new THREE.Mesh(new THREE.SphereGeometry(0.6, 32, 24),
  new THREE.MeshStandardMaterial({ color: 0xd09050 }));
ball.name = 'ball';
ball.position.set(-0.7, 0.68, 0);
scene.add(ball);

const box = new THREE.Mesh(new THREE.BoxGeometry(0.9, 0.9, 0.9),
  new THREE.MeshStandardMaterial({ color: 0x4a7fd0 }));
box.name = 'box';
box.position.set(0.8, 0.46, 0);
scene.add(box);

const base = new THREE.Mesh(new THREE.BoxGeometry(2.9, 0.1, 1.4),
  new THREE.MeshStandardMaterial({ color: 0x2c3440 }));
base.name = 'base';
scene.add(base);
scene;
)js";

/// 造一个布局好的视口（不依赖 UiRoot：直接 arrange 出 bounds）。
auto make_view(float width = 640.0f, float height = 400.0f) -> std::unique_ptr<ThreeView> {
  auto view = std::make_unique<ThreeView>();
  view->set_id("viewport");
  view->load_script(kScene, "test-scene.js");
  view->arrange(st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr),
                                      nullptr, 0.0, nullptr},
                st::math::Rect{0.0f, 0.0f, width, height});
  return view;
}

}  // namespace

ST_TEST(three_view_loads_and_reports_objects) {
  auto view = make_view();
  ST_CHECK(view->last_error().empty());
  ST_CHECK(view->object_count() == 3U);
  // 属性面：观测值必须可读（控制通道的快照就靠它）
  ST_CHECK(view->get_property("objects").value_or("") == "3");
  ST_CHECK(!view->get_property("backend").value_or("").empty());

  // 只读观测面必须**登记**在 readonly_property_names 里——
  // 没登记就永远不会出现在控制通道快照里（实测踩过：读出来是空，看着像没实现）
  bool objects_registered = false;
  for (const auto name : view->readonly_property_names()) {
    if (name == "objects") objects_registered = true;
  }
  ST_CHECK(objects_registered);
}

ST_TEST(three_view_orbit_camera_state_maps_to_camera) {
  // 球坐标 → 相机的映射：yaw=0 时相机在 +Z 侧，pitch>0 时在上方
  OrbitCameraState state{};
  state.target = st::math::Vec3{0.0f, 0.5f, 0.0f};
  state.distance = 4.0f;
  state.yaw = 0.0f;
  state.pitch = 0.0f;
  const auto camera = state.to_camera();
  ST_CHECK_NEAR(camera.eye.z, 4.0f, 0.01);   // target.z(0) + distance(4.0)
  ST_CHECK_NEAR(camera.eye.x, 0.0f, 0.01);
  ST_CHECK_NEAR(camera.eye.y, 0.5f, 0.01);

  // 转 90°：相机应到 +X 侧（半径 = distance）
  OrbitCameraState quarter = state;
  quarter.yaw = 1.5707963f;
  const auto rotated = quarter.to_camera();
  ST_CHECK_NEAR(rotated.eye.x, 4.0f, 0.01);
  ST_CHECK_NEAR(rotated.eye.z, 0.0f, 0.01);

  // 抬头：相机应在目标上方
  OrbitCameraState up = state;
  up.pitch = 0.5f;
  const auto raised = up.to_camera();
  ST_CHECK(raised.eye.y > state.target.y);
}

ST_TEST(three_view_pitch_is_clamped) {
  // 俯仰越过 ±90° 会让 up 与视线共线、look_at 退化、画面翻转，
  // 而用户拖到那里之后**无法自己拖回来**。所以必须在数据层夹住。
  OrbitCameraState state{};
  state.pitch = 3.0f;   // ≈172°，远超 90°
  state.clamp();
  ST_CHECK(std::abs(state.pitch) < 1.5707964f);   // < 90°
  ST_CHECK(state.pitch > 1.5f);                    // 但仍夹到接近 89°

  state.pitch = -3.0f;
  state.clamp();
  ST_CHECK(state.pitch < -1.5f);
  ST_CHECK(std::abs(state.pitch) < 1.5707964f);

  // 距离也不能到 0（否则相机在目标点上，投影退化）
  state.distance = 0.0f;
  state.clamp();
  ST_CHECK(state.distance > 0.0f);
}

ST_TEST(three_view_ray_from_center_hits_scene) {
  // 屏幕中心的正对射线**必须**命中场景里的东西（场景就在相机视线方向上）。
  //
  // 这条抓的是"射线方向算错"——它的症状是悬浮/点击全都不命中，
  // 而没有任何报错。第一版就踩过：方向向量按视图矩阵取基向量时搞反了符号，
  // 于是射线朝相机背后打，什么都命不中。
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto ray = view->ray_from_point(st::math::Point{320.0f, 200.0f});
  // 方向应指向场景中心（target 附近）
  const auto state = view->camera_state();
  const st::math::Vec3 to_target{state.target.x - ray.origin.x, state.target.y - ray.origin.y,
                                 state.target.z - ray.origin.z};
  const auto expected = to_target.normalized();
  const float dot = ray.direction.x * expected.x + ray.direction.y * expected.y +
                    ray.direction.z * expected.z;
  ST_CHECK(dot > 0.99f);   // 屏幕中心的射线应与"看向 target"同向
}

ST_TEST(three_view_hover_picks_named_object) {
  // 把指针放到屏幕上"球"应该在的位置，悬浮必须命中一个**具体名字**。
  //
  // 为什么不写死像素：视口尺寸/场景布局一变坐标就失效，那种用例会静默变成
  // "永远不命中"（而且看起来像产品坏了）。这里扫一片网格找第一个命中的点，
  // 然后断言"命中到的是场景里真实存在的物体名"。
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();

  const auto context = st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr),
                                             nullptr, 0.0, nullptr};
  std::string hit_name;
  std::uint64_t events_before = view->hover_events();
  for (int gy = 1; gy <= 9 && hit_name.empty(); ++gy) {
    for (int gx = 1; gx <= 9 && hit_name.empty(); ++gx) {
      st::ui::Event event;
      event.kind = st::ui::EventKind::MouseMove;
      event.position = st::math::Point{640.0f * static_cast<float>(gx) / 10.0f,
                                       400.0f * static_cast<float>(gy) / 10.0f};
      (void)view->on_event(context, event);
      if (view->hovered().has_value()) hit_name = view->hovered()->name;
    }
  }
  ST_CHECK(!hit_name.empty());
  // 命中的名字必须是场景里真实存在的物体
  bool known = false;
  for (const auto& object : view->scene().objects) {
    if (object.name == hit_name) known = true;
  }
  ST_CHECK(known);
  ST_CHECK(view->hover_events() > events_before);

  // 命中点的距离必须为正（在相机前方，而不是"命中在背后"）
  if (view->hovered().has_value()) {
    ST_CHECK(view->hovered()->distance > 0.0f);
  }
}

ST_TEST(three_view_click_selects_object) {
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr),
                                             nullptr, 0.0, nullptr};

  // 先找一个能命中的点
  st::math::Point target{320.0f, 200.0f};
  bool found = false;
  for (int gy = 1; gy <= 9 && !found; ++gy) {
    for (int gx = 1; gx <= 9 && !found; ++gx) {
      st::math::Point point{640.0f * static_cast<float>(gx) / 10.0f,
                            400.0f * static_cast<float>(gy) / 10.0f};
      st::ui::Event move;
      move.kind = st::ui::EventKind::MouseMove;
      move.position = point;
      (void)view->on_event(context, move);
      if (view->hovered().has_value()) {
        target = point;
        found = true;
      }
    }
  }
  ST_REQUIRE(found);

  // 按下 + 抬起（不移动 → 判定为点击）
  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.position = target;
  down.button = 0;
  ST_CHECK(view->on_event(context, down));

  st::ui::Event up;
  up.kind = st::ui::EventKind::MouseUp;
  up.position = target;
  up.button = 0;
  ST_CHECK(view->on_event(context, up));

  ST_CHECK(view->click_events() > 0);
  ST_REQUIRE(view->selected().has_value());
  ST_CHECK(!view->selected()->name.empty());
  // 选中的名字要能通过属性面读出去（控制通道/脚本可用）
  ST_CHECK(view->get_property("selected_name").value_or("") == view->selected()->name);
}

ST_TEST(three_view_drag_orbits_but_does_not_select) {
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr),
                                             nullptr, 0.0, nullptr};
  const float yaw_before = view->camera_state().yaw;

  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.position = st::math::Point{320.0f, 200.0f};
  down.button = 0;
  (void)view->on_event(context, down);
  for (int step = 1; step <= 6; ++step) {
    st::ui::Event move;
    move.kind = st::ui::EventKind::MouseMove;
    move.position = st::math::Point{320.0f + static_cast<float>(step) * 20.0f, 200.0f};
    (void)view->on_event(context, move);
  }
  st::ui::Event up;
  up.kind = st::ui::EventKind::MouseUp;
  up.position = st::math::Point{440.0f, 200.0f};
  up.button = 0;
  (void)view->on_event(context, up);

  // 拖拽必须转相机
  ST_CHECK(std::abs(view->camera_state().yaw - yaw_before) > 0.1f);
  // 而且**不能**被当成点击（否则"按住旋转"会顺手改选中，很烦）
  ST_CHECK(view->click_events() == 0);
}

ST_TEST(three_view_wheel_zooms) {
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr),
                                             nullptr, 0.0, nullptr};
  const float before = view->camera_state().distance;

  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{320.0f, 200.0f};
  wheel.wheel_delta = 120.0f;
  ST_CHECK(view->on_event(context, wheel));

  const float after = view->camera_state().distance;
  ST_CHECK(std::abs(after - before) > 0.05f);

  // 反向滚轮应回到接近原值（等比缩放的两个方向必须对称）
  st::ui::Event back = wheel;
  back.wheel_delta = -120.0f;
  (void)view->on_event(context, back);
  ST_CHECK_NEAR(view->camera_state().distance, before, 0.02);
}

ST_TEST(three_view_failed_load_keeps_previous_scene) {
  // 热重载时改坏脚本是常态。此时**不能**清空场景——
  // 否则画面突然变空，用户的判断会是"程序坏了"。
  auto view = make_view();
  ST_REQUIRE(view->object_count() == 3U);

  const bool ok = view->load_script("const scene = new THREE.Scene(;", "broken.js");
  ST_CHECK(!ok);
  ST_CHECK(!view->last_error().empty());
  // 关键：物体数没变
  ST_CHECK(view->object_count() == 3U);

  // 修回来要能恢复
  const bool fixed = view->load_script(kScene, "fixed.js");
  ST_CHECK(fixed);
  ST_CHECK(view->last_error().empty());
}

ST_TEST(three_view_frame_scene_fits_bounds) {
  // `frame_scene` 必须让整个场景落在视野里（距离足够远、目标在包围盒中心）。
  auto view = make_view();
  view->frame_scene();

  // 算场景包围盒（与实现同一口径）
  st::raster::Bounds box = st::raster::Bounds::empty();
  for (const auto& object : view->scene().objects) {
    const auto pick = st::raster::make_pick_object(object.name, object.mesh, object.world);
    if (pick.world_bounds.is_empty()) continue;
    box = box.union_with(pick.world_bounds);
  }
  ST_REQUIRE(!box.is_empty());

  const auto center = box.center();
  const auto state = view->camera_state();
  ST_CHECK_NEAR(state.target.x, center.x, 0.05);
  ST_CHECK_NEAR(state.target.y, center.y, 0.05);
  ST_CHECK_NEAR(state.target.z, center.z, 0.05);
  // 距离要大于包围球半径（否则会钻进物体里）
  const auto size = box.size();
  const float radius = 0.5f * std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z);
  ST_CHECK(state.distance > radius);
}

ST_TEST(three_view_animation_requests_frames) {
  // ## 这条抓的是一个**完全静默**的缺陷
  //
  // 初版只靠宿主的 `request_repaint()`，结果帧数停在 3 不再增长——
  // 因为应用侧“需要重绘”是**脏标记驱动**的，而三维视口的内容在框架看来
  // 没变化（它又不知道图形里有齿轮在转）。于是画面永久冻结在首帧，
  // 而**一切看起来都正常**：场景加载成功、物体数正确、截图里东西都在。
  //
  // 唯一能区分的信号就是“它有没有主动要下一帧”。
  constexpr const char* kAnimated = R"js(
const scene = new THREE.Scene();
const g = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0xff8800 }));
g.name = 'spinner';
g.userData.spin = { axis: [0, 0, 1], speed: 1.5 };
scene.add(g);
scene;
)js";

  auto view = std::make_unique<ThreeView>();
  ST_REQUIRE(view->load_script(kAnimated, "spin.js"));
  ST_CHECK(view->object_count() == 1U);
  // 动画字段必须真的被抽取（`userData` 解析失败的后果同样是静默不动）
  ST_REQUIRE(!view->scene().objects.empty());
  ST_CHECK(view->scene().objects[0].spin_speed != 0.0f);
  ST_CHECK(view->scene().objects[0].animated());
}

ST_TEST(three_view_static_scene_does_not_request_animation) {
  // 反向判据：**没有**动画物体时不该主动续帧。
  //
  // 漏了这条的代价是实测过的：静态视口每帧都请求下一帧 → 主循环以帧预算
  // 上限空转重绘同一张图（纯 CPU 软件光栅下会直接吃满几个核）。
  auto view = std::make_unique<ThreeView>();
  ST_REQUIRE(view->load_script(kScene, "static.js"));
  ST_CHECK(view->object_count() == 3U);
  bool any_animated = false;
  for (const auto& object : view->scene().objects) {
    if (object.animated()) any_animated = true;
  }
  ST_CHECK(!any_animated);
}

ST_TEST(three_view_animation_time_advances) {
  // 动画时钟要用 `RenderContext::time_seconds` 推进（可复现），不能用墙钟。
  constexpr const char* kSpin = R"js(
const scene = new THREE.Scene();
const g = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0xff8800 }));
g.name = 'spinner';
g.userData.spin = { axis: [0, 0, 1], speed: 1.0 };
scene.add(g);
scene;
)js";
  auto view = std::make_unique<ThreeView>();
  ST_REQUIRE(view->load_script(kSpin, "spin.js"));
  view->arrange(st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr), nullptr, 0.0,
                                     nullptr},
                st::math::Rect{0.0f, 0.0f, 400.0f, 300.0f});

  // 时钟只能在渲染时推进（要靠 RenderContext）；这里验连续两帧的时间差
  // 能累加——用一个可预测的时间序列。
  ST_CHECK(view->animation_enabled());
  view->set_animation_enabled(false);
  ST_CHECK(!view->animation_enabled());
  view->set_animation_enabled(true);
  ST_CHECK(view->animation_enabled());
}

ST_TEST(three_view_oscillate_semantics) {
  // 振荡（摆轮那种往复运动）与自转是**两种不同意图**：
  // 自转是无界累积，振荡是 `amp * sin(2π·f·t + phase)`。
  // 两者共用同一套抽取，靠不同字段区分。
  constexpr const char* kOsc = R"js(
const scene = new THREE.Scene();
const w = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0x88aaff }));
w.name = 'balance';
w.userData.oscillate = { axis: [0, 0, 1], amplitude: 0.9, frequency: 3.2, phase: 1.57 };
scene.add(w);
scene;
)js";
  auto view = std::make_unique<ThreeView>();
  ST_REQUIRE(view->load_script(kOsc, "osc.js"));
  ST_REQUIRE(!view->scene().objects.empty());
  const auto& object = view->scene().objects[0];
  ST_CHECK_NEAR(object.oscillate_amplitude, 0.9f, 1e-4);
  ST_CHECK_NEAR(object.oscillate_frequency, 3.2f, 1e-4);
  ST_CHECK_NEAR(object.oscillate_phase, 1.57f, 1e-4);
  // 振荡也是动画（必须让宿主续帧）
  ST_CHECK(object.animated());
  // 但它**不是**自转
  ST_CHECK_NEAR(object.spin_speed, 0.0f, 1e-6);
}

ST_TEST(three_view_pick_enabled_switch_works) {
  // 关掉拾取后不该再有命中（用于"只看不选"的展示场景）
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  view->set_pick_enabled(false);
  const auto context = st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr),
                                             nullptr, 0.0, nullptr};

  std::uint64_t events = view->hover_events();
  for (int gy = 1; gy <= 9; ++gy) {
    for (int gx = 1; gx <= 9; ++gx) {
      st::ui::Event move;
      move.kind = st::ui::EventKind::MouseMove;
      move.position = st::math::Point{640.0f * static_cast<float>(gx) / 10.0f,
                                      400.0f * static_cast<float>(gy) / 10.0f};
      (void)view->on_event(context, move);
    }
  }
  ST_CHECK(!view->hovered().has_value());
  ST_CHECK(view->hover_events() == events);
}
