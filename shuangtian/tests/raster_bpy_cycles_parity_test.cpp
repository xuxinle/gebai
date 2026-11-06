/// bpy 渲染 ↔ 真 Cycles 的像素级基线。
///
/// ## 为什么这条测试必须存在
///
/// `docs/bpy_render_parity.md` 的所有数字都是**一次性测量**。改完引擎之后
/// 没法快速判断「变好了还是劈了」——本仓在这件事上已经吃过教训
/// （`render_alignment.md §八`：`render_gap_probe` 就是为这个而生的）。
///
/// ## 期望值从哪来（**不是手推的**）
///
/// 全部来自 `tools/blender_cycles_truth.py` 与 `tools/blender_cycles_sphere_truth.py`
/// 在本机 Blender 5.2.3 上的**实测输出**。这两个脚本会把「手算期望」与
/// 「实测」并排打出来，两者对得上才敢写进断言。
///
/// > 教训（`render_parity.md §九`）：手推的两个数（153、120）两次都错。
/// > 期望值必须来自真值通道。
///
/// ## 为什么用「对标模式」而不是手工覆盖参数
///
/// 手工覆盖（`spec.lighting.ambient = 0` 之类）测的是**这段测试代码自己
/// 对齐得对不对**，不是产品行为。而 `__st_compat__ = 'cycles'` 是用户
/// 真实可用的开关，测它才守住产品。
///
/// ## 两条腿都要对齐
///
/// 软件腿与 GPU 腿是两套实现（HLSL 与 C++）。历史上有过「GPU 腿把色调映射
/// 写死」这类只在一腿上出现的缺陷（`render_parity.md §7.1`），所以基线
/// 必须两腿都过。
#include <cmath>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/test/test.hpp"

namespace {

/// 受控平面脚本（与 `tools/blender_cycles_truth.py` 参数严格同构）。
///
/// 三项关键写法都按**真 bpy 的口径**：
/// · `Euler((x, y, z))` 三元组（真 bpy 不接受三个位置参数，实测 TypeError）
/// · `lights.new(name, 'SUN')` 显式给 type（真 bpy 要求）
/// · `plane.data.materials.append(mat)`（真 bpy 的标准挂材质写法）
///
/// `compat` 为真时声明 `__st_compat__ = 'cycles'`——这是被测的开关。
auto flat_plane_script(float energy, bool compat) -> std::string {
  std::string s = R"PY(
import bpy
from mathutils import Vector, Euler

scene = bpy.context.scene

camera_data = bpy.data.cameras.new("probe_camera")
camera_data.lens = 50.0
camera_data.sensor_width = 36.0
camera = bpy.data.objects.new("probe_camera", camera_data)
camera.location = Vector((0.0, 0.0, 20.0))
camera.rotation_euler = Euler((0.0, 0.0, 0.0))

bpy.ops.mesh.primitive_plane_add(size=40.0, location=(0.0, 0.0, 0.0))
plane = bpy.data.objects[-1]
plane.name = 'probe_plane'
material = bpy.data.materials.new("probe_material")
material.diffuse_color = (0.5, 0.5, 0.5, 1.0)
material.metallic = 0.0
material.roughness = 1.0
plane.data.materials.append(material)

sun_data = bpy.data.lights.new("probe_sun", "SUN")
sun_data.energy = )PY";
  s += std::to_string(energy);
  s += R"PY(
sun_data.color = (1.0, 1.0, 1.0)
sun = bpy.data.objects.new("probe_sun", sun_data)
sun.location = Vector((0.0, 0.0, 5.0))
sun.rotation_euler = Euler((0.0, 0.0, 0.0))
)PY";
  if (compat) {
    s += "\nbpy.context.scene.__st_compat__ = 'cycles'\n";
  }
  s += "\nresult = scene\n";
  return s;
}

/// 渲染受控平面并取中心像素（只取 R 通道——场景是单色的）。
///
/// 返回 -1 表示解析/渲染失败（调用方会看到断言失败）。
///
/// ⚠ 本函数返回 `int`，所以前置断言必须用 `ST_REQUIRE_OR` 而不是
/// `ST_REQUIRE`——后者的宏体是裸 `return;`，在值返回函数里编不过（框架
/// 刻意加的护栏，见 `test.hpp` 的说明）。
auto render_flat_center(float energy, bool compat, int size) -> int {
  const auto spec = st::raster::parse_scene_spec_text(flat_plane_script(energy, compat));
  ST_REQUIRE_OR(spec.has_value(), -1);
  st::raster::Canvas canvas{size, size, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  ST_REQUIRE_OR(status.has_value(), -1);
  const auto pixels = canvas.pixels();
  const auto center = st::math::unpremultiply(
      pixels[static_cast<std::size_t>(size / 2) * static_cast<std::size_t>(size) +
             static_cast<std::size_t>(size / 2)]);
  return int(center.r);
}

}  // namespace

