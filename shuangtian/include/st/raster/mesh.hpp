#pragma once

/// 三维网格：**平台无关**的几何数据与生成/加载。
///
/// 为什么它不在 `gl.hpp` / `gpu.hpp` 里：网格是纯数据 + 纯解析（顶点、索引、OBJ 文本），
/// 与"用哪个图形 API 画"毫无关系。它原先住在 `raster::gl` 里，导致：
/// ① 换后端（GL → D3D11）就得把整套解析搬一遍；
/// ② 非 Windows 平台上这份能力**整个消失**（`gl` 是 Windows 专用的）；
/// ③ OBJ 加载不得不依赖 GL 模块才能编译。
/// 抽到这里之后，任何后端、任何平台都能用它——几何与传输方式彻底分开。
///
/// 顶点的内存布局在 `kVertexFloats` 处定义一次，所有后端共用：
/// 布局是"后端上传缓冲"与"生成器产出"之间的契约，写两遍必然漂移。

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/color.hpp"
#include "st/math/matrix.hpp"

namespace st::raster {

/// 网格：交错顶点（位置 3 + 法线 3 + 顶点色 3）。索引用 32 位。
///
/// 为什么带顶点色：光照之外还需要"材质感"（每个面不同色才能看出朝向），
/// 而引入贴图会拉进纹理管线与资源管理——当前范围不需要。
struct Mesh {
  /// 每顶点分量数（位置 3 + 法线 3 + 颜色 3）。后端上传缓冲按它算 stride。
  static constexpr std::size_t kVertexFloats = 9U;

  std::vector<float> vertices{};   ///< 每顶点 `kVertexFloats` 个 float
  std::vector<std::uint32_t> indices{};

  [[nodiscard]] auto vertex_count() const noexcept -> std::size_t {
    return vertices.size() / kVertexFloats;
  }
  [[nodiscard]] auto is_empty() const noexcept -> bool {
    return vertices.empty() || indices.empty();
  }

  /// 第 `index` 个顶点的 9 个分量（位置 3 + 法线 3 + 颜色 3）。
  /// 只读访问供测试与导出用；写一律走 `vertices`（避免两套修改路径）。
  [[nodiscard]] auto operator[](std::size_t index) const -> std::span<const float> {
    const std::size_t offset = index * kVertexFloats;
    if (offset + kVertexFloats > vertices.size()) return {};
    return std::span<const float>(vertices.data() + offset, kVertexFloats);
  }

  /// 立方体（边长 `size`，各面不同色：一眼能看出朝向）。
  [[nodiscard]] static auto cube(float size = 1.0f) -> Mesh;
  /// 球（经纬网格，`segments` 越大越圆）。
  [[nodiscard]] static auto sphere(float radius = 0.6f, int segments = 24) -> Mesh;
  /// 长方体（三轴独立边长）。
  [[nodiscard]] static auto box(float x, float y, float z) -> Mesh;

  /// 从 **OBJ 文本**加载网格（Wavefront OBJ，只取几何：`v` / `vn` / `f`）。
  ///
  /// 支持：`f` 的四种写法（`v`、`v/vt`、`v//vn`、`v/vt/vn`）、**负索引**（相对引用）、
  /// 多边形面（扇形三角化）、缺法线时**按面计算**。
  /// 不支持（如实说明）：`vt` 纹理坐标、材质库（`mtllib`/`usemtl`）、自由曲面、多对象分组
  /// —— 取到就跳过，而不是猜。这不是"完整的 OBJ 实现"，是"够画出模型的子集"。
  ///
  /// @return 失败：空中/无有效面（`Invalid`）。**单个坏面只跳过**，不让整个文件失败。
  [[nodiscard]] static auto load_obj(std::string_view text, math::Color base_color)
      -> Result<Mesh>;

  /// 从文件加载（路径按 UTF-8 处理，经 `st::fs`——Windows 下中文路径才不会坏）。
  [[nodiscard]] static auto load_obj_file(std::string_view path, math::Color base_color)
      -> Result<Mesh>;
};

/// 把单个顶点写入交错缓冲（生成器与 OBJ 加载共用，保证布局只有一处定义）。
void push_vertex(std::vector<float>& out, math::Vec3 position, math::Vec3 normal,
                 math::Vec3 color);

}  // namespace st::raster
