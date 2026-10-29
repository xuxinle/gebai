#pragma once

/// @file primitives.hpp
/// @brief **参数化图元库**：对标 Blender「Add ▸ Mesh ▸ …」与游戏引擎内置形状。
///
/// ## 为什么单独一个头（而不是往 `Mesh` 里继续加静态工厂）
///
/// `Mesh` 的静态工厂里混着三类东西：① 通用形状（球/盒/柱）；② 贴图专用载体
/// （`textured_slab` / `textured_icon_box`）；③ 兜底阴影（`flattened`）。
/// 后两类的**参数语义是某个应用的需要**（图标面、投影），把它们和新图元放在一起，
/// 图元库的契约（"水密、法线朝外、UV 0..1、确定性"）就会被一堆特例冲淡。
///
/// 所以图元**另开一层**：这里只放"一个建模软件该有的基础形状"，
/// 契约统一、可被逐项断言。`Mesh` 上的老工厂原样保留（不改变既有画面）。
///
/// ## 契约（每个 `make_*` 都满足，测试逐项断言）
///
/// 1. **右手系、Y 轴朝上、单位尺度**（半径/高按参数，不额外缩放）；
/// 2. **闭合体水密**：每条边恰好被两个三角形共享（`open_*` 开头的单面形状除外，
///    它们的名字里就写着 open）；
/// 3. **法线朝外且单位长**；三角形**逆时针**为正面（与 `CullMode::Back` 一致）；
/// 4. **UV 在 0..1**（需要贴图时有意义；不需要贴图的形状也统一给）；
/// 5. **确定性**：同一组参数永远产出同一字节序列（无随机、无时间依赖）。
///
/// ## 参数名与 Blender 对齐（便于把 Blender 里的数值直接搬过来）
///
/// | 参数 | 含义 | Blender 面板上的名字 |
/// |---|---|---|
/// | `radius` | 半径 / 外半径 | Radius |
/// | `depth` | **Y 方向的长**（柱/锥/胶囊的总高） | Depth（不是 Depth 轴，是长度） |
/// | `segments` | 绕 Y 轴一圈的分段数 | Vertices（柱/锥） |
/// | `rings` | 从下到上的环数（球是 latitudinal rings） | Rings（UV Sphere） |
/// | `subdivisions` | 细分次数（每 +1 三角形数 ×4） | Subdivisions（Ico Sphere） |
/// | `minor_radius` | 管径 / 小半径 | Minor Radius |
/// | `major_segments` / `minor_segments` | 主环/截面环的细分 | Major/Minor Segments |
/// | `twist` | 扭转角（弧度，整段高度上的总扭转） | 无对应（工程件常用） |
///
/// ⚠ 单位：本文件所有角度参数都是**弧度**。要从度数转：
/// `degrees * 3.14159265f / 180.0f`；本头提供两个小函数 `radians()` / `degrees()`
/// （放在这里而不是 `math` 层：当前只有三维图元用它，等真正有第二个使用者再上提）。
///
/// 颜色口径：`math::Color` 入参是 **sRGB 字节**（内部解码成线性）；
/// 不给颜色时用白色，颜色交给材质的 `base_color`（这正是 Blender 的做法）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster::primitives {

/// 圆周率（float 版；`<numbers>` 的 `pi_v<float>` 在部分标准库实现里仍要拼写完整命名空间）。
inline constexpr float kPi = 3.14159265358979323846f;

/// 度 → 弧度（生成脚本里写度数最常见，给它一个显式入口比到处写乘法可靠）。
[[nodiscard]] constexpr auto radians(float degrees_value) noexcept -> float {
  return degrees_value * (kPi / 180.0f);
}

/// 弧度 → 度（日志/报错里要给人看的数值用）。
[[nodiscard]] constexpr auto degrees(float radians_value) noexcept -> float {
  return radians_value * (180.0f / kPi);
}

/// 图元种类（字符串 ↔ 枚举的唯一映射；脚本层与 glTF 导入都经它）。
///
/// 名字取**最通用的称呼**（大模型最容易写对的那一个）：
/// 同一个形状的其它常见别名在 `parse_kind` 里接受（`sphere` / `uv_sphere` /
/// `icosphere` / `ball`），但**规范名**只有一个，写出去的一律是规范名。
enum class PrimitiveKind : std::uint8_t {
  Plane,        ///< `plane`：可细分的平面（+Y 朝上）
  Cube,         ///< `cube`：立方体（可独立三轴 → 即 box）
  UvSphere,     ///< `uv_sphere`：经纬球（最常用）
  IcoSphere,    ///< `ico_sphere`：正二十面体细分球（三角面均匀，适合雕塑/低模）
  Cylinder,     ///< `cylinder`：圆柱（带盖）
  Cone,         ///< `cone`：圆锥/圆台（`top_radius=0` 是尖锥）
  Capsule,      ///< `capsule`：胶囊（柱 + 两端半球，角色/药丸最常用）
  Torus,        ///< `torus`：圆环（甜甜圈）
  Disc,         ///< `disc`：圆盘（单面）
  Circle,       ///< `circle`：圆环线（单面，做"描边/光环"的几何载体）
  Grid,         ///< `grid`：网格平面（可分段，做地面/棋盘）
  Tube,         ///< `tube`：管（无端盖的圆柱，`open_tube` 别名）
  Prism,        ///< `prism`：N 棱柱（`sides=3` 三棱柱、`6` 六角柱）
  Gear,         ///< `gear`：齿轮（齿数/齿高可调）
  TorusKnot,    ///< `torus_knot`：环面纽结（p,q 纽结）
  Arrow,        ///< `arrow`：箭头（方向指示/坐标系轴）
  Wedge,        ///< `wedge`：楔形（斜坡、屋顶）
  Tetrahedron,  ///< `tetrahedron`：正四面体
  Octahedron,   ///< `octahedron`：正八面体
  Dodecahedron, ///< `dodecahedron`：正十二面体
  Icosahedron,  ///< `icosahedron`：正二十面体
  Helix,        ///< `helix`：螺旋管（弹簧/螺纹）
};

