// 生态素材兼容性探针：读真实形态的 glTF，报告**哪些信息被保留、哪些被丢了**。
//
// ## 为什么需要这个探针（而不是只写单元测试）
//
// "能不能吸收 three.js / Blender 的素材"这个问题，答案不在"能读文件"（那份
// glTF 我们的读取器早就读得动），而在**读进来之后信息还剩多少**：
// 贴图还在吗？材质参数对吗？UV 对吗？没有被支持的特性是**安静丢掉**还是**报错**？
//
// 单元测试逐项钉行为，但不回答"一份典型的生态素材整体上信噪比多少"。
// 这个探针就是那个总账：读一份素材，逐项打分，并**明确列出丢失项**。
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/raster/gltf.hpp"
#include "st/raster/mesh_ops.hpp"

namespace {

struct Finding {
  std::string item;
  bool ok{false};
  std::string note;
};

void report_line(const Finding& finding) {
  st::log::out("  [{}] {:<34} {}\n", finding.ok ? "OK" : "缺失", finding.item, finding.note);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> targets;
  for (int i = 1; i < argc; ++i) targets.emplace_back(argv[i]);
  if (targets.empty()) {
    // 默认读本仓自造的三份"生态典型形态"
    targets = {"tmp/eco/three-like.gltf", "tmp/eco/atlas.gltf", "tmp/eco/draco.gltf"};
  }

  int total_items = 0;
  int kept_items = 0;
  for (const std::string& path : targets) {
    st::log::out("\n=== {} ===\n", path);
    if (!st::fs::exists(path)) {
      st::log::out("  （文件不存在，跳过）\n");
      continue;
    }
    const auto scene = st::raster::gltf_read_file(path);
    if (!scene.has_value()) {
      st::log::out("  [错误] {}\n", scene.error().message);
      ++total_items;
      continue;
    }
    st::log::out("  网格 {} 个 · 材质 {} 个 · 节点 {} 个 · 动画 {} 段 · 骨架 {}\n",
                 scene->meshes.size(), scene->materials.size(), scene->nodes.size(),
                 scene->animations.size(),
                 scene->skeleton.has_value() ? std::to_string(scene->skeleton->joint_count())
                                             : std::string("无"));

    std::vector<Finding> findings;
    // 几何
    std::size_t triangles = 0;
    std::size_t uv_meshes = 0;
    std::size_t skinned_meshes = 0;
    for (const auto& mesh : scene->meshes) {
      triangles += mesh.indices.size() / 3U;
      if (mesh.has_uvs()) ++uv_meshes;
      if (mesh.has_skins()) ++skinned_meshes;
    }
    findings.push_back({"几何（三角形）", triangles > 0, std::format("{} 个三角形", triangles)});
    findings.push_back({"UV 坐标", uv_meshes > 0,
                        std::format("{}/{} 个网格带 UV", uv_meshes, scene->meshes.size())});
    // 材质
    bool any_color = false;
    for (const auto& material : scene->materials) {
      if (material.base_color.r != 255 || material.base_color.g != 255 || material.base_color.b != 255) {
        any_color = true;
      }
    }
    const bool has_any_texture = [&] {
      for (const auto& material : scene->materials) {
        if (material.base_color_texture >= 0) return true;
      }
      return false;
    }();
    findings.push_back({"材质 base_color（因子）", !scene->materials.empty(),
                        scene->materials.empty()
                            ? "无材质"
                            : (any_color ? "有颜色" : (has_any_texture ? "全白（由贴图供色）"
                                                                       : "全白"))});
    // 贴图：本框架的 GltfScene 目前没有贴图字段 —— 这一项必然缺失
    // 贴图：现在能从 images/textures 读出来了
    std::size_t texture_loaded = 0;
    std::size_t texture_used = 0;
    for (const auto& material : scene->materials) {
      if (material.base_color_texture >= 0) ++texture_used;
    }
    for (const auto& texture : scene->textures) {
      if (texture.is_valid()) ++texture_loaded;
    }
    findings.push_back({"材质贴图（baseColorTexture）", texture_loaded > 0 && texture_used > 0,
                        std::format("解码 {} 张，{} 个材质引用", texture_loaded, texture_used)});
    // 扩展诊断
    if (scene->extensions.has_blocking()) {
      std::string blocking;
      for (const auto& name : scene->extensions.blocking) {
        if (!blocking.empty()) blocking += ", ";
        blocking += name;
      }
      findings.push_back({"扩展（必需）", false, "阻断：" + blocking});
    } else {
      findings.push_back({"扩展（必需）", true,
                          std::format("used {} 个，忽略 {} 个", scene->extensions.used.size(),
                                      scene->extensions.ignored.size())});
    }
    // ## 骨架 / 动画：素材里**没有**不算"丢失"
    //
    // 初版把"没有骨架"记为缺失，于是任何静态模型都被扣两分——那是在惩罚素材，
    // 不是在测框架。这里改成：有就核对，没有就标 N/A（不计入总账分母的判据）。
    if (scene->skeleton.has_value() || skinned_meshes > 0) {
      findings.push_back({"骨架 / 蒙皮", scene->skeleton.has_value(),
                          std::format("{} 个关节 · {} 个蒙皮网格",
                                      scene->skeleton.has_value() ? scene->skeleton->joint_count() : 0U,
                                      skinned_meshes)});
    }
    if (!scene->animations.empty()) {
      findings.push_back({"动画", true, std::format("{} 段", scene->animations.size())});
    }

    for (const Finding& finding : findings) {
      report_line(finding);
      ++total_items;
      if (finding.ok) ++kept_items;
    }
  }

  st::log::out("\n=== 总账：{}/{} 项信息被保留（{:.0f}%）===\n", kept_items, total_items,
               total_items > 0 ? 100.0 * static_cast<double>(kept_items) /
                                     static_cast<double>(total_items)
                               : 0.0);
  return 0;
}
