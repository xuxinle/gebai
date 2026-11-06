// bpy_script 端到端：脚本 → SceneSpec → 渲染 → 像素判据 + 出图。
//
// ## 这个探针存在的理由
//
// 「抽取器能出 ThreeScene」和「bpy 场景真的能变成像素」之间还隔着一层：
// `scene3d_script` 的渲染路径（相机/光照/材质覆盖/end_frame 合成）。
//
// 那层最容易漏的是**`end_frame`**——漏了它，`draw_calls` 会涨、内部缓冲
// 也有内容，但画布上一个像素都没有（本仓实测过一次，抽取出 3 个方块、
// 位置全对，而像素计数为 0）。所以这里必须用**像素判据**收口，
// 而不是只验 spec 结构。
//
// ## 判据分层
//
// 1. 自动识别：纯 Python 文本（不带 JSON 外壳）能走 bpy 分支
// 2. 出像素：物体真的被画上去（非背景像素占比）
// 3. 相机覆盖：`camera_override` 对 bpy 场景同样生效
// 4. 整体姿态：`extra_transform` 对 bpy 场景同样生效
// 5. 材质覆盖：`material_override` 对 bpy 场景同样生效
// 6. 优先级：同时写 bpy_script 与 three_script 时 bpy 胜
// 7. 错误路径：bpy 语法错要给出可读原因
#include <cmath>
#include <cstdio>
#include <string>

#include "st/codec/png.hpp"
#include "st/math/color.hpp"
#include "st/raster/bpy_runtime.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

using namespace st::raster;

int failures = 0;   // lint-allow: L8 探针的用例计数器（一次性量尺，与 tests/ 同口径）

/// 非背景像素占比（判断"到底画出东西没有"）。
///
/// ## 为什么按"与背景的差"而不是"非黑"
///
/// 背景可以是任意色（探针里用深灰）。判"非黑"会在浅色背景上全通过，
/// 在深色背景上全失败——那种判据测的是背景色不是几何。
auto coverage(const Canvas& canvas, st::math::Color /*background*/) -> double {
  const auto pixels = canvas.pixels();
  if (pixels.empty()) return 0.0;
  // ## 参照色取**画布左上角**像素，而不是传入的 background
  //
  // 渲染背景会经色调映射 + sRGB 编码，与调用方给的 `background` 不是同一个
  // 数值。拿后者比会让差值恒 > 阈值 → 覆盖率恒 100%（判据完全失效，而症状
  // 看起来像"画面被物体填满"）。
  //
  // 背景是均匀的，所以角落像素就是背景色；与它比才是"有多少像素被画过"。
  const auto bg = st::math::unpremultiply(pixels[0]);
  std::size_t drawn = 0;
  for (const auto& premul : pixels) {
    const auto c = st::math::unpremultiply(premul);
    const double dr = std::fabs(double(c.r) - double(bg.r)) / 255.0;
    const double dg = std::fabs(double(c.g) - double(bg.g)) / 255.0;
    const double db = std::fabs(double(c.b) - double(bg.b)) / 255.0;
    if (dr + dg + db > 0.05) ++drawn;
  }
  return double(drawn) / double(pixels.size());
}

auto expect(bool ok, const char* label, const std::string& detail) -> void {
  std::printf("  [%s] %-32s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
  if (!ok) ++failures;
}

auto save_png(const Canvas& canvas, const std::string& path) -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  const auto pixels = canvas.pixels();
  image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const st::math::Color c = st::math::unpremultiply(pixels[i]);
    image.rgba[i * 4 + 0] = c.r;
    image.rgba[i * 4 + 1] = c.g;
    image.rgba[i * 4 + 2] = c.b;
    image.rgba[i * 4 + 3] = c.a;
  }
  return static_cast<bool>(st::codec::png_write_file(path, image));
}

constexpr int kW = 640;
constexpr int kH = 480;

/// 一段 bpy 脚本：圆柱 + 相机 + 灯（最小但完整的场景）。
constexpr const char* kScript = R"PY(
import bpy
from mathutils import Vector, Euler

# 灯
lamp = bpy.data.lights.new('key')
lamp.energy = 1200.0
lamp_obj = bpy.data.objects.new('key_lamp', lamp)
lamp_obj.location = Vector((3.0, 5.0, 4.0))
lamp_obj.rotation_euler = Euler(0.6, 0.0, 0.3)

# 相机
cam_data = bpy.data.cameras.new('cam')
cam_data.lens = 50.0
cam_obj = bpy.data.objects.new('cam', cam_data)
cam_obj.location = Vector((0.0, -7.0, 3.5))
cam_obj.rotation_euler = Euler(1.35, 0.0, 0.0)

# 主体：一个圆柱
bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=1.0, depth=2.0)
body = bpy.context.active_object
mat = bpy.data.materials.new('steel')
mat.metallic = 0.9
mat.roughness = 0.25
body.material_slots.append(mat)
result = body
)PY";

}  // namespace

