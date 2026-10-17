#pragma once

/// 裁云（Nimbus）· **场景渲染引擎**——把导演脚本里的一个分镜画成一帧。
///
/// ## 分辨率无关
///
/// 全部排版按 **1920×1080 设计坐标系**写（字体大小、卡片尺寸、留白都是这套坐标里的
/// 常数），绘制时整体乘 `scale = 物理宽 / 1920`。于是同一份代码：
/// - 预览（640×360，`scale = 0.333`）与成片（1920×1080，`scale = 1.0`）**构图完全一致**；
/// - 想导出 4K 只改输出尺寸，字号跟着放大——而不是把小图拉伸（那会让字发虚）。
///
/// 字形仍按**物理字号**栅格化（`TextPort` 的约定），所以放大不会损失锐度。
///
/// ## 纯函数
///
/// `render_scene(target, scene, local_time, duration, port)` 只依赖入参：同一时刻渲染
/// 两次**逐像素相同**。这是 e2e 确定性判据（设计文档 §7.1）与逐帧导出可复现的前提。
///
/// ## 观感的落点
///
/// "高质量"在这里是一组具体选择，全部集中在本文件（可整体调，改动会同时影响五个模板）：
/// 深色电影底 + 单色强调光晕；进入一律 `ease_out_expo`（快出慢收）；元素错峰 0.12~0.2s；
/// 大标题带字距（CJK 逐字加距）；卡片 24 圆角 + 1px 低透明描边 + 双层柔和投影；
/// 背景缓漂 + 暗角。**不追求"元素多"，追求节奏与层次**。

#include <cstdint>

#include "director.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/text_port.hpp"

namespace caiyun {

/// 设计坐标系尺寸（全部排版常数以此为单位）。
inline constexpr float kDesignWidth = 1920.0f;
inline constexpr float kDesignHeight = 1080.0f;

/// 画一个分镜的一帧。
///
/// - `local_time`：场景内时间（秒）；**超出 `scene_duration` 时按最后一帧保持**
///   （片段被移动/修剪后仍能出画面，而不是黑屏）；
/// - `scene_duration`：该场景的标称时长（用于把入场/打字节奏铺满）；
/// - `text_port` 为空时只画几何、不画字（导出路径一定有端口，此分支只为兜底）。
void render_scene(st::raster::Canvas& target, const Scene& scene, double local_time,
                  double scene_duration, const st::ui::TextPort* text_port);

// —— 缓动（集中在此：改一处即改全片手感）——
[[nodiscard]] auto ease_out_expo(float t) -> float;
[[nodiscard]] auto ease_out_cubic(float t) -> float;
[[nodiscard]] auto ease_in_out_cubic(float t) -> float;
[[nodiscard]] auto ease_out_back(float t) -> float;

}  // namespace caiyun
