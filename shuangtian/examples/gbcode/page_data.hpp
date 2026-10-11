/// gbcode 数据与工具（与界面无关）：内置样例表、轻量检查、缓冲区读取、目录扫描。
/// 拆分自 main.cpp 第一节，零逻辑改动。

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

/// 内置样例文件（无 `--workspace` 时的自包含工作区）。
struct Sample {
  std::string name;
  std::string language;
  std::string code;
  std::string tree_key;  ///< 资源管理器里的路径（相对工作区根）
};

[[nodiscard]] std::vector<Sample> samples();

/// 自定义语言：业务日志（演示"规则即数据"，与内置语言走同一台扫描器）。
void register_custom_language();

/// stlog 样例文本（`--language stlog` / open_stlog 用）。
inline constexpr std::string_view kStlogSample =
    "# 业务日志（自定义语言：运行时注册的规则）\n"
    "INFO  service=gateway trace_id=9f2c 请求进入 duration=12ms\n"
    "DEBUG service=gateway trace_id=9f2c 命中缓存 true\n"
    "WARN  service=renderer component=font 字形缓存接近上限\n"
    "ERROR service=net trace_id=9f2c 连接失败 resp=null  # 需要重试\n"
    "FATAL service=core 磁盘写入失败，进程退出\n";

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
  std::string path{};    ///< 真实文件绝对路径（空 = 内置样例，保存走内存模拟）

  /// 相等：`State<T>::set` 用它判变化（整个 vector 的比较依赖它）。
  /// 文本比较成本可接受（只在写入时判一次）；真要省可就只比元数据，
  /// 但那样「内容变了却不重组」会让编辑器显示与数据脱节——宁可多比。
  auto operator==(const OpenBuffer& other) const -> bool = default;
};

/// 读文件进列表（已打开则刷新内容，保持同一标签）；返回该标签下标（失败 = size()）。
[[nodiscard]] std::size_t read_into_buffers(std::vector<OpenBuffer>& buffers, const std::string& path);

/// 树目录扫描的最小条目（比 `st::fs::DirEntry` 轻：不带 path/size）。
struct DirEntryLite {
  std::string name{};
  bool is_dir{false};
};

/// 扫描一层目录并归一为「目录在前、各自字典序、忽略点开头」的稳定序。
/// 与旧实现同口径，但面向工作区树的缓存扫描（每帧重扫的系统调用不再能承受）。
[[nodiscard]] std::vector<DirEntryLite> list_dir_sorted(const std::string& root);

}  // namespace gbcode_ui
