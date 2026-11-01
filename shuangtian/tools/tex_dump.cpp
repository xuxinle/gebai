
#include <cstdio>
#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/raster/gltf.hpp"
namespace {
auto run(int, char**) -> int {
  auto loaded = st::raster::gltf_read_file("assets/models/gltf/CesiumMan.glb");
  if (!loaded.has_value()) return 1;
  for (std::size_t i = 0; i < loaded->textures.size(); ++i) {
    const auto& t = loaded->textures[i];
    std::printf("tex[%zu] %dx%d name=%s valid=%d\n", i, t.width, t.height, t.name.c_str(),
                t.is_valid() ? 1 : 0);
    if (t.is_valid()) {
      st::codec::PngImage img;
      img.width = static_cast<std::uint32_t>(t.width);
      img.height = static_cast<std::uint32_t>(t.height);
      img.rgba = t.rgba;
      const auto wrote = st::codec::png_write_file("build/cesium-tex0.png", img);
      (void)wrote;
      std::printf("  -> build/cesium-tex0.png\n");
    }
  }
  for (std::size_t i = 0; i < loaded->materials.size(); ++i) {
    const auto& m = loaded->materials[i];
    std::printf("mat[%zu] %s base=(%.2f,%.2f,%.2f) metal=%.2f rough=%.2f tex=%d\n", i,
                m.name.c_str(), static_cast<double>(m.base_color.r) / 255.0,
                static_cast<double>(m.base_color.g) / 255.0, static_cast<double>(m.base_color.b) / 255.0,
                static_cast<double>(m.metallic), static_cast<double>(m.roughness),
                m.base_color_texture);
  }
  return 0;
}
}  // namespace
ST_MAIN(run)
