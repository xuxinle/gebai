#pragma once

/// @file scene_graph.hpp
/// @brief **组合模型的空间体系**：节点树、局部坐标系、枢轴（pivot）、世界变换求解。
///
/// ## 为什么单独一层（而不是把字段塞进 `SceneObjectSpec`）
///
/// "一个物体摆在哪"和"一组物体怎么作为一个整体运动"是两件事。前者只要一个矩阵，
/// 后者需要：父子链、每个节点自己的局部坐标系、以及**绕哪里转**（枢轴）。
/// 把这些算清楚是纯数学（不碰几何、不碰渲染），所以单独一层：
/// 任何调用方（场景脚本、裁云、导出器、动画）都能用同一套求解，不必各自写一遍
/// `parent * child` —— 而那种重复实现几乎必然在某一处写反乘序。
///
/// ## 约定（与 Blender / glTF 一致，别在调用点另立一套）
///
/// | 项 | 约定 |
/// |---|---|
/// | 手性 | 右手系，+Y 朝上，相机看向 -Z |
/// | 角度 | **弧度**（字段名带 `_degrees` 的才是度） |
/// | 变换顺序 | 先缩放 → 再旋转 → 最后平移（矩阵写作 `T · R · S`） |
/// | 局部坐标 | 子节点的变换**相对父节点的坐标系**（父动子动） |
/// | 枢轴 | 旋转与缩放**绕枢轴**；平移不受枢轴影响（与 Blender 的 origin 同语义） |
///
/// ## 枢轴（pivot）到底解决什么
///
/// 没有枢轴时，一切旋转绕"局部原点"。而现实里的组合体几乎都不是这样：
/// 门的枢轴在合页上、机械臂的枢轴在肩关节、整台设备的枢轴在底座中心。
/// 没有枢轴就只能靠"父节点套一层空组 + 手工算偏移"来模拟，而那个偏移量
/// 一改零件尺寸就失效。
///
/// 一个数就能表达：
///
/// ```text
/// local = T(translate) · T(pivot) · R(fixed) · S(scale) · T(-pivot)
/// ```
///
/// `pivot = (0,0,0)` 时退化成 `T · R · S`（旧行为，逐像素不变）。

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh_ops.hpp"

namespace st::raster {

/// 一个节点的局部变换（平移 / 欧拉角 / 缩放）。
///
/// 单独成一个结构（而不是三个裸 `Vec3`）的理由:它们总是在一起被传递、
/// 一起被序列化、一起被合成矩阵；分开写会让每个签名都长成
/// `(Vec3, Vec3, Vec3, ...)`，读的人必须回查第 2 个参数是平移还是旋转。
struct TransformTRS {
  math::Vec3 translation{0.0f, 0.0f, 0.0f};
  /// 欧拉角（弧度）；合成顺序 **X → Y → Z**（先绕 X，再绕 Y，最后绕 Z）。
  ///
  /// 为什么钉死顺序：欧拉角本身有歧义（同一姿态有 6 种写法），不钉死则
  /// "我明明设了 90°" 和"画面是歪的"会同时成立。X→Y→Z 与 Blender 的
  /// `rotation_euler`（XYZ 模式）一致，便于把数值直接搬过来。
  math::Vec3 rotation{0.0f, 0.0f, 0.0f};
  math::Vec3 scale{1.0f, 1.0f, 1.0f};

  [[nodiscard]] auto is_identity() const noexcept -> bool;
};

/// 枢轴的**预设**（按几何包围盒推），用于"不想手算数字"的场合。
///
/// 为什么给预设而不是只给坐标：搭一个"站在地上的机器人"时，
/// 枢轴应该是脚底中心——而那是**几何决定**的，手算要先生成网格再看包围盒。
/// 预设让脚本直接写 `"pivot_at": "bottom"`，改零件尺寸也不用重算。
enum class PivotPreset : std::uint8_t {
  Origin = 0,   ///< 局部原点（默认；旧行为）
  Center = 1,   ///< 包围盒中心
  Bottom = 2,   ///< 包围盒底面中心（`x/z` 取中心、`y` 取 min）——"站在地上"的枢轴
  Top = 3,      ///< 包围盒顶面中心
};

/// 名字 ↔ 预设（`"origin"` / `"center"` / `"bottom"` / `"top"`）。
[[nodiscard]] auto pivot_preset_name(PivotPreset preset) noexcept -> std::string_view;
[[nodiscard]] auto parse_pivot_preset(std::string_view name) -> std::optional<PivotPreset>;
/// 全部预设名（给报错里的"可用清单"用）。
[[nodiscard]] auto pivot_preset_names() -> std::string;
/// 由包围盒算枢轴点；`Origin` 或空盒返回零向量。
[[nodiscard]] auto pivot_from_bounds(const Bounds& bounds, PivotPreset preset) noexcept
    -> math::Vec3;

/// 一个节点（场景树的一个结点）。
///
/// ## 叶子与分组是同一个类型
///
/// 为什么不分成 `GroupNode` / `MeshNode` 两个类型：两者的**空间语义完全相同**
/// （都有局部坐标系与枢轴，都能挂子节点），唯一的差别是"自己有没有几何"。
/// 分成两个类型会让递归函数写两遍、让"把叶子改成分组"变成改类型——
/// 而那是组合建模里最常做的一次编辑。
struct SceneNode {
  /// 名字（在整棵树里唯一）。空 = 未命名（按路径寻址时只能用 `parent/child` 形式）。
  ///
  /// 为什么必须有名字：组合模型的价值在于"能单独操作某个零件"
  ///（动画面板的通路、导出后在引擎里找对象、报错时说清是哪个节点）。
  std::string name{};

