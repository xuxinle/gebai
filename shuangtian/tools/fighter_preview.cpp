/// 角色 3D 模型的**离屏预览**工具。
///
/// 为什么需要它：格斗里的角色是"每帧动态摆姿"的，直接改游戏再看效果，
/// 一次迭代要启动 + 操作 + 截图；而模型本身的**形状/配色/比例**是静态的，
/// 先用一份独立脚本把它渲染出来，改一版看一版，几秒钟一轮。
///
/// 用法：
///   st build fighter_preview && ./build/dev/bin/fighter_preview out.png
/// 输出：两个角色并排（左甜甜圈、右户圣），带光照与地面投影。
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/paint.hpp"
#include "st/codec/png.hpp"
#include "st/raster/scene3d.hpp"

namespace {

using st::math::Color;
using st::math::Mat4;
using st::math::Rect;

/// 一个 3D 部件：网格 + 变换。
///
/// ⚠ 颜色**建网格时就已经烘进顶点色**了（`Mesh::colored_*` / `Mesh::cylinder(...,color)`）。
/// 早先版本在建完后才 `part.color = ...`，结果整个模型是一片白——
/// 因为那两个字段根本没人读（颜色不是网格的属性，是顶点的一部分）。
struct Part {
  st::raster::Mesh mesh;
  Mat4 model;
};

auto translate(float x, float y, float z) -> Mat4 {
  return Mat4::translation(st::math::Vec3{x, y, z});
}



/// 球（用于头、手、亮片等）。
auto ball(float radius, float x, float y, float z, Color color) -> Part {
  return Part{st::raster::Mesh::colored_sphere(radius, 20, color), translate(x, y, z)};
}

/// 圆柱（用于四肢）。`Mesh::cylinder` 自带 color（含顶/底盖）。
auto rod(float radius, float height, float x, float y, float z, Color color) -> Part {
  return Part{st::raster::Mesh::cylinder(radius, height, 16, color), translate(x, y, z)};
}

/// 方块（用于躯干、帽檐、眼镜框）。
auto cube(float sx, float sy, float sz, float x, float y, float z, Color color) -> Part {
  return Part{st::raster::Mesh::colored_box(sx, sy, sz, color), translate(x, y, z)};
}

/// 甜甜圈：美国国旗棒球帽 + 红色运动夹克。
auto build_donut() -> std::vector<Part> {
  const Color kSkin{0xE8, 0xB4, 0x92, 0xFF};
  const Color kJeans{0x2E, 0x34, 0x42, 0xFF};
  const Color kShoe{0xF2, 0xF2, 0xF2, 0xFF};
  const Color kJacket{0xD9, 0x2B, 0x3A, 0xFF};   // 正红运动夹克
  const Color kWhite{0xFA, 0xFA, 0xFA, 0xFF};
  const Color kFlagBlue{0x1F, 0x3A, 0x93, 0xFF};
  const Color kFlagRed{0xBF, 0x0A, 0x30, 0xFF};
  std::vector<Part> parts;

  // 腿与鞋
  parts.push_back(rod(0.085f, 0.60f, -0.13f, -0.30f, 0.0f, kJeans));
  parts.push_back(rod(0.085f, 0.60f, 0.13f, -0.30f, 0.0f, kJeans));
  parts.push_back(cube(0.19f, 0.10f, 0.30f, -0.13f, -0.655f, 0.05f, kShoe));
  parts.push_back(cube(0.19f, 0.10f, 0.30f, 0.13f, -0.655f, 0.05f, kShoe));

  // 红色运动夹克（略宽的箱体）
  parts.push_back(cube(0.48f, 0.62f, 0.28f, 0.0f, 0.24f, 0.0f, kJacket));
  // 胸前白色跑动条（运动夹克的标志）
  parts.push_back(cube(0.055f, 0.56f, 0.30f, -0.155f, 0.24f, 0.0f, kWhite));
  parts.push_back(cube(0.055f, 0.56f, 0.30f, 0.155f, 0.24f, 0.0f, kWhite));
  // 拉链（中间深色细条）
  parts.push_back(cube(0.022f, 0.58f, 0.305f, 0.0f, 0.24f, 0.0f, Color{0x8E, 0x18, 0x24, 0xFF}));

  // 手臂（夹克袖）+ 手
  // ⚠ 肩关节要**在躯干外面**：早先 x=±0.315 而躯干半宽 0.24，圆柱半径 0.078，
  //    算下来袖子里缘 0.237 已经插进躯干——截图里就是"手臂长在身上"。
  //    改成 x=±0.335 + 肩球做过渡，看起来才是"接上去的"。
  // 肩部过渡球要**小、贴着躯干、略低于肩线**——早先半径 0.095 顶在 y=0.50，
  // 截图里是两个明显的红球挂在肩上（像袖章）。半径 0.072 + 内收 0.02 才像"肩"。
  parts.push_back(ball(0.072f, -0.300f, 0.485f, 0.0f, kJacket));
  parts.push_back(ball(0.072f, 0.300f, 0.485f, 0.0f, kJacket));
  parts.push_back(rod(0.075f, 0.44f, -0.322f, 0.27f, 0.0f, kJacket));
  parts.push_back(rod(0.075f, 0.44f, 0.322f, 0.27f, 0.0f, kJacket));
  parts.push_back(ball(0.086f, -0.325f, 0.02f, 0.0f, kSkin));
  parts.push_back(ball(0.086f, 0.325f, 0.02f, 0.0f, kSkin));

  // 脖子与头
  parts.push_back(rod(0.072f, 0.11f, 0.0f, 0.58f, 0.0f, kSkin));
  parts.push_back(ball(0.205f, 0.0f, 0.76f, 0.0f, kSkin));

  // —— 美国国旗棒球帽 ——
  //
  // 国旗三要素都要看得见：**深蓝帽体 + 红白横条 + 白星**。
  // 早先条纹用 `torus` 环绕帽体，但环的半径(0.205) 小于帽体(0.216)，
  // 被完全包在球里——屏幕上一条纹都没有（"帽子上什么都没有"）。
  // 改成**略大于帽体**的扁圆柱（像个箍），才真的浮在表面。
  parts.push_back(ball(0.213f, 0.0f, 0.812f, -0.012f, kFlagBlue));
  // 红白横条：两个稍大的扁箍
  parts.push_back(rod(0.220f, 0.030f, 0.0f, 0.842f, -0.012f, kFlagRed));
  parts.push_back(rod(0.218f, 0.026f, 0.0f, 0.874f, -0.012f, kWhite));
  parts.push_back(rod(0.214f, 0.024f, 0.0f, 0.902f, -0.012f, kFlagRed));
  // 帽檐：前伸的扁方块（略前倾，像真的帽檐）
  parts.push_back(cube(0.345f, 0.030f, 0.30f, 0.0f, 0.795f, 0.235f, kFlagBlue));
  // 帽徽白星（正面，帽檐上方）
  parts.push_back(ball(0.044f, 0.0f, 0.855f, 0.185f, kWhite));

  return parts;
}

/// 户圣：亮片眼镜 + 灰色 polo 衫。
auto build_husheng() -> std::vector<Part> {
  const Color kSkin{0xF0, 0xC0, 0x9A, 0xFF};
  const Color kPants{0x3A, 0x3E, 0x46, 0xFF};
  const Color kShoe{0x1E, 0x1E, 0x22, 0xFF};
  const Color kPolo{0x9A, 0x9E, 0xA6, 0xFF};     // 中灰 polo
  const Color kCollar{0x7E, 0x82, 0x8A, 0xFF};
  const Color kGold{0xC8, 0xA8, 0x50, 0xFF};
  const Color kHair{0x24, 0x1E, 0x1A, 0xFF};
  std::vector<Part> parts;

  // 腿与鞋
  parts.push_back(rod(0.085f, 0.60f, -0.13f, -0.30f, 0.0f, kPants));
  parts.push_back(rod(0.085f, 0.60f, 0.13f, -0.30f, 0.0f, kPants));
  parts.push_back(cube(0.19f, 0.10f, 0.30f, -0.13f, -0.655f, 0.05f, kShoe));
  parts.push_back(cube(0.19f, 0.10f, 0.30f, 0.13f, -0.655f, 0.05f, kShoe));

  // 灰色 polo（比夹克窄一点）
  parts.push_back(cube(0.46f, 0.62f, 0.27f, 0.0f, 0.24f, 0.0f, kPolo));
  // polo 领（深一号，翻在肩上）
  parts.push_back(cube(0.32f, 0.10f, 0.29f, 0.0f, 0.575f, 0.0f, kCollar));
  // 门襟 + 两颗纽扣
  parts.push_back(cube(0.040f, 0.24f, 0.285f, 0.0f, 0.44f, 0.0f, kCollar));
  parts.push_back(ball(0.016f, 0.0f, 0.50f, 0.15f, kGold));
  parts.push_back(ball(0.016f, 0.0f, 0.42f, 0.15f, kGold));

  // 手臂：polo 短袖 + 肤色前臂（同样外移，避免插进躯干）
  parts.push_back(ball(0.070f, -0.292f, 0.485f, 0.0f, kPolo));
  parts.push_back(ball(0.070f, 0.292f, 0.485f, 0.0f, kPolo));
  parts.push_back(rod(0.078f, 0.24f, -0.315f, 0.40f, 0.0f, kPolo));
  parts.push_back(rod(0.078f, 0.24f, 0.315f, 0.40f, 0.0f, kPolo));
  parts.push_back(rod(0.066f, 0.32f, -0.315f, 0.12f, 0.0f, kSkin));
  parts.push_back(rod(0.066f, 0.32f, 0.315f, 0.12f, 0.0f, kSkin));
  parts.push_back(ball(0.082f, -0.315f, -0.05f, 0.0f, kSkin));
  parts.push_back(ball(0.082f, 0.315f, -0.05f, 0.0f, kSkin));

  // 脖子与头
  parts.push_back(rod(0.072f, 0.11f, 0.0f, 0.58f, 0.0f, kSkin));
  parts.push_back(ball(0.205f, 0.0f, 0.76f, 0.0f, kSkin));
  // 头发（深色，稍大、上移、后偏）
  parts.push_back(ball(0.212f, 0.0f, 0.795f, -0.025f, kHair));

  // —— 亮片眼镜（本题的视觉重点）——
  // 镜片：**深色镜片**（反光才显得出来）+ 一圈亮片小球
  // 早先镜片是白色球，配白亮片 ⇒ 一片糊白，完全看不出是眼镜。
  const Color kLens{0x3A, 0x2E, 0x4E, 0xFF};
  parts.push_back(Part{st::raster::Mesh::colored_sphere(0.080f, 20, kLens),
                       translate(-0.085f, 0.775f, 0.172f)});
  parts.push_back(Part{st::raster::Mesh::colored_sphere(0.080f, 20, kLens),
                       translate(0.085f, 0.775f, 0.172f)});
  // 金色镜框（两个扁圆环卡在镜片外缘）
  parts.push_back(Part{st::raster::Mesh::torus(0.079f, 0.011f, 20, 6, kGold),
                       translate(-0.082f, 0.775f, 0.175f)});
  parts.push_back(Part{st::raster::Mesh::torus(0.079f, 0.011f, 20, 6, kGold),
                       translate(0.082f, 0.775f, 0.175f)});
  // 鼻梁
  parts.push_back(cube(0.055f, 0.016f, 0.02f, 0.0f, 0.775f, 0.182f, kGold));
  // 镜腿（两侧伸向耳后）
  parts.push_back(cube(0.19f, 0.014f, 0.016f, -0.175f, 0.775f, 0.085f, kGold));
  parts.push_back(cube(0.19f, 0.014f, 0.016f, 0.175f, 0.775f, 0.085f, kGold));
  // 亮片：两侧镜片各撒一圈白色小球（"bling"的读感就来自这些高光点）
  for (int i = 0; i < 8; ++i) {
    const float angle = static_cast<float>(i) * 0.785f + 0.4f;
    const float cx = std::cos(angle) * 0.062f;
    const float cy = std::sin(angle) * 0.062f;
    const Color sparkle{0xFF, 0xFF, 0xFF, 0xFF};
    parts.push_back(ball(0.017f, -0.085f + cx, 0.775f + cy, 0.230f, sparkle));
    parts.push_back(ball(0.017f, 0.085f + cx, 0.775f + cy, 0.230f, sparkle));
  }

  return parts;
}

/// 渲染**两个角色**到同一张画布。
///
/// ⚠ 必须是**一个场景**：早先按角色各调一次 `begin_frame/end_frame`，
/// 结果第二个人整个不见了——因为 `begin_frame` 会清屏，第二次把第一次的成果覆盖掉。
/// （这个症状很像"模型没建出来"，实际是后一次渲染抹掉了前一次。）
void render_both(st::raster::Canvas& canvas, const std::vector<Part>& left,
                 const std::vector<Part>& right) {
  const int w = static_cast<int>(canvas.physical_width());
  const int h = static_cast<int>(canvas.physical_height());
  auto scene = st::raster::Scene3D::create(w, h);
  if (!scene.has_value()) {
    std::fprintf(stderr, "Scene3D 创建失败\n");
    return;
  }
  auto& s3 = **scene;
  s3.begin_frame(Color{0x12, 0x14, 0x1C, 0xFF});

  // 相机放正中，两个人分列左右（间距 1.7 ≈ 肩宽的 3.5 倍，足够看清各自细节）。
  st::raster::Camera camera{};
  camera.eye = st::math::Vec3{0.0f, 0.62f, 4.4f};
  camera.target = st::math::Vec3{0.0f, 0.12f, 0.0f};
  camera.fov_y_degrees = 36.0f;
  s3.set_camera(camera);

  st::raster::Lighting lighting{};
  lighting.light_from = st::math::Vec3{0.45f, 0.85f, 0.6f};
  lighting.ambient = 0.5f;
  lighting.diffuse = 0.85f;
  lighting.rim = 0.4f;
  s3.set_lighting(lighting);

  const auto draw_set = [&](const std::vector<Part>& parts, float dx) {
    for (const Part& part : parts) {
      // 平移叠加：把整组部件整体挪到左/右。
      s3.draw_mesh(part.mesh, Mat4::translation(st::math::Vec3{dx, 0.0f, 0.0f}) * part.model);
    }
  };
  draw_set(left, -0.85f);
  draw_set(right, 0.85f);

  s3.end_frame(canvas, Rect{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
}

auto run(int argc, char** argv) -> int {
  const std::string out = argc > 1 ? argv[1] : std::string("fighter-preview.png");
  st::raster::Canvas canvas(1000, 620, 1.0f);
  canvas.clear(Color{0x10, 0x12, 0x18, 0xFF});

  const auto donut = build_donut();
  const auto husheng = build_husheng();
  render_both(canvas, donut, husheng);

  // 导出：`pixels()` 是**预乘** RGBA8（内存序 R<<24|G<<16|B<<8|A），
  // 而 PNG 要非预乘 —— 必须还原，否则半透明边缘会发黑。
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  const auto pixels = canvas.pixels();
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t p32 = pixels[i];
    const auto a = static_cast<std::uint8_t>(p32 & 0xFFU);
    auto r = static_cast<std::uint8_t>((p32 >> 24) & 0xFFU);
    auto g = static_cast<std::uint8_t>((p32 >> 16) & 0xFFU);
    auto b = static_cast<std::uint8_t>((p32 >> 8) & 0xFFU);
    if (a != 0U && a != 255U) {
      r = static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(r) * 255U / a));
      g = static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(g) * 255U / a));
      b = static_cast<std::uint8_t>(std::min(255U, static_cast<unsigned>(b) * 255U / a));
    }
    image.rgba[i * 4 + 0] = r;
    image.rgba[i * 4 + 1] = g;
    image.rgba[i * 4 + 2] = b;
    image.rgba[i * 4 + 3] = a;
  }
  const auto wrote = st::codec::png_write_file(out, image);
  if (!wrote.has_value()) {
    std::fprintf(stderr, "写入 %s 失败\n", out.c_str());
    return 1;
  }
  std::printf("→ %s\n", out.c_str());
  return 0;
}

}  // namespace

ST_MAIN(run)
