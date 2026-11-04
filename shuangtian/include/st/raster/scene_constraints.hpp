#pragma once

/// @file scene_constraints.hpp
/// @brief **声明式装配约束**：attach / connect / align——把「零件摆到哪」
/// 从手工算坐标变成「引用另一个零件的特征」。
///
/// ## 为什么需要它（没有约束时的实测代价）
///
/// 蒸汽朋克差分机的第一版（`scene_xml_showcase_probe.cpp`）里，压力计管子的
/// 长度、排气管的落点、张紧轮的高度全是**手算硬编码**：
///
/// - 改底座高度 → 立柱、齿轮组、排气管、压力计**全部要逐个重算**（实测漏改
///   即悬浮/穿模，人眼评审三轮才找完）；
/// - 「管子从 A 接到 B」这种本质是**两点定线**的东西，人肉算方向角+长度，
///   每根管都是一次三角函数作业，写错没有任何报错——只是画面里歪着。
///
/// 约束层把这层「装配关系」显式写进场景：
///
/// ```xml
/// <Gear name="small" attach="top_of:plinth" .../>          <!-- 贴上就完 -->
/// <Tube  name="pipe" connect="col_cap:reactor_base" size="0.1, auto"/>  <!-- 两点定线 -->
/// <Cylinder name="cap" align="axis:main_shaft" .../>       <!-- 轴线对齐 -->
/// ```
///
/// ## 三种约束（刻意只有三种）
///
/// | 约束 | 语义 | 解算结果 |
/// |---|---|---|
/// | `attach` | 我的锚点 = 目标锚点（世界系） | 改写 `translate` |
/// | `connect` | 我的 `bottom`/`top` 锚点对准 A、B | 改写 `translate` + `rotation`（Y 轴对准 A→B）+ `size` 的 Y 项（`auto` 时=距离） |
/// | `align` | 我的轴线与目标轴线同线 | 改写 `translate`（XY 对齐，保 Y） |
///
/// 原语只有三种是刻意的：它们覆盖装配里「贴、连、对」三类关系；更复杂的
/// （齿轮啮合相位、平行四边形联动）属于**行为**，应交给动画轨道，不该混进
/// 静态装配层。
///
/// ## 锚点语法（与 `pivot_at` 同一张词表 + 偏移）
///
/// `bottom / center / top / left / right / front / back`，大小写不敏感；
/// 后缀 `+dx,dy,dz` 在锚点上加偏移（世界系）：`top_of:plinth+0,0.1,0`。
/// 默认锚点：`attach` 用 `bottom`（「坐上去」），`connect` 用 `bottom→top`。
///
/// ## 解算时机与写回（这条决定了下游零改动）
///
/// 解算发生在**解析之后、建树（`resolve`）之前**，结果**写回 `translate` /
/// `rotation` / `size`**：
///
/// - 动画、蒙皮、渲染、序列化、glTF 导出**全部零改动**——它们看到的仍是
///   普通的 TRS；
/// - `st_api --normalize` 输出的就是解算后的值（「引擎理解成了什么」含约束）；
/// - 约束字段本身保留在 spec 里（`serialize` 时写出），增量编辑不丢装配关系。
///
/// ## 依赖规则（为什么「只准引用先出现的」）
///
/// 约束目标必须在**当前对象之前已完成解算**。树的形态里天然满足：父与兄
/// 先于子。跨树引用（后面的对象）→ 报错（`attach：目标 "xxx" 尚未解算——
/// 约束只能引用先出现的名字`）。这条规则让解算**一遍拓扑序完成**，
/// 不需要迭代求解器——可预测、确定性、有环必报（环 = 前向引用，天然非法）。
///
/// ## 与 `pivot_at` 的分工
///
/// `pivot_at` 回答「**我自己的旋转绕哪转**」（自身包围盒的特征点）；
/// 约束回答「**我相对别人摆哪**」（别人的特征点）。两者正交、可同用：
/// 表针 `pivot_at="bottom"` + `attach="front_of:gauge_body+0,0,0.09"`。
///
/// ## 有意不做的事
///
/// - **不做迭代求解器**（CAD 那种尺寸链）：声明即解算，一遍出结果。
///   需要迭代的关系应该重新审视建模方式（中间加分组节点）。
/// - **不约束动画帧**：约束解算初始姿态；之后动画轨道接管（改 `rotation`
///   的轨道会与 `connect` 写的旋转叠加——语义是「先装配、再动画」，直觉且够用）。
/// - **不做面贴合的法线对齐**（`attach` 不改朝向）：要转向用 `rotation`
///   显式写或用 `connect`。

#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <cstdint>

