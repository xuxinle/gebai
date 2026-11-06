// 两腿 AgX 一致性：**同一份受控平面**，软件腿 vs GPU 腿，逐像素比。
//
// ## 为什么必须单独做这个判定
//
// 用 `bpy_scale_impact` 那种“整场景统计量”对比两腿时，两者都会被场景的
// 构图、材质、阴影影响，数字对不上分不清是「AgX 实现不同」还是
// 「渲染本来就有差」。而受控平面（0.5 灰 + 正对 SUN + 黑背景）里
// **只有一个数**——两腿的差只可能来自色调映射链。
//
// ## 判据
//
// | 结果 | 结论 |
// |---|---|
// | 两腿相等 | AgX 链在两腿一致（**期望**）|
// | GPU 更亮 | GPU 少做了一步（比如漏了反解）|
// | GPU 更暗 | GPU 多做了一步（比如重复编码）|
//
// 用法：agx_two_legs <LUT 路径>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

// 受控平面脚本：0.5 灰 + SUN energy=1 正对 + 无 world。
// 用 `__st_compat__='cycles'` 之外的**默认口径**（AgX），这样测的是 AgX 链本身。
auto script() -> std::string {
  return R"PY(
import bpy
from mathutils import Vector, Euler

scene = bpy.context.scene
cd = bpy.data.cameras.new("C")
cd.lens = 50.0
cd.sensor_width = 36.0
cam = bpy.data.objects.new("C", cd)
cam.location = Vector((0.0, 0.0, 20.0))
cam.rotation_euler = Euler((0.0, 0.0, 0.0))

bpy.ops.mesh.primitive_plane_add(size=40.0, location=(0.0, 0.0, 0.0))
pl = bpy.data.objects[-1]
m = bpy.data.materials.new("M")
m.diffuse_color = (0.5, 0.5, 0.5, 1.0)
m.metallic = 0.0
m.roughness = 1.0
pl.data.materials.append(m)

sd = bpy.data.lights.new("S", "SUN")
sd.energy = 1.0
s = bpy.data.objects.new("S", sd)
s.location = Vector((0.0, 0.0, 5.0))
s.rotation_euler = Euler((0.0, 0.0, 0.0))

# 关掉 ambient/IBL 保底（只为拿到与真 Blender 同构的输入），
# 但**不动 tone_map** —— 本工具的目的是验 AgX 链，不是验对标口径。
result = scene
)PY";
}

/// 渲染一次并取中心像素（R 通道）。
auto render_center() -> int {
  const auto spec = st::raster::parse_scene_spec_text(script());
  if (!spec.has_value()) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return -1;
  }
  st::raster::Canvas canvas{128, 128, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  if (!status.has_value()) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return -1;
  }
  const auto pixels = canvas.pixels();
  const auto c = st::math::unpremultiply(
      pixels[static_cast<std::size_t>(64) * 128 + 64]);
  return int(c.r);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
#if defined(_WIN32)
    _putenv_s("ST_AGX_LUT", argv[1]);
#else
    setenv("ST_AGX_LUT", argv[1], 1);
#endif
  }
  // 后端由环境变量 `ST_SCENE3D_BACKEND` 决定（software / 空 = 自动选 GPU）。
  // 本工具自己跑一次，所以调用方要用**两次进程**（分别设环境变量）。
  const char* backend = std::getenv("ST_SCENE3D_BACKEND");
  const int px = render_center();
  std::printf("backend=%s  center=%d\n", backend != nullptr ? backend : "(auto/gpu)", px);
  return px >= 0 ? 0 : 1;
}
