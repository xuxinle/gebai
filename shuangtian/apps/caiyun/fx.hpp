#pragma once

/// 裁云 · 影像特效的能力**已下沉到框架**（`st/raster/texture.hpp`）。
///
/// 这里只做转发与别名：应用内不再保留第二份实现，能力的唯一真值源在框架里，
/// 海报（镂月）等其他应用可直接复用同一套原语。
///
/// 保留本头文件而不是让调用点全部改名的原因：`motion.cpp` / `project.cpp`
/// 里用非限定名调用（如 `paint_glow(...)`），转发层让它们一行都不用改；
/// 而实现只有一份。
#include "st/raster/texture.hpp"

namespace caiyun {

using st::raster::apply_camera;
using st::raster::backdrop_is_light;
using st::raster::camera_progress;
using st::raster::CameraMove;
using st::raster::depth_of_field;
using st::raster::GlowSpec;
using st::raster::hash01;
using st::raster::paint_glow;
using st::raster::paint_god_ray;
using st::raster::paint_grain;
using st::raster::paint_light_sweep;
using st::raster::paint_particles;
using st::raster::ParticleField;
using st::raster::rotate_point_deg;
using st::raster::rotate_points_about_bounds;
using st::raster::VectorCamera;
using st::raster::ZoomCurve;

}  // namespace caiyun
