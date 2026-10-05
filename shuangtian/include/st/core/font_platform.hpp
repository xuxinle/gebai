#pragma once

#include <string>
#include <vector>

/// 平台字体偏好：**系统默认字体链的候选按平台给出**。
///
/// 为什么单独成一个平台层：字体路径本身是平台知识（`C:/Windows/Fonts/…`、
/// `/usr/share/fonts/…`、`/System/Library/Fonts/…`），而「哪一族该优先」是
/// **产品取向**——Windows 上要中英文同体（微软雅黑自带拉丁字形）、代码要 Consolas。
/// 把两张表（路径 + 平台）混写在字型引擎里，会让「改一个默认字体」变成跨平台事故。
///
/// 约定：返回的每一档都**已按优先级排序**，且**都先检查文件存在**；探测不到即缺省。
namespace st::platform {

/// 一档字体候选。
struct FontPreference {
  std::string path{};
  /// 该档的 CJK 归属：`FontStack::find_face(..., bold)` 按**下标**把常规档配到同族
  /// 粗体面，所以这个标记必须与粗体链**逐位一致**（否则拉丁粗体会拿到中文字体的面，
  /// 中文渲染成豆腐块——见 `text.cpp` 的实测记录）。
  bool cjk{false};
};

/// 正文档（比例字体）候选：中英文默认字体。
///
/// 顺序语义 = 回退链序。**首档是平台首选**（Windows：微软雅黑，中英文同体）；
/// 其后是"通用拉丁 → 通用 CJK → 符号"的回退层，保证首选缺席时仍能显示
/// 拉丁、汉字与 ✓/✗ 这类几何符号。
[[nodiscard]] auto preferred_text_fonts() -> std::vector<FontPreference>;

/// 正文档的**粗体**面（与 `preferred_text_fonts` **逐位同序**）。
[[nodiscard]] auto preferred_text_fonts_bold() -> std::vector<FontPreference>;

/// 等宽候选（代码用）。首档是平台首选（Windows：Consolas）。
[[nodiscard]] auto preferred_mono_fonts() -> std::vector<FontPreference>;

}  // namespace st::platform
