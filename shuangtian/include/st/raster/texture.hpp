#pragma once

/// **影像质感层**（cinematic texture）：景深、光晕、粒子、颗粒、体积光、光扫、运镜。
///
/// ## 这一层解决什么
///
/// 「有结构」与「有质感」是两件事。版式系统能让画面**正确**，但发布会/预告片/
/// 海报的观感来自**摄影语言**：光从哪来、哪里虚、哪里有一层尘、镜头在怎么动。
/// 这些在纯几何绘制里完全没有——于是画面即使排版无误，读起来仍像「展示板」。
///
/// 本层把这类**氛围**原语集中到框架，供任何应用复用（视频分镜、海报、启动页…）。
///
/// ## 三条设计约束
///
/// 1. **确定性**：同一时刻渲染两次必须逐像素相同（全仓 e2e 判据）。所以粒子与颗粒
///    不用随机数发生器，而用**哈希函数**（`hash01`）——同一个 (index, frame) 永远
///    得到同一个值，无需保存状态，也不需要随机引擎。
/// 2. **可关闭且零开销**：每个特效应有独立强度参数，`0` = 完全不画。
/// 3. **不依赖整画布模糊**：本层不假设有整幅高斯模糊原语——景深用**降采样→双线性
///    放大**近似（一次 1/4 降采样即低通滤波），径向光晕用多层同心圆近似径向渐变。
///    两者在观感上接近真实现，成本低一到两个数量级。
///
/// ## 边界
///
/// 只画**氛围**（光、尘、雾、虚化、镜头运动），不画主体。主体（文字/图形/素材）
/// 仍由调用方的层系统负责；本层效果按层叠加在主体**之下**或**之上**，
/// 不能替主体做表达。
///
/// ## 坐标约定
///
/// 除显式标注「归一化 0~1」的字段外，一律**逻辑像素**；`device_scale` 参数用于
/// 把逻辑尺寸换算到物理像素（物理 = 逻辑 × device_scale），由调用方传入当前画布
/// 的缩放值。

#include <cstddef>
#include <cstdint>
#include <utility>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/surface.hpp"

