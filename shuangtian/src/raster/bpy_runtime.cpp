// 霜天 · bpy 场景抽取器（实现）
//
// ## 分工
//
// | 层 | 干什么 |
// |---|---|
// | `src/python/extract.py`（Python） | 遍历 bpy 场景、三角化、算世界矩阵 → 扁平 JSON |
// | 本文件（C++） | JSON → `ThreeScene`（纯数据搬运，无任何 bpy 知识） |
//
// 抽取逻辑放 Python 侧是刻意的：在 C++ 里逐层读 bpy 对象（每个属性一次
// `mp_load_attr` + 类型检查）代码量极大且易错。详见 `bpy_runtime.hpp`。
#include "st/raster/bpy_runtime.hpp"

#include <cstdint>

#include <cmath>
#include <string>
#include <vector>

#include "st/core/json.hpp"
#include "st/math/matrix.hpp"
// Vec3 / Mat4 都定义在 matrix.hpp（本框架没有单独的 vec.hpp）
#include "st/python/runtime.hpp"

namespace st::raster {
namespace {

/// 从 JSON 数组读 N 个 double（不足补 0）。
auto read_doubles(const Json& value, std::size_t count) -> std::vector<double> {
  std::vector<double> out;
  out.reserve(count);
  if (!value.is_array()) {
    out.resize(count, 0.0);
    return out;
  }
  for (const auto& item : value) {
    if (out.size() >= count) break;
    out.push_back(item.is_number() ? item.get<double>() : 0.0);
  }
  out.resize(count, 0.0);
  return out;
}

/// 16 个数（**列主序**）→ `Mat4`。
///
/// ## 列主序是硬约定
///
/// Python 侧 `_flatten_matrix` 已经把 bpy 的**行主序**矩阵转成列主序，
/// 这里按列填。两处任何一处漏了转置，症状都是"位置对了但旋转/缩放错位"
/// ——比完全不出现还难查。
auto read_matrix(const Json& value) -> math::Mat4 {
  const auto numbers = read_doubles(value, 16);
  math::Mat4 out = math::Mat4::identity();
  // `Mat4::m` 就是列主序的裸数组（`m[col*4 + row]`），与 Python 侧
  // `_flatten_matrix` 的输出**布局完全一致**——所以这里直接搬，
  // 不再做任何转置。**两处都别"顺手转一次"**：那会正好转回行主序，
  // 症状是"位置对了但旋转反了"，比完全不出现还难查。
  for (std::size_t i = 0; i < 16; ++i) {
    out.m[i] = static_cast<float>(numbers[i]);
  }
  return out;
}

/// 读物体的几何（`vertices` 是每顶点 9 个 float 的交错表）。
auto read_mesh(const Json& item) -> Mesh {
  Mesh mesh{};
  const Json& verts = json_at(item, "vertices");
  if (verts.is_array()) {
    mesh.vertices.reserve(verts.size() * Mesh::kVertexFloats);
    for (const auto& vertex : verts) {
      const auto numbers = read_doubles(vertex, Mesh::kVertexFloats);
      for (std::size_t i = 0; i < Mesh::kVertexFloats; ++i) {
        mesh.vertices.push_back(static_cast<float>(numbers[i]));
      }
    }
  }
  const Json& indices = json_at(item, "indices");
  if (indices.is_array()) {
    mesh.indices.reserve(indices.size());
    for (const auto& index : indices) {
      const auto value = index.is_number() ? index.get<std::int64_t>() : 0LL;
      mesh.indices.push_back(static_cast<std::uint32_t>(value < 0 ? 0 : value));
    }
  }
  return mesh;
}

/// 读材质（PBR 子集）。
///
/// 接受两种输入：一个**材质对象**（如 `item["material"]`），或一个单独的材质
/// 节点（如 `item["materials"][k]`）——两者字段完全相同，所以共用一个函数。
auto read_material_node(const Json& value) -> Material {
  Material material{};
  if (!value.is_object()) return material;

  const auto color = read_doubles(json_at(value, "base_color"), 3);
  material.base_color = math::Vec3{static_cast<float>(color[0]),
                                   static_cast<float>(color[1]),
                                   static_cast<float>(color[2])};
  material.base_color_set = true;   // 抽出来的场景**总是**显式给色
  material.metallic = static_cast<float>(json_get_double(value, "metallic", 0.0));
  material.roughness = static_cast<float>(json_get_double(value, "roughness", 0.5));
  material.emission = static_cast<float>(json_get_double(value, "emission", 0.0));
  material.opacity = static_cast<float>(json_get_double(value, "opacity", 1.0));
  material.depth_write = material.opacity >= 1.0f;
  return material;
}

/// 读材质（兼容单材质旧格式：`item["material"]`）。
auto read_material(const Json& item) -> Material {
  return read_material_node(json_at(item, "material"));
}

/// 读相机。缺失时用一份保守默认（并已在 warnings 里报过）。
auto read_camera(const Json& value) -> Camera {
  Camera camera{};
  if (!value.is_object()) {
    // 没相机：放在一个能看见原点的位置（不返回空相机——那会让下游
    // 算出一个退化视图，看起来像"渲染坏了"）
    camera.eye = math::Vec3{0.0f, 1.2f, 4.5f};
    camera.target = math::Vec3{0.0f, 0.3f, 0.0f};
    return camera;
  }
  const auto eye = read_doubles(json_at(value, "eye"), 3);
  const auto target = read_doubles(json_at(value, "target"), 3);
  camera.eye = math::Vec3{static_cast<float>(eye[0]), static_cast<float>(eye[1]),
                          static_cast<float>(eye[2])};
  camera.target = math::Vec3{static_cast<float>(target[0]),
                             static_cast<float>(target[1]),
                             static_cast<float>(target[2])};
  // ⚠ `fov_y`：bpy 给的是**弧度**（`camera.data.angle_y`），
  // 而本框架的 `Camera.fov_y_degrees` 要**角度**。必须转——漏了这步的
  // 症状是"视野极窄、物体被放大到只看到一角"，而它看起来像相机位置错了。
  const double fov_rad = json_get_double(value, "fov_y", 0.0);
  if (fov_rad > 0.01) {
    camera.fov_y_degrees = static_cast<float>(fov_rad * 180.0 / 3.141592653589793);
  }
  const double near_plane = json_get_double(value, "near", 0.0);
  if (near_plane > 0.0) camera.near_z = static_cast<float>(near_plane);
  const double far_plane = json_get_double(value, "far", 0.0);
  if (far_plane > near_plane) camera.far_z = static_cast<float>(far_plane);
  return camera;
}

/// 读灯光列表 → `Lighting`。
///
/// ## 只把"主光"放进 `Lighting`，其余进 `extra_lights`
///
/// `Lighting` 是场景的**基础照明**（环境 + 主方向光）。bpy 脚本可能建
/// 任意多盏灯，而那些应当作为附加光。取能量最高的当主光——它与
/// `ThreeRuntime` 的做法一致（那边也按 intensity 挑）。
/// Blender 的 `SUN energy`（W/m²）→ 本内核的 irradiance。
///
/// ## 旧式为何必须废弃
///
/// 旧实现是 `energy/1000 × 0.769`。那个 `0.769` 是**为补偿旧模型缺 1/π
/// 而反推出来的**（当时只有一个自由度，于是它把"缺 π"吞进了自己）。
/// 物理内核的漫反射走 `albedo/π`，同一个能量需要多乘一个 π 才落到同一亮度。
///
/// ## 但"补回 π"还不够——旧标定本身是在错误前提下做的
///
/// 旧标定用的是 `bpy_watch.py`（**三盏灯**：2600 / 900 / 1500）去拟合真 Blender
/// 的参照图，而旧模型只算**一盏**灯。用一个未知量（k）去拟合一个模型缺少的
/// 自由度（其余两盏灯），得到的 k 自然会被撑大。
///
/// 现在内核支持全部灯光（`ShadeInput::extra_lights`），三盏灯真的都参与计算，
/// 再套那个被撑大的 k 会直接过曝——实测：主体均值 150.1，而真 Blender 是 78.1。
///
/// ## 现在的定义：按物理量纲归一，不拟合观感
///
/// Blender 的 SUN `energy` 是**辐照度**（W/m²），本内核的 `Lighting::diffuse`
/// 乘进 `irradiance = saturate(n·l) × color × intensity` 之后也是辐照度。
/// 两者同一量纲，所以常数只负责一件事：**把 Blender 的数值尺度映射到
/// 本框架"1.0 = 正常受光"的尺度**。
///
/// 取 `energy 1 W/m² → intensity 1.0`（量纲 1:1，不做单位换算）：
///
///     kSunEnergyToIrradiance = 1.0
///
/// ## 为何不再除 1000（2026-11-06 修）
///
/// 旧代码是 `energy / 1000.0 * kSunEnergyToIrradiance`，与本函数的注释
/// **自相矛盾**：注释说“1000 W/m² → 1.0”，而对 `energy = 1.0` 算出 `0.001`。
///
/// 那个 `/1000` 是旧式 `energy/1000 × 0.769` 的分母残留（`0.769` 已删，
/// 分母没人清理）。它是**量纲错**，不是取舍：
///
/// Blender 的 SUN `energy` 单位就是 **W/m²（辐照度）**，而内核的
/// `diffuse` 乘进 `irradiance = n·l × color × intensity` 之后也是辐照度。
/// 两者同量纲，系数只能是 1。
///
/// ## 实测依据（不是推的）
///
/// 受控平面（0.5 灰 + SUN energy=1 正对 + 无环境光）：
///
/// | | 像素 |
/// |---|---|
/// | 真 Cycles（`tools/blender_cycles_truth.py`） | **111** |
/// | 本框架（去掉 /1000 且 ambient=0、ibl=0、tone=None） | **112** |
/// | 本框架（旧：/1000 + ambient .35 + ibl 1.0 + AgX） | 123 |
///
/// 全画面 262144 个像素**最大差 1 级**（p50=p90=p99=max=1），
/// 那 1 级来自 Principled 的镜面项（实测占比 0.796%）与 8bit 舍入边界。
///
/// ## 为何也不用“补偿 AgX”
///
/// 真 Blender 的观感还经过它自己的 AgX 3D LUT，而本框架的 AgX 是解析拟合、
/// 低值区偏亮（见 `docs/BACKLOG.md` P1）。**那个偏差不由本系数补偿**——
/// 否则色调映射修好之后这里又会偏暗，变成两个错误互相抵消。
constexpr float kSunEnergyToIrradiance = 1.0f;

auto read_lighting(const Json& items) -> Lighting {
  Lighting lighting{};
  if (!items.is_array() || items.empty()) return lighting;

  std::size_t key_index = 0;
  double key_energy = -1.0;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const double energy = json_get_double(items[i], "energy", 0.0);
    if (energy > key_energy) {
      key_energy = energy;
      key_index = i;
    }
  }

