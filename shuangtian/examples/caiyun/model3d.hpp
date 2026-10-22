#pragma once

/// 裁云（Nimbus）· **三维图层**：把真实三维几何画进画面的一块矩形。
///
/// ## 为什么这一层存在（而不只是「多一个图层类型」）
///
/// 二维图元最终都是「往画布写色块」，参数少、无状态。三维不同，它必须回答
/// 三件二维不存在的事：**相机在哪、光从哪来、模型怎么转**。而且它背后是一个
/// 有状态、有显存资源的子系统（深度缓冲、顶点缓冲、着色器管线）。
///
/// 所以这里的职责是**翻译**：把脚本里的 `Layer` 字段（角度/距离/转速）
/// 翻译成 `raster::Scene3D` 的调用序列，并把结果合成进画布。
///
/// ## 渲染走框架的两条腿，本层不判断
///
/// `raster::Scene3D` 是平台中立接口：有显卡走 D3D11、无显卡自动落软件光栅器。
/// 本层**不写任何 if (有显卡)**——那会把降级决策散进应用代码，而它应该
/// 只在一个地方（框架的 `create`）。实测差距很大（1080p 1100 面：
/// GPU 光栅 0.06ms vs 软件 62ms），但那正是框架该管的。
///
/// ## 为什么用线程局部对象池
///
/// 一个三维场景持有颜色目标 + 深度缓冲 + 网格缓冲。1080p 下深度缓冲是 8MB，
/// 颜色目标 8MB——每帧重建/销毁会是“每帧几次大分配 + 显存重新提交”。
/// 而裁云导出是**逐帧顺序渲染**（每线程一条），同一尺寸的场景完全可以复用。
/// 所以按「尺寸 + 后端」缓存，换尺寸才重建（与 `MonitorView::frame_` 同一套思路）。
///
/// ## 诚实边界
///
/// · **没有贴图/UV**：`Mesh` 只有「位置 + 法线 + 顶点色」。所以模型是**纯色 + 渐变**，
///   做不了“贴一张产品 logo 到盒子正面”——那需要 UV 与纹理管线（框架当前没有）。
/// · **没有阴影/反射/环境贴图**：光照是 Lambert + 环境 + 边缘光。
/// · **没有三维文字**：文字仍由渲染器画二维（可读、可控），不参与透视。
/// · **OBJ 只取几何**：材质库（mtllib/usemtl）、纹理坐标一律跳过。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "layer.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/mesh.hpp"
#include "st/raster/scene3d.hpp"

namespace caiyun {

/// 一次三维图层渲染的「这一刻的状态」（由脚本字段与时间求出来）。
///
/// 单独成结构是为了**可测**：相机/光照/旋转的求值是纯函数（只依赖 layer 与 time），
/// 与“真的去画”解耦。于是单测可以直接断言「时间推进后角度变大」「相机半径
/// 从 from 走到 to」，而不需要截图比对。
struct Model3dPose {
  /// 模型自转（弧度，绕 Y）
  float yaw{0.0f};
  /// 模型固定倾斜（弧度，绕 X）
  float tilt{0.0f};
  /// 竖直浮动位移（模型单位）
  float bob_y{0.0f};
  /// 相机位置（世界坐标，模型中心在原点）
  st::math::Vec3 eye{};
  /// 相机看向的目标点
  st::math::Vec3 target{};
  /// 视野角（度）
  float fov_y_degrees{38.0f};
  /// 主光方向（**指向光源**，已归一化）
  st::math::Vec3 light_direction{};
  /// 环境项与边缘光强度（直接来自脚本字段，夹取过）
  float ambient{0.35f};
  float rim{0.25f};
};

/// 按图层字段与场景内时间求相机/光照/旋转状态（纯函数，可单测）。
///
/// `local_time` 是**场景内**时间（秒）；`scene_duration` 用于把推拉进度归一
/// （`cam_dolly_from → to` 占满整段）。
[[nodiscard]] auto evaluate_model3d(const Layer& layer, float local_time, float scene_duration)
    -> Model3dPose;

/// 按 `model_shape` 造网格（带缓存）。
///
/// 缓存理由：球/环的顶点生成是 O(段数²)，每帧重算在导出时会白白吃掉 CPU
/// （实测 48 段球约 2300 顶点，单次 ~40µs，300 帧 × 多图层就上来了）。
/// 缓存的键是「形状串 + 颜色 + 细分」——三者任一变化才重建。
///
/// 失败（未知形状 / OBJ 读不到）返回空网格，并**不抛**：素材缺失是常态，
/// 渲染侧画占位框而不是让整帧失败（与 `image` 图层的处理一致）。
[[nodiscard]] auto mesh_for_layer(const Layer& layer, std::string_view asset_basis)
    -> std::shared_ptr<const st::raster::Mesh>;

/// 把三维图层画到 `canvas` 的 `box` 矩形里（逻辑坐标，物理像素）。
///
/// `box` 已经是**物理像素**（调用方用 `Painter::rect` 换算过）——本函数不做
/// 设计坐标缩放，因为它同时被预览与导出调用，两者的 scale 不同。
///
/// `alpha` 是整体不透明度（入场动画用），0 直接跳过。
void paint_model3d(st::raster::Canvas& canvas, const Layer& layer, st::math::Rect box,
                   float local_time, float scene_duration, float alpha,
                   std::string_view asset_basis);

/// 把一个**三维组**画进画布：组内所有 `model_group` 相同的图层共用一个场景。
///
/// ## 为何必须共享一个场景（这是“交互”的前提）
///
/// 桌面块与图标块必须是**同一个世界里的两个物体**：同一相机 → 同一透视；
/// 同一坐标系 → 图标“立在桌面上”是个几何事实而不是观感凑合。
/// 各自独立场景时，它们只是两张互不相干的图，图标永远不可能真的踩在块面上。
///
/// ## 坐标语义（组内）
///
/// · `head`（组内首个图层）的矩形 `[x,y,w,h]` = **画面框**（也决定场景分辨率）。
/// · 后续成员的 `x/y` 被解释为**在画面框内的相对位置**（设计像素），`w/h` 不用。
/// · 成员的世界位置由 `x/y` 映射：X 从 `-1..1`、Z 从 `-1..1`（以场景中心为原点）。
/// · `model_scale` 是**世界单位里的尺寸**（1.0 = 基准尺寸），所以改它 = 改物体大小。
/// · `cam_*` / `light_*` 取 **head 的**（一个世界只有一台相机、一个主光）。
///
/// 这样写脚本时的心智模型很直：“桌面铺满整块，图标放在它右半边、抬高一点”。
void paint_model3d_group(st::raster::Canvas& canvas, const Layer& head,
                         const std::vector<const Layer*>& members, float local_time,
                         float scene_duration, std::string_view asset_basis,
                         const LayerState& head_state, float canvas_w, float canvas_h,
                         float design_w, float design_h);

}  // namespace caiyun
