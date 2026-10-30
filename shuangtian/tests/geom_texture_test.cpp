/// 材质贴图的回归测试：加载、UV 变换、repeat/clamp、与顶点色相乘、错误处理。
///
/// ## 判据为什么用"颜色差值"而不是"看着像贴了图"
///
/// 贴图类的错误几乎都是**静默的**：
/// - 路径没解析（相对路径按工作目录找）→ 加载失败 → 回落顶点色，画面"就是没贴图"；
/// - UV 变换写反（`uv/scale` 而不是 `uv*scale`）→ 图案密度反了，肉眼看不出；
/// - repeat 用 `fmod`（负数返回负值）→ 负 UV 区域全取边缘色；
/// - 贴图与顶点色的乘法顺序无影响，但**忘了乘**会让顶点色失效（染色功能丢失）。
///
/// 所以每条都生成一张已知的测试图，然后断言"某个位置的颜色必须接近某个值"。
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Color;

/// 造一张 2×2 的四色测试图（左上红、右上绿、左下蓝、右下白）。
///
/// 用 2×2 而不是更大的图：**每个像素的归属在插值后仍然可判别**
///（`uv` 落在某个象限时，最近邻/双线性的结果都能对上某一个已知色）。
auto make_test_png(const std::string& path) -> bool {
  st::codec::PngImage image{};
  image.width = 2;
  image.height = 2;
  image.rgba = {
      255, 0,   0,   255,   // (0,0) 红
      0,   255, 0,   255,   // (1,0) 绿
      0,   0,   255, 255,   // (0,1) 蓝
      255, 255, 255, 255,   // (1,1) 白
  };
  return st::codec::png_write_file(path, image).has_value();
}

/// 渲染一张并返回像素（`width × height`，RGBA8）。
auto render_to_pixels(const std::string& json, int width, int height) -> std::vector<std::uint8_t> {
  auto spec = st::raster::parse_scene_spec_text(json);
  if (!spec) {
    st::log::err("解析失败：{}\n", spec.error().message);
    return {};
  }
  st::raster::Canvas canvas(width, height);
  const auto status = st::raster::render_scene(*spec, canvas, Color{0, 0, 0, 255});
  if (!status) {
    st::log::err("渲染失败：{}\n", status.error().message);
    return {};
  }
  const auto bytes = canvas.to_rgba8();
  return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

auto pixel_at(const std::vector<std::uint8_t>& pixels, int width, int x, int y)
    -> std::array<int, 3> {
  const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)) * 4U;
  if (at + 2U >= pixels.size()) return {0, 0, 0};
  return {pixels[at], pixels[at + 1U], pixels[at + 2U]};
}

/// 一张正对相机的平面（俯视），用于贴图测试。
/// `extra_fields` 追加到对象里（贴图相关字段）。
auto plane_json(const std::string& texture_path, const std::string& extra_fields) -> std::string {
  // 用 R"j(...)j" 定界：内容里有 `)"`（JSON 字符串收尾 + 拼接），
  // 无定界符的 raw string 会被提前终结（lint L15 就是查这个）
  return std::string(R"j({
    "camera": { "eye": [0.0, 4.0, 0.01], "target": [0.0, 0.0, 0.0], "fov": 45 },
    "lighting": { "ambient": 1.0, "diffuse": 0.0 },
    "objects": [
      { "name": "sheet", "shape": "plane", "radius": 1.0, "depth": 2.0,
        "color": "#ffffff", "texture": ")j") +
         texture_path + R"j(", )j" + extra_fields + R"j( }
    ]
  })j";
}

}  // namespace

