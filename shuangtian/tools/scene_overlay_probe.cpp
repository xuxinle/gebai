// 验证 `camera_override` / `extra_transform` / `material_override` 三个叠加窗。
//
// ## 为何要单独验这三个
//
// 它们是"组合能力"的基础设施：裁云的 `model_*` 字段全靠这三扇窗接进
// three 场景。若任一扇没生效，症状是**画面完全不变**——不报错、也不警告，
// 只能靠对比渲染结果才能发现（实测就在 `camera_override` 上卡了一轮）。
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

constexpr const char* kScript = R"js(
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(30, 1, 0.1, 500);
camera.position.set(0, 0, 60);
camera.lookAt(0, 0, 0);
scene.add(camera);
scene.add(new THREE.AmbientLight(0x8899bb, 0.8));
const key = new THREE.DirectionalLight(0xffffff, 2.2);
key.position.set(3, 4, 5);
scene.add(key);
// 一个不对称的物体：转起来一定看得出变化
const box = new THREE.Mesh(new THREE.BoxGeometry(20, 20, 20),
  new THREE.MeshStandardMaterial({ color: 0xc79a4e, metalness: 0.9, roughness: 0.25 }));
box.name = 'box';
box.position.set(4, 0, 0);
scene.add(box);
scene;
)js";

auto painted_ratio(const st::raster::Canvas& canvas) -> double {
  const auto pixels = canvas.pixels();
  if (pixels.empty()) return 0.0;
  const std::uint32_t bg = pixels[0];
  std::size_t painted = 0;
  for (const std::uint32_t pixel : pixels) {
    if (pixel != bg) ++painted;
  }
  return static_cast<double>(painted) / static_cast<double>(pixels.size());
}

/// 两张画布的像素差异比例。
auto diff_ratio(const st::raster::Canvas& a, const st::raster::Canvas& b) -> double {
  const auto pa = a.pixels();
  const auto pb = b.pixels();
  if (pa.size() != pb.size() || pa.empty()) return 1.0;
  std::size_t different = 0;
  for (std::size_t i = 0; i < pa.size(); ++i) {
    if (pa[i] != pb[i]) ++different;
  }
  return static_cast<double>(different) / static_cast<double>(pa.size());
}

auto render(const st::raster::SceneSpec& spec, st::raster::Canvas& canvas) -> bool {
  auto created = st::raster::Scene3D::create(canvas.physical_width(), canvas.physical_height());
  if (!created.has_value()) return false;
  const auto status = st::raster::render_scene_into(spec, **created, canvas,
                                                    st::math::Color{10, 12, 16, 255});
  return status.has_value();
}

}  // namespace

int main() {
  const int size = 320;
  auto base_spec = st::raster::scene_spec_from_three(kScript, "<probe>");
  if (!base_spec.has_value()) {
    std::printf("场景求值失败：%s\n", base_spec.error().message.c_str());
    return 1;
  }

  st::raster::Canvas base{size, size, 1.0f};
  if (!render(*base_spec, base)) {
    std::printf("基准渲染失败\n");
    return 1;
  }
  std::printf("基准：已绘制 %.2f%%\n", painted_ratio(base) * 100.0);

  // ① extra_transform：整体绕 Y 转 40°
  {
    st::raster::SceneSpec spec = *base_spec;
    spec.extra_transform = st::math::Mat4::rotation(st::math::Vec3{0.0f, 1.0f, 0.0f}, 0.7f);
    st::raster::Canvas canvas{size, size, 1.0f};
    if (!render(spec, canvas)) {
      std::printf("① extra_transform 渲染失败\n");
    } else {
      const double ratio = diff_ratio(base, canvas);
      std::printf("① extra_transform：差异 %.2f%%  %s\n", ratio * 100.0,
                  ratio > 0.01 ? "✓ 生效" : "✗ 没生效");
    }
  }

  // ② camera_override：换成一个明显不同的机位
  {
    st::raster::SceneSpec spec = *base_spec;
    st::raster::Camera camera{};
    camera.eye = st::math::Vec3{60.0f, 40.0f, 0.0f};   // 从 +X 侧看
    camera.target = st::math::Vec3{0.0f, 0.0f, 0.0f};
    camera.up = st::math::Vec3{0.0f, 1.0f, 0.0f};
    camera.fov_y_degrees = 45.0f;
    camera.near_z = 0.1f;
    camera.far_z = 500.0f;
    spec.camera_override = camera;
    st::raster::Canvas canvas{size, size, 1.0f};
    if (!render(spec, canvas)) {
      std::printf("② camera_override 渲染失败\n");
    } else {
      const double ratio = diff_ratio(base, canvas);
      std::printf("② camera_override：差异 %.2f%%  %s\n", ratio * 100.0,
                  ratio > 0.01 ? "✓ 生效" : "✗ 没生效");
    }
  }

  // ③ material_override：换成纯红塑料
  {
    st::raster::SceneSpec spec = *base_spec;
    st::raster::Material material{};
    material.base_color = st::math::Vec3{1.0f, 0.0f, 0.0f};
    material.base_color_set = true;
    material.metallic = 0.0f;
    material.roughness = 0.9f;
    spec.material_override = material;
    st::raster::Canvas canvas{size, size, 1.0f};
    if (!render(spec, canvas)) {
      std::printf("③ material_override 渲染失败\n");
    } else {
      const double ratio = diff_ratio(base, canvas);
      std::printf("③ material_override：差异 %.2f%%  %s\n", ratio * 100.0,
                  ratio > 0.01 ? "✓ 生效" : "✗ 没生效");
    }
  }
  return 0;
}
