/// 裁云 · 品牌与版式系统实现（见 `brand.hpp`）。

#include "brand.hpp"

#include <algorithm>

namespace caiyun {

auto Grid::column_x(int index) const -> float {
  const int safe_index = std::clamp(index, 0, columns - 1);
  const float unit = (content_width() + gutter) / static_cast<float>(columns);
  return margin + static_cast<float>(safe_index) * unit;
}

auto Grid::span_width(int span) const -> float {
  const int safe_span = std::clamp(span, 1, columns);
  const float unit = (content_width() + gutter) / static_cast<float>(columns);
  return static_cast<float>(safe_span) * unit - gutter;
}

auto Grid::baseline(int index, float step) const -> float {
  // 从内容区顶部起算：这样"第 0 条基线"永远等于内容区顶部，
  // 换画幅比例时整块内容跟着走，而不是浮在半空。减去安全边距的余量后，
  // 内容会留出上下呼吸区（不至于顶到画面边缘）。
  const float start = height * 0.16f;
  return start + static_cast<float>(index) * step;
}

}  // namespace caiyun