  const Json& key = items[key_index];
  // ⚠ `Lighting.light_from` 是**指向光源**的方向（与 bpy 相机的 -Z 视线相反）。
  // Python 侧抽出来的是"灯光照向哪"（-Z），所以要**取负**——
  // 漏了这步的症状是"物体被从背面照亮、正面全黑"。
  const auto direction = read_doubles(json_at(key, "direction"), 3);
  lighting.light_from = math::Vec3{static_cast<float>(-direction[0]),
                                   static_cast<float>(-direction[1]),
                                   static_cast<float>(-direction[2])};
  // ## 主光强度：物理口径重标（2026-11-06）
  //
  // ### 旧式为何必须废弃
  //
  // 旧实现是 `diffuse = energy/1000 × 0.769`。那个 `0.769` 是**为了补偿
  // 旧模型缺 1/π 而反推出来的**（标定时只能调一个自由度，于是它把
  // “缺 π”吞进了自己）。换物理内核后，漫反射已经走 `albedo/π`，
  // 再乘一个“反补偿”系数就是错两遍。
  //
  // 实测（`bpy_watch.py`，同一张真 Blender 参照图）：
  //
  // | 实现 | 主体均值 | 暗部占比 |
  // |---|---|---|
  // | 旧式（带 0.769） | 55.2 | **98.9%** |
  // | 真 Blender | 75.7 | 6.7% |
  //
  // 整体偏暗 20 级——正是 `1/π ≈ 0.318` 与 `0.769` 没对上的量级。
  //
  // ### 新式：量纲其实是对得上的
  //
  // Blender 的 SUN `energy` 单位是 **W/m²**，也就是**辐照度**；
  // 而物理内核里 `Lighting::diffuse` 乘进 `irradiance = saturate(n·l) × color × intensity`
  // 之后也是辐照度。两者是**同一量纲**，不需要“单位换算”，只需要一个
  // 让两边“中灰落点一致”的归一常数（Blender 自己的曝光/视图变换也参与其中）。
  //
  // 所以这里只保留一个常数 `kSunEnergyToIrradiance`，它的意义明确定义为：
  // **「Blender SUN energy 1 W/m² 对应本内核的多少 irradiance」**。
  //
  // 标定依据与实测表记在 `docs/bpy_render_parity.md`。
  if (key_energy > 0.0) {
    lighting.diffuse = static_cast<float>(key_energy) * kSunEnergyToIrradiance;
  }