/// 规范名（写进脚本/glTF/日志的**唯一**名字）。
[[nodiscard]] auto kind_name(PrimitiveKind kind) noexcept -> std::string_view;

/// 把（可能带别名的）名字解析成种类。失败返回 `std::nullopt`。
///
/// 接受的别名（全部小写、忽略大小写与 `-`/空格）：
/// `plane`/`quad`、`cube`/`box`、`sphere`/`uv_sphere`/`ball`/`globe`、
/// `ico_sphere`/`icosphere`、`cylinder`/`tube`? （`tube` 有独立语义，见下）、
/// `cone`、`capsule`/`pill`、`torus`/`donut`/`doughnut`、`disc`/`disk`、
/// `circle`/`ring`、`grid`、`tube`/`open_tube`/`pipe`、`prism`/`ngon_prism`、
/// `gear`/`cog`、`torus_knot`/`knot`、`arrow`、`wedge`/`ramp`、
/// `tetrahedron`/`tetra`、`octahedron`/`octa`、`dodecahedron`/`dodeca`、
/// `icosahedron`/`icosa`、`helix`/`spring`/`coil`。
[[nodiscard]] auto parse_kind(std::string_view name) -> std::optional<PrimitiveKind>;

/// 全部规范名（顺序 = 枚举顺序），用于生成"可用形状"的提示文本与文档。
[[nodiscard]] auto all_kind_names() -> std::string;

/// 所有 `make_*` 的公共参数。
///
/// ## 为什么用一个参数结构而不是一长串位置参数
///
/// 十二个图元各有一组参数，位置参数会让调用点变成 `make_cylinder(0.5f, 1.0f, 32, true, 0.0f, color)`
/// —— 读的人必须回查签名才知道第 5 个是什么（而模型**不会**回查，它会猜）。
/// 具名参数（`{.radius = 0.5f, .segments = 32}`）自带解释，且新增参数不破坏调用点。
///
/// 缺省值是"一个单位大小的普通图元"：所有 `make_*(kind, {})` 都产出半径/半长 0.5 左右的形状。
struct PrimitiveParams {
  /// 半径 / 外半径（球、柱、锥的底面、环的主半径、棱柱外接圆半径）。
  float radius{0.5f};
  /// Y 方向总长（柱、锥、胶囊、棱柱、齿轮的厚度）。**不是半径**。
  float depth{1.0f};
  /// Y 方向的**半**长（当 `depth` 不便表达时用；`depth` 优先）。
  float height{0.0f};

  /// 绕 Y 轴一圈的分段（柱/锥/盘/棱柱的边数）。
  int segments{32};
  /// 球的纬度环数（只对 `uv_sphere` 有意义）。
  int rings{16};
  /// 二十面体球/正多面体的细分次数（0 = 原始二十面体）。
  int subdivisions{2};

  /// 环的主半径（`torus`/`torus_knot`）与管半径。
  float major_radius{0.5f};
  float minor_radius{0.18f};
  int major_segments{32};
  int minor_segments{16};

  /// 锥的顶面半径（0 = 尖锥；>0 = 圆台）。
  float top_radius{0.0f};

  /// 网格平面的两向分段（`grid`）。
  int columns{8};
  int rows{8};

  /// 棱柱的边数（与 `segments` 同义；优先用 `sides`，非 0 时覆盖 `segments`）。
  int sides{0};

  /// 齿轮：齿数、齿高、齿宽占比（0..1）。
  int teeth{12};
  float tooth_height{0.08f};
  float tooth_width{0.5f};

  /// 纽结的 (p, q)（互质时是纽结，不互质时是链环）。
  int knot_p{2};
  int knot_q{3};

  /// 螺旋：圈数、每圈半径收缩量。
  float turns{3.0f};
  float taper{0.0f};

  /// 扭转变形：整段高度上的**总**扭转角（弧度，只对棱柱/齿轮/管有意义）。
  float twist{0.0f};