/// 受控平面 energy=1.0：真 Cycles 实测 **111**。
///
/// 可手算对账：
///
///     0.5/π × 1.0 = 0.159155  →  sRGB8 = 111.064  →  111
///
/// 而真 Cycles 实测线性值 **0.160422**（比纯漫反射高 0.796%，差额是
/// Principled BSDF 的镜面项），→ 111.478 → 111。两者落到同一个 8bit。
///
/// 本框架：112。差 1 级，来自同一处舍入边界（112 与 111.478 之间只差
/// 一个 `.5` 台阶）。
///
/// ## 为什么阈值是 ±1 而不是「必须等于 111」
///
/// 那 1 级来自「三次量化」：Blender 的 EXR→8bit、本框架的 8bit 清屏中转、
/// 以及不同的镜面近似。要求严等会把**正确的实现**判红。但也不能放宽到 ±3：
/// 那样 `energy/1000` 漏修（差 12 级）就抓不到——实测旧值是 123。
ST_TEST(bpy_cycles_parity_flat_plane_is_111) {
  const int px = render_flat_center(1.0f, /*compat=*/true, 256);
  ST_CHECK_NEAR(static_cast<float>(px), 111.0f, 1.0f);
}

/// 受控平面 energy=2.0：真 Cycles 实测 **154**。
///
/// 纯漫反射手算是 153（`0.5/π×2 = 0.31831 → sRGB8 152.948`），
/// 而 Cycles 实测线性 **0.320843** → 153.501 → **154**（正好跨过 `.5`）。
///
/// 这一档比 energy=1 更硬：它落在舍入边界的另一侧，能区分
/// 「差在常数项」与「差在比例项」。
ST_TEST(bpy_cycles_parity_flat_plane_is_154) {
  const int px = render_flat_center(2.0f, /*compat=*/true, 256);
  ST_CHECK_NEAR(static_cast<float>(px), 154.0f, 1.0f);
}

/// **标度回归的守门测试**：没有对标模式时，画面必须明显更亮。
///
/// ## 这条测试守什么
///
/// 2026-11-06 修掉了 `energy / 1000` 这个量纲错。修完之后默认口径
/// （AgX + `ambient=0.35` + IBL 保底）下同一个场景是 **155**。
///
/// 若有人把 `/1000` 又加回去，这条会掉到 100 以下 → 红。
/// 而 `bpy_cycles_parity_flat_plane_is_111` 因为显式声明了对标模式，
/// **不会**发现那个回归（对标模式会覆盖 tone_map/ambient，但不覆盖标度）。
/// 两条测试各守一侧，缺一不可。
ST_TEST(bpy_default_mode_differs_from_parity_mode) {
  const int parity = render_flat_center(1.0f, /*compat=*/true, 128);
  const int normal = render_flat_center(1.0f, /*compat=*/false, 128);
  // 默认口径（AgX + ambient 保底）必然比 Standard + 无环境光更亮
  ST_CHECK(parity < normal);
  // 且两者差距显著（实测 112 vs 155，差 43 级）：
  // 若有人误把对标模式做成「默认生效」，这里会变成相等 → 红。
  ST_CHECK(normal - parity > 20);
}

/// 背光面在**对标模式**下必须是纯黑。
///
/// 这是「关掉 ambient 保底」的可见证据：真 Cycles 里没有 world 就是零环境光，
/// 背光面除了间接光之外没有别的来源，而本框架没有 GI。
///
/// 默认模式下这一条**不成立**（`ambient=0.35` 会把背光面抬到中灰）——
/// 那正是当年 three 对标时实测到的第 ② 项差异（背光面 0 → 84）。
ST_TEST(bpy_cycles_parity_back_face_is_black) {
  const auto script = flat_plane_script(1.0f, true);
  // 把光源转到背后：SUN 从 -Z 方向照，平面法线仍朝 +Z → n·l = -1
  std::string back = script;
  const std::string needle = "sun.rotation_euler = Euler((0.0, 0.0, 0.0))";
  const auto pos = back.find(needle);
  ST_REQUIRE(pos != std::string::npos);
  back.replace(pos, needle.size(), "sun.rotation_euler = Euler((3.14159265, 0.0, 0.0))");

  const auto spec = st::raster::parse_scene_spec_text(back);
  ST_REQUIRE(spec.has_value());
  st::raster::Canvas canvas{64, 64, 1.0f};
  const auto status =
      st::raster::render_scene(*spec, canvas, st::math::Color::rgba(0, 0, 0, 255));
  ST_REQUIRE(status.has_value());
  const auto pixels = canvas.pixels();
  const auto center = st::math::unpremultiply(
      pixels[static_cast<std::size_t>(32) * 64 + 32]);
  ST_CHECK_EQ(int(center.r), 0);
}