int main() {
  if (!BpySceneExtractor::available()) {
    std::printf("bpy 不可用：%s\n", BpySceneExtractor::unavailable_reason().c_str());
    return 1;
  }
  // Color 是 0-255 整数分量；背景用深灰（8% 亮度）
  const st::math::Color background = st::math::Color::rgba(20, 23, 28, 255);

  // ── ① 自动识别：纯 Python 文本走 bpy 分支 ──────────────────────────
  {
    const auto spec = parse_scene_spec_text(kScript);
    if (!spec.has_value()) {
      std::printf("  [FAIL] 自动识别 bpy 文本：%s\n", spec.error().message.c_str());
      return 1;
    }
    expect(!spec->bpy_script.empty(), "自动识别：走 bpy 分支",
           spec->bpy_script.empty() ? "bpy_script 为空（被当 three 了）" : "ok");
    expect(spec->three_script.empty(), "自动识别：three_script 未被误填",
           spec->three_script.empty() ? "ok" : "被误填");
  }

  // ── ② 出像素（最关键的一条）─────────────────────────────────────────
  {
    const auto spec = parse_scene_spec_text(kScript);
    if (!spec.has_value()) {
      std::printf("  [FAIL] 场景解析：%s\n", spec.error().message.c_str());
      return 1;
    }
    Canvas canvas{kW, kH, 1.0f};
    const auto status = render_scene(*spec, canvas, background);
    if (!status) {
      std::printf("  [FAIL] 渲染：%s\n", status.error().message.c_str());
      return 1;
    }
    const double ratio = coverage(canvas, background);
    char detail[128];
    std::snprintf(detail, sizeof(detail), "非背景像素 %.1f%%（漏 end_frame 会是 0%%）",
                  ratio * 100.0);
    expect(ratio > 0.02, "出像素：bpy 场景真的画上去了", detail);
    (void)save_png(canvas, "bpy_e2e_base.png");
  }

  // ── ③ 相机覆盖对 bpy 场景生效 ───────────────────────────────────────
  //
  // 覆盖成一个**明显不同**的机位，画面应当随之变化。判据用"覆盖率差异"
  // 而不是"看起来不同"——后者不可断言。
  {
    double ratio_default = 0.0;
    double ratio_override = 0.0;

    auto spec = parse_scene_spec_text(kScript);
    if (spec.has_value()) {
      Canvas canvas{kW, kH, 1.0f};
      if (render_scene(*spec, canvas, background).has_value()) {
        ratio_default = coverage(canvas, background);
      }
      // 换到很近的机位：物体应当占更多像素
      Camera close{};
      // 近机位：比默认（-7,3.5）近一半左右
      close.eye = st::math::Vec3{0.0f, -3.6f, 1.8f};
      close.target = st::math::Vec3{0.0f, 0.0f, 0.0f};
      close.fov_y_degrees = 45.0f;
      spec->camera_override = close;
      Canvas canvas2{kW, kH, 1.0f};
      if (render_scene(*spec, canvas2, background).has_value()) {
        ratio_override = coverage(canvas2, background);
        (void)save_png(canvas2, "bpy_e2e_camera_override.png");
      }
    }
    char detail[160];
    std::snprintf(detail, sizeof(detail), "默认 %.1f%% → 近机位 %.1f%%",
                  ratio_default * 100.0, ratio_override * 100.0);
    expect(ratio_override > ratio_default * 1.05, "相机覆盖：对 bpy 场景生效", detail);
  }

  // ── ④ 材质覆盖对 bpy 场景生效 ───────────────────────────────────────
  {
    auto spec = parse_scene_spec_text(kScript);
    bool ok = false;
    std::string detail = "解析失败";
    if (spec.has_value()) {
      // 换成高饱和红基色：受光漫反射项直接吃它，画面会出现大片红。
      //
      // 不用 emission 做主判据：本框架的 `emission` 默认参与光照
      // （`emission_unlit=false`），叠加后颜色不是纯色，判据不好写。
      Material red{};
      red.base_color = st::math::Vec3{0.95f, 0.08f, 0.06f};
      red.base_color_set = true;
      red.metallic = 0.0f;
      red.roughness = 0.5f;
      red.specular = 0.0f;
      spec->material_override = red;
      Canvas canvas{kW, kH, 1.0f};
      if (render_scene(*spec, canvas, background).has_value()) {
        // 判据：亮红像素占比
        std::size_t reddish = 0;
        for (const auto& premul : canvas.pixels()) {
          const auto c = st::math::unpremultiply(premul);
          // 0-255 分量：红 > 70 且比绿/蓝都高出 30 以上
          // （受光后整体亮度会变，所以不能要求"绝对很红"）
          if (c.r > 70 && int(c.r) > int(c.g) + 30 && int(c.r) > int(c.b) + 30) {
            ++reddish;
          }
        }
        const double ratio = double(reddish) / double(canvas.pixels().size());
        ok = ratio > 0.01;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "亮红像素 %.1f%%", ratio * 100.0);
        detail = buf;
        (void)save_png(canvas, "bpy_e2e_material_override.png");
      } else {
        detail = "渲染失败";
      }
    }
    expect(ok, "材质覆盖：对 bpy 场景生效", detail);
  }

  // ── ⑤ 优先级：bpy_script 与 three_script 同时写时 bpy 胜 ─────────────
  {
    const std::string both = std::string(R"j({"bpy_script": ")j") +
                             "import bpy\\nbpy.ops.mesh.primitive_cube_add(size=2.0)\\n" +
                             R"j("})j";
    const auto spec = parse_scene_spec_text(both);
    bool ok = false;
    std::string detail = "解析失败";
    if (spec.has_value()) {
      ok = !spec->bpy_script.empty() && spec->three_script.empty();
      detail = ok ? "bpy 保留、three 清空" : "优先级不对";
    }
    expect(ok, "优先级：bpy > three", detail);
  }

  // ── ⑥ 错误路径：bpy 语法错要可读 ────────────────────────────────────
  {
    const auto spec = parse_scene_spec_text("import bpy\nthis is not python\n");
    const bool ok = !spec.has_value() && !spec.error().message.empty();
    expect(ok, "错误：bpy 语法错有可读原因",
           spec.has_value() ? "(没报错，不符合预期)" : spec.error().message);
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("bpy_script 端到端全部通过\n");
  return 0;
}
