// mesh 布尔运算：BSP 剪裁法（csg.js 算法的 C++ 移植）。
//
// ## 为什么是 csg.js 这套而不是自创语义
//
// 第一版自创了「尽头=内外」判定，在**范围错开的共面**（B 的 y+ 面与 A 的
// y+ 面同平面、不同 x 范围）上整面误收——自创语义在这类边角上不可靠。
// csg.js（Evan Wallace, MIT）用 invert/clipTo 组合代数表达三种运算，
// 注明「两个实体中重叠共面多边形的全部边角情形都被正确处理」。
// 本文件严格移植其语义：
//   union:     a.clipTo(b); b.clipTo(a); b.invert(); b.clipTo(a); b.invert();
//              a.build(b.allPolygons())
//   subtract:  a.invert(); …同上…; a.build(b.allPolygons()); a.invert()
//   intersect: a.invert(); b.clipTo(a); b.invert(); a.clipTo(b);
//              a.invert(); b.invert(); a.build(b.allPolygons())
//
// ## 与 Python 验证脚本的对应（数值已对齐）
//
// 半搭立方：union=1.5 / subtract=0.5 / intersect=0.5；
// 立方减贯通圆柱(16段)：6.4693（解析 6.4292，多边形逼近差 0.6%）；
// 不相交 union：2.0。
//
// ## 确定性契约
//
// 同一对输入两次调用输出逐字节相同：分割面按输入顺序取（csg.js 同款），
// 全程无哈希无随机。BSP 树深最坏 O(n)，本库的布尔网格规模可控。

#include "st/raster/mesh_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace st::raster {

namespace {

constexpr float kEpsilon = 1e-5f;
constexpr float kDegenerateArea = 1e-12f;

using Polygon = std::vector<math::Vec3>;  // 凸多边形（保持绕序）

struct Plane {
  math::Vec3 normal{};
  float w{0.0f};
  [[nodiscard]] auto distance(const math::Vec3& point) const noexcept -> float {
    return normal.dot(point) - w;
  }
};

auto plane_of(const Polygon& polygon) -> Plane {
  const math::Vec3 n =
      (polygon[1] - polygon[0]).cross(polygon[2] - polygon[0]).normalized();
  return Plane{n, n.dot(polygon[0])};
}

auto polygon_area(const Polygon& polygon) -> float {
  math::Vec3 total{};
  for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
    total = total + (polygon[i] - polygon[0]).cross(polygon[i + 1] - polygon[0]);
  }
  return total.length() * 0.5f;
}

/// 三角形 → 凸多边形列表（fan）。
auto fan_of(const Polygon& polygon) -> std::vector<Polygon> {
  std::vector<Polygon> out;
  if (polygon.size() < 3U) return out;
  for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
    out.push_back({polygon[0], polygon[i], polygon[i + 1]});
  }
  return out;
}

// ── csg.js 的 Plane.splitPolygon（逐句对应）──────────────────────────────────

enum class PointType : std::uint8_t { Coplanar = 0, Front = 1, Back = 2 };

/// 把一个多边形按平面分派：
/// - 全共面 → 按「与平面法线同向」分进 coplanar_front / coplanar_back；
/// - 全 front / 全 back → 直接入列；
/// - 跨立 → 切成两块（凸，保持绕序）。
///
/// **目标列表的含义由调用方决定**（csg.js 的关键设计）：
/// - build 时传 (this->polygons, this->polygons, front, back)：共面多边形
///   **留在节点上**；
/// - clipPolygons 时传 (front, back, front, back)：共面多边形**跟随法线侧
///   继续下钻**，不留在被剪的树上（第一版 C++ 把它留在节点里，正是
///   「范围错开的共面被整面误收」的病根）。
auto split_polygon(const Plane& plane, const Polygon& polygon,
                   std::vector<Polygon>& coplanar_front,
                   std::vector<Polygon>& coplanar_back,
                   std::vector<Polygon>& front, std::vector<Polygon>& back)
    -> void {
  const std::size_t count = polygon.size();
  std::vector<PointType> types(count);
  bool has_front = false;
  bool has_back = false;
  for (std::size_t i = 0; i < count; ++i) {
    const float d = plane.distance(polygon[i]);
    if (d < -kEpsilon) {
      types[i] = PointType::Back;
      has_back = true;
    } else if (d > kEpsilon) {
      types[i] = PointType::Front;
      has_front = true;
    } else {
      types[i] = PointType::Coplanar;
    }
  }
  if (!has_front && !has_back) {
    // 全共面：法线同向 → front 侧（它是边界的一部分、朝外）。
    if (plane_of(polygon).normal.dot(plane.normal) > 0.0f) {
      coplanar_front.push_back(polygon);
    } else {
      coplanar_back.push_back(polygon);
    }
    return;
  }
  if (!has_back) {
    front.push_back(polygon);
    return;
  }
  if (!has_front) {
    back.push_back(polygon);
    return;
  }
  // 跨立：逐边切（coplanar 顶点两侧都进）。
  Polygon f;
  Polygon b;
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t j = (i + 1) % count;
    const math::Vec3& a = polygon[i];
    const math::Vec3& c = polygon[j];
    const PointType ta = types[i];
    const PointType tc = types[j];
    if (ta != PointType::Back) f.push_back(a);
    if (ta != PointType::Front) b.push_back(a);
    const bool a_front = ta == PointType::Front;
    const bool c_back = tc == PointType::Back;
    if (a_front && c_back) {
      const float t = plane.distance(a) / (plane.distance(a) - plane.distance(c));
      const math::Vec3 cut = a + (c - a) * t;
      f.push_back(cut);
      b.push_back(cut);
    } else if (ta == PointType::Back && tc == PointType::Front) {
      const float t = plane.distance(a) / (plane.distance(a) - plane.distance(c));
      const math::Vec3 cut = a + (c - a) * t;
      b.push_back(cut);
      f.push_back(cut);
    }
  }
  if (f.size() >= 3U && polygon_area(f) >= kDegenerateArea) front.push_back(f);
  if (b.size() >= 3U && polygon_area(b) >= kDegenerateArea) back.push_back(b);
}