  // `extra_lights` 是**定长 `std::array`**（渲染器按固定上限循环，
  // 见 `kMaxExtraLights` 的说明），所以按下标填、填满即止。
  std::size_t slot = 0;
  for (std::size_t i = 0; i < items.size() && slot < lighting.extra_lights.size(); ++i) {
    if (i == key_index) continue;
    Light light{};
    light.kind = LightKind::Directional;   // extras 只支持平行光语义
    const auto dir = read_doubles(json_at(items[i], "direction"), 3);
    // 与主光一样：`Light.direction` 是**指向光源**，要取负
    light.direction = math::Vec3{static_cast<float>(-dir[0]), static_cast<float>(-dir[1]),
                                 static_cast<float>(-dir[2])};
    const auto pos = read_doubles(json_at(items[i], "position"), 3);
    light.position = math::Vec3{static_cast<float>(pos[0]), static_cast<float>(pos[1]),
                                static_cast<float>(pos[2])};
    const auto rgb = read_doubles(json_at(items[i], "color"), 3);
    light.color = math::Vec3{static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                             static_cast<float>(rgb[2])};
    const double energy = json_get_double(items[i], "energy", 1000.0);
    // 与主光**同一系数**（物理口径，量纲 1:1）。
    //
    // 旧值 `× 0.75` 与主光的 `× 0.769` 是两个不同的数——历史上两次分别调参
    // 留下的。现在都归到同一个常量：同一类灯就该用同一个换算，否则
    // “换一个灯当主光”会让画面亮度跳变。
    //
    // ⚠ 这里曾与主光一起多除了一次 1000（已修，见 `kSunEnergyToIrradiance` 的说明）。
    // 两者必须同步：只修一处会让“多灯场景里主光换了就变色”。
    light.intensity = static_cast<float>(energy) * kSunEnergyToIrradiance;
    lighting.extra_lights[slot++] = light;
  }
  return lighting;
}

}  // namespace

