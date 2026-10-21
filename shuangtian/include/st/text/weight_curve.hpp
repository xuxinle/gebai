/// 字重曲线：**跨字号的连续字重映射**（自成一体的口径）。
///
/// ## 为什么需要它
///
/// 同一字体在不同物理字号下，笔画相对像素网格的占比不同，导致**视觉字重随字号漂移**。
/// 霜天实测（暗色主题、默认参数、9~36px）：
///
/// | 物理px | 9 | 15 | 24 | 36 |
/// |---|---|---|---|---|
/// | 平均覆盖率 | 0.590 | 0.655 | 0.717 | 0.778 |
///
/// 漂移 **31.8%**，形状约 `0.262 + 0.144*ln(px)`（R²=0.988）——小字偏淡、大字偏黑。
///
/// ## 为什么是"采样点 + 分段线性"
///
/// 三种做法各自的毛病：
/// - **公式**能拟合趋势，但没法精确压住某一档的偏差；
/// - **分档**（低于某档一档、高于另一档）在档位边界**跳变**——字号连续变化时
///   用户会看到"字突然变粗"，而且永远覆盖不全；
/// - **采样点 + 分段线性插值**：点可以随便加（哪档偏就补一个点），中间连续无跳变。
///
/// ## 可调性（设计目标之一）
///
/// - 表可以从文件加载（`load_from_file`），改数值不必重编译；
/// - 未覆盖的字号在相邻两点间**线性插值**；超出范围取**端点值**（不外推斜率）；
/// - 空表 = 完全关闭（落回全局 `coverage_gamma` / `coverage_contrast_strength`）。
///
/// ## 与 `set_fitted_gamma` 的关系
///
/// `set_fitted_gamma` 是二档（`px <= 阈值 ? fitted : global`），**已知会在边界跳变**，
/// 保留仅为兼容老调用方；本类是新口径，两者同时设置时**本类优先**。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace st::text {

class WeightCurve {
 public:
  /// 一个采样点：物理字号 + 该档的 gamma / 对比度。
  struct Sample {
    float pixel_size{0.0f};
    float gamma{1.0f};
    float contrast{1.0f};
  };

  /// 该字号的映射结果。
  struct Value {
    float gamma{1.0f};
    float contrast{0.0f};
    /// 是否命中曲线（false = 表为空，调用方应落回全局参数）。
    bool active{false};
  };

  WeightCurve() = default;
  explicit WeightCurve(std::vector<Sample> samples);

  /// 加载采样点（内部会**排序并按字号去重**，后者覆盖前者）。
  /// 非法点（非有限值、字号 <= 0）被丢弃；γ 夹到 [0.3, 4]、对比度夹到 [0, 3]。
  void set_samples(std::vector<Sample> samples);

  /// 从文本加载。每行 `<px> <gamma> <contrast> [# 注释]`；
  /// 空行忽略。返回成功解析的点数。
  auto load_from_text(std::string_view text) -> std::size_t;
  /// 从文件加载（不存在时返回 0，不报错——曲线是可选增强）。
  auto load_from_file(std::string_view path) -> std::size_t;

  [[nodiscard]] auto empty() const noexcept -> bool { return samples_.empty(); }
  [[nodiscard]] auto samples() const noexcept -> const std::vector<Sample>& { return samples_; }

  /// 按物理字号查表。表空时返回 `{global_gamma, global_contrast, false}`。
  [[nodiscard]] auto evaluate(float pixel_size, float global_gamma,
                              float global_contrast) const noexcept -> Value;

  /// 表格摘要（诊断用）。
  [[nodiscard]] auto describe() const -> std::string;

 private:
  std::vector<Sample> samples_;   ///< 按 pixel_size 升序
};

}  // namespace st::text