  /// 本身是否承载几何（false = 纯分组/空物体，用于给一组零件一个共同的枢轴）。
  bool has_geometry{true};

  TransformTRS local{};
  /// 枢轴点（**在自身局部坐标系里**）；旋转与缩放绕它进行。
  math::Vec3 pivot{0.0f, 0.0f, 0.0f};
  /// 是否可见。不可见的节点不渲染、也**不参与包围盒**（否则隐藏一个零件会让
  /// 枢轴突然跳位——那是很难查的一类"改了别处导致这里歪了"）。
  bool visible{true};

  std::vector<SceneNode> children{};

  [[nodiscard]] auto is_group() const noexcept -> bool { return !has_geometry; }
};

/// 本节点的**局部矩阵**：`T(translation) · T(pivot) · R · S · T(-pivot)`。
[[nodiscard]] auto local_matrix(const SceneNode& node) noexcept -> math::Mat4;

/// 一个已求解的节点（树展平后的结果）。
struct ResolvedNode {
  /// 路径（`"robot/arm/upper"`）：用 `/` 连接各级名字；未命名节点用 `#序号`。
  ///
  /// 为什么同时给路径与名字：名字在树里唯一，但**同名节点在不同分支下**是
  /// 允许的（"左轮/轮毂"与"右轮/轮毂"）；路径永远唯一，日志与报错用它才不歧义。
  std::string path{};
  std::string name{};
  std::size_t depth{0};
  int parent{-1};                              ///< 指向 `ResolvedNode` 的下标；-1 = 根
  math::Mat4 world{math::Mat4::identity()};    ///< 已含父链
  /// 本节点的局部 TRS（**已含动画采样结果**）。
  ///
  /// 为什么把它带出来：调用方常常需要在"世界矩阵之外"拿到局部值
  /// （例如枢轴预设要重算局部矩阵、导出动画要取当前姿态）。
  /// 只给 `world` 的话，重算局部就会退回场景里的初始值——实测症状是
  /// "设了枢轴的节点纹丝不动，其它节点正常"。
  TransformTRS local{};
  bool visible{true};                          ///< 自身与全部祖先都可见才为真
  bool has_geometry{true};
};

/// 展平结果（顺序 = 深度优先先序，父一定在子之前）。
struct ResolvedTree {
  std::vector<ResolvedNode> nodes{};
  /// 名字 → 下标（仅当名字唯一；`resolve` 已校验唯一性）。
  std::map<std::string, std::size_t> name_index{};
  /// 按名字查下标（仅当名字唯一时有意义；重复名字见 `resolve` 的校验）。
  [[nodiscard]] auto find_by_name(std::string_view name) const -> const ResolvedNode*;
  [[nodiscard]] auto find_by_path(std::string_view path) const -> const ResolvedNode*;
};

/// 求解整棵树的世界变换。
///
/// ## 校验（全部带路径，便于生成代码的模型自我修正）
///
/// - **重名**：同一棵树里两个同名节点 → `Invalid`（按名字寻址会歧义）；
/// - **深度上限**：超过 `kMaxDepth` → `Invalid`（防病态输入把递归打爆）。
///
/// ## 为什么父一定在子之前
///
/// 展平顺序是深度优先先序。调用方按顺序画就能保证"父的世界矩阵已算好"，
/// 不必二次遍历；导出 glTF 时也正好按这个顺序建节点。
[[nodiscard]] auto resolve(const std::vector<SceneNode>& roots) -> Result<ResolvedTree>;

/// 深度上限（与 JSON 解析的 128 层同量级；场景树比文档浅得多，32 足够）。
inline constexpr std::size_t kMaxSceneDepth = 32;

/// 把 `parent`（按名字）指向的节点，把子节点挂过去（**扁平写法 → 树**）。
///
/// 支持"嵌套写"与"扁平写 + parent 引用"两种等价形态：
///
/// ```json
/// // 嵌套
/// { "name": "arm", "kind": "group", "children": [ {"shape":"box"} ] }
/// // 扁平（更适合大场景的增量编辑：不必维护括号层级）
/// [ { "name": "arm", "kind": "group" }, { "shape": "box", "parent": "arm" } ]
/// ```
///
/// ## 顺序无关
///
/// `parent` 可以指向**后面**才出现的名字（两遍解析）。理由是实际的编辑顺序
/// 往往是"先写零件、再想起给它加个父节点"，要求先声明父会让人反复挪动段落。
///
/// 错误：找不到父（`NotFound`）、形成环（`Invalid`）、自指（`Invalid`）。
auto attach_by_parent(std::vector<SceneNode>& nodes, const std::vector<std::string>& parent_names)
    -> Status;

/// 把一个网格的几何**烘焙进世界坐标**（应用 `world` 矩阵），并清除枢轴含义。
///
/// 用途：把组合模型压成一个可导出的单网格（引擎里"一个 draw call"），
/// 或做碰撞体/包围盒。注意这是**破坏性**的：之后不能再按零件单独改。
auto bake_into(std::vector<Mesh>& accumulator, const Mesh& mesh,
                             const math::Mat4& world) -> void;

}  // namespace st::raster