ST_TEST(texture_loads_and_tints_by_vertex_color) {
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_texture_test");
  (void)st::fs::create_directories(dir);
  const std::string png = st::fs::join(dir, "quad.png");
  ST_REQUIRE(make_test_png(png));

  // 白顶点色 + 贴图 → 输出接近贴图本身的颜色
  // 环境光 1.0、漫反射 0 → 颜色基本原样（只经色调映射）
  const auto pixels = render_to_pixels(plane_json(png, R"("roughness": 1.0)"), 120, 120);
  ST_REQUIRE(!pixels.empty());

  // ## 判据：四个象限的主色通道必须**各不相同**
  //
  // 不钉具体 RGB 值：渲染管线有曝光/色调映射，绝对值会被压缩。
  // 但"哪个通道最大"是稳健的——红块 R>G,B、绿块 G>R,B、蓝块 B>R,G、白块三个都高。
  const auto quadrant = [&](int qx, int qy) {
    // 采样点取象限中心（避免边缘的插值过渡带）
    const int x = qx == 0 ? 30 : 90;
    const int y = qy == 0 ? 30 : 90;
    return pixel_at(pixels, 120, x, y);
  };
  const auto top_left = quadrant(0, 0);
  const auto top_right = quadrant(1, 0);
  const auto bottom_left = quadrant(0, 1);
  const auto bottom_right = quadrant(1, 1);
  st::log::out("[tex] TL={},{},{} TR={},{},{} BL={},{},{} BR={},{},{}\n", top_left[0],
               top_left[1], top_left[2], top_right[0], top_right[1], top_right[2],
               bottom_left[0], bottom_left[1], bottom_left[2], bottom_right[0],
               bottom_right[1], bottom_right[2]);

  // ## 判据按**实测的 UV 映射**写，不假设"左上 = uv(0,0)"
  //
  // `plane` 图元的 UV 布局是它自己的事（这个图元把 v 的方向与图片行序做了翻转，
  // 且 u 从左到右）。实测 2×2 测试图的落位是：
  //   屏幕左上 = 蓝、右上 = 白、左下 = 红。
  // 钉"哪个象限偏哪个通道"能证明贴图确实被采样（没加载时整张是均匀灰），
  // 而不必把图元的 UV 约定固化进测试——那是另一层的事，改了图元就要改测试。
  // 四个象限各对应测试图的一个像素。
  // 判据用"哪个通道**独占**领先"（而不是绝对 RGB 值）——渲染管线有曝光与色调映射，
  // 绝对值会被压缩，但"红块里 R 远大于 G/B"是稳健的。
  const auto dominant = [](const std::array<int, 3>& pixel) {
    if (pixel[0] > pixel[1] + 30 && pixel[0] > pixel[2] + 30) return 0;   // 红
    if (pixel[1] > pixel[0] + 30 && pixel[1] > pixel[2] + 30) return 1;   // 绿
    if (pixel[2] > pixel[0] + 30 && pixel[2] > pixel[1] + 30) return 2;   // 蓝
    return 3;                                                             // 灰/白（无独占通道）
  };
  const int dom_tl = dominant(top_left);
  const int dom_tr = dominant(top_right);
  const int dom_bl = dominant(bottom_left);
  const int dom_br = dominant(bottom_right);
  st::log::out("[tex] dominant TL={} TR={} BL={} BR={}\n", dom_tl, dom_tr, dom_bl, dom_br);
  // 四个象限必须**覆盖**测试图里的三种纯色 + 白（= 贴图真的被逐像素采样了）
  ST_CHECK(dom_tl == 2);   // 左上 = 蓝
  ST_CHECK(dom_bl == 0);   // 左下 = 红
  ST_CHECK(dom_br == 1);   // 右下 = 绿
  ST_CHECK(dom_tr == 3);   // 右上 = 白（三通道都高）

  // 顶点色相乘：改用半红顶点色 → 输出的红通道被压低（染色生效）
  std::string tinted = plane_json(png, R"("roughness": 1.0)");
  const std::size_t at = tinted.find("\"color\": \"#ffffff\"");
  ST_REQUIRE(at != std::string::npos);
  tinted.replace(at, std::string("\"color\": \"#ffffff\"").size(), "\"color\": \"#808080\"");
  const auto dim = render_to_pixels(tinted, 120, 120);
  ST_REQUIRE(!dim.empty());
  // ## 用**白块**（右上）测染色，不用蓝块
  //
  // 左上象限是蓝块（R=0），乘任何顶点色 R 都还是 0 —— 拿它测"红通道被压低"
  // 会得到一个恒假（或恒真）的判据，测不出任何东西。
  // 白块三个通道都高，乘 50% 灰后每个通道都应当减半左右，判断力最强。
  const auto dim_top_right = pixel_at(dim, 120, 90, 30);
  st::log::out("[tex] tinted TR={},{},{}\n", dim_top_right[0], dim_top_right[1],
               dim_top_right[2]);
  ST_CHECK(dim_top_right[0] < top_right[0] - 30);
  ST_CHECK(dim_top_right[1] < top_right[1] - 30);
  ST_CHECK(dim_top_right[2] < top_right[2] - 30);
}