auto parse_extracted_json(std::string_view json_text) -> Result<ThreeScene> {
  auto parsed = json_parse(json_text);
  if (!parsed.has_value()) {
    return st::unexpected(parsed.error().code,
                          "bpy 抽取结果不是合法 JSON：" + parsed.error().message);
  }
  const Json& root = *parsed;
  if (!root.is_object()) {
    return st::unexpected(ErrorCode::Parse, "bpy 抽取结果的顶层不是对象");
  }

  ThreeScene scene{};

  // ── 物体 ────────────────────────────────────────────────────────────
  const Json& objects = json_at(root, "objects");
  if (objects.is_array()) {
    scene.objects.reserve(objects.size());
    for (const auto& item : objects) {
      ThreeObject object{};
      object.name = json_get_string(item, "name", "object");
      object.mesh = read_mesh(item);
      if (object.mesh.vertices.empty()) continue;   // 空几何不占位
      object.material = read_material(item);
      // 多材质槽位表：非空时渲染按 `mesh.material_groups` 逐组换材质
      const Json& slots = json_at(item, "materials");
      if (slots.is_array()) {
        object.materials.reserve(slots.size());
        for (const auto& slot : slots) {
          object.materials.push_back(read_material_node(slot));
        }
      }
      // 分组表：三元组 `[index_start, index_count, material_index]`
      const Json& groups = json_at(item, "material_groups");
      if (groups.is_array()) {
        object.mesh.material_groups.reserve(groups.size() * 3U);
        for (const auto& group : groups) {
          if (!group.is_array() || group.size() < 3) continue;
          for (std::size_t k = 0; k < 3; ++k) {
            const auto value = group[k].is_number() ? group[k].get<std::int64_t>() : 0LL;
            object.mesh.material_groups.push_back(
                static_cast<std::uint32_t>(value < 0 ? 0 : value));
          }
        }
        // 分组退化（不足一组）就清空，回到单材质路径——防“搬了一半”的异常维数
        if (object.mesh.material_groups.size() % 3U != 0U) {
          object.mesh.material_groups.clear();
        }
      }
      object.world = read_matrix(json_at(item, "world"));
      object.visible = json_get_bool(item, "visible", true);
      scene.objects.push_back(std::move(object));
    }
  }

  // ── 相机 / 灯光 / 诊断 ──────────────────────────────────────────────
  scene.camera = read_camera(json_at(root, "camera"));
  scene.lighting = read_lighting(json_at(root, "lights"));

  // ## 世界背景 → 环境反射色（2026-11-05 加）
  //
  // 环境反射是**金属出色的唯一来源**（纯 Cook-Torrance 下金属其余像素全黑）。
  //
  // 而 `Lighting::env_sky` 默认是 `(0.52, 0.58, 0.68)`——一个**明亮的天空**。
  // 典型 bpy 脚本的世界背景只相当于线性 `0.006`，**差 87 倍**。
  //
  // 实测（`tools/subject_stats`，只统计主体）：
  //
  // | | 主体均值 | 暗部 |
  // |---|---|---|
  // | 本框架（用亮天空） | **139.6** | **0.0%** |
  // | 真 Blender | 78.1 | 34.6% |
  // | 真 three.js | 74.0 | 50.2% |
  //
  // 亮近一倍、完全没有暗部——因为金属被一个不存在的亮天空照亮。
  // ## 世界色 → IBL 探测器 + 清屏色（2026-11-06 重做）
  //
  // Blender 的 `world` 是**均匀发光环境**：它既提供环境光照，也当背景。
  // 物理内核下这两件事必须分开表达：
  //
  // | 用途 | 本框架怎么表达 |
  // |---|---|
  // | 环境光照（辐照度） | `ibl_sky` / `ibl_ground` + `ibl_strength` |
  // | 背景色（清屏） | `background_linear`（由 `render_impl` 直接用） |
  //
  // ## 为何不能只给 IBL 而不管背景
  //
  // 实测（`bpy_watch.py` vs 真 Blender `blender_reference.png`）：
  //
  // | | 背景像素 |
  // |---|---|
  // | 真 Blender | rgb(71,76,83) |
  // | 本框架（只设 IBL，背景走调用方默认） | rgb(60,60,60) |
  //
  // 背景占画面 **80%**，这 11 级差直接把全图暗部占比变成 91.8%（真 Blender 6.7%）。
  // 所以“世界色”必须同时喂给背景。
  //
  // ## 与浏览器对标的衔接
  //
  // `background_linear` 是**线性值**，与 three 的 `scene.background = Color(...)`
  // 同一口径，两者走同一条 `encode_pixel` 输出链——这就是“三条输入路径同一内核”
  // 在背景上的体现。
  {
    const Json& world = json_at(root, "world");
    if (world.is_array() && world.size() >= 3) {
      const auto rgb = read_doubles(world, 3);
      const math::Vec3 background{static_cast<float>(rgb[0]), static_cast<float>(rgb[1]),
                                  static_cast<float>(rgb[2])};
      // IBL：Blender 的 world 是“各个方向同亮”，所以天文两个探针取**同色**。
      //
      // 旧实现把地面乘了 0.5，那是为了人造“上亮下暗”的型制——而 Blender 的
      // world 是真的均匀，加 0.5 等于把一个均匀环境画成渐变，对着参照图反而偏。
      scene.lighting.ibl_sky = background;
      scene.lighting.ibl_ground = background;
      // 强度 1.0：world 的亮度就是环境亮度（不再乘一个人为系数）。
      scene.lighting.ibl_strength = 1.0f;
      // 背景：world 颜色就是清屏色（线性值与 three 同口径）。
      scene.background_linear = background;
      scene.background_set = true;
    }
  }

  // 「对标」声明与「脚本是否真的设了 world」——两者都必须从 Python 侧取，
  // 不能在 C++ 侧推（兼容层的 scene.world 永远非空，见 extract.py 的说明）。
  const Json& compat = json_at(root, "compat");
  if (compat.is_string()) {
    scene.bpy_compat = compat.get<std::string>();
  }
  scene.world_set = json_get_bool(root, "world_set", false);

  const Json& warnings = json_at(root, "warnings");
  if (warnings.is_array()) {
    for (const auto& item : warnings) {
      if (item.is_string()) scene.warnings.push_back(item.get<std::string>());
    }
  }

  return scene;
}

