#pragma once

/// @file mesh_ops.hpp
/// @brief 网格**建模运算**：变换、合并、焊接、法线、包围盒、尺寸归一、OBJ 写出。
///
/// ## 为什么要有这一层
///
/// `Mesh` 是"顶点 + 索引 + UV"的**裸数据**，`Mesh` 的静态工厂只负责"造出一块形状"。
/// 而建模真正的工作量在**改**：把零件摆到位置、拼成一个物体、把接缝焊上、
/// 把尺寸归一到一个统一尺度、再导出给别人。
///
/// 这些运算原先散落在各处（裁云 `model3d.cpp` 里自己拼矩阵、探针里自己做 AABB），
/// 于是同一个概念有三份实现，而**错误也只会在其中一份里被发现**。抽到这里之后：
/// ① 变换的法线处理、绕序翻转这类"写错就静默变暗"的细节只有一处；
/// ② 任何后端/任何平台都能用（不碰图形 API，纯数据）。
///
/// ## 坐标系与单位约定（全库统一，别在调用点另外约定）
///
/// | 项 | 约定 |
/// |---|---|
/// | 手性 | **右手系**，Y 轴朝上，相机看向 -Z（与 `math::Mat4::look_at` 一致） |
/// | 三角形绕序 | **逆时针**（从外侧看）= 正面；与顶点法线同向 |
/// | 长度单位 | 无量纲（场景单位）；导出 glTF 时按 **米** 声明（`metersPerUnit=1`） |
/// | 角度入参 | 凡是叫 `angle` / `rotate` 的都是**弧度**；字段名带 `_degrees` 的才是度 |
/// | 颜色入参 | `math::Color` 是 **sRGB 字节**（内部解码成线性）；`math::Vec3` 视为**线性** |
///
/// ## 与 Blender 的对应关系（便于互相搬运参数）
///
/// | 本模块 | Blender |
/// |---|---|
/// | `Mesh::uv_sphere(radius, segments, rings)` | `Add ▸ UV Sphere`（Rings = `rings`） |
/// | `Mesh::ico_sphere(radius, subdivisions)` | `Add ▸ Ico Sphere`（Subdivisions） |
/// | `Mesh::cylinder(radius, depth, vertices)` | `Add ▸ Cylinder`（Depth = 高） |
/// | `resize(mesh, size)` | `Object ▸ Transform ▸ Dimensions` |
/// | `merge({...})` | `Object ▸ Join`（Ctrl+J） |
/// | `weld(mesh, eps)` | `Mesh ▸ Merge by Distance`（M） |
/// | `flat_normals` / `smooth_normals` | `Object ▸ Shade Flat` / `Shade Smooth` |
/// | `to_obj_text` / `write_gltf` | `File ▸ Export ▸ Wavefront (.obj)` / `glTF 2.0` |

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/mesh.hpp"

namespace st::raster {

/// 轴对齐包围盒（AABB）。
struct Bounds {
  math::Vec3 min{};
  math::Vec3 max{};