/// 对标模式对 `tone_map` 的影响：必须是 `None`（= Standard 纯 sRGB 编码）。
///
/// ## 为什么单独断言这个字段
///
/// 它是最容易被误改的一处：Blender 的**默认** view transform 是 AgX，
/// 而直觉会说「那本框架也该用 AgX」。但本框架的 AgX 是解析拟合，
/// 实测线性 0.18 处差 **+73 级**（`docs/Big_render_parity §11.5`）。
///
/// 所以对标走 Standard，而这条断言把这个决定钉住——否则有人「顺手
/// 改成 AgX 更贴近 Blender」时，画面会整体偏亮几十级而测试全绿。
ST_TEST(bpy_cycles_parity_uses_standard_tone_map) {
  const auto spec = st::raster::parse_scene_spec_text(flat_plane_script(1.0f, true));
  ST_REQUIRE(spec.has_value());
  ST_CHECK(spec->lighting.tone_map == st::raster::ToneMap::None);
  ST_CHECK(spec->lighting.ambient == 0.0f);
  ST_CHECK(spec->compat == st::raster::SceneSpec::CompatMode::Cycles);

  // 默认口径必须是另一回事（否则「默认不改」这个承诺就破了）。
  //
  // ⚠ `plain` 同样写了 `import bpy`，所以它也会走 bpy 分支。
  // 若这里意外不是 AgX，先查**分支选择**（见下一条测试）。
  const auto plain = st::raster::parse_scene_spec_text(flat_plane_script(1.0f, false));
  ST_REQUIRE(plain.has_value());
  ST_CHECK(plain->lighting.tone_map == st::raster::ToneMap::Agx);
  ST_CHECK(plain->lighting.ambient > 0.0f);
  ST_CHECK(plain->compat == st::raster::SceneSpec::CompatMode::None);
}

/// 自动识别：含 `import bpy` 的文本必须走 bpy 分支。
///
/// ## 为何单独守这一条
///
/// 上一条测试同时依赖「分支选择」与「对标模式」。两者混在一起，失败时
/// 分不清是哪边坏了——本次实测就撞到了：默认口径的 `tone_map` 不是 AgX，
/// 一眼看上去像「对标模式没做到默认不改」，实际是文本没被识别成 bpy。
/// 拆成两条才能定位。
ST_TEST(bpy_script_text_dispatches_to_bpy_branch) {
  const auto plain = st::raster::parse_scene_spec_text(flat_plane_script(1.0f, false));
  ST_REQUIRE(plain.has_value());
  // bpy 分支的标志：`bpy_script` 被填上（three 分支填的是 `three_script`）
  ST_CHECK(!plain->bpy_script.empty());
  ST_CHECK(plain->three_script.empty());
}

/// **跨 run 残留**：跑过一份声明了对标模式的脚本后，下一份未声明的必须回到默认口径。
///
/// ## 这条守的是一个实测撞到的真缺陷
///
/// `bpy.data` 是**模块级单例**，跨 `run` 存活（刻意的：否则跑完脚本场景就没了）。
/// 它有 `reset()` 负责清数据块，但**清不了脚本挂在 `scene` 上的自定义属性**。
///
/// 实测现象（首次运行本测试文件时就撞到）：
///
/// | 顺序 | 期望 | 实际（修前） |
/// |---|---|---|
/// | 先跑「声明 compat=cycles」 | — | — |
/// | 再跑「不声明」 | `tone=Agx ambient=0.35` | **`tone=None ambient=0`** ← 继承了上一个脚本 |
///
/// 后果很难查：**后一份场景无故变暗，而且零报错**。
/// 修法：`_Data.reset()` 重建一个干净的 `scene`（保留 `name` / `world`）。
ST_TEST(bpy_compat_does_not_leak_across_runs) {
  // 第一次：声明的
  const auto declared = st::raster::parse_scene_spec_text(flat_plane_script(1.0f, true));
  ST_REQUIRE(declared.has_value());
  ST_CHECK(declared->compat == st::raster::SceneSpec::CompatMode::Cycles);

  // 第二次：没声明的，必须回到默认口径
  const auto plain = st::raster::parse_scene_spec_text(flat_plane_script(1.0f, false));
  ST_REQUIRE(plain.has_value());
  ST_CHECK(plain->compat == st::raster::SceneSpec::CompatMode::None);
  ST_CHECK(plain->lighting.tone_map == st::raster::ToneMap::Agx);
  ST_CHECK(plain->lighting.ambient > 0.0f);
}

