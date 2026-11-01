/// 验证「2D 骨架 → glTF 骨骼」映射是否正确 —— 只打数据，不画图。
///
/// ## 为什么需要它
///
/// 现在的症状是"角色躺在地上、比例被拉长"。可能的原因有四种，
/// 从画面上**无法区分**：
/// 1. 绑定方向假设错了（我以为腿朝 -Y，实际不是）；
/// 2. 绑定世界矩阵取错了（节点 transform 不是我想的那个）；
/// 3. 旋转叠加顺序错了；
/// 4. 单位换算错了（像素/米）。
///
/// 本探针把它们分开：在**绑定姿势**下（2D 姿态 = 静止姿态），
/// 逐关节打印"目标位置"与"绑定位置"，两者应当几乎相同。
/// 若差得远，就是第 1 或第 4 个原因。
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "st/core/entry.hpp"
#include "st/math/geometry.hpp"
#include "st/math/matrix.hpp"
#include "st/raster/gltf.hpp"

namespace {

using st::math::Mat4;
using st::math::Vec3;

auto run(int, char**) -> int {
  auto loaded = st::raster::gltf_read_file("assets/models/gltf/CesiumMan.glb");
  if (!loaded.has_value()) {
    std::fprintf(stderr, "读取失败\n");
    return 1;
  }
  const auto& scene = *loaded;

  std::printf("=== glTF 节点（名 → 世界矩阵的平移与基向量）===\n");
  for (const auto& node : scene.nodes) {
    if (node.name.empty()) continue;
    const Mat4& m = node.transform;
    // 列主序：m[0..2] = X 基向量，m[4..6] = Y 基，m[8..10] = Z 基，m[12..14] = 平移
    std::printf("%-32s T=(%6.3f,%6.3f,%6.3f)  X=(%5.2f,%5.2f,%5.2f)  Y=(%5.2f,%5.2f,%5.2f)\n",
                node.name.c_str(), static_cast<double>(m.m[12]), static_cast<double>(m.m[13]),
                static_cast<double>(m.m[14]), static_cast<double>(m.m[0]),
                static_cast<double>(m.m[1]), static_cast<double>(m.m[2]),
                static_cast<double>(m.m[4]), static_cast<double>(m.m[5]),
                static_cast<double>(m.m[6]));
  }

  std::printf("\n=== Z_UP / Armature 节点的完整基向量（判断朝上轴）===\n");
  for (const auto& node : scene.nodes) {
    if (node.name != "Z_UP" && node.name != "Armature" && node.name != "Cesium_Man") continue;
    const Mat4& m = node.transform;
    std::printf("%-12s X=(%5.2f,%5.2f,%5.2f) Y=(%5.2f,%5.2f,%5.2f) Z=(%5.2f,%5.2f,%5.2f)\n",
                node.name.c_str(), m.m[0], m.m[1], m.m[2], m.m[4], m.m[5], m.m[6], m.m[8], m.m[9],
                m.m[10]);
  }

  std::printf("\n=== 骨骼的\"指向子关节\"方向（这是绑定方向）===\n");
  // 按节点名建父子关系：CesiumMan 的层级从上面输出可读出来，
  // 这里用"每个关节在绑定姿势下的位置 → 其子关节位置"直接算。
  std::map<std::string, Vec3> pos;
  for (const auto& node : scene.nodes) {
    if (node.name.empty()) continue;
    pos[node.name] = Vec3{node.transform.m[12], node.transform.m[13], node.transform.m[14]};
  }
  // 硬编码层级（来自上面输出的观察），只用于打印验证。
  const std::pair<const char*, const char*> kChain[] = {
      {"Skeleton_torso_joint_1", "Skeleton_torso_joint_2"},
      {"Skeleton_torso_joint_2", "torso_joint_3"},
      {"torso_joint_3", "Skeleton_neck_joint_1"},
      {"Skeleton_neck_joint_1", "Skeleton_neck_joint_2"},
      {"torso_joint_3", "Skeleton_arm_joint_R"},
      {"Skeleton_arm_joint_R", "Skeleton_arm_joint_R__2_"},
      {"Skeleton_arm_joint_R__2_", "Skeleton_arm_joint_R__3_"},
      {"torso_joint_3", "Skeleton_arm_joint_L__4_"},
      {"Skeleton_arm_joint_L__4_", "Skeleton_arm_joint_L__3_"},
      {"Skeleton_arm_joint_L__3_", "Skeleton_arm_joint_L__2_"},
      {"Skeleton_torso_joint_1", "leg_joint_R_1"},
      {"leg_joint_R_1", "leg_joint_R_2"},
      {"leg_joint_R_2", "leg_joint_R_3"},
      {"leg_joint_R_3", "leg_joint_R_5"},
      {"Skeleton_torso_joint_1", "leg_joint_L_1"},
      {"leg_joint_L_1", "leg_joint_L_2"},
      {"leg_joint_L_2", "leg_joint_L_3"},
      {"leg_joint_L_3", "leg_joint_L_5"},
  };
  for (const auto& [parent, child] : kChain) {
    if (pos.count(parent) == 0 || pos.count(child) == 0) continue;
    const Vec3 p = pos[parent];
    const Vec3 c = pos[child];
    const Vec3 d{c.x - p.x, c.y - p.y, c.z - p.z};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    std::printf("%-30s → %-30s  d=(%6.3f,%6.3f,%6.3f) |d|=%.3f\n", parent, child,
                static_cast<double>(d.x), static_cast<double>(d.y), static_cast<double>(d.z),
                static_cast<double>(len));
  }
  return 0;
}

}  // namespace

ST_MAIN(run)
