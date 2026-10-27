/// `scene3d_script` 的 `obj:` 形状支持。
///
/// ## 为什么需要这组用例
///
/// 三维几何原本只有五种内置工厂（sphere/box/cylinder/torus/plane）。
/// 任何超出这五种的形状——齿轮、弯管、产品外壳——都只能走外部网格文件。
/// `model_shape`（裁云图层字段）一直支持 `obj:<路径>`，但
/// **`model_script` 的 `objects[].shape` 不支持**：白名单只列五个名字，
/// 写 `obj:xxx.obj` 会得到「只接受 sphere/box/plane/cylinder/torus」。
///
/// 两层能力不一致的代价很实在：同一份 OBJ 写在图层字段里能用、
/// 写进场景脚本就不能用，而且报错信息还会让人以为是路径写错了。
/// 本文件锁住修复后的契约。
///
/// ## 覆盖三件事
///
/// 1. `obj:` 能加载并画出东西（不只是解析通过）；
/// 2. `asset_basis` 参与相对路径解析（否则换个工作目录就"模型不见了"）；
/// 3. 裸 `obj:`（后面没有路径）被拒绝，且错误指向 `shape`。
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

/// 写一个最小可用 OBJ（四面体）到临时目录，返回目录路径。
///
/// 为何用四面体而不是球：三角形数量少到能肉眼判断"是不是它"，
/// 且不依赖任何外部素材文件——测试要能在干净仓库里跑。
[[nodiscard]] auto make_tetra_dir() -> std::string {
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_scene3d_obj_test");
  (void)st::fs::create_directories(dir);
  const std::string text =
      "o tetra\n"
      "v 0.0 0.9 0.0\n"
      "v 0.8 -0.5 0.5\n"
      "v -0.8 -0.5 0.5\n"
      "v 0.0 -0.5 -0.9\n"
      "f 1 2 3\n"
      "f 1 3 4\n"
      "f 1 4 2\n"
      "f 2 4 3\n";
  const std::string path = st::fs::join(dir, "tetra.obj");
  (void)st::fs::write_text(path, text);
  return dir;
}

/// 渲染一个物体，返回「非背景像素数」。
///
/// 用非背景像素数而不是平均亮度：后者会被背景稀释，
/// 一个占据 5% 画面的物体在均值里只差几个点，判据很难定。
[[nodiscard]] auto lit_pixel_count(const std::string& scene_json) -> int {
  const auto spec = st::raster::parse_scene_spec_text(scene_json);
  if (!spec) {
    std::printf("解析失败：%s\n", spec.error().message.c_str());
    return -1;
  }
  auto scene = st::raster::Scene3D::create(240, 180);
  if (!scene) return -1;
  st::raster::Canvas canvas{240, 180, 1.0f};
  const auto status =
      st::raster::render_scene_into(*spec, **scene, canvas, st::math::Color{0, 0, 0, 255});
  if (!status) {
    std::printf("渲染失败：%s\n", status.error().message.c_str());
    return -1;
  }
  const auto bytes = canvas.to_rgba8();  // 字节序 R,G,B,A
  int count = 0;
  for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
    if (bytes[i] > 12 || bytes[i + 1] > 12 || bytes[i + 2] > 12) ++count;
  }
  return count;
}

}  // namespace

ST_TEST(scene3d_script_accepts_obj_shape) {
  const std::string dir = make_tetra_dir();
  const std::string abs_obj = st::fs::join(dir, "tetra.obj");

  // ① 绝对路径：最直接的用法。
  const std::string json_abs =
      R"({"camera":{"eye":[0,0.4,3.2],"target":[0,0,0],"fov":40},)"
      R"("lighting":{"key_direction":[0.3,0.6,0.7],"ambient":0.35,"diffuse":0.8,"rim":0.1},)"
      R"("objects":[{"shape":"obj:)" + abs_obj + R"(","color":"#ffffff","scale":[1.2,1.2,1.2]}]})";
  const int abs_pixels = lit_pixel_count(json_abs);
  std::printf("绝对路径非背景像素：%d\n", abs_pixels);
  // 判据：四面体在这个取景下应当覆盖数百个像素。给宽区间（下限 200），
  // 因为具体像素数取决于投影与抗锯齿，不该锁死。
  ST_CHECK(abs_pixels > 200);
}

ST_TEST(scene3d_script_resolves_obj_against_asset_basis) {
  const std::string dir = make_tetra_dir();

  // 只给相对文件名、把基准目录放进 spec.asset_basis —— 与裁云的实际用法一致。
  const std::string json_rel =
      R"({"camera":{"eye":[0,0.4,3.2],"target":[0,0,0],"fov":40},)"
      R"("lighting":{"key_direction":[0.3,0.6,0.7],"ambient":0.35,"diffuse":0.8,"rim":0.1},)"
      R"("objects":[{"shape":"obj:tetra.obj","color":"#ffffff","scale":[1.2,1.2,1.2]}]})";

  auto spec = st::raster::parse_scene_spec_text(json_rel);
  ST_CHECK(spec.has_value());
  if (!spec) return;
  // 不设基准：相对路径落到进程工作目录，通常不存在 → 渲染应失败而不是静默空白。
  {
    auto scene = st::raster::Scene3D::create(240, 180);
    ST_CHECK(scene.has_value());
    if (!scene) return;
    st::raster::Canvas canvas{240, 180, 1.0f};
    const auto bad = st::raster::render_scene_into(*spec, **scene, canvas,
                                                   st::math::Color{0, 0, 0, 255});
    // 这里不断言一定失败（工作目录里碰巧有同名文件也算通过），
    // 只用它把"下面设了基准必须成功"衬托成有意义的对照。
    std::printf("未设基准目录：%s\n", bad.has_value() ? "意外成功" : "按预期失败");
  }
  // 设了基准：必须成功且画出东西。
  spec->asset_basis = dir;
  auto scene2 = st::raster::Scene3D::create(240, 180);
  ST_CHECK(scene2.has_value());
  if (!scene2) return;
  st::raster::Canvas canvas2{240, 180, 1.0f};
  const auto ok = st::raster::render_scene_into(*spec, **scene2, canvas2,
                                                st::math::Color{0, 0, 0, 255});
  ST_CHECK(ok.has_value());
  if (!ok) {
    std::printf("设基准目录后仍失败：%s\n", ok.error().message.c_str());
    return;
  }
  const auto bytes = canvas2.to_rgba8();
  int count = 0;
  for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
    if (bytes[i] > 12 || bytes[i + 1] > 12 || bytes[i + 2] > 12) ++count;
  }
  std::printf("基准目录解析后非背景像素：%d\n", count);
  ST_CHECK(count > 200);
}

ST_TEST(scene3d_script_rejects_bare_obj_prefix) {
  // `obj:` 后面什么都不写必须是**解析错误**，而不是静默加载一个空网格。
  // 静默失败在这里特别危险：画面上只是"模型没出现"，会被当成摆位或相机问题。
  const std::string json =
      R"({"objects":[{"shape":"obj:"}]})";
  const auto spec = st::raster::parse_scene_spec_text(json);
  ST_CHECK(!spec.has_value());
  if (spec) return;
  const std::string message = spec.error().message;
  std::printf("裸 obj: 的报错：%s\n", message.c_str());
  ST_CHECK(message.find("obj:") != std::string::npos);
}