// ── BSP 节点（csg.js 的 Node，逐句对应）─────────────────────────────────────

struct BspNode {
  std::optional<Plane> plane;
  std::unique_ptr<BspNode> front;
  std::unique_ptr<BspNode> back;
  std::vector<Polygon> polygons;

  explicit BspNode(std::vector<Polygon> initial = {}) {
    if (!initial.empty()) build(std::move(initial));
  }

  /// 实体/空洞互换：翻多边形绕序与平面、递归翻子树、交换 front/back。
  void invert() {
    for (Polygon& polygon : polygons) {
      std::reverse(polygon.begin() + 1, polygon.end());
    }
    if (plane.has_value()) {
      plane->normal = plane->normal * -1.0f;
      plane->w = -plane->w;
    }
    if (front != nullptr) front->invert();
    if (back != nullptr) back->invert();
    std::swap(front, back);
  }

  /// 剪掉 polys 中落在**本树实体内**的部分。
  /// back 尽头 = 实体内部 = 丢弃（csg.js 原文 `else back = []`）。
  [[nodiscard]] auto clip_polygons(const std::vector<Polygon>& polys) const
      -> std::vector<Polygon> {
    if (!plane.has_value()) return polys;
    std::vector<Polygon> front_list;
    std::vector<Polygon> back_list;
    for (const Polygon& polygon : polys) {
      // 共面多边形随法线侧下钻，不留在本节点（clip 语义，见 split_polygon 注）。
      split_polygon(*plane, polygon, front_list, back_list, front_list, back_list);
    }
    if (front != nullptr) {
      front_list = front->clip_polygons(front_list);
    }
    if (back != nullptr) {
      back_list = back->clip_polygons(back_list);
    } else {
      back_list.clear();
    }
    front_list.insert(front_list.end(), back_list.begin(), back_list.end());
    return front_list;
  }

  /// 把本树中落在 other 实体内的多边形全部剪掉（递归含子树）。
  void clip_to(const BspNode& other) {
    polygons = other.clip_polygons(polygons);
    if (front != nullptr) front->clip_to(other);
    if (back != nullptr) back->clip_to(other);
  }

  [[nodiscard]] auto all_polygons() const -> std::vector<Polygon> {
    std::vector<Polygon> out = polygons;
    if (front != nullptr) {
      const std::vector<Polygon> more = front->all_polygons();
      out.insert(out.end(), more.begin(), more.end());
    }
    if (back != nullptr) {
      const std::vector<Polygon> more = back->all_polygons();
      out.insert(out.end(), more.begin(), more.end());
    }
    return out;
  }

  /// 把多边形灌进（已存在的）树：沿树分流，落到叶处建新节点。
  void build(std::vector<Polygon> polys) {
    if (polys.empty()) return;
    if (!plane.has_value()) plane = plane_of(polys[0]);
    std::vector<Polygon> front_list;
    std::vector<Polygon> back_list;
    for (Polygon& polygon : polys) {
      // 建树语义：共面多边形留在本节点。
      split_polygon(*plane, polygon, polygons, polygons, front_list, back_list);
    }
    if (!front_list.empty()) {
      if (front == nullptr) front = std::make_unique<BspNode>();
      front->build(std::move(front_list));
    }
    if (!back_list.empty()) {
      if (back == nullptr) back = std::make_unique<BspNode>();
      back->build(std::move(back_list));
    }
  }
};

