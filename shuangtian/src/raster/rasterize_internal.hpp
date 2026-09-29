#pragma once

/// 内部接口（不对外）：扫描线覆盖率光栅化。实现见 `src/raster/rasterizer.cpp`。

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "st/raster/path.hpp"

namespace st::raster::detail {

/// 抗锯齿轮廓填充（扫描线 + 逐子样本覆盖率），直接混合到画布。
void fill_path_aa(Canvas& canvas, const Path& path, const Paint& paint, const DrawOptions& options);

/// 把路径覆盖率写入遮罩（路径坐标减去 `origin` 得到遮罩局部坐标）。用于裁剪与阴影。
void rasterize_mask(Mask& mask, const Path& path, float origin_x = 0.0f, float origin_y = 0.0f);

/// 线段扩展为闭合四边形（描边用）。
[[nodiscard]] auto stroke_to_path(const Path& path, float width, float flatten_tolerance) -> Path;

}  // namespace st::raster::detail