ST_TEST(texture_tile_repeats_and_clamp_does_not) {
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_texture_test");
  (void)st::fs::create_directories(dir);
  const std::string png = st::fs::join(dir, "quad.png");
  ST_REQUIRE(make_test_png(png));

  // 平铺 2×2：整张平面应当出现 4 组四色块（共 8×8 个色块的一半可见）
  // 判据：**同一象限内**出现"颜色翻转"——例如左上象限的上半部与下半部主色不同。
  const auto tiled = render_to_pixels(plane_json(png, R"("texture_tile": 2.0)"), 160, 160);
  ST_REQUIRE(!tiled.empty());
  // 未平铺时左上象限是一个大色块；平铺后同一区域被切成多个小色块。
  // 采样点取 (40,40) 与 (40,120)：同一个 x，跨过平铺边界后应落到不同色块。
  const auto tiled_a = pixel_at(tiled, 160, 40, 40);
  const auto tiled_b = pixel_at(tiled, 160, 40, 120);
  st::log::out("[tex] tile quadrant: {},{},{} vs {},{},{}\n", tiled_a[0], tiled_a[1],
               tiled_a[2], tiled_b[0], tiled_b[1], tiled_b[2]);
  // 平铺后同一 x、不同 y 的两个点应当落在不同色块上（主色不同）
  const bool differing = std::abs(tiled_a[0] - tiled_b[0]) > 10 ||
                         std::abs(tiled_a[1] - tiled_b[1]) > 10 ||
                         std::abs(tiled_a[2] - tiled_b[2]) > 10;
  ST_CHECK(differing);

  // clamp：UV 超出后取边缘色 → 整张图**不会有重复色块**（大面积单色）
  const auto clamped =
      render_to_pixels(plane_json(png, R"("texture_tile": 2.0, "texture_clamp": true)"), 160, 160);
  ST_REQUIRE(!clamped.empty());
  const auto clamp_a = pixel_at(clamped, 160, 40, 40);
  st::log::out("[tex] clamp sample: {},{},{}\n", clamp_a[0], clamp_a[1], clamp_a[2]);
  // ## clamp 的判据要落在"UV 真的超出 0..1"的位置
  //
  // `plane` 的 UV 是 0..1，平铺 2 次后变 0..2 —— **超出只在 u>1 或 v>1 的半边**。
  // 所以取右半边（x 靠近右边缘）与上半边比较，那里 UV 才真的被夹住。
  // 我最初用左侧 (40,40) vs (40,120)：那里 u 在 0..1 内、v 被夹，
  // 两点落在不同的 v —— 判据本身写错了位置，与 clamp 实现无关。
  const auto clamp_right_top = pixel_at(clamped, 160, 140, 30);
  const auto clamp_right_mid = pixel_at(clamped, 160, 140, 80);
  const bool right_same = std::abs(clamp_right_top[0] - clamp_right_mid[0]) < 12 &&
                          std::abs(clamp_right_top[1] - clamp_right_mid[1]) < 12 &&
                          std::abs(clamp_right_top[2] - clamp_right_mid[2]) < 12;
  st::log::out("[tex] clamp right: {},{},{} vs {},{},{}\n", clamp_right_top[0],
               clamp_right_top[1], clamp_right_top[2], clamp_right_mid[0], clamp_right_mid[1],
               clamp_right_mid[2]);
  // clamp 下右半边（u>1）全被夹到 u=1 的边缘列 → 竖直方向同色
  ST_CHECK(right_same);
}

ST_TEST(texture_missing_file_falls_back_without_failing) {
  // 贴图不存在时：**不中断渲染**（一张图丢了不该让整帧出不来），回落顶点色。
  // 判据：渲染成功 + 画面有物体（非全黑）。
  const auto pixels = render_to_pixels(
      plane_json("C:/definitely/not/here/missing.png", R"("roughness": 1.0)"), 80, 80);
  ST_REQUIRE(!pixels.empty());
  int lit = 0;
  for (int y = 0; y < 80; ++y) {
    for (int x = 0; x < 80; ++x) {
      const auto pixel = pixel_at(pixels, 80, x, y);
      if (pixel[0] + pixel[1] + pixel[2] > 20) ++lit;
    }
  }
  st::log::out("[tex] missing-file lit pixels={}\n", lit);
  ST_CHECK(lit > 0);   // 物体仍然画出来了（顶点色）
}

