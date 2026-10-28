#include <cmath>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"
#include "st/text/text.hpp"

/// **文本行高必须是整像素**——它是整条布局链的小数源头。
///
/// ## 守的是什么（实测代价）
///
/// 字体度量算出的行高是小数：默认字体下 15px → **19.797**、12px → **15.838**、
/// 11px → **14.518**。而 `Text::measure` 直接拿它当高度，于是小数沿布局链
/// 一路传下去——gallery 一屏 105 个节点的实测：
///
/// | 指标 | 修复前 | 修复后 |
/// |---|---|---|
/// | 非整像素坐标 | **40.2%**（169/420） | 22.9%（96/420） |
/// | `Text` 高度非整 | **43/64** | 0/16 |
/// | 卡片高度 | 88.315 | 93（整） |
///
/// 两个具体代价：① 基线落在半像素 ⇒ 字形栅格化时纵向被反馈，竖笔发虚且粗细不一；
/// ② 卡片边界非整 ⇒ 1px 描边无法盖满一整行像素（`paint_box` 里“边框吸整像素”
/// 只是治标，源头在行高）。
///
/// ## 为何判据是"取整"而不是"等于某个系数"
///
/// 行高应当**跟随字体度量**（换字体会变），所以不能写死。能钉住的只有
/// "它落在整像素上、且不小于字体给的原始行高"（向下取整会挤行，见实现处的说明）。
ST_TEST(line_height_snaps_to_whole_pixels) {
  auto stack = st::text::FontStack::system_default();
  if (!stack) return;  // 无字体环境：跳过（不是通过）
  const st::text::TextRenderer renderer{*stack};
  // 覆盖 UI 实际用到的字号档（11/12/14/15/17/20/26）
  for (const float size : {11.0F, 12.0F, 14.0F, 15.0F, 17.0F, 20.0F, 26.0F}) {
    const float height = renderer.line_height(size);
    st::log::out("[line-height] size={:.1f} → {:.3f}\n", static_cast<double>(size),
              static_cast<double>(height));
    // ① 必须是整像素（小数会污染整条布局链）
    ST_CHECK(std::abs(height - std::round(height)) <= 1e-3F);
    // ② 必须不小于字号（否则一行字放不进自己的行高）
    ST_CHECK(height >= size);
    // ③ 上下留白要够——实测过挤的行高会让 11px 小字的行间距只剩 3px
    ST_CHECK(height - size >= 2.0F);
  }
}

/// 行高取整后，**相邻两个字号的行高不能相等**（否则视觉层级会塌）。
///
/// 这是取整的副作用风险：向上取整会把 11px（14.518→15）与 12px（15.838→16）
/// 拉开，但若某两个字号的度量恰好落在同一整数上，两档字号的行高就一样了——
/// 那会让"小一号"在行距上完全看不出来。本条钉住实际用到的档位互不相同。
ST_TEST(line_height_is_strictly_increasing_across_the_scale) {
  auto stack = st::text::FontStack::system_default();
  if (!stack) return;  // 无字体环境：跳过（不是通过）
  const st::text::TextRenderer renderer{*stack};
  const float xs2 = renderer.line_height(11.0F);
  const float xs = renderer.line_height(12.0F);
  const float sm = renderer.line_height(14.0F);
  const float base = renderer.line_height(15.0F);
  ST_CHECK(xs2 < xs);
  ST_CHECK(xs < sm);
  ST_CHECK(sm < base);
}
