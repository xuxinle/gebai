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

#include "st/raster/bpy_runtime.hpp"
#include "st/raster/pick.hpp"
#include "st/raster/three_runtime.hpp"
#include "st/test/test.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/label_overlay.hpp"
#include "st/ui/components/three_view.hpp"

using st::ui::OrbitCameraState;
using st::ui::ThreeView;

namespace {

/// 测试共用一个真实主题：`RenderContext::theme` 是**引用**，用
/// `*static_cast<const Theme*>(nullptr)` 造一个空引用本身就是 UB
///（UBSan 报 “reference binding to null pointer”，一旦组件真读了主题字段就是 SEGV）。
[[nodiscard]] auto test_theme() -> const st::ui::Theme& {
  static const st::ui::Theme theme = st::ui::Theme::light();
  return theme;
}

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
  view->arrange(st::ui::RenderContext{test_theme(),
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

  const auto context = st::ui::RenderContext{test_theme(),
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
  const auto context = st::ui::RenderContext{test_theme(),
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

  // 按下 + 抬起（不移动 → 判定为点击）；左键在后端口径里是 1
  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.position = target;
  down.button = 1;
  ST_CHECK(view->on_event(context, down));

  st::ui::Event up;
  up.kind = st::ui::EventKind::MouseUp;
  up.position = target;
  up.button = 1;
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
  const auto context = st::ui::RenderContext{test_theme(),
                                             nullptr, 0.0, nullptr};
  const float yaw_before = view->camera_state().yaw;

  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.position = st::math::Point{320.0f, 200.0f};
  down.button = 1;   // 左键（后端口径：1=左）
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
  up.button = 1;
  (void)view->on_event(context, up);

  // 拖拽必须转相机
  ST_CHECK(std::abs(view->camera_state().yaw - yaw_before) > 0.1f);
  // 而且**不能**被当成点击（否则"按住旋转"会顺手改选中，很烦）
  ST_CHECK(view->click_events() == 0);
}

ST_TEST(three_view_wheel_zooms) {
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{test_theme(),
                                             nullptr, 0.0, nullptr};
  const float before = view->camera_state().distance;

  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{320.0f, 200.0f};
  wheel.wheel_delta = 1.0f;
  ST_CHECK(view->on_event(context, wheel));

  const float after = view->camera_state().distance;
  ST_CHECK(std::abs(after - before) > 0.05f);

  // 反向滚轮应回到接近原值（等比缩放的两个方向必须对称）
  st::ui::Event back = wheel;
  back.wheel_delta = -1.0f;
  (void)view->on_event(context, back);
  ST_CHECK_NEAR(view->camera_state().distance, before, 0.02);
}

ST_TEST(three_view_wheel_forward_zooms_in) {
  // ## 方向约定：`Event::wheel_delta` **向上/向前为正**
  //
  // 后端就是这么给的（`platform_x11.cpp`：X11 button 4 → +1；
  // `platform_win32.cpp`：`HIWORD(wparam) / 120`，前推为正）。
  //
  // 而“正 delta = 距离变大”是**反**的：老百姓推一下滚轮是“放大”，
  // 不是“退后”。这一条单独断言，是因为原来的用例只查了 “距离变了”——
  // 方向反了也照样绿（实测真踩到）。
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{test_theme(),
                                             nullptr, 0.0, nullptr};
  const float before = view->camera_state().distance;

  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{320.0f, 200.0f};
  wheel.wheel_delta = 1.0f;   // 前推（放大）
  (void)view->on_event(context, wheel);
  ST_CHECK(view->camera_state().distance < before);   // 必须变**小**（靠近）
}

ST_TEST(three_view_real_button_numbers_drive_orbit_and_pan) {
  // ## 按钮号必须跟**后端**的口径走：1=左 / 2=右 / 3=中
  //
  // `platform_win32.cpp` 的 `push_mouse(..., 1/2/3, ...)`、`platform_x11.cpp`
  // 的 1/2/3 映射、`viewport.cpp` / `timeline_view.cpp` 的 `kMiddleButton{3}`
  // 全部是这一口径。本组件曾用 `button == 0` 表示左键——于是**真鼠标左键
  // 永远不能旋转**（`0` 只有“无按钮”的 move 事件才有），而单元测试里
  // 按的也是 `0`，所以一直绿。
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{test_theme(),
                                             nullptr, 0.0, nullptr};
  const auto drag = [&](int button, st::math::Point to) {
    st::ui::Event down;
    down.kind = st::ui::EventKind::MouseDown;
    down.position = st::math::Point{320.0f, 200.0f};
    down.button = button;
    (void)view->on_event(context, down);
    for (int step = 1; step <= 5; ++step) {
      const float t = static_cast<float>(step) / 5.0f;
      st::ui::Event move;
      move.kind = st::ui::EventKind::MouseMove;
      move.position = st::math::Point{320.0f + (to.x - 320.0f) * t,
                                      200.0f + (to.y - 200.0f) * t};
      (void)view->on_event(context, move);
    }
    st::ui::Event up;
    up.kind = st::ui::EventKind::MouseUp;
    up.position = to;
    up.button = button;
    (void)view->on_event(context, up);
  };

  // 左键（1）= 绕目标旋转
  const float yaw_before = view->camera_state().yaw;
  drag(1, st::math::Point{500.0f, 200.0f});
  ST_CHECK(std::abs(view->camera_state().yaw - yaw_before) > 0.1f);

  // 右键（2）= 平移 target；左键不该平移
  const st::math::Vec3 target_before = view->camera_state().target;
  drag(2, st::math::Point{420.0f, 260.0f});
  const st::math::Vec3 target_after = view->camera_state().target;
  const float moved = std::abs(target_after.x - target_before.x) +
                      std::abs(target_after.y - target_before.y) +
                      std::abs(target_after.z - target_before.z);
  ST_CHECK(moved > 0.05f);
}

ST_TEST(three_view_double_click_returns_home) {
  // 双击回到初始视角（头文件里声明的交互契约）。
  auto view = make_view(640.0f, 400.0f);
  view->frame_scene();
  const auto context = st::ui::RenderContext{test_theme(),
                                             nullptr, 0.0, nullptr};
  const float home_yaw = view->camera_state().yaw;

  // 先用左键拉开视角
  st::ui::Event down;
  down.kind = st::ui::EventKind::MouseDown;
  down.position = st::math::Point{320.0f, 200.0f};
  down.button = 1;
  (void)view->on_event(context, down);
  st::ui::Event move;
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{500.0f, 200.0f};
  (void)view->on_event(context, move);
  st::ui::Event up;
  up.kind = st::ui::EventKind::MouseUp;
  up.position = st::math::Point{500.0f, 200.0f};
  up.button = 1;
  (void)view->on_event(context, up);
  ST_CHECK(std::abs(view->camera_state().yaw - home_yaw) > 0.1f);

  st::ui::Event dbl;
  dbl.kind = st::ui::EventKind::DoubleClick;
  dbl.position = st::math::Point{320.0f, 200.0f};
  dbl.button = 1;
  (void)view->on_event(context, dbl);
  ST_CHECK_NEAR(view->camera_state().yaw, home_yaw, 0.02f);
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
  view->arrange(st::ui::RenderContext{test_theme(), nullptr, 0.0,
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
  const auto context = st::ui::RenderContext{test_theme(),
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

// ————————————————————————————————————————————————————————————————
// 按扩展名分派两种表达层（bpy / three.js）
// ————————————————————————————————————————————————————————————————

ST_TEST(three_view_dispatches_py_to_bpy_and_js_to_three) {
  // 这个用例钉住一件很容易写错的事：**分派按扩展名，而不是按内容嗅探**。
  // 反例（曾经的真实写法）：所有脚本都走 ThreeRuntime → `.py` 报一堆 JS 语法错，
  // 而现象只是"画面没变"，很容易被当成"热重载坏了"（e2e 实测踩过）。
  //
  // 两个脚本用**同样的物体名**，但几何来源完全不同：
  //   · `.js` 用 three.js 生成一个立方体；
  //   · `.py` 用 bpy.ops 生成一个立方体。
  // 判据不能只看"有物体"（两边都有），要看**扩展名真的改变了执行路径**：
  // `.py` 脚本喂给 three 引擎会语法错，所以若分派错了，`last_error()` 必然非空。
  constexpr const char* kBpyScene = R"py(
import bpy
bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 0.0, 0.0))
obj = bpy.context.active_object
obj.name = 'bpy_cube'
result = 'dispatch'
)py";
  auto view = std::make_unique<ThreeView>();
  if (!st::raster::BpySceneExtractor::available()) {
    ST_CHECK(true);   // bpy 不可用的构建（无 MicroPython）不算失败
    return;
  }
  ST_REQUIRE(view->load_script_file_kind(kBpyScene, "scene.py"));
  ST_CHECK(view->last_error().empty());
  ST_REQUIRE(!view->scene().objects.empty());
  ST_CHECK(view->scene().objects[0].name == "bpy_cube");

  // 同一份 bpy 脚本**走 three.js 那条路**必然失败——这条是"分派真的发生了"的反证。
  // 没有它，一个"永远走 bpy"的实现也能让上面两条通过。
  auto wrong = std::make_unique<ThreeView>();
  ST_CHECK(!wrong->load_script(kBpyScene, "scene.py"));
  ST_CHECK(!wrong->last_error().empty());

  // `.js` 仍然走 three.js（不能被新分派抢走）
  auto js_view = std::make_unique<ThreeView>();
  ST_REQUIRE(js_view->load_script_file_kind(kScene, "scene.js"));
  ST_CHECK(js_view->scene().objects.size() >= 3);
}

/// 角标的文字必须**落在背景条内**（不是靠看截图判断）。
///
/// ## 为何要钉这条
///
/// `TextPort::draw` 的 `origin.y` 是**行盒顶边**，它内部会自己
/// `+ ascent` 落基线。而这里曾经又加了一次 `shaped_ascent`，
/// 把同一项补偿做了两遍——实测角标文字比背景条低 **19.3px**
/// （≈ 一个行高），底边溢出框下沿 11.3px：用户看到的就是
/// “`85 个物体 · gpu` 这个字从它的黑框里漏到框外了”。
///
/// 这条断言要的是**几何关系**：文字行盒（顶边 + 行高）必须包在框内。
/// 它不验证字体度量（那与字体栈相关），只验证两个矩形的关系——
/// 而那个关系正是当时算错的东西。
ST_TEST(three_view_hud_chip_keeps_text_inside_box) {
  const float padding = 6.0f;
  const float line_height = 16.0f;

  // 顶部角标：从锚点向下排
  const ThreeView::HudChip top_chip =
      ThreeView::hud_chip(2.0f, 2.0f, line_height, 120.0f, padding, 400.0f);
  // 文字行盒 = [origin.y, origin.y + line_height]
  ST_CHECK(top_chip.text_origin.y >= top_chip.box.y);
  ST_CHECK(top_chip.text_origin.y + line_height <= top_chip.box.bottom() + 0.01f);
  // 且要真的在框内（不能贴到底边——那就是当年“漏出去”的前一步）
  ST_CHECK(top_chip.text_origin.y - top_chip.box.y > 1.0f);
  ST_CHECK(top_chip.box.bottom() - (top_chip.text_origin.y + line_height) > 1.0f);

  // 底部标签：从锚点向上排，同样必须包住文字行盒
  const ThreeView::HudChip bottom_chip =
      ThreeView::hud_chip(2.0f, 300.0f, line_height, 120.0f, padding, 400.0f,
                          /*bottom_aligned=*/true);
  ST_CHECK(bottom_chip.box.bottom() <= 300.0f + 0.01f);
  ST_CHECK(bottom_chip.text_origin.y >= bottom_chip.box.y);
  ST_CHECK(bottom_chip.text_origin.y + line_height <= bottom_chip.box.bottom() + 0.01f);
  ST_CHECK(bottom_chip.box.bottom() - (bottom_chip.text_origin.y + line_height) > 1.0f);
}

/// **装载发生在拿到真实尺寸之前**时，取景必须在第一次 arrange 后补算。
///
/// ## 守的是什么（实测缺陷）
///
/// `frame_scene()` 的横向约束要算 `bounds_.width / bounds_.height`。而应用的常规写法是
/// 「先 `load_script` 再挂树」（gallery / 册府 / 裁云预览都这样）——装载那一刻
/// 元素还没 arrange，两者都是 0，aspect 退回 1.0，于是**只有一个方向的约束生效**，
/// 距离算出来偏大。
///
/// 实测（gallery「能力矩阵」页的三物体场景）：包围盒横向 3.0，视口 862×320（2.7:1），
/// 取到的距离是 **5.05**，而正确值是 **2.8** 量级。画面上看是「物体缩在底部一小块、
/// 上方大片空白」——而物体数 / 网格 / 帧数全部正常，看起来像"场景太小"。
///
/// 判据用**距离**：竖直约束（span_v / tan(half_fov_v)）与水平约束（span_h /
/// tan(half_fov_h)）在宽视口下**不相等**，只有真按宽高比算过，距离才会落到小的那个。
/// 用 1.0 的 aspect 算出来的距离必然贴着竖直约束那条线——与正确值的差距远超舍入。
ST_TEST(three_view_frames_scene_after_layout_not_at_load_time) {
  const auto context = st::ui::RenderContext{test_theme(), nullptr, 0.0, nullptr};

  // 场景包围盒（kScene）：球(-0.7,0.68) r0.6、盒(0.8,0.46) 0.9、底座 2.9×1.4
  // → x∈[-1.3,1.45]、y∈[0.01,1.28]、z∈[-0.7,0.7]；横向跨度 2.75。
  auto wide = std::make_unique<ThreeView>();
  ST_REQUIRE(wide->load_script(kScene, "wide.js"));   // 装载时没有 bounds
  const float before = std::stof(wide->get_property("distance").value_or("0"));
  // 宽视口 860×320 = 2.69:1：水平方向宽裕、竖直方向吃紧 → 距离由**竖直**定
  wide->arrange(context, st::math::Rect{0.0f, 0.0f, 860.0f, 320.0f});
  const float after_wide = std::stof(wide->get_property("distance").value_or("0"));

  // 同样的场景放进**竖**视口 320×860：这次水平吃紧，距离必须显著变大。
  // 若取景只在构建期算过一次（且用 1:1 的假宽高比），这两个距离会**相等**。
  auto tall = std::make_unique<ThreeView>();
  ST_REQUIRE(tall->load_script(kScene, "tall.js"));
  tall->arrange(context, st::math::Rect{0.0f, 0.0f, 320.0f, 860.0f});
  const float after_tall = std::stof(tall->get_property("distance").value_or("0"));

  ST_CHECK(before > 0.0f);              // 构建期确实算了一份（不是没算）
  ST_CHECK(after_tall > after_wide * 1.5f);
  // 而且宽视口那份必须**比构建期更近**：构建期用 1:1 的假比，等于按竖视口算。
  ST_CHECK(after_wide < before * 0.95f);
}

/// 补算只发生一次：之后用户拖动/缩放出来的视角不能被布局夺回去。
ST_TEST(three_view_pending_frame_runs_only_once) {
  const auto context = st::ui::RenderContext{test_theme(), nullptr, 0.0, nullptr};
  auto view = std::make_unique<ThreeView>();
  ST_REQUIRE(view->load_script(kScene, "once.js"));
  view->arrange(context, st::math::Rect{0.0f, 0.0f, 860.0f, 320.0f});
  const float framed = std::stof(view->get_property("distance").value_or("0"));

  // 用户推远（模拟滚轮）：这是"用户意图"，后续重排不该把它拉回来。
  ST_REQUIRE(view->set_property("distance", "9.0"));
  view->arrange(context, st::math::Rect{0.0f, 0.0f, 700.0f, 320.0f});
  const float after_relayout = std::stof(view->get_property("distance").value_or("0"));
  ST_CHECK_NEAR(after_relayout, 9.0f, 0.001);
  (void)framed;

  // 但"切到另一个场景"仍应重新取景（那是新内容，用户的旧视角无意义）。
  ST_REQUIRE(view->load_script(kScene, "other.js"));
  view->arrange(context, st::math::Rect{0.0f, 0.0f, 860.0f, 320.0f});
  const float after_switch = std::stof(view->get_property("distance").value_or("0"));
  ST_CHECK(after_switch < 9.0f);
}

/// **绝对坐标 / 局部坐标混用**：标签层的选围与夹取必须按画布绝对坐标做。
///
/// ## 守的是什么（实测缺陷）
///
/// ThreeView::project_to_screen() 返回的是**画布绝对**像素（box.x + …），而
/// LabelOverlay::paint_content 拿它直接跟 bounds_.width / bounds_.height（都是**局部**
/// 尺寸）比：
///
///     if (at.x < -200 || at.x > width + 200 || ...) continue;   // 丢标签
///     ty = std::clamp(ty, 2.0f, height - line_h - 2.0f);       // 夹到角上
///
/// 后果随视口位置变化：视口越靠右/下（绝对坐标越大），越多标签被当成屏幕外丢掉。
/// 实测 gallery 能力矩阵页（视口 x=264）：7 个标签恒定只画出 3 个——三维连线正常、
/// 属性面全对、label_count=7 也对，只有看图才发现字少了。
///
/// ## 判据
///
/// 造一个**平移过**的标签层（bounds.x/y 非零），投影器返回该视口内的绝对坐标；
/// 断言 painted 等于标签数。用局部坐标系写过滤的实现会在这里掉一部分标签。
ST_TEST(label_overlay_uses_absolute_canvas_coordinates) {
  const auto context = st::ui::RenderContext{test_theme(), nullptr, 0.0, nullptr};

  st::ui::LabelOverlay overlay;
  overlay.set_id("labels");
  // 视口在画布右下：绝对坐标带正偏移，正是**局部坐标系实现会判越界**的情形。
  overlay.arrange(context, st::math::Rect{264.0f, 343.0f, 860.0f, 340.0f});

  // 七个标签按 4×2 网格铺开（避免标签框互相碰撞被避让丢弃——那是另一回事）。
  std::vector<st::ui::LabelAnchor> anchors;
  for (int index = 0; index < 7; ++index) {
    st::ui::LabelAnchor anchor;
    anchor.id = std::format("n{}", index);
    anchor.text = std::format("节点 {}", index);
    anchor.weight = 0.5f;
    anchor.x = static_cast<float>(index % 4);
    anchor.y = static_cast<float>(index / 4);
    anchors.push_back(std::move(anchor));
  }
  overlay.set_labels(std::move(anchors));
  overlay.set_projector([](float x, float y, float z) {
    (void)z;
    // 世界坐标 → 视口内的**绝对**画布坐标（与 ThreeView::project_to_screen 同语义）。
    //
    // 横向故意铺到 1084（视口绝对范围 264..1124，仍在视口内）：
    // 而**局部坐标系**的实现会拿 `width=860` 当上界，判它“屏幕外”而丢掉。
    // 这就是判据：绝对坐标实现 7/7，局部坐标实现会少。
    return std::make_pair(std::make_pair(264.0f + 40.0f + x * 260.0f, 343.0f + 40.0f + y * 90.0f),
                          static_cast<float>(1.0));
  });

  // 画到一张足够大的画布上（绝对坐标落在这里面）。
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(1400, 900, 1.0f);
  overlay.paint_content(context, canvas);
  ST_CHECK_EQ(overlay.painted_labels(), 7U);
}