ST_TEST(texture_relative_path_resolves_against_asset_basis) {
  // 相对路径必须按 `asset_basis` 解析（与 obj:/gltf: 同一套语义）。
  // 踩过的坑：只按工作目录找 → 加载失败 → 静默回落顶点色 → "怎么没贴上"。
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_texture_basis");
  (void)st::fs::create_directories(dir);
  ST_REQUIRE(make_test_png(st::fs::join(dir, "quad.png")));

  const std::string json =
      std::string(R"({
    "camera": { "eye": [0.0, 4.0, 0.01], "target": [0.0, 0.0, 0.0], "fov": 45 },
    "lighting": { "ambient": 1.0, "diffuse": 0.0 },
    "asset_basis": ")") +
      dir + R"(",
    "objects": [
      { "name": "sheet", "shape": "plane", "radius": 1.0, "depth": 2.0,
        "color": "#ffffff", "texture": "quad.png" }
    ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  // Windows 的临时目录会返回 8.3 短名（`ADMINI~1`），内容等价但字符串不同，
  // 所以只校验"非空且指向一个目录"（用原串 `dir` 判目录，用解析结果判非空）
  ST_CHECK(!spec->asset_basis.empty());
  ST_CHECK(st::fs::is_directory(dir));
  // 渲染并检查左上象限偏红（说明贴图被找到了）
  st::raster::Canvas canvas(120, 120);
  const auto status = st::raster::render_scene(*spec, canvas, Color{0, 0, 0, 255});
  ST_REQUIRE(status.has_value());
  const auto bytes = canvas.to_rgba8();
  const std::vector<std::uint8_t> pixels(bytes.begin(), bytes.end());
  const auto bottom_left = pixel_at(pixels, 120, 30, 90);
  st::log::out("[tex] basis BL={},{},{}\n", bottom_left[0], bottom_left[1], bottom_left[2]);
  // 左下偏红（实测的 UV 落位，见上一条用例的说明）——这证明相对路径被解析到了
  ST_CHECK(bottom_left[0] > bottom_left[1] + 10);
}

ST_TEST(texture_transform_is_parsed_faithfully) {
  // 解析出来的值必须就是给定的（不是"和默认值碰巧相同"）
  const std::string json = R"({
    "objects": [ { "shape": "plane", "texture": "a.png", "texture_scale_u": 3.0,
                   "texture_scale_v": 4.0, "texture_offset_u": 0.25, "texture_offset_v": -0.5,
                   "texture_clamp": true } ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  const auto& object = (*spec).objects[0];
  ST_CHECK_EQ(object.texture, std::string("a.png"));
  ST_CHECK_NEAR(object.texture_scale_u, 3.0f, 1e-5f);
  ST_CHECK_NEAR(object.texture_scale_v, 4.0f, 1e-5f);
  ST_CHECK_NEAR(object.texture_offset_u, 0.25f, 1e-5f);
  ST_CHECK_NEAR(object.texture_offset_v, -0.5f, 1e-5f);
  ST_CHECK(object.texture_clamp);

  // `texture_tile` 是便利字段：一个数同时设两个方向
  const std::string tiled = R"({
    "objects": [ { "shape": "plane", "texture": "a.png", "texture_tile": 5.0 } ]
  })";
  auto tiled_spec = st::raster::parse_scene_spec_text(tiled);
  ST_REQUIRE(tiled_spec.has_value());
  ST_CHECK_NEAR((*tiled_spec).objects[0].texture_scale_u, 5.0f, 1e-5f);
  ST_CHECK_NEAR((*tiled_spec).objects[0].texture_scale_v, 5.0f, 1e-5f);
}

ST_TEST(texture_asset_basis_is_parsed_not_just_used) {
  // ## 这条钉住一个"字段被用到但从没被解析"的缺陷
  //
  // `resolve_shape_path` 一直在用 `spec.asset_basis`，但解析器里原先**没有读**
  // 这个字段，于是它恒为空：`obj:` / `gltf:` / `texture` 的相对路径全部退回
  // "按工作目录找"。已有用例能过，是因为它们从 C++ 直接给 `spec.asset_basis`
  // 赋值——走 JSON 的路径从未被覆盖。
  //
  // 教训：字段"被用到"不等于"被解析到"，两头都要有用例（所以这里专门测 JSON 解析）。
  const std::string json = R"({
    "asset_basis": "/some/asset/root",
    "objects": [ { "shape": "cube", "texture": "brick.png" } ]
  })";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  ST_CHECK_EQ(spec->asset_basis, std::string("/some/asset/root"));

  // 缺省时保持空（不凭空捏一个值）
  auto without = st::raster::parse_scene_spec_text(R"({"objects": []})");
  ST_REQUIRE(without.has_value());
  ST_CHECK(without->asset_basis.empty());
}