auto BpySceneExtractor::available() -> bool {
  return st::mp::Runtime::available();
}

auto BpySceneExtractor::unavailable_reason() -> std::string {
  return st::mp::Runtime::unavailable_reason();
}

auto BpySceneExtractor::extract(std::string_view script) -> Result<ThreeScene> {
  auto& runtime = st::mp::Runtime::instance();

  // ── ① 重置场景（关键）─────────────────────────────────────────────
  //
  // `bpy.data` 是模块级单例，跨 `run` 存活。不重置的话第二个脚本会看到
  // 第一个留下的对象，报 `ValueError: 已存在同名数据块：'Cube'`——
  // 那个错看起来像"脚本重名了"，实际是**上一个脚本的残留**。
  //
  // 重置放在**跑脚本前**而不是后：抽取完用户可能还想接着用那个场景
  // （调试、多次 `extract` 同一份），所以不能在抽取后清。
  const auto reset = runtime.run("import bpy\nbpy.data.reset()\n", "<bpy_reset>");
  if (!reset.has_value()) {
    return st::unexpected(reset.error().code,
                          "bpy 场景重置失败：" + reset.error().message);
  }

  // ── ② 跑脚本（建出 bpy 场景）────────────────────────────────────────
  const auto executed = runtime.run(script, "<bpy_script>");
  if (!executed.has_value()) {
    return st::unexpected(executed.error().code, executed.error().message);
  }

  // ── ③ 抽取（Python 侧已把逻辑写好，这里只要 JSON）──────────────────
  //
  // ## 为什么"跑脚本"和"抽取"是两次 run
  //
  // `st_mp_run` 每次用干净命名空间（见 mp_bridge.c 的说明），所以脚本里
  // 定义的 bpy 数据**在 run 结束后仍然存在**——它们挂在 `sys.modules['bpy']`
  // 的模块对象上（`bpy.data` 是个模块级单例），不依赖那次 run 的 globals。
  //
  // 这也是为什么抽取能分两步：场景状态住在模块里，不在脚本的命名空间里。
  const auto dumped = runtime.run(
      "import _st_extract\n"
      "result = _st_extract.extract_json()\n",
      "<bpy_extract>");
  if (!dumped.has_value()) {
    return st::unexpected(dumped.error().code,
                          "bpy 场景抽取失败：" + dumped.error().message);
  }

  // ── ④ JSON → ThreeScene ─────────────────────────────────────────────
  return parse_extracted_json(dumped->value);
}

}  // namespace st::raster




