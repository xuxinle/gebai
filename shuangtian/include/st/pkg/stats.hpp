#pragma once

/// 源码结构度量（`st stats`）：把"结构评审"里要反复数的东西变成一条命令。
///
/// ## 为什么要有它
///
/// 结构评审要回答的问题反复是同几个：**哪个函数最大？哪个头被包含得最多？
/// 重复真源有几处？** 这些原先靠一次性脚本现写现算——既慢（一旦写成"每行回去找函数开头"
/// 就成了 O(n²)），又不可复现（评审结论里写"最大函数 511 行"，下次没人能一键复核）。
/// 这里一次做对，之后每次改动都能重跑同一把尺子。
///
/// ## 实现要点（都是"别写成慢的"）
///
/// - **单遍扫描**：逐行推进，靠一个"当前函数栈"跟踪归属——不做"从本行往回找函数头"
///   那种回溯（那正是 O(n²) 的来源）。
/// - **先剥离注释与字符串**：注释里的 `{`/`}`/`(` 会把括号匹配带偏（实测：把 42 行的
///   函数算成 1536 行），而字符串里的花括号（markdown 解析器里满地都是）会让深度全错。
/// - **不构造正则对象**：`std::regex` 构造昂贵，逐文件重建是常见的性能陷阱
///   （`lint.cpp` 里有同样的教训记录）。这里只做字符级判定。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 单个函数体（或"看起来像函数"的顶层块）的度量。
struct FunctionStat {
  std::string file{};   ///< 相对工程根
  std::string name{};   ///< 尽力而为的函数名（解析不出时为空）
  int line{0};          ///< 起始行（1 基）
  int lines{0};         ///< 行数（含签名与注释）
  int complexity{0};    ///< 近似圈复杂度（if/for/while/case/catch/&&/|| 计数）
};

/// 一个文件的度量。
struct FileStat {
  std::string path{};      ///< 相对工程根
  int lines{0};            ///< 总行数（含注释）
  int code_lines{0};       ///< 剥离注释/字符串后的非空行数
  int functions{0};        ///< 识别到的函数数
  int max_function{0};     ///< 最大函数行数
  std::string max_at{};    ///< 最大函数的位置（`文件:行`）
};

/// 头文件被包含的次数（"改它要重编多少"的直接读数）。
struct IncludeStat {
  std::string header{};   ///< 相对工程根
  int includers{0};       ///< 包含它的文件数（去重到文件）
};

struct ProjectStats {
  int files{0};
  int lines{0};
  int code_lines{0};
  int functions{0};
  /// 函数长度分布（归一化后行数）。
  int functions_over_200{0};
  int functions_over_150{0};
  int functions_over_100{0};
  std::vector<FileStat> biggest_files{};      ///< 按行数降序
  std::vector<FunctionStat> biggest_functions{};  ///< 按行数降序
  std::vector<FunctionStat> complex_functions{};  ///< 按近似复杂度降序
  std::vector<IncludeStat> hot_headers{};     ///< 按被包含次数降序
  /// 头文件**爆炸半径**：包含它的 `.cpp` 数（含传递）——即“改这个头要重编多少”。
  std::vector<IncludeStat> blast_radius{};    ///< 按传递包含它的 .cpp 数降序
};

/// 扫描工程（`include/`、`src/`、`tests/`、`examples/`、`tools/`）。
///
/// `top` 控制每个榜单的长度（默认 10）。`extension_filter` 为空时扫 `.cpp` 与 `.hpp`。
[[nodiscard]] auto collect_stats(std::string_view root, std::size_t top = 10)
    -> Result<ProjectStats>;

}  // namespace st::pkg