namespace st::raster {

using st::math::Color;

/// 确定性哈希 → `[0,1)`。同一个 (a,b) 永远得到同一个值。
///
/// 为什么不用 `std::mt19937`：那需要保存状态，而渲染是**纯函数**（同一时刻两次
/// 渲染必须相同）。哈希天然无状态，且可按「第 i 个粒子的第 j 个属性」独立取值，
/// 不必按顺序生成——这让粒子可以任意裁减而不改变其余粒子的外观。
[[nodiscard]] auto hash01(std::uint32_t a, std::uint32_t b) -> float;

/// 背景是否算「浅色底」——决定铺光与暗角要不要上。
///
/// 铺光与暗角在**深色底**上是加分（前者给「光从哪来」的体积感，后者把视线收到
/// 中心），但在**浅色/米白底**上是灾难：暗角在四角刷出大黑角，铺光把白底揩成
/// 一块脏灰。
///
/// 判据用**亮度**而不是给调用方加开关：浅底不该有铺光/暗角是普适的，而
/// 「每个写浅色画面的人都记得关掉它」是又一处必然漏的开关。
/// 且取上下两色中**较亮**的一侧：渐变可能一亮一暗，任一为浅色都会露馅。
[[nodiscard]] auto backdrop_is_light(st::math::Color top, st::math::Color bottom) -> bool;

/// 景深：把画面做一次「廉价失焦」（降采样 + 双线性放大）。
///
/// `strength` 0~1：0 = 不处理（零开销），1 = 最大虚化（约等于 1/6 降采样）。
/// 返回处理后的新画布（源画布不改）。
[[nodiscard]] auto depth_of_field(const st::raster::Surface& source, float strength)
    -> st::raster::Canvas;

/// 粒子场参数（光尘 / 星点 / 浮尘）。
struct ParticleField {
  int count{70};            ///< 粒子数（按画面面积自动缩放）
  float size_min{1.2f};     ///< 最小直径（逻辑像素）
  float size_max{4.8f};     ///< 最大直径
  /// 景深：`0` = 全部清晰、`1` = 大小粒子的虚化差最大。
  ///
  /// 真实做法是「远的粒子更糊」，这里用**尺寸 + 透明度**双通道表达：
  /// 小且淡 = 远（看起来虚），大且亮 = 近。比真模糊便宜一个数量级，
  /// 观感上足够（人眼判断景深靠虚化与亮度差，不是逐像素模糊度）。
  float depth_factor{0.85f};
  float alpha{0.5f};        ///< 整体透明度
  /// 横向漂移速度（逻辑像素/秒）：让「尘」缓慢流动而不是静止贴图。
  float drift_x{3.0f};
  float drift_y{-1.6f};
  Color color{200, 220, 255, 255};
};

/// 画粒子场（在给定画布上叠加）。
///
/// `time` 用于漂移与闪烁；同一 `time` 渲染两次结果完全相同。
void paint_particles(st::raster::Surface& target, const ParticleField& field, double time,
                     float device_scale);

/// 背景颗粒层：细小微光点，**每帧位置都不同**（但完全确定，无随机）。
///
/// 它解决的是「静止段看起来像卡住」——安静不等于冻结。
///
/// @param count        颗粒数（建议 200~600；再多就成噪点噪声了）
/// @param alpha        亮度（建议 0.05~0.2）
/// @param fps          帧率——时间按帧量化，保证同输入两次出片逐帧一致
void paint_grain(st::raster::Surface& target, int count, float alpha, double time, double fps,
                 float device_scale);

/// 光晕参数（镜头光晕 / 体积光 / 屏幕光扫）。
struct GlowSpec {
  float cx{0.5f};           ///< 光源横向位置（0~1 画面比例）
  float cy{0.35f};          ///< 光源纵向位置
  float radius{0.42f};      ///< 光晕半径（相对画面**短边**的比例）
  float intensity{0.55f};   ///< 中心强度（0~1）
  Color color{120, 180, 255, 255};
  /// 光晕的「扁平度」（1 = 正圆；>1 横向拉伸，像镜头光斑）。
  float stretch{1.0f};
};

/// 画径向光晕（多层同心圆近似径向渐变）。
///
/// 为什么不用真径向渐变：线性渐变是快速路径，径向要逐像素采样
/// （1080p 一次约 25ns/px ≈ 50ms/帧，导出时会拖垮）。用 12~16 层同心圆叠出来的
/// 径向感在观感上与真径向几乎无差，成本低一到两个数量级。
void paint_glow(st::raster::Surface& target, const GlowSpec& spec, float device_scale,
                float alpha_mul = 1.0f);

/// 横向光扫（一道亮带扫过画面，转场/强调常用）。
///
/// `progress` 0~1：光带从左扫到右；超出范围时不画（零开销）。
void paint_light_sweep(st::raster::Surface& target, float progress, float width_ratio,
                       Color color, float intensity, float device_scale);

/// 体积光（一束斜向的光柱，像从窗口/舞台打下来）。
///
/// `angle` 是光柱方向（弧度）；用若干条半透明平行四边形叠出「边缘柔和」的效果。
void paint_god_ray(st::raster::Surface& target, float origin_x, float origin_y, float angle,
                   float width_ratio, float length_ratio, Color color, float intensity,
                   float device_scale);

/// **矢量相机**：在绘制期施加的缩放（而非画完再重采样）。
///
/// ## 为何需要它（「大幅钻穿不糊」的唯一路径）
///
/// 重采样（`apply_camera`）在小幅度运镜下很好：一次重采样、所有图元一起动，
/// 语义一致。但它的**细节天花板就是源分辨率**：200× 时一个源像素变成 200px 方块，
/// 双线性插值把这个方块抹匀，所以必糊。每帧按当前尺度**重新绘制**才能保持锐利——
/// 矢量矩形的细节是无限的。
///
/// ## 实测（1920×1080，同一场景只改矢量相机开关）
///
/// | 倍率 | 重采样 p99.99 边缘梯度 | 矢量 | 倍数 |
/// |---|---|---|---|
/// | 9.7× | 18.0 | 151.0 | 8.4× |
/// | 14.1× | 13.0 | 155.0 | 11.9× |
/// | 18.4× | 10.0 | 142.6 | 14.3× |
/// | 22.8× | 7.0 | 92.0 | 13.1× |
///
/// 重采样随倍率**持续退化**（18→13→10→7），矢量**基本保持**。
///
/// **限制**：只支持等比缩放 + 锚点（不支持旋转/震动）；屏幕空间效果（粒子/三维/
/// 光晕）不经过坐标变换入口，不会跟着缩放。
struct VectorCamera {
  float zoom{1.0f};
  /// 锚点（**逻辑坐标**）。缩放时这个点在屏幕上不动。
  float anchor_x{960.0f};
  float anchor_y{540.0f};

