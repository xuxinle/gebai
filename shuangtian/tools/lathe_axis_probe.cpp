// 实测 `LatheGeometry` 的轴向与 `rotation.x` 的效果。
//
// 推导容易出错（右手系绕 X 转 ±90° 到底是 Y→Z 还是 Y→-Z），
// 而错一次的代价是“表壳盖住表盘”这种很难归因的现象。
// 所以直接画三个不同旋转的旋转体，看它们在屏幕上的位置。
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/pick.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/three_runtime.hpp"

int main() {
  // 一个明显的“圆台”：底部半径 2、顶部半径 0.3、高度 3。
  // 如果 Lathe 的轴向是 Y，它会竖直立着；转到 Z 后应该指向观察者。
  const char* source = R"js(
const s = new THREE.Scene();
const c = new THREE.PerspectiveCamera(40, 16/9, 0.1, 100);
c.position.set(0, 0, 14);
c.quaternion.setFromAxisAngle(new THREE.Vector3(0,1,0), 0);
s.add(c);
s.add(new THREE.AmbientLight(0xffffff, 1.2));

// 不旋转：Lathe 的原生姿态
const a = new THREE.Mesh(new THREE.LatheGeometry([
  new THREE.Vector2(2.0, -1.5), new THREE.Vector2(0.3, 1.5)
], 32), new THREE.MeshStandardMaterial({ color: 0xff3333 }));
a.name = 'no_rotation';
a.position.set(-4, 0, 0);
s.add(a);

// rotation.x = +PI/2
const b = new THREE.Mesh(new THREE.LatheGeometry([
  new THREE.Vector2(2.0, -1.5), new THREE.Vector2(0.3, 1.5)
], 32), new THREE.MeshStandardMaterial({ color: 0x33ff33 }));
b.name = 'rot_x_plus90';
b.rotation.x = Math.PI / 2;
b.position.set(0, 0, 0);
s.add(b);

// rotation.x = -PI/2
const d = new THREE.Mesh(new THREE.LatheGeometry([
  new THREE.Vector2(2.0, -1.5), new THREE.Vector2(0.3, 1.5)
], 32), new THREE.MeshStandardMaterial({ color: 0x3333ff }));
d.name = 'rot_x_minus90';
d.rotation.x = -Math.PI / 2;
d.position.set(4, 0, 0);
s.add(d);

s;
)js";

  auto scene = st::raster::ThreeRuntime::instance().evaluate(source, "<probe>");
  if (!scene.has_value()) {
    std::printf("求值失败：%s\n", scene.error().message.c_str());
    return 1;
  }
  for (const auto& object : scene->objects) {
    const auto pick = st::raster::make_pick_object(object.name, object.mesh, object.world);
    const auto& b = pick.world_bounds;
    std::printf("%-16s 盒 min=(%.2f,%.2f,%.2f) max=(%.2f,%.2f,%.2f)  跨度=(%.2f,%.2f,%.2f)\n",
                object.name.c_str(), static_cast<double>(b.min.x), static_cast<double>(b.min.y),
                static_cast<double>(b.min.z), static_cast<double>(b.max.x),
                static_cast<double>(b.max.y), static_cast<double>(b.max.z),
                static_cast<double>(b.max.x - b.min.x),
                static_cast<double>(b.max.y - b.min.y),
                static_cast<double>(b.max.z - b.min.z));
  }
  std::printf("\n判读：哪个轴的跨度是 3（高度）就说明轴向在哪个轴。\n");
  return 0;
}
