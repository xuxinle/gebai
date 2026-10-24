/// 颜色管线：线性 ↔ sRGB 与色调映射。
///
/// ## 为什么需要它（实测诊断）
///
/// `software_scene.cpp` 现在是这样存的：
/// ```cpp
/// const float gain = scale * lighting.exposure;
/// return Vec3{clamp01(color.x*gain), ...};   // ← 线性值
/// ...
/// canvas_.set_pixel(..., Color{uint8(lit.x*255), ...});   // ← 当成 sRGB 存
/// ```
///
/// 两处错，而且都是"看不出哪里错、只觉得不对"的那种：
///
/// 1. **线性值当 sRGB 存**：0.5 线性 ≈ 0.735 sRGB。整幅画面**系统性偏暗**，
///    暗部细节被压死。这不是调参能救的——它是一条错误的曲线。
/// 2. **先 clamp 再存**：`clamp01` 把 HDR 截断在 1.0。高光、日光、自发光
///    全部糊成一片纯白。做 PBR 之前不改这里，等于把错误放大。
///
/// ## 与 Blender 的对应
///
/// - sRGB 分段公式与 `gpu_shader_colorspace.bsl.hh` 的
///   `scene_linear_to_rec709_srgb` 同一组常数（0.04045 / 12.92 / 1.055 / 2.4）。
/// - 色调映射**不照搬 AgX**：Blender 的 AgX 是 OpenColorIO 外部配置里的
///   LUT/样条，不在 C++ 源码中，且为影视分级设计（要配合 Log 空间）。
///   实时渲染用**解析式**更合适，也能被单元测试逐点钉住。
///
/// ## 三条纪律
///
/// 1. **两条腿（软件 / GPU）必须用同一套函数**——否则"降级"就变成"另一种长相"。
/// 2. **默认不改变现有画面**：`Exposure` 与 `ToneMap::None` 组合下，
///    输出与改动前逐像素相同（渐进采用，老场景不受影响）。
/// 3. 所有函数是**纯函数**（无状态、无静态缓存）：确定性是裁云逐帧复现的前提。

#pragma once

#include <cmath>
#include <cstdint>

#include "st/math/color.hpp"
#include "st/math/matrix.hpp"

namespace st::raster {

/// 色调映射算子。
///
/// 选择依据（都是解析式，可逐点断言）：
/// - `None`：不映射，只 clamp。**默认值**，保证老场景逐像素不变。
/// - `Reinhard`：`x/(1+x)`。最简，暗部压得住，高光会灰。
/// - `Aces`：Narkowicz 2015 的 ACES 近似（电影感，高光滚降柔和）。
/// - `Filmic`：Uncharted 2 的 Hable 曲线（对比更强，暗部更暗）。
enum class ToneMap : std::uint8_t {
  None = 0,
  Reinhard = 1,
  Aces = 2,
  Filmic = 3,
};

/// sRGB 编码：[0,1] 线性 → [0,1] 非线性（用于存进 8bit 缓冲）。
///
/// 分段点 0.0031308 对应 0.04045 的解码侧；两段在分段点连续
/// （验证见 `tests/raster_color_pipeline_test.cpp`）。
[[nodiscard]] auto linear_to_srgb(float linear) noexcept -> float;

/// sRGB 解码：[0,1] 非线性 → [0,1] 线性（读入纹理/颜色时用）。
[[nodiscard]] auto srgb_to_linear(float srgb) noexcept -> float;

/// 三分量版本（逐通道，无矩阵——我们只处理 Rec.709 原色）。
[[nodiscard]] auto linear_to_srgb(math::Vec3 linear) noexcept -> math::Vec3;
[[nodiscard]] auto srgb_to_linear(math::Vec3 srgb) noexcept -> math::Vec3;

/// 8bit 量化：线性值 → sRGB 字节。**这是存盘前的最后一步**。
///
/// 负数按 0 处理、超过 1 按 1（sRGB 编码前必须先 clamp——编码函数定义域是 [0,1]）。
[[nodiscard]] auto linear_to_srgb8(float linear) noexcept -> std::uint8_t;

/// 色调映射：HDR 线性值 → [0,1] 线性值（**之后**再做 sRGB 编码）。
///
/// 顺序不能反：先编码再映射等于在非线性空间做曲线，暗部会被压得不均匀。
[[nodiscard]] auto tone_map(math::Vec3 hdr, ToneMap op) noexcept -> math::Vec3;

/// 单通道版本（供探针逐算子对照）。
[[nodiscard]] auto tone_map_channel(float x, ToneMap op) noexcept -> float;

/// 完整输出链：HDR 线性 → 曝光 → 色调映射 → sRGB 编码 → 8bit。
///
/// 抽成一个函数而不是让调用方拼三步：**顺序错了画面就错了**，
/// 而顺序错误很难从画面上定位（它看起来像"配色不对"）。
[[nodiscard]] auto encode_pixel(math::Vec3 hdr_linear, float exposure,
                                ToneMap op) noexcept -> math::Color;

}  // namespace st::raster
