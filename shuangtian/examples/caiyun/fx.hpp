#pragma once

/// 裁云（Nimbus）· **影像特效层**：让画面从"信息展示"变成"发布会质感"。
///
/// ## 这一层解决什么
///
/// 前面的版式系统（`brand.hpp`）解决的是"有结构"；但发布会/预告片的观感来自
/// **摄影语言**：景深、光晕、粒子、运镜。这三样在纯几何绘制里完全没有——
/// 于是画面即使排版正确，读起来仍然像"PPT 而不是片子"。
///
/// ## 三条设计约束（都来自实测/成本考虑）
///
/// 1. **确定性**：同一时刻渲染两次必须逐像素相同（全仓的 e2e 判据）。
///    所以粒子不用随机数发生器，而是**哈希函数**（`hash01`）——同一个
///    (index, frame) 永远得到同一个值，不需要保存状态，也不需要随机引擎。
/// 2. **可关闭**：每个特效应有独立的强度参数，0 = 完全关闭且**零开销**
///    （导出时如果不需要可以关掉；也方便 e2e 做 A/B）。
/// 3. **不模糊也要有"失焦感"**：真实景深需要整幅高斯模糊（本框架没有整画布模糊
///    原语，`blur_mask` 只作用于遮罩）。这里用**降采样→双线性放大**近似：
///    一次 1/4 降采样就等于低通滤波，视觉上接近轻度模糊而成本极低。
///
/// ## 边界
///
/// 只画**氛围**（光、尘、雾、虚化），不画主体。主体（文字/图形）仍由
/// `motion.cpp` 的场景模板负责——特效应始终在主体**之下**或**之上**按层叠加，
/// 不能替主体做表达。

#include <cstdint>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"

