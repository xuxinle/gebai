/// USD（Universal Scene Description）导出：把裁云的三维内容交给下游三维管线。
///
/// ## 只导什么
///
/// **几何 + 材质 + 变换 + 相机 + 光照**。不导：
/// - 意图与历史（那是创作过程，不是场景内容）；
/// - 动画/时间轴（裁云的动效由 JS 编排层逐帧求值，不是关键帧曲线——
///   导出的静态场景是该镜的**落定态**，动画要在下游重建）；
/// - 裁云特有的视觉效果（景深/粒子/光晕/光扫）。它们不是三维内容，
///   是画面处理；USD 里没有对应概念，硬塞只会产生"看起来导出了其实丢了"。
///
/// ## 为什么不引 USD SDK
///
/// 计划里明写了"不在 P1 引入 USD 依赖"——那会把编译复杂度提前引爆。
/// 本层输出的是 **`.usda`（ASCII 文本）**：USD 官方支持的格式，
/// 人能直接读、能进 git diff、能被 `usdcat` 转成 `.usdc`。
///
/// 代价（必须写明）：
/// - 不生成 `.usdc` 二进制（下游要的话跑一次 `usdcat --out x.usdc x.usda`）；
/// - 不做 USD 的层合成（sublayer/reference/variant）——只出一个展平的层；
/// - 材质用 `UsdPreviewSurface`（USD 的通用预览材质），不是 Marmoset/Arnold 的
///   专有 BRDF。参数映射是**近似**的，见下方逐项说明。
///
/// ## 坐标系：右手 Y-up
///
/// 霜天的三维是 Y-up 右手系（与 USD 默认一致），所以**不做轴变换**。
/// 这一条要写死在文档里：一旦有人以为需要转 Z-up，整个场景会倒。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/raster/scene3d_script.hpp"

namespace caiyun {

using st::Result;
using st::Status;

/// 导出的一口（一份 `model_script` 的作用域）对应的 USD 场景。
struct UsdExportOptions {
  /// 目标 `.usda` 文件路径。
  std::string output_path{};
  /// 根 prim 名（USD 里习惯是大驼峰，如 `World` / `Scene`）。
  std::string root_name{"World"};
  /// 是否导出相机（默认导：下游要知道从哪看）。
  bool include_camera{true};
  /// 是否导出光照（默认导）。
  bool include_lighting{true};
  /// 每个 Prim 的材质名（同一个场景里共用一份材质，避免几十份重复定义）。
  std::string material_name{"PreviewMaterial"};
};

/// 把一份 `SceneSpec` 导出成 `.usda` 文本。
///
/// 失败：`obj:` 网格加载不出来（此时**整份导出失败**而不是跳过该对象——
/// 静默少一个物体是最难发现的导出缺陷）。
[[nodiscard]] auto scene_spec_to_usda(const st::raster::SceneSpec& spec,
                                      const UsdExportOptions& options) -> Result<std::string>;

/// 直接从 JSON 文本导出（命令行入口用）。
[[nodiscard]] auto export_usda_from_script_text(std::string_view json_text,
                                                const UsdExportOptions& options)
    -> Result<std::string>;

}  // namespace caiyun