#include "st/core/error.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh_ops.hpp"

namespace st::raster {

/// 前置声明。
///
/// ⚠ 必须写 `struct` 而不是 `class`：`SceneObjectSpec` 的实际定义是 `struct`，
/// 两者混用会让 clang 报 `-Wmismatched-tags`（MSVC ABI 下可能导致链接错误）。
struct SceneObjectSpec;

/// 锚点名（`pivot_at` 同一张词表的八方向 + center）。
struct AnchorKind {
  enum Value : std::uint8_t {
    Bottom, Center, Top, Left, Right, Front, Back,
  };
  Value value{Bottom};

  /// 解析（大小写不敏感）。失败返回 `nullopt`（调用方负责给候选报错）。
  [[nodiscard]] static auto parse(std::string_view name) -> std::optional<AnchorKind>;
  [[nodiscard]] auto name() const -> std::string_view;
};

/// 一个锚点引用：`top_of:plinth+0,0.1,0` → {anchor=Top, target="plinth", offset}。
/// `target` 为空 = 引用**自己**（`attach="bottom"` 单独出现没意义，但
/// `connect` 的一端可以是「我自己的锚点」……本版不支持，保留扩展位）。
struct AnchorRef {
  AnchorKind anchor{};
  std::string target{};
  math::Vec3 offset{0.0f, 0.0f, 0.0f};
};

/// 解析锚点引用串（`"<anchor>[_of]:<name>[+dx,dy,dz]"`）。
///
/// 宽松接受三种写法（都在锚点词表内时）：
/// - `top_of:plinth`（推荐，读作「plinth 的顶面」）
/// - `top:plinth`
/// - `plinth`（省略锚点 = 该约束的默认锚点）
[[nodiscard]] auto parse_anchor_ref(std::string_view text) -> Result<AnchorRef>;

/// 约束解算的回调接口：约束层不 include 场景脚本（避免循环依赖），
/// 几何与查找由调用方提供。
struct ConstraintContext {
  /// 按名字取一个对象的**世界系包围盒**（含其子树；已含当前已解算的变换）。
  /// 返回 `nullopt` = 名字不存在（调用方往往有更全的诊断信息，这里只判空）。
  const std::function<std::optional<Bounds>(const std::string& name)>& world_bounds_of;

  /// 按名字取一个对象的**世界系锚点**（包围盒特征点，世界系坐标）。
  const std::function<std::optional<math::Vec3>(const std::string& name, AnchorKind anchor)>&
      world_anchor_of;
};

/// 包围盒的锚点（六面中心 + 体心）。约束层不重复定义几何，只做特征点选取；
/// 公开给调用方（构造 `ConstraintContext::world_anchor_of` 回调时用）。
[[nodiscard]] auto anchor_of_bounds(const Bounds& box, AnchorKind anchor) -> math::Vec3;

/// 解算一个 `attach` 约束：返回我应当处于的**世界系平移量**。
///
/// `attach`（如 `top_of:plinth`）= 我的 `my_anchor` 对准目标的 `ref_anchor`。
/// 调用方把世界平移换算回局部（减去父的世界平移——本版约定约束均在
/// **世界系**表达，父节点不再另行变换；带变换的父请用分组节点包装）。
[[nodiscard]] auto solve_attach(const AnchorRef& mine, const AnchorRef& target,
                                const ConstraintContext& context) -> Result<math::Vec3>;

/// 解算 `connect`：A、B 两个世界锚点。
struct ConnectSolution {
  math::Vec3 midpoint{};      ///< 我的世界平移（AB 中点）
  math::Vec3 rotation_deg{};  ///< 使 +Y 指向 A→B 的欧拉角（度，X→Y→Z 序）
  float length{0.0f};         ///< |B-A|（`size` 的 Y 项写 `auto` 时用）
  math::Vec3 direction{};     ///< A→B 单位向量（诊断/下游用）
};

[[nodiscard]] auto solve_connect(const AnchorRef& from, const AnchorRef& to,
                                 const ConstraintContext& context) -> Result<ConnectSolution>;

/// 解算 `align`：把我的轴线（Y）对准目标的轴线（目标包围盒中心 XZ）。
/// 返回我应当处于的世界平移（XZ 取目标中心，Y 保持我的当前值）。
[[nodiscard]] auto solve_align(const AnchorRef& target, float my_current_y,
                               const ConstraintContext& context) -> Result<math::Vec3>;

/// 「+Y 指向 direction」的欧拉角（度）。connect 内部用；单独暴露给测试。
[[nodiscard]] auto rotation_to_point_y(math::Vec3 direction) -> math::Vec3;

}  // namespace st::raster