  /// 空盒（min > max）：没有顶点时的返回值，可安全参与 `union_with`。
  [[nodiscard]] static auto empty() noexcept -> Bounds;
  [[nodiscard]] auto is_empty() const noexcept -> bool;
  /// 尺寸（各轴长度）。空盒返回零向量。
  [[nodiscard]] auto size() const noexcept -> math::Vec3;
  /// 中心点。
  [[nodiscard]] auto center() const noexcept -> math::Vec3;
  /// 与另一点/另一盒取并。
  [[nodiscard]] auto union_with(math::Vec3 point) const noexcept -> Bounds;
  [[nodiscard]] auto union_with(const Bounds& other) const noexcept -> Bounds;
};

/// 计算包围盒（遍历顶点位置）。
[[nodiscard]] auto bounds(const Mesh& mesh) noexcept -> Bounds;

/// **按 4×4 矩阵变换网格**（模型矩阵烘焙进几何）。
///
/// ## 为什么法线不能直接乘矩阵（踩过的经典坑）
///
/// 法线是**余切向量**，非均匀缩放后必须用**逆转置矩阵**变换，否则：
/// 沿 Y 拉长 2 倍的盒子，侧面法线会倾斜、光照变成"被压扁的光"——
/// 画面看着"只是有点怪"，很难定位到是法线错了。
///
/// ## 镜像（行列式为负）怎么办
///
/// 负缩放的矩阵会**翻转三角形的朝向**，于是几何法线与变换后的顶点法线反向，
/// 背面剔除立刻把物体剔成内壁（实测就是这么"物体消失"的）。
/// 所以这里在 `determinant < 0` 时**按三角形翻转绕序**，两者重新对齐。
[[nodiscard]] auto transform(const Mesh& mesh, const math::Mat4& matrix) -> Mesh;

/// 便捷变换：平移 → 旋转（欧拉角 X→Y→Z，弧度）→ 缩放（与 Blender 的
/// `Object ▸ Transform` 同序：先缩放、再旋转、最后平移，写成矩阵是 `T·R·S`）。
[[nodiscard]] auto transform(const Mesh& mesh, math::Vec3 translate, math::Vec3 rotate_radians,
                             math::Vec3 scale) -> Mesh;

/// **合并多个网格为一个**（Blender 的 Join）。
///
/// 顶点/索引/UV 全部重映射（不是简单拼接索引——那会全部指向第一个网格）。
///
/// ## UV 的取舍（重要）
///
/// 结果只有在**每一块都有 UV** 时才有 UV；否则整体丢掉 UV（回到顶点色/材质色）。
/// 理由：留一半 UV 会让"有没有 UV"变成一个逐三角形的事实，而当前渲染管线
/// 是**整网格**判定的（`Mesh::has_uvs()`），半有半无只会得到花屏。
[[nodiscard]] auto merge(std::span<const Mesh> parts) -> Mesh;
[[nodiscard]] auto merge(const Mesh& a, const Mesh& b) -> Mesh;

/// **焊接**（Blender 的 Merge by Distance）：把「位置与法线都落在 `epsilon` 内」
/// 的顶点并成一个。
///
/// 为何法线也参与判据：只按位置合并会把**硬边**（盒子的棱）焊成软边——
/// 棱上的法线来自不同面，合并后取谁的都是错的，观感变成"圆角盒子"。
/// 位置 + 法线同判据才能既合上接缝、又保住硬边。
///
/// UV 跟随顶点一起合并（取第一次出现的值）；`epsilon <= 0` 时原样返回。
[[nodiscard]] auto weld(const Mesh& mesh, float epsilon = 1e-5f) -> Mesh;

/// 翻转朝向：三角形绕序反向 + 顶点法线取反（用于"内壁/反面"或修掉导反的模型）。
[[nodiscard]] auto flip(const Mesh& mesh) -> Mesh;

/// 法线重算模式。
enum class NormalShading : std::uint8_t {
  Smooth = 0,  ///< 按**位置**累加面法线求平均（球面/管道需要）
  Flat = 1,    ///< 每三角形独立（盒子/棱柱需要，等价于 Blender 的 Shade Flat）
};

/// 重算法线（丢弃原有法线）。
///
/// `Flat` 会**复制顶点**（每三角形 3 个独立顶点），因为硬边不可能靠共享顶点表达。
[[nodiscard]] auto recompute_normals(const Mesh& mesh, NormalShading shading) -> Mesh;

/// 把网格**按包围盒归一到指定尺寸**（Blender 的 `Dimensions`）。
///
/// 只缩放、不平移（保持原位相对关系）；某一轴尺寸为 0 时不缩放该轴
/// （避免除零——扁平平面的厚度轴就是 0）。
[[nodiscard]] auto resize(const Mesh& mesh, math::Vec3 target_size) -> Mesh;

/// 把包围盒**中心**移到给定点（默认原点）。
[[nodiscard]] auto center_at(const Mesh& mesh, math::Vec3 target = math::Vec3{}) -> Mesh;

/// 把包围盒**底面**贴到 `y = floor_y` 平面（"放到地上"）。
///
/// 为什么单独给一个函数：搭场景时"物体浮空/陷进地面"是最常见的构图错误，
/// 而它只需要一句 `sit_on_ground(mesh, 0.0f)`，不必每个调用方自己算 center/size。
[[nodiscard]] auto sit_on_ground(const Mesh& mesh, float floor_y = 0.0f) -> Mesh;

/// 网格摘要（顶点数/三角形数/包围盒/是否有 UV），**给模型看的自检信息**。
///
/// 返回 JSON 文本：`{"vertices":24,"triangles":12,"bounds":{"min":[...],"max":[...]},
/// "has_uvs":false}`。生成几何的调用方（尤其是模型）用它回答"我刚造出来的是什么"，
/// 而不是靠渲染一张图去猜。
[[nodiscard]] auto describe(const Mesh& mesh) -> std::string;

/// 导出为 **OBJ 文本**（`v` / `vn` / `vt` / `f`；Y-up，右手系）。
///
/// 与 `Mesh::load_obj` 成对：往返保几何与法线（UV 只在这一版里保留，加载器暂不读 `vt`）。
/// OBJ 的价值在于**人和模型都读得懂**，且能被 Blender / 各大引擎 / 在线查看器直接导入。
[[nodiscard]] auto to_obj_text(const Mesh& mesh, std::string_view object_name = "mesh")
    -> std::string;

/// 写出 OBJ 文件（自动建父目录）。错误：`Io`。
[[nodiscard]] auto write_obj_file(std::string_view path, const Mesh& mesh,
                                  std::string_view object_name = "mesh") -> Status;

}  // namespace st::raster
