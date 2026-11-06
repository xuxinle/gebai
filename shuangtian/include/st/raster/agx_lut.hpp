/// AgX 的**真值实现**：走 Blender 的 OCIO LUT（`AgX_Base_sRGB.cube`）。
///
/// ## 为什么必须换成 LUT（实测差距）
///
/// 本框架原先用 Troy Sobotka 的**解析拟合**。与 Blender 5.2.3 的 `AgX`
/// view transform 对账（`tools/blender_agx_truth.py`，自发光平面给精确线性值，
/// `tools/agx_chain_dump.py` 用 OCIO 复核）：
///
/// | 线性输入 | 本框架（解析） | 真 Blender AgX | 差 |
/// |---|---|---|---|
/// | 0.05 | 107 | **58** | +49 |
/// | 0.18（中灰） | 188 | **118** | **+70** |
/// | 0.5 | 232 | **170** | +62 |
/// | 1.0 | 245 | **197** | +48 |
/// | 2.0 | 251 | **217** | +34 |
///
/// 纯色连**色相**都不对：
///
/// | 线性 | 本框架 | 真 Blender |
/// |---|---|---|
/// | (1,0,0) | 橙 | **rgb(219,57,33)** |
/// | (0,0,1) | 偏紫 | **rgb(20,86,209)** |
///
/// **结论**：解析拟合在「观感」上像，在「数值」上不可用。要像素级只能拿
/// Blender 自己的 LUT。
///
/// ## Blender 的 AgX 链（`config.ocio` + OCIO 2.5.0 实测）
///
/// ```text
/// 场景线性 (Rec.709)
///   ├── M ──→ E-Gamut 线性        （见 kRec709ToEGamut）
///   ├── lg2 分配 [-12.47393, 12.5260688117] ──→ [0,1]³
///   ├── AgX_Base_sRGB.cube       （LUT_3D_SIZE 57, tetrahedral 插值）
///   └── 幂 2.4 ──→ sRGB 显示值
/// ```
///
/// `M` 不是手抄的：由 `tools/agx_chain_dump.py` 让 **OCIO 自己**算
/// （`config.getProcessor('Linear Rec.709 (sRGB)', 'Linear FilmLight E-Gamut')`），
/// 所以它必然是 Blender 渲染时用的那个矩阵。
///
/// ## 为什么 LUT 运行时加载而不是嵌入
///
/// 57³×3×4B ≈ **2.2 MB**。嵌入意味着仓库多一个 2.9 MB 的源文件、每次改
/// 都要重新生成、且 Blender 换版本时无法替换。改成运行时按路径加载：
///
/// | 情形 | 行为 |
/// |---|---|
/// | 找到 `.cube` | 走 LUT，**与 Blender 逐点一致** |
/// | 找不到 | 退回 `agx_analytic()`（旧解析拟合），并在 `unavailable_reason()` 说明 |
///
/// 回退不是「假装对齐」——它明确报出「这次没有走真值路径」。调用方可据此
/// 决定是否降低精度承诺。
///
/// ## 搜索路径
///
/// 1. 环境变量 `ST_AGX_LUT`（绝对路径，最高优先）
/// 2. Blender 默认安装位置（Windows / Linux / macOS 各版本）
/// 3. 仓库内 `resources/AgX_Base_sRGB.cube`
///
/// ## 与两条腿的关系
///
/// 本层的 `apply()` 是**纯函数**（只读全局常量表），所以软件腿与 D3D11 腿
/// 调同一个函数就必然逐像素一致——不需要在两腿各写一遍 LUT 采样
/// （那正是本仓反复批判的「两条腿长成不同样子」）。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/matrix.hpp"

namespace st::raster::agx {

/// LUT 是否已就绪（能读到 `.cube`）。
[[nodiscard]] auto available() noexcept -> bool;

/// 不可用的具体原因（可用时返回空串）。
///
/// 为何单独给这个：只给 bool 时"为什么"全被吞掉，排查只能靠猜。
[[nodiscard]] auto unavailable_reason() -> const char*;

/// LUT 边长（`LUT_3D_SIZE`）。`0` = 不可用。
[[nodiscard]] auto lut_size() noexcept -> int;

/// AgX 三维变换：线性 Rec.709 → sRGB 显示值（[0,1] **非线性**，可直接 ×255）。
///
/// 输入超出 LUT 覆盖范围时按边界钳制——OCIO 的 lg2 分配同样钳制
/// （`vars: [-12.47393, 12.5260688117]` 是对数域，超出即不可表示）。
[[nodiscard]] auto apply(math::Vec3 linear_rec709) noexcept -> math::Vec3;

/// 旧的解析拟合（Troy Sobotka）。**只在 LUT 不可用时使用**，且结果会偏亮
/// 几十级——保留它是为了「降级后画面不至于变成另一种长相」，不是为了对齐。
[[nodiscard]] auto analytic(math::Vec3 linear_rec709) noexcept -> math::Vec3;

/// 强制重新探测 LUT（测试用：切换环境变量后让缓存失效）。
void invalidate_cache() noexcept;

}  // namespace st::raster::agx
