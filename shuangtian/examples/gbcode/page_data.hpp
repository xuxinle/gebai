/// gbcode 数据与工具（与界面无关）：轻量检查、缓冲区读取、自定义语言注册。
///
/// ⚠ 2026-10-18：**内置样例表已删除**（`samples()` / `Sample`）。
/// 理由：gbcode 转为正式使用后，工作区数据一律来自磁盘（`--workspace` / 当前目录），
/// “无参数时给几份假文件”是演示形态，不是产品形态。

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"

namespace gbcode_ui {

/// 编辑器字号档位默认值（相对正文 `font_base` 的倍数）。
///
/// 为什么用档位而不是绝对像素：主题是字号缩放的唯一真值源——
/// 绝对像素会与 `--ui-font-scale` 脱钩（实测踩到：UI 文字 15→22.5，编辑器恒为 13.5）。
/// 档位还让“设置面板列几档字号”变成一件自然的事。
///
/// `1.0` = **与正文同级**（与组件默认一致）。`--editor-font-scale` 可覆盖。
constexpr float kEditorFontScale{1.0f};

/// 自定义语言：业务日志（`stlog`）。
///
/// 这不是演示摆拍：它验证一个**真能力**——运行时注册的语言规则
/// 与内置语言走同一台扫描器（“规则即数据”）。产物是用户可用的高亮语言
///（`include/st/text/language_registry.hpp`），而不是界面上的弹窗。
void register_custom_language();

/// 一条轻量检查结果（**真检查**，不是占位数据）。
struct Problem {
  std::size_t line{0};   ///< 1 起行号
  bool warning{true};    ///< true = 警告，false = 错误
  std::string message{};
};

/// 便宜的逐行检查（不引入分析器）：行尾空白 / Tab 缩进 / TODO·FIXME / 超长行。
[[nodiscard]] std::vector<Problem> lint_text(std::string_view text);

/// 一个打开的编辑器缓冲区。
struct OpenBuffer {
  std::string key;       ///< 业务身份（样例名或绝对路径）——标签按它对账
  std::string label;     ///< 标签名（文件名）
  std::string language;
  std::string text;      ///< 当前文本（切回来时恢复）
  bool dirty{false};     ///< 修改点
  std::string path{};    ///< 真实文件绝对路径（空 = 尚无命名的未保存缓冲区）
  /// **二进制文件（只读打开）**：见 `looks_binary`。
  ///
  /// 为何要这个位（实测踩到）：二进制文件按文本读进来有两个后果——
  /// ① 屏幕上是一堆控制字符（无意义，也谈不上高亮）；
  /// ② 更危险：它**可以被编辑并 Ctrl+S 写回磁盘**——实测输入 3 个字节后保存，
  ///    文件就变成了乱码。所以在打开那一刻就判定“这不是文本”，
  ///    按只读展示（编辑器 read_only=true）+ 保存路径直接拒绕。
  bool binary{false};

  /// 相等：`State<T>::set` 用它判变化（整个 vector 的比较依赖它）。
  /// 文本比较成本可接受（只在写入时判一次）；真要省可就只比元数据，
  /// 但那样「内容变了却不重组」会让编辑器显示与数据脱节——宁可多比。
  auto operator==(const OpenBuffer& other) const -> bool = default;
};

/// 读文件进列表（已打开则刷新内容，保持同一标签）；返回该标签下标（失败 = size()）。
[[nodiscard]] std::size_t read_into_buffers(std::vector<OpenBuffer>& buffers, const std::string& path);

/// 这段字节像不像二进制（而非文本）。
///
/// 判据（工程折中，不求严格）：头 8 KiB 里出现 `\0`，或控制字符（除 `\t\r\n\f\v\b`）
/// 占比 > 5%。两条合起来既能把 PE/PNG/压缩包一次认出来，也不会误伤正常源码
///（源码里偶发一个 ANSI 转义字符不至于被当成二进制）。
[[nodiscard]] bool looks_binary(std::string_view bytes);

/// 树目录扫描的最小条目（比 `st::fs::DirEntry` 轻：不带 path/size）。
struct DirEntryLite {
  std::string name{};
  bool is_dir{false};
};

/// 扫描一层目录并归一为「目录在前、各自字典序、忽略点开头」的稳定序。
/// 与旧实现同口径，但面向工作区树的缓存扫描（每帧重扫的系统调用不再能承受）。
[[nodiscard]] std::vector<DirEntryLite> list_dir_sorted(const std::string& root);

}  // namespace gbcode_ui
