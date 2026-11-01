/// 把 SVG 图标光栅化成 PNG 拼版 —— **图标写错了不会报错，只会画不出来**。
///
/// ## 为什么需要这个工具
///
/// `svg_registry().add_single(id, source)` 对坏 SVG 的失败方式很安静：
/// 路径语法错、`fill` 写错、`viewBox` 缺失——**装载可能成功、绘制返回 false**，
/// 而调用方（`TitleBar::set_icon`）拿不到任何信号。症状只有"标题栏图标不出现"，
/// 而那既可能是注册晚了、也可能是 SVG 本身坏，从界面上分不清。
///
/// 所以先把每个 SVG 单独光栅化成 PNG：**一张图就能看出画的是不是那个东西**。
///
/// ## 用法
///
/// ```
/// st_svg_sheet <输出.png> <尺寸> <svg 路径>...
/// ```
///
/// 拼版规则：横向并排、每格留 8px 呼吸、**深底/浅底各一行**——品牌标记是白色
/// 前景，只在深色底上才看得见；浅底那行是为了确认"底色圆角方"本身的形态。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/icon.hpp"

namespace {

constexpr int kPad = 10;

auto render_one(std::string_view id, std::string_view source, int size) -> st::codec::PngImage {
  st::codec::PngImage out;
  out.width = static_cast<std::uint32_t>(size);
  out.height = static_cast<std::uint32_t>(size);
  out.rgba.assign(static_cast<std::size_t>(size) * size * 4U, 0U);

  auto& registry = st::ui::svg_registry();
  if (!registry.add_single(std::string(id), std::string(source))) {
    std::fprintf(stderr, "  [!] %.*s: SVG 装载失败（XML/属性有问题）\n",
                 static_cast<int>(id.size()), id.data());
    return out;
  }
  // 经注册表绘制 = 与运行期完全同一条链路（同一份解析器 + 同一份光栅化）。
  // 用 `Canvas`（软件光栅器）：图标只需要 CPU 像素，不开 GPU 设备。
  st::raster::Canvas surface(size, size, 1.0f);
  surface.clear(st::math::Color::rgba(0, 0, 0, 0));
  const st::math::Rect box{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size)};
  if (!registry.draw(surface, std::string(id), box,
                     st::math::Color::rgba(255, 255, 255, 255))) {
    std::fprintf(stderr, "  [!] %.*s: 绘制返回 false（路径/属性可能有问题）\n",
                 static_cast<int>(id.size()), id.data());
  }

  // `pixels()` 是**预乘** RGBA8，内存序 `R<<24 | G<<16 | B<<8 | A`
  // （见 `canvas.cpp` 的 `pack()`）。PNG 要非预乘 + 按字节 R,G,B,A 排。
  //
  // 早先这里按 `A<<24|R<<16|G<<8|B` 解，结果所有图都变成“白/灰”——
  // 因为 R 位置拿到了 A(255)、而真 R 被当成 A 解成 0/低值。
  const auto pixels = surface.pixels();
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t rgba = pixels[i];
    const auto a = static_cast<std::uint8_t>(rgba & 0xFFU);
    std::uint8_t r = static_cast<std::uint8_t>((rgba >> 24) & 0xFFU);
    std::uint8_t g = static_cast<std::uint8_t>((rgba >> 16) & 0xFFU);
    std::uint8_t b = static_cast<std::uint8_t>((rgba >> 8) & 0xFFU);
    if (a != 0U && a != 255U) {
      const auto unpremultiply = [a](std::uint8_t value) -> std::uint8_t {
        const int scaled = static_cast<int>(value) * 255 / static_cast<int>(a);
        return static_cast<std::uint8_t>(std::min(255, scaled));
      };
      r = unpremultiply(r);
      g = unpremultiply(g);
      b = unpremultiply(b);
    }
    out.rgba[i * 4] = r;
    out.rgba[i * 4 + 1] = g;
    out.rgba[i * 4 + 2] = b;
    out.rgba[i * 4 + 3] = a;
  }
  return out;
}

auto run(int argc, char** argv) -> int {
  if (argc < 4) {
    std::fputs("用法: st_svg_sheet <输出.png> <尺寸> <svg 路径>...\n", stderr);
    return 2;
  }
  const std::string output = argv[1];
  const int size = std::atoi(argv[2]);
  if (size < 8 || size > 512) {
    std::fprintf(stderr, "尺寸需在 8~512 之间，得到 %d\n", size);
    return 2;
  }

  std::vector<st::codec::PngImage> images;
  std::vector<std::string> names;
  for (int i = 3; i < argc; ++i) {
    const std::string path = argv[i];
    const auto text = st::fs::read_text(path);
    if (!text) {
      std::fprintf(stderr, "读不到 %s\n", path.c_str());
      return 1;
    }
    const std::string leaf = path.substr(path.find_last_of("/\\") + 1);
    names.push_back(leaf);
    images.push_back(render_one(leaf, *text, size));
  }

  const int cell = size + kPad * 2;
  const int columns = static_cast<int>(images.size());
  st::codec::PngImage sheet;
  sheet.width = static_cast<std::uint32_t>(cell * columns);
  sheet.height = static_cast<std::uint32_t>(cell * 2);
  sheet.rgba.assign(static_cast<std::size_t>(sheet.width) * sheet.height * 4U, 0U);

  // 两行底：深（品牌标记的原生场景）/ 浅（看底色形状）
  const std::uint8_t backgrounds[2][3] = {{32, 34, 38}, {238, 239, 242}};
  for (int row = 0; row < 2; ++row) {
    for (int y = 0; y < cell; ++y) {
      for (int x = 0; x < cell * columns; ++x) {
        const std::size_t o = (static_cast<std::size_t>(row * cell + y) *
                               sheet.width + static_cast<std::size_t>(x)) * 4;
        sheet.rgba[o] = backgrounds[row][0];
        sheet.rgba[o + 1] = backgrounds[row][1];
        sheet.rgba[o + 2] = backgrounds[row][2];
        sheet.rgba[o + 3] = 255;
      }
    }
  }

  for (int index = 0; index < columns; ++index) {
    const auto& img = images[static_cast<std::size_t>(index)];
    for (int row = 0; row < 2; ++row) {
      const int dx = index * cell + kPad;
      const int dy = row * cell + kPad;
      for (std::uint32_t y = 0; y < img.height; ++y) {
        for (std::uint32_t x = 0; x < img.width; ++x) {
          const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
          const std::uint8_t a = img.rgba[i + 3];
          if (a == 0) continue;
          const std::size_t o = (static_cast<std::size_t>(dy + static_cast<int>(y)) *
                                 sheet.width + static_cast<std::size_t>(dx + static_cast<int>(x))) * 4;
          const float alpha = static_cast<float>(a) / 255.0f;
          for (int c = 0; c < 3; ++c) {
            sheet.rgba[o + c] = static_cast<std::uint8_t>(
                static_cast<float>(img.rgba[i + c]) * alpha +
                static_cast<float>(sheet.rgba[o + c]) * (1.0f - alpha));
          }
          sheet.rgba[o + 3] = 255;
        }
      }
    }
  }

  if (auto status = st::codec::png_write_file(output, sheet); !status) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", status.error().message.c_str());
    return 1;
  }
  std::printf("→ %s  (%d 个图标, 每格 %dpx, 深/浅两行)\n", output.c_str(), columns, size);
  for (std::size_t i = 0; i < names.size(); ++i) std::printf("   [%zu] %s\n", i, names[i].c_str());
  return 0;
}

}  // namespace

ST_MAIN(run)