  /// 世界坐标 → 屏幕逻辑坐标（未乘 device scale）。
  [[nodiscard]] auto apply(float x, float y) const -> std::pair<float, float> {
    if (zoom == 1.0f) return {x, y};
    return {(x - anchor_x) * zoom + anchor_x, (y - anchor_y) * zoom + anchor_y};
  }

  /// 物理像素坐标 → 物理像素（`scale` 已在坐标里，这里除以再乘回）。
  ///
  /// ## 为何需要这个入口
  ///
  /// `path` / `ellipse` 这类图元直接画到 canvas，其坐标已是物理像素，不经过接收
  /// 逻辑坐标的变换入口。若它们不过相机，就会出现「同一画面的 rect/text 在放大、
  /// 而曲线全程不动」——很难看出是哪类图元漏了。
  [[nodiscard]] auto apply_physical(float px, float py, float scale) const
      -> std::pair<float, float> {
    if (zoom == 1.0f) return {px, py};
    const auto [x, y] = apply(px / scale, py / scale);
    return {x * scale, y * scale};
  }
};

/// 缩放曲线（两种是不同的语言）。
enum class ZoomCurve : std::uint8_t {
  Linear,   ///< `from + (to-from)*t`——镜头匀速（摄影语法）
  ExpoIn,   ///< 绝大部分变化挤在末尾——「吸进去」
  ExpoOut,  ///< 绝大部分变化在前段——「弹出来」
};

/// 运镜参数（摄影机运动）。
///
/// ## 为什么镜头运动会显著改变观感
///
/// 静态构图的画面读起来像「展示板」；同样的内容加上**缓慢而确定的镜头运动**，
/// 就从「图」变成「镜头拍的」。这不是特效，是摄影语法。
///
/// ## 三条经验（直接用，别自己调）
///
/// 1. **推（zoom 变大约）比拉更常用**：观众注意力随时间收拢；拉适合收尾。
/// 2. **幅度小、时长长**：整段推 4~8% 就够；推到 20% 会像「缩放动画」，很廉价。
/// 3. **旋转必须极小**：0.3~0.8 度能「让画面活」，超过 2 度立刻让人晕。
///
/// ## 曲线与幅度要匹配
///
/// · `Linear` + 小幅度（1.0 → 1.08）：**镜头呼吸**，观众意识不到有运镜。
/// · `ExpoIn` + 大幅度（1 → 300）：**钻穿尺度**，观众意识到「世界比我以为的大」。
///
/// 不要用指数驱动小幅度（前 90% 时间几乎不动，看起来像卡了），
/// 也不要用线性驱动大幅（一路匀速放大，像缩放动画）。
struct CameraMove {
  float zoom_from{1.0f};    ///< 起点缩放（1 = 原始）
  float zoom_to{1.0f};      ///< 终点缩放
  float rotate_deg{0.0f};   ///< 整段旋转总量（度，建议 <= 1.0）
  float pan_x{0.0f};        ///< 整段横向位移（相对画面宽度的比例）
  float pan_y{0.0f};        ///< 整段纵向位移
  /// 冲击震动：入场/转场瞬间的衰减抖动（幅度按画面短边比例）。
  /// 做成「正弦 × 指数衰减」——恒定抖动会让画面持续模糊，观感差。
  float shake{0.0f};
  float shake_decay{6.0f};  ///< 衰减速率（1/秒）
  float shake_hz{18.0f};    ///< 抖动频率
  /// 缩放的**销点**（画面归一化坐标 0~1）；负值 = 用画面中心。
  ///
  /// 销点是「从这个点里钻出去」的全部：销点处的元素在屏幕上**纹丝不动**，
  /// 变大的只有周围的一切。不给销点就是「整幅等比放大」——两种观感完全不同。
  ///
  /// **必须是归一化坐标**（而非像素）：同一份脚本要能出 1080p/4K/竖版，
  /// 归一化坐标在每种画幅下指向同一个相对位置。
  float anchor_x{-1.0f};
  float anchor_y{-1.0f};
  /// 缩放曲线。默认 `Linear` = 逐像素不变的旧行为。
  ZoomCurve zoom_curve{ZoomCurve::Linear};
};

/// 对整幅画面施加运镜（缩放 + 旋转 + 平移 + 震动）。
///
/// ## ⚠ 安全边距（这条必须知道）
///
/// 放大（zoom > 1）会把画面边缘裁掉。若内容本来就贴到安全边距（如 1080p 的 96px
/// = 5%），推到 1.06 倍就只剩 1%——观感上像「排版坏了」而不是「镜头推进」。
/// 所以：**推镜不超过 1.08**，且内容必须落在版式安全区内。拉镜（zoom < 1）会露出
/// 画面外的空白，需要 `edge_fill` 兜底（默认黑场，与淡入淡出一致）。
///
/// ## 为何用「渲染到暂存画布再贴回」而不是改每个绘制调用
///
/// 改每个图元的坐标需要把变换塞进所有绘制方法，且文字的光栅化尺寸会跟着变。
/// 先整幅渲染再变换，只有**一次**重采样，且所有图元（含文字）一起动，语义一致。
/// 代价是一次全画布重采样（1080p 约 8~12ms/帧）——换来「任何场景都能免费获得
/// 运镜」，这个取舍值得。
[[nodiscard]] auto apply_camera(const st::raster::Surface& source, const CameraMove& move,
                                float progress, double time, float device_scale)
    -> st::raster::Canvas;

/// 相机进度（**窗口语义的唯一真相**）。
///
/// 窗口未启用（`window_end <= window_start`）时退回整段线性；启用时只为
/// `[window_start, window_end]` 推进，窗口外夹在 0/1。
///
/// ## 为何需要它
///
/// 没有窗口时进度是硬编码的 `time / duration`，于是「钻 8 秒、然后静静呆 4 秒
/// 让观众读」**表达不出来**——相机只能一路走到幕尾。
///
/// ## 为何不能靠拉长时长代替
///
/// 拉长让整段变慢，但 `ExpoIn` 的**末段变化最快**，拉长反而让尾部更刷。
/// 曲线与窗口是正交的两件事；用曲线去模拟窗口，方向恰好相反。
[[nodiscard]] auto camera_progress(float time, float duration, float window_start,
                                   float window_end) -> float;

/// 绕 `center` 旋转一点（度，顺时针为正）。
///
/// 它是「旋转图元」的全部几何内容。把这个算术独立出来才能给它写单测：
/// 断言「35° 后轴对齐包围盒变胖 1.39 倍且面积守恒」。
[[nodiscard]] auto rotate_point_deg(st::math::Point point, st::math::Point center,
                                    float degrees) -> st::math::Point;

/// 一组点绕自身包围盒中心旋转（多边形与旋转矩形共用）。
///
/// 与 `rotate_point_deg` 分开的理由：旋转中心得从**点集**推出来，而「用哪个中心」
/// 恰好是这类图元最容易写错的地方（按顶点平均会让分布不均的多边形整体偏移）。
void rotate_points_about_bounds(st::math::Point* points, std::size_t count, float degrees);

}  // namespace st::raster
