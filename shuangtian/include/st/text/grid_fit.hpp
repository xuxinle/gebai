#pragma once

/// 字形**网格拟合**（grid fitting / hinting）：把轮廓的笔画边缘吸附到像素网格。
///
/// **为什么需要它**（2026-10-01 由 vsedit 反馈逐像素对照定位）：
/// 13.5px 正文在 1.25 DPI 下是 16.88 物理像素，笔画宽 1.2~1.7px 且边缘落在**分数相位**上——
/// 实测 184 条竖笔画里**没有一条**边缘落在整数网格上（0.0%），91.8% 是"每边各一个过渡像素"
/// 的缓坡，55.4% 的边缘正好落在像素正中间。这就是"字看着糊"的主因，**与抗锯齿模式无关**
/// （灰度/亚像素都过不了这一关）。
///
/// **为什么不实现 TrueType 指令解释器**（本轮已实测证伪，见 `tools/hinting_gain_probe.cpp`）：
/// | 路径 | 拉丁 TrueType | 中文 CFF |
/// |---|---|---|
/// | 无 hinting（现状） | 3.2% | 5.2% |
/// | 读字体自带指令（`TARGET_LIGHT/NORMAL`） | 3.2% | 4.3% |
/// | **几何网格拟合（auto-hinter 式）** | **49.0%** | **20.6%** |
///
/// 两条结论：① 读字体自带指令 ≈ 什么都没做（连拉丁都没有改善）；② 中文界面字体是 **CFF**，
/// 只有 stem hint、没有 TT 那套指令，`TARGET_MONO` 对它同样无效——**给中文做 TT 指令
/// 解释器是白工**。唯一有效的是**不看指令、从轮廓几何自推笔画位置**的拟合。
///
/// **做法**（对齐 FreeType auto-hinter 的思路，但按本框架的口径简化）：
/// 1. **取笔画**：轮廓里的近垂直边两两配对成"竖笔画"（`x_left < x_right`，宽度 ≤ `max_width`，
///    y 区间重叠），并按 y 重叠归组成**stem 组**（同一竖列的多个笔画一起移动）；
/// 2. **定策略**：按 FreeType 的口径取 `round`（对齐边缘）/ `floor`/`ceil`（保方向）/
///    `center`（对齐笔画中心，等宽字体与 CJK 用这个以免字身前倾）；
/// 3. **吸附**：把组的**左边缘**按策略移到目标位置，整组（含其控制的曲线控制点）一起平移 `Δ`；
/// 4. **横画同法**（同一套逻辑作用在 y 方向）；
/// 5. **护栏**：单点位移超过 `max_shift`（默认 0.5px）或整字数位移超限时**放弃拟合**——
///    宁可保持原样，也不让字形走样。
///
/// **关键不变式：字宽（advance）绝不改变**。拟合只动轮廓点，不动 `hmtx` 的字宽，
/// 所以排版与之前逐像素相同（这是硬约束，有测试钉住）。
///
/// 只对**竖画/横画**做拟合，不碰曲线段的形状（控制点跟着端点平移）——
/// 这是 auto-hinter 在"轻"档位下的做法，也是小字收益最大、代价最小的一档。

#include <span>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/path.hpp"

namespace st::text {

/// 拟合强度（对应 FreeType 的 `FT_LOAD_TARGET_*` 语义）。
enum class GridFitMode : std::uint8_t {
  /// 不拟合（现状；无头截图/回归测试的可逐像素断言口径）。
  Off,
  /// **轻**（推荐默认）：只把笔画边缘吸附到像素网格，不动字面位置——
  /// 与 FreeType 的 auto-hinter `light` 档同一取向，形变最小。
  Light,
  /// **常规**：除边缘吸附外，还把笔画中心轻微归位（对 CJK 更稳，避免字身前倾）。
  Normal,
};

/// 拟合参数。
struct GridFitOptions {
  GridFitMode mode{GridFitMode::Light};
  /// 认定为"竖笔画"的最大宽度（像素）。超过它就不是笔画而是字身的宽部件，
  /// 吸附它会明显改变字形——宁可不动。
  float max_stem_width{2.6f};
  /// 单点允许的最大位移（像素）。超过就整体放弃（护栏，见文件头）。
  float max_shift{0.5f};
  /// 整字数位移超过该比例时放弃（防止"为了锐化把字挪出去"）。
  float max_mean_shift{0.25f};
};

/// 拟合结果（供测试与诊断；调用方通常只用 `path`）。
struct GridFitResult {
  raster::Path path{};      ///< 拟合后的轮廓（未生效时与入参相同）
  bool applied{false};      ///< 是否真的做了拟合（护栏触发时为 false）
  int vertical_stems{0};    ///< 参与拟合的竖笔画数
  int horizontal_stems{0};  ///< 参与拟合的横笔画数
  float mean_shift{0.0f};   ///< 平均位移（px，越小形变越小）
};

/// 对**像素空间**的轮廓做网格拟合。入参 `path` 的坐标必须是物理像素（y 向下），
/// 即 `TextRenderer` 在 `size × device_scale` 缩放后、栅格化之前的形态。
[[nodiscard]] auto grid_fit(const raster::Path& path, const GridFitOptions& options = {})
    -> GridFitResult;

}  // namespace st::text
