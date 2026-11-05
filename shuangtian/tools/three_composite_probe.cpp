// 复现 `ThreeView` 的合成路径：**带 DPI 缩放的目标画布** + Scene3D + end_frame。
//
// ## 为什么要这个探针
//
// `three_check` 能正常渲染同一份场景，而 `ThreeView` 视口里只剩背景色。
// 两者最大的环境差异是**目标画布的 device_scale**：探针用 1.0，
// 视口用 1.5（Windows 150% 缩放）。这个探针就是把这个变量单独拎出来。
#include <cstdio>
#include <string>

#include "st/core/fs.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/three_runtime.hpp"

namespace {

/// 统计“非背景像素”比例。背景取左上角像素。
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

/// 用指定缩放画布做一次“begin/draw/end”全流程。
auto run_case(const st::raster::ThreeScene& scene, float scale, bool physical_destination)
    -> double {
  const int width = 400;
  const int height = 300;
  st::raster::Canvas canvas{width, height, scale};
  canvas.clear(st::math::Color{10, 12, 18, 255});

  const int phys_w = canvas.physical_width();
  const int phys_h = canvas.physical_height();
  auto created = st::raster::Scene3D::create(phys_w, phys_h);
  if (!created.has_value()) {
    std::printf("  场景创建失败：%s\n", created.error().message.c_str());
    return -1.0;
  }
  auto& backend = **created;

  backend.begin_frame(st::math::Color{10, 12, 18, 255});
  backend.set_camera(scene.camera);
  backend.set_lighting(scene.lighting);
  for (const auto& object : scene.objects) {
    if (object.visible) backend.draw_mesh(object.mesh, object.world);
  }
  // 两种目标矩形：逻辑（视口现在的写法）与物理（框架自己的写法）
  const st::math::Rect destination =
      physical_destination
          ? st::math::Rect{0.0f, 0.0f, static_cast<float>(phys_w), static_cast<float>(phys_h)}
          : st::math::Rect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
  backend.end_frame(canvas, destination);
  std::printf("  draw_calls=%zu backend=%s destination=%.0fx%.0f(phys %dx%d)\n",
              static_cast<std::size_t>(backend.draw_calls()), backend.backend().c_str(),
              static_cast<double>(destination.width), static_cast<double>(destination.height),
              phys_w, phys_h);
  return painted_ratio(canvas);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path = argc > 1 ? argv[1] : "examples/jianmu/scenes/01-product.js";
  const auto source = st::fs::read_text(path);
  if (!source.has_value()) {
    std::printf("读不到 %s\n", path.c_str());
    return 1;
  }
  auto scene = st::raster::ThreeRuntime::instance().evaluate(*source, path);
  if (!scene.has_value()) {
    std::printf("求值失败：%s\n", scene.error().message.c_str());
    return 1;
  }
  std::printf("场景 %zu 个物体\n", scene->objects.size());

  struct Case {
    float scale;
    bool physical;
    const char* label;
  };
  const Case cases[] = {
      {1.0f, false, "scale=1.0 逻辑矩形（= three_check 的环境）"},
      {1.0f, true, "scale=1.0 物理矩形（两者在 scale=1 时相同）"},
      {1.5f, false, "scale=1.5 逻辑矩形（**视口现在的写法**）"},
      {1.5f, true, "scale=1.5 物理矩形（框架自己的写法）"},
  };
  for (const auto& item : cases) {
    std::printf("%s\n", item.label);
    const double ratio = run_case(*scene, item.scale, item.physical);
    std::printf("  → 已绘制 %.2f%%\n", ratio * 100.0);
  }

  // ## 同一场景、同一画布，**连续画三帧**
  //
  // 这一项用来判定一个很具体的怀疑：GPU 腿的 `ensure_readback()`（回读到
  // 软件画布）可能在多次调用后返回陈旧/空数据。视口是**每帧都画**的，
  // 而探针之前每次都是新建画布——正好把“反复画”这个变量漏掉了。
  {
    std::printf("连续三帧（同一画布，scale=1.5）\n");
    const int width = 400;
    const int height = 300;
    st::raster::Canvas canvas{width, height, 1.5f};
    auto created = st::raster::Scene3D::create(canvas.physical_width(), canvas.physical_height());
    if (created.has_value()) {
      auto& backend = **created;
      for (int frame = 0; frame < 3; ++frame) {
        canvas.clear(st::math::Color{10, 12, 18, 255});
        backend.begin_frame(st::math::Color{10, 12, 18, 255});
        backend.set_camera(scene->camera);
        backend.set_lighting(scene->lighting);
        for (const auto& object : scene->objects) {
          if (object.visible) backend.draw_mesh(object.mesh, object.world);
        }
        backend.end_frame(canvas, st::math::Rect{0.0f, 0.0f, static_cast<float>(width),
                                                 static_cast<float>(height)});
        std::printf("  第 %d 帧：已绘制 %.2f%%（draw_calls=%llu）\n", frame + 1,
                    painted_ratio(canvas) * 100.0,
                    static_cast<std::size_t>(backend.draw_calls()));
      }
    }
  }
  return 0;
}
