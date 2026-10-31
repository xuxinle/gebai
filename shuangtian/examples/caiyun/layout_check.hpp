/// 布局自检：**在渲染之前**用文字度量找出"画面上会出问题"的地方。
///
/// ## 为什么需要它（这是 schema 校验拦不住的那一类）
///
/// `schema.cpp` 能拦"字段名写错 / 类型不对 / 越界"——都是**语法层**问题。
/// 但实测踩到的这类问题它一个都拦不住：
///
/// | 实测踩到的 | schema 能拦吗 |
/// |---|---|
/// | 副标题正好压在底部页码条上 | ❌ |
/// | 标题 `y=6%` 撞上品牌角标区 | ❌ |
/// | 文字框 `w=40%` 放不下 30 个汉字（溢出画布） | ❌ |
/// | 两条文字图层的矩形互相重叠 | ❌ |
///
/// 这些**只能渲完看图发现**——而看图对智能体是最后一步、代价最高的一步。
/// 本模块把这类检查提前到"启动之前"，用 `TextPort::measure()` 算真实文字尺寸，
/// 不需要渲染一个像素。
///
/// ## 检查什么（以及为什么只查这些）
///
/// 只查**确定错**的情形，不猜审美：
///
/// 1. **出界**：图层的可见矩形超出画布（观者直接看不到一部分内容）
/// 2. **文字溢出**：实测宽度 > 图层框宽（会被裁掉或压到相邻内容）
/// 3. **重叠**（仅文字层之间）：两个文字层的框相交
///
/// 不检查"是否好看""间距是否协调"——那属于审美，机器判不了，
/// 强行给建议只会制造噪音。

#ifndef CAIYUN_LAYOUT_CHECK_HPP
#define CAIYUN_LAYOUT_CHECK_HPP

#include <string>
#include <vector>

#include "director.hpp"

namespace st::ui {
class TextPort;
}  // namespace st::ui

namespace caiyun {

/// 一条布局问题的严重度。
enum class LayoutIssueLevel : std::uint8_t {
  Warning,  ///< 大概不好看/可能读不清，但没坏
  Error,    ///< 确定错：内容看不见或互相遮挡
};

/// 一条布局问题。
struct LayoutIssue {
  LayoutIssueLevel level{LayoutIssueLevel::Warning};
  /// 定位路径，与 schema 诊断同格式（`scenes[2].layers[3]`）
  std::string path{};
  /// 出问题的图层名（空 = 匿名图层）
  std::string layer{};
  /// 一句人话，说清"哪里错、错到什么程度"
  std::string message{};
};

/// 一次布局自检的完整结果。
struct LayoutReport {
  std::vector<LayoutIssue> issues{};
  /// 实际参与检查的图层数（用于区分"没问题"与"没检查"）
  int checked_layers{0};

  [[nodiscard]] auto error_count() const -> int;
  [[nodiscard]] auto warning_count() const -> int;
  [[nodiscard]] auto empty() const -> bool { return issues.empty(); }
};

/// 对整份剧本做布局自检。
///
/// `text_port` 用于量文字真实尺寸；传 `nullptr` 时跳过所有与文字尺寸
/// 相关的检查（只做出界与重叠），并在结果里少报那两项——
/// **不静默当成"文字宽度为 0"**（那会让溢出检查永远通过，比不检查更糟）。
[[nodiscard]] auto check_layout(const Script& script, const st::ui::TextPort* text_port)
    -> LayoutReport;

}  // namespace caiyun

#endif  // CAIYUN_LAYOUT_CHECK_HPP