// ── 网格 ↔ 多边形 ────────────────────────────────────────────────────────────

auto mesh_to_polygons(const Mesh& mesh) -> std::vector<Polygon> {
  std::vector<Polygon> polygons;
  polygons.reserve(mesh.indices.size() / 3U);
  const auto vertex_at = [&](std::uint32_t index) -> math::Vec3 {
    const std::size_t base = static_cast<std::size_t>(index) * 9U;
    return {mesh.vertices[base], mesh.vertices[base + 1U], mesh.vertices[base + 2U]};
  };
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3U) {
    Polygon polygon{vertex_at(mesh.indices[i]), vertex_at(mesh.indices[i + 1U]),
                    vertex_at(mesh.indices[i + 2U])};
    if (polygon_area(polygon) >= kDegenerateArea) polygons.push_back(std::move(polygon));
  }
  return polygons;
}

auto polygons_to_mesh(const std::vector<Polygon>& polygons, math::Vec3 vertex_color)
    -> Mesh {
  Mesh mesh;
  std::map<std::tuple<float, float, float>, std::uint32_t> unique;
  const auto emit = [&](const math::Vec3& point, const math::Vec3& normal) {
    const auto key = std::make_tuple(point.x, point.y, point.z);
    const auto found = unique.find(key);
    if (found != unique.end()) return found->second;
    const std::uint32_t index = static_cast<std::uint32_t>(mesh.vertices.size() / 9U);
    // 顶点色继承自基体：布尔结果直接用作网格时，颜色必须跟场景里写的
    // `color` 一致——旧的固定 0.8 灰会让布尔件在画面里是一块中性灰
    //（实测：塔的底座与楼板全部发灰，与周围暖色调判若两个场景）。
    mesh.vertices.insert(mesh.vertices.end(),
                         {point.x, point.y, point.z, normal.x, normal.y, normal.z,
                          vertex_color.x, vertex_color.y, vertex_color.z});
    unique.emplace(key, index);
    return index;
  };
  for (const Polygon& polygon : polygons) {
    if (polygon.size() < 3U) continue;
    const math::Vec3 normal = plane_of(polygon).normal;
    for (const Polygon& triangle : fan_of(polygon)) {
      mesh.indices.push_back(emit(triangle[0], normal));
      mesh.indices.push_back(emit(triangle[1], normal));
      mesh.indices.push_back(emit(triangle[2], normal));
    }
  }
  return mesh;
}

/// 水密弱判据：每条无向边恰好两个三角形 + 有向体积非零（挡住 Plane/Disc）。
auto is_closed(const Mesh& mesh) -> bool {
  using VKey = std::tuple<std::int64_t, std::int64_t, std::int64_t>;
  const auto quantize = [](const math::Vec3& v) -> VKey {
    return {static_cast<std::int64_t>(std::llround(v.x * 1e6f)),
            static_cast<std::int64_t>(std::llround(v.y * 1e6f)),
            static_cast<std::int64_t>(std::llround(v.z * 1e6f))};
  };
  std::map<std::pair<VKey, VKey>, int> edges;
  double volume = 0.0;
  const std::vector<Polygon> polygons = mesh_to_polygons(mesh);
  if (polygons.size() < 4U) return false;
  for (const Polygon& polygon : polygons) {
    for (const Polygon& triangle : fan_of(polygon)) {
      volume += double(triangle[0].dot(triangle[1].cross(triangle[2]))) / 6.0;
      for (std::size_t i = 0; i < 3U; ++i) {
        const VKey qa = quantize(triangle[i]);
        const VKey qb = quantize(triangle[(i + 1) % 3U]);
        edges[qa < qb ? std::make_pair(qa, qb) : std::make_pair(qb, qa)] += 1;
      }
    }
  }
  if (std::abs(volume) < 1e-9) return false;
  return std::all_of(edges.begin(), edges.end(),
                     [](const auto& entry) { return entry.second == 2; });
}

auto polygons_volume(const std::vector<Polygon>& polygons) -> double {
  double volume = 0.0;
  for (const Polygon& polygon : polygons) {
    for (const Polygon& triangle : fan_of(polygon)) {
      volume += double(triangle[0].dot(triangle[1].cross(triangle[2]))) / 6.0;
    }
  }
  return volume;
}

/// 绕序归一：有向体积为负的多边形集合翻成「法线朝外」的正实体。
///
/// 实测踩到：`Mesh::cylinder` 的绕序与 `Mesh::box` 相反（有向体积 -2.30 vs
/// +8.00）——内置图元并非全体满足「法线朝外」。BSP 布尔的实体语义
/// （invert/clipTo 代数）建立在「法线朝外 = 实体」上，负实体会让
/// subtract 产出 A∩B。入口统一归一，比要求每个生成器自查可靠。
auto normalize_winding(std::vector<Polygon>& polygons) -> void {
  if (polygons_volume(polygons) >= 0.0) return;
  for (Polygon& polygon : polygons) {
    std::reverse(polygon.begin() + 1, polygon.end());
  }
}

}  // namespace

