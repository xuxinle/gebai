#pragma once

/// `st::text` 的**内部**共享件（`src/text/` 下多个实现文件共用，不进公开头）。
///
/// 建它的理由：`LcsOp`/`lcs_backtrack` 原先是 `diff.cpp` 匿名 namespace 里的私货，
/// 而 `merge.cpp`（三方合并）要的是**同一套**行级比较——各写一份必然分家，
/// 而分家的表现是「diff 说这行没改、merge 说改了」这类无法解释的矛盾。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace st::text::detail {

/// 差异操作（LCS 回溯的产物）。
enum class LcsOp : std::uint8_t { Equal, Delete, Insert };

struct LcsOpItem {
  LcsOp op{LcsOp::Equal};
  std::size_t old_index{0};
  std::size_t new_index{0};
};

/// LCS 回溯：把两段序列变成「相等 / 删除 / 插入」的操作序列。
///
/// 贪心策略（相等则取、否则取 LCS 值大的那一侧）保证结果**稳定且确定**：
/// 同一对输入永远给同一份差异，不随容器实现或遍历顺序变化——
/// 这是「并列视图与导出的补丁必须一致」的前提，也是三方合并可信的前提。
[[nodiscard]] inline auto lcs_backtrack(const std::vector<std::string_view>& old_side,
                                        const std::vector<std::string_view>& new_side)
    -> std::vector<LcsOpItem> {
  const std::size_t n = old_side.size();
  const std::size_t m = new_side.size();
  std::vector<LcsOpItem> ops;
  ops.reserve(n + m);
  const std::size_t cols = m + 1;
  std::vector<std::uint32_t> dp((n + 1) * cols, 0);
  const auto cell = [&dp, cols](std::size_t i, std::size_t j) -> std::uint32_t& {
    return dp[i * cols + j];
  };
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      if (old_side[i] == new_side[j]) {
        cell(i, j) = cell(i + 1, j + 1) + 1;
      } else {
        cell(i, j) = std::max(cell(i + 1, j), cell(i, j + 1));
      }
    }
  }
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n && j < m) {
    if (old_side[i] == new_side[j]) {
      ops.push_back(LcsOpItem{.op = LcsOp::Equal, .old_index = i, .new_index = j});
      ++i;
      ++j;
    } else if (cell(i + 1, j) >= cell(i, j + 1)) {
      ops.push_back(LcsOpItem{.op = LcsOp::Delete, .old_index = i, .new_index = j});
      ++i;
    } else {
      ops.push_back(LcsOpItem{.op = LcsOp::Insert, .old_index = i, .new_index = j});
      ++j;
    }
  }
  while (i < n) ops.push_back(LcsOpItem{.op = LcsOp::Delete, .old_index = i++, .new_index = j});
  while (j < m) ops.push_back(LcsOpItem{.op = LcsOp::Insert, .old_index = i, .new_index = j++});
  return ops;
}

/// 两段文本的行数乘积上限（超过就不再求 LCS）。
///
/// 与 `diff.cpp` 同口径：**不做静默截断**——退化成"整段替换/整文件冲突"是如实反映
/// "这两份差得太远"，而截断会显示出一个「看起来正常但少了几行」的假结果。
///
/// ⚠ 4M 格看似很大，但它是**行数乘积**：2000 行的文件（一个中等规模的 `.cpp`/`.hpp`）
/// 两边相乘就是 4.004M，**恰好越过上限**——于是“只改了 3 行”被报成“整篇重写”
/// （实测：gbcode 点开 page.hpp 差异，左栏全红、右栏全绿）。
///
/// 所以判定规模时**必须先剪掉公共前后缀**（见 `common_span`）：两个相近的文件
/// 剪完通常只剩几十行，那才是“真正需要 LCS 的那块”。
inline constexpr std::size_t kMaxLcsCells = 4'000'000;

/// 两段行序列的**公共前缀 / 后缀**长度。
///
/// 为何它是差异算法的第一道工序（而不只是优化）：
/// - **正确性**：不剪就按整份规模判上限，大文件一律退化为“整篇替换”
///   （上面 `kMaxLcsCells` 的注释记了实测代价）；
/// - **性能**：`lcs_backtrack` 的 DP 表是 (n+1)×(m+1) 个 `uint32`，
///   4M 格 ≈ 16MB 分配 + 逐格填充——剪完只剩真正的变更块，通常小两个数量级。
///
/// 剪枝不改变结果：公共前后缀里的行**必然**在 LCS 里（它们一一相等且顺序一致），
/// 所以剪掉它们与“整份求 LCS 再过滤”等价。
struct CommonSpan {
  std::size_t prefix{0};   ///< 开头多少行相同
  std::size_t suffix{0};   ///< 末尾多少行相同（与前缀不重叠）
};

[[nodiscard]] inline auto common_span(const std::vector<std::string_view>& old_side,
                                      const std::vector<std::string_view>& new_side)
    -> CommonSpan {
  const std::size_t limit = std::min(old_side.size(), new_side.size());
  CommonSpan span;
  while (span.prefix < limit && old_side[span.prefix] == new_side[span.prefix]) ++span.prefix;
  // 后缀：从尾巴往中间比，**不与前缀重叠**（否则“全相同”会被算两遍）。
  while (span.prefix + span.suffix < limit &&
         old_side[old_side.size() - 1 - span.suffix] == new_side[new_side.size() - 1 - span.suffix]) {
    ++span.suffix;
  }
  return span;
}

}  // namespace st::text::detail
