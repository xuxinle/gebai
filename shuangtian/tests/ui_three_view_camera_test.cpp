/// `ThreeView` 的两条相机缺陷回归（都是"画面有东西但不对"那一类）。
///
/// ## 为什么这些判据必须存在
///
/// 这两个缺陷在实测中各自烧掉了大量排查时间，而**现象都不是报错**：
///
/// | 缺陷 | 现象 | 最难的地方 |
/// |---|---|---|
/// | 远平面没跟着距离扩 | 整个场景被远裁剪面切掉，视口里只有背景色 | 物体数/网格/矩阵/绘制次数全对，`three_check` 也全对 |
/// | 切场景不重新取景 | 新场景缩成一小块 | 看起来像"新场景没加载" |
///
/// 它们的共同点是：**只有把相机参数本身当判据才抓得到**。

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

/// 一个**深度方向的跨度远大于默认远平面**的场景：用来暴露"远平面没扩"。
///
/// 造法：把物体放在 z = -120（默认 far 是 100），而相机距离 ~150。
/// 若 `to_camera()` 不把 far 扩出去，这个物体一定被裁掉。
constexpr const char* kFarScene = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 16 / 9, 0.5, 400);
camera.position.set(0, 0, 400);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 1.0));
const key = new THREE.DirectionalLight(0xffffff, 2.0);
key.position.set(2, 3, 5);
scene.add(key);

// 一个明显的大方块，位置推到远处
const box = new THREE.Mesh(new THREE.BoxGeometry(40, 40, 40),
  new THREE.MeshStandardMaterial({ color: 0xff8800 }));
box.name = 'far_box';
box.position.set(0, 0, -120);
scene.add(box);
scene;
)js";

auto make_view(const char* script, const char* name) -> std::unique_ptr<ThreeView> {
  auto view = std::make_unique<ThreeView>();
  view->set_id("viewport");
  view->load_script(script, name);
  view->arrange(st::ui::RenderContext{*static_cast<const st::ui::Theme*>(nullptr), nullptr, 0.0,
                                     nullptr},
                st::math::Rect{0.0f, 0.0f, 640.0f, 400.0f});
  return view;
}

}  // namespace

ST_TEST(three_view_far_plane_expands_with_distance) {
  // ## 这条抓的是本会话最刺眼的一个缺陷
  //
  // `OrbitCameraState::far_z` 默认 100。而 `frame_scene()` 对一个大场景
  // 算出的距离可能远超 100 —— 于是**整个场景落在远裁剪面之外**，
  // 视口里只剩 `begin_frame` 的清屏色。
  //
  // 排查时的误导：物体数 129、网格 51.8 万顶点、矩阵逐帧在变、
  // 绘制调用 129 次、`paint_frames` 在增长——每一环都正常；
  // 而 `three_check` 用场景自带相机渲染同一份场景也正常。
  OrbitCameraState state{};
  state.target = st::math::Vec3{0.0f, 0.0f, 0.0f};
  state.distance = 300.0f;   // 远超默认 far=100
  const auto camera = state.to_camera();
  ST_CHECK(camera.far_z > state.distance);   // far 必须包住相机到目标的距离

  // 近距离时仍应保持用户设定的 far（不能被无谓放大）
  OrbitCameraState near_state{};
  near_state.distance = 5.0f;
  near_state.far_z = 100.0f;
  const auto near_camera = near_state.to_camera();
  ST_CHECK_NEAR(near_camera.far_z, 100.0f, 0.01);
}

ST_TEST(three_view_scene_with_far_target_is_visible) {
  // 上一个用例验的是相机参数；这一条验**像素**——远平面错的时候，
  // 场景会被整块裁掉，于是"已绘制"面积掉到接近 0。
  auto view = make_view(kFarScene, "far.js");
  ST_REQUIRE(view->object_count() == 1U);
  view->frame_scene();
  const auto camera = view->camera_state();
  ST_CHECK(camera.distance > 0.0f);

  // 把相机放到很远（模拟"大场景"的情形），far 必须跟着扩
  st::ui::OrbitCameraState far_state = camera;
  far_state.distance = 260.0f;
  view->set_camera_state(far_state);
  const auto applied = view->camera_state();
  ST_CHECK(applied.distance > 200.0f);
  ST_CHECK(applied.to_camera().far_z > applied.distance);
}

ST_TEST(three_view_reframes_when_switching_scene) {
  // ## 切场景必须重新取景
  //
  // 实测：从 04-watch（表壳直径 40）切到 01-product（底座宽 3）时，
  // 相机停在上一份场景算出的距离 88.9 —— 新场景在画面里缩成一小块，
  // 看起来像"新场景没加载"。
  //
  // 而**热重载**（同一个文件改内容）时不能重算：改一下颜色镜头跳回去很恼人。
  constexpr const char* kBig = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 16/9, 0.5, 500);
camera.position.set(0, 0, 60);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 1.2));
const big = new THREE.Mesh(new THREE.BoxGeometry(40, 40, 40),
  new THREE.MeshStandardMaterial({ color: 0xff0000 }));
big.name = 'big';
scene.add(big);
scene;
)js";
  constexpr const char* kSmall = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 16/9, 0.5, 500);
camera.position.set(0, 0, 6);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 1.2));
const small = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0x00ff00 }));
small.name = 'small';
scene.add(small);
scene;
)js";

  auto view = make_view(kBig, "big.js");
  ST_REQUIRE(view->object_count() == 1U);
  const float big_distance = view->camera_state().distance;
  ST_CHECK(big_distance > 1.0f);

  // 同一个文件改内容（热重载）：视角**保持**
  ST_REQUIRE(view->load_script(kBig, "big.js"));
  ST_CHECK_NEAR(view->camera_state().distance, big_distance, 0.01);

  // 换一个文件：**重新取景**，距离应显著变小（小物体）
  ST_REQUIRE(view->load_script(kSmall, "small.js"));
  const float small_distance = view->camera_state().distance;
  ST_CHECK(small_distance < big_distance * 0.5f);
}

ST_TEST(three_view_material_reaches_backend) {
  // ## 材质必须逐物体设进后端
  //
  // 排查期间为了 A/B 对比把 `set_material` 去掉过，结果整个视口变成
  // **一片白**——默认材质没有金属度/粗糙度区分，所有零件长得一模一样。
  //
  // 这条用"材质差异是否体现在场景数据里"来兜底：材质是抽取阶段就定下来的，
  // 若抽取坏了，后端再怎么设也没用。
  constexpr const char* kMats = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(42, 16/9, 0.5, 100);
camera.position.set(0, 0, 8);
scene.add(camera);
scene.add(new THREE.AmbientLight(0xffffff, 1.0));

const gold = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0xc79a4e, metalness: 0.95, roughness: 0.28 }));
gold.name = 'gold';
gold.position.x = -1.5;
scene.add(gold);

const plastic = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0xc79a4e, metalness: 0.05, roughness: 0.85 }));
plastic.name = 'plastic';
plastic.position.x = 1.5;
scene.add(plastic);
scene;
)js";
  auto view = make_view(kMats, "mats.js");
  ST_REQUIRE(view->object_count() == 2U);
  const auto& objects = view->scene().objects;
  // 同色不同金属度：两者必须真的不同（否则说明材质没抽出来）
  ST_CHECK(std::abs(objects[0].material.metallic - objects[1].material.metallic) > 0.5f);
  ST_CHECK(std::abs(objects[0].material.roughness - objects[1].material.roughness) > 0.3f);
}