  /// 平面/盘/网格的朝向：`false` = 朝 +Y（水平，地面用），`true` = 朝 +Z（立面用）。
  bool face_camera{false};

  /// 半球是否保留（胶囊两端的盖；关掉=圆柱）。
  bool cap_top{true};
  bool cap_bottom{true};

  /// 顶点色（sRGB 字节）。不给用白色——颜色优先由材质决定（与 Blender 一致）。
  math::Color color{255, 255, 255, 255};

  /// 是否给 UV（个别形状算 UV 很贵且有歧义；默认给）。
  bool with_uv{true};
};

/// 归一化参数（夹到合法范围、按 `sides` 覆盖 `segments` 等），`make_*` 内部先调它。
///
/// 为什么要有：`segments = 2` 的圆柱/`radius = 0` 的球会产出退化三角形，
/// 它们能画但会污染 AABB、让法线出现 NaN。夹取一次比在每个生成器里各写一遍可靠。
[[nodiscard]] auto sanitized(PrimitiveParams params) -> PrimitiveParams;

// ────────────────────────────────────────────────────────────────────────────
// 生成器（全部满足文件头的五条契约）
// ────────────────────────────────────────────────────────────────────────────

/// 平面（+Y 或 +Z 朝上，单面）。
[[nodiscard]] auto make_plane(const PrimitiveParams& params) -> Mesh;
/// 立方体/长方体（`radius` 是半边长，即边长 2·radius；三轴可用 `depth`/`height` 调）。
[[nodiscard]] auto make_cube(const PrimitiveParams& params) -> Mesh;
/// 经纬球（Blender 的 UV Sphere）。
[[nodiscard]] auto make_uv_sphere(const PrimitiveParams& params) -> Mesh;
/// 二十面体细分球（Blender 的 Ico Sphere）。
[[nodiscard]] auto make_ico_sphere(const PrimitiveParams& params) -> Mesh;
/// 圆柱（带上下盖）。
[[nodiscard]] auto make_cylinder(const PrimitiveParams& params) -> Mesh;
/// 锥/圆台。
[[nodiscard]] auto make_cone(const PrimitiveParams& params) -> Mesh;
/// 胶囊。
[[nodiscard]] auto make_capsule(const PrimitiveParams& params) -> Mesh;
/// 圆环。
[[nodiscard]] auto make_torus(const PrimitiveParams& params) -> Mesh;
/// 圆盘（单面，+Y 或 +Z）。
[[nodiscard]] auto make_disc(const PrimitiveParams& params) -> Mesh;
/// 圆环线（单面，宽度为 0 的同心双圈 → 用两个半径做细环带）。
[[nodiscard]] auto make_circle(const PrimitiveParams& params) -> Mesh;
/// 网格平面（可分段；做地面/棋盘/波浪底板）。
[[nodiscard]] auto make_grid(const PrimitiveParams& params) -> Mesh;
/// 管（无端盖）。
[[nodiscard]] auto make_tube(const PrimitiveParams& params) -> Mesh;
/// N 棱柱（`sides=3` 三棱柱）。
[[nodiscard]] auto make_prism(const PrimitiveParams& params) -> Mesh;
/// 齿轮（齿形挤出）。
[[nodiscard]] auto make_gear(const PrimitiveParams& params) -> Mesh;
/// 环面纽结。
[[nodiscard]] auto make_torus_knot(const PrimitiveParams& params) -> Mesh;
/// 箭头（四棱锥头 + 方杆，沿 +Y 指向）。
[[nodiscard]] auto make_arrow(const PrimitiveParams& params) -> Mesh;
/// 楔形（斜坡）。
[[nodiscard]] auto make_wedge(const PrimitiveParams& params) -> Mesh;
/// 正四面体 / 正八面体 / 正十二面体 / 正二十面体。
[[nodiscard]] auto make_tetrahedron(const PrimitiveParams& params) -> Mesh;
[[nodiscard]] auto make_octahedron(const PrimitiveParams& params) -> Mesh;
[[nodiscard]] auto make_dodecahedron(const PrimitiveParams& params) -> Mesh;
[[nodiscard]] auto make_icosahedron(const PrimitiveParams& params) -> Mesh;
/// 螺旋管（弹簧/螺纹）。
[[nodiscard]] auto make_helix(const PrimitiveParams& params) -> Mesh;

/// 按种类分派（脚本层与界面共用这一个入口）。
[[nodiscard]] auto make(PrimitiveKind kind, const PrimitiveParams& params) -> Mesh;

/// 解析 `kind_name` 再生成；名字不认识时返回带**可用清单**的 `Parse` 错误。
///
/// 错误信息刻意列出全部规范名：生成脚本的模型拿到"收到 dodecahedronx"没法自行修正，
/// 拿到可用清单能立刻改对（这是`scene3d_script` 已经验证过的做法）。
[[nodiscard]] auto make_from_name(std::string_view name, const PrimitiveParams& params)
    -> Result<Mesh>;

}  // namespace st::raster::primitives