namespace caiyun {

using st::math::Color;

/// 确定性哈希 → [0,1)。同一个 (a,b) 永远同一个值。
///
/// 为什么不用 `std::mt19937`：那需要保存状态，而渲染是**纯函数**（同一时刻两次
/// 渲染必须相同）。哈希天然无状态，且可以按"第 i 个粒子的第 j 个属性"独立取值，
/// 不必按顺序生成——这让粒子可以任意裁减而不改变其余粒子的外观。
[[nodiscard]] auto hash01(std::uint32_t a, std::uint32_t b) -> float;

/// 景深：把画面做一次"廉价失焦"（降采样 + 双线性放大）。
///
/// `strength` 0~1：0 = 不处理（零开销），1 = 最大虚化（约等于 1/6 降采样）。
/// 返回处理后的新画布（原画布不改）。
[[nodiscard]] auto depth_of_field(const st::raster::Canvas& source, float strength)
    -> st::raster::Canvas;

/// 粒子场参数（光尘 / 星点 / 浮尘）。
struct ParticleField {
  int count{70};            ///< 粒子数（按画面面积自动缩放）
  float size_min{1.2f};     ///< 最小直径（物理像素）
  float size_max{4.8f};     ///< 最大直径
  /// 景深：`0` = 全部清晰、`1` = 大小粒子的虚化差最大。
  ///
  /// 真实做法是"远的粒子更糊"，而这里用**尺寸 + 透明度**双通道表达：
  /// 小且淡 = 远（看起来虚），大且亮 = 近。比真模糊便宜一个数量级，
  /// 观感上足够（人眼判断景深靠的是虚化与亮度差，不是逐像素模糊度）。
  float depth_factor{0.85f};
  float alpha{0.5f};        ///< 整体透明度
  /// 横向漂移速度（像素/秒，设计坐标）：让"尘"缓慢流动而不是静止贴图。
  float drift_x{3.0f};
  float drift_y{-1.6f};
  Color color{200, 220, 255, 255};
};

/// 画粒子场（在给定画布上叠加）。
///
/// `time` 用于漂移与闪烁；同一 `time` 渲染两次结果完全相同。
void paint_particles(st::raster::Canvas& target, const ParticleField& field, double time,
                     float device_scale);

/// 光晕参数（镜头光晕 / 体积光 / 屏幕光扫）。
struct GlowSpec {
  float cx{0.5f};           ///< 光源横向位置（0~1 画面比例）
  float cy{0.35f};          ///< 光源纵向位置
  float radius{0.42f};      ///< 光晕半径（相对画面**短边**的比例）
  float intensity{0.55f};   ///< 中心强度（0~1）
  Color color{120, 180, 255, 255};
  /// 光晕的"扁平度"（1 = 正圆；>1 横向拉伸，像镜头光斑）。
  float stretch{1.0f};
};

/// 画径向光晕（多层同心圆近似径向渐变）。
///
/// 为什么不用真径向渐变：本框架的 `Gradient::linear` 是竖向/线性快速路径，
/// 径向要逐像素采样（1080p 一次约 25ns/px ≈ 50ms/帧，导出时会拖垮）。
/// 用 12~16 层同心圆叠出来的径向感在观感上与真径向几乎无差，成本低一到两个数量级。
void paint_glow(st::raster::Canvas& target, const GlowSpec& spec, float device_scale,
                float alpha_mul = 1.0f);

/// 横向光扫（一道亮带扫过画面，发布会转场/强调常用）。
///
/// `progress` 0~1：光带从左扫到右；超出范围时不画（零开销）。
void paint_light_sweep(st::raster::Canvas& target, float progress, float width_ratio,
                       Color color, float intensity, float device_scale);

/// 运镜参数（摄影机运动）。
///
/// ## 为什么镜头运动会显著改变观感
///
/// 静态构图的画面读起来像"展示板"；同样的内容加上**缓慢而确定的镜头运动**，
/// 就从"图"变成"镜头拍的"。这不是特效，是摄影语法。
///
/// ## 三条实拍经验（直接用，别自己调）
///
/// 1. **推（zoom_in）比拉更常用**：观众注意力随时间收拢；反过来用（拉）适合收尾。
/// 2. **幅度小、时长长**：整段推 4~8% 就够；推到 20% 会像"缩放动画"，很廉价。
/// 3. **旋转必须极小**：0.3~0.8 度能"让画面活"，超过 2 度立刻让人晕。
struct CameraMove {
  float zoom_from{1.0f};    ///< 起点缩放（1 = 原始）
  float zoom_to{1.0f};      ///< 终点缩放
  float rotate_deg{0.0f};   ///< 整段旋转总量（度，建议 <= 1.0）
  float pan_x{0.0f};        ///< 整段横向位移（相对画面宽度的比例）
  float pan_y{0.0f};        ///< 整段纵向位移
  /// 冲击震动：入场/转场瞬间的衰减抖动（幅度按画面短边比例）。
  /// 做成"正弦 × 指数衰减"——恒定抖动会让画面持续模糊，观感差。
  float shake{0.0f};
  float shake_decay{6.0f};  ///< 衰减速率（1/秒）
  float shake_hz{18.0f};    ///< 抖动频率
};

/// 对整幅画面施加运镜（缩放 + 旋转 + 平移 + 震动）。
///
/// ## ⚠ 安全边距（这条必须知道）
///
/// 放大（zoom > 1）会把画面边缘裁掉。若内容本来就贴到安全边距（版式边距 96px
/// 在 1080p 上是 5%），推到 1.06 倍就只剩 1%——实测数据页的「100」「10」
/// 两个大字几乎顶到画面边缘，观感上像"排版坏了"而不是"镜头推进"。
///
/// 所以：**推镜（zoom > 1）不超过 1.08**，且内容必须落在版式安全区
/// （`Grid::margin` 之内）。拉镜（zoom < 1）反而会露出画面外的空白，
/// 所以需要 `edge_fill` 兜底（默认黑场，与淡入淡出一致）。
///
/// ## 为何用"渲染到暂存画布再贴回"而不是改每个绘制调用
///
/// 改每个图元的坐标需要把变换矩阵塞进 Painter 的所有方法（十多个），
/// 且文字的光栅化尺寸会跟着变（字会变大变糊）。先整幅渲染再变换，
/// 只有**一次**重采样，且所有图元（含文字）一起动，语义一致。
///
/// 代价是一次全画布重采样（1080p 约 8~12ms/帧）——比逐图元变换贵，
/// 但换来的是"任何场景都能免费获得运镜"，这个取舍值得。
[[nodiscard]] auto apply_camera(const st::raster::Canvas& source, const CameraMove& move,
                                float progress, double time, float device_scale)
    -> st::raster::Canvas;

/// 体积光（一束斜向的光柱，像从窗口/舞台打下来）。
///
/// `angle` 是光柱方向（弧度）；用若干条半透明平行四边形叠出"边缘柔和"的效果。
void paint_god_ray(st::raster::Canvas& target, float origin_x, float origin_y, float angle,
                   float width_ratio, float length_ratio, Color color, float intensity,
                   float device_scale);

}  // namespace caiyun