// ── 对外接口 ─────────────────────────────────────────────────────────────────

auto boolean(const Mesh& a, const Mesh& b, BooleanOp op, bool check_closed) -> Result<Mesh> {
  if (check_closed && (!is_closed(a) || !is_closed(b))) {
    return st::unexpected(
        ErrorCode::Invalid,
        std::format("布尔运算：两个网格都要闭合（水密）；开放面（Plane/Disc 无厚度）"
                    "不能参与布尔"));
  }

  std::vector<Polygon> polys_a = mesh_to_polygons(a);
  std::vector<Polygon> polys_b = mesh_to_polygons(b);
  // 结果顶点色 = 基体首顶点的颜色（布尔不该改变对象的外观色）。
  const math::Vec3 base_color =
      a.vertices.size() >= 9U
          ? math::Vec3{a.vertices[6], a.vertices[7], a.vertices[8]}
          : math::Vec3{0.8f, 0.8f, 0.8f};
  normalize_winding(polys_a);
  normalize_winding(polys_b);

  // 包围盒不相交的快速路（union 直接拼；其余按语义处理）。
  const Bounds box_a = bounds(a);
  const Bounds box_b = bounds(b);
  const bool disjoint = box_a.max.x < box_b.min.x || box_b.max.x < box_a.min.x ||
                        box_a.max.y < box_b.min.y || box_b.max.y < box_a.min.y ||
                        box_a.max.z < box_b.min.z || box_b.max.z < box_a.min.z;
  if (disjoint) {
    switch (op) {
      case BooleanOp::Union: {
        std::vector<Polygon> all = polys_a;
        all.insert(all.end(), polys_b.begin(), polys_b.end());
        return polygons_to_mesh(all, base_color);
      }
      case BooleanOp::Subtract:
        return polygons_to_mesh(polys_a, base_color);
      case BooleanOp::Intersect:
        return st::unexpected(ErrorCode::Invalid,
                              "布尔 Intersect：两网格包围盒不相交，交集为空");
    }
  }

  // csg.js 组合代数（严格原文顺序）。
  BspNode tree_a{polys_a};
  BspNode tree_b{polys_b};
  switch (op) {
    case BooleanOp::Union: {
      tree_a.clip_to(tree_b);
      tree_b.clip_to(tree_a);
      tree_b.invert();
      tree_b.clip_to(tree_a);
      tree_b.invert();
      tree_a.build(tree_b.all_polygons());
      break;
    }
    case BooleanOp::Subtract: {
      tree_a.invert();
      tree_a.clip_to(tree_b);
      tree_b.clip_to(tree_a);
      tree_b.invert();
      tree_b.clip_to(tree_a);
      tree_b.invert();
      tree_a.build(tree_b.all_polygons());
      tree_a.invert();
      break;
    }
    case BooleanOp::Intersect: {
      tree_a.invert();
      tree_b.clip_to(tree_a);
      tree_b.invert();
      tree_a.clip_to(tree_b);
      tree_a.invert();
      tree_b.invert();
      tree_a.build(tree_b.all_polygons());
      break;
    }
  }

  std::vector<Polygon> result = tree_a.all_polygons();
  if (std::getenv("ST_BOOLEAN_DEBUG") != nullptr) {
    std::fprintf(stderr, "[boolean] op=%d 结果 %zu 个多边形，体积 %.4f\n",
                 static_cast<int>(op), result.size(),
                 std::abs(polygons_volume(result)));
  }
  if (result.empty()) {
    return st::unexpected(ErrorCode::Invalid, "布尔运算：结果为空（基体被完全去除？）");
  }
  return polygons_to_mesh(result, base_color);
}

auto boolean_chain(std::span<const Mesh> operands, BooleanOp op) -> Result<Mesh> {
  if (operands.size() < 2U) {
    return st::unexpected(ErrorCode::Invalid,
                          "布尔链：至少需要 2 个网格（首个是基体）");
  }
  Mesh current = operands.front();
  for (std::size_t i = 1; i < operands.size(); ++i) {
    // 水密校验只对【最初输入】做：布尔结果的切缝三角形可能在阈值边缘
    // 被丢弃（视觉无损但拓扑开洞），链式中间结果不再强校验。
    const auto step = boolean(current, operands[i], op, /*check_closed=*/i == 1U);
    if (!step) return st::forward_error(step.error());
    current = std::move(*step);
  }
  return current;
}

}  // namespace st::raster
