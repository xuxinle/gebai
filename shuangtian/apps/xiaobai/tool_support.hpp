/// 小白内置工具的**纯函数层**（`sh` / `read` / `write` / `edit` 的判定逻辑）。
///
/// ## 为何要抽成头文件
///
/// 这些函数原先只在 `apps/xiaobai/main.cpp` 里，测试只能**复制一份同语义实现**
/// 来断言（见 `tests/xiaobai_tools_test.cpp` 的旧写法）。那种测试测的是
/// “复制品对不对”，**实现改了它照样绿**——等于没有护栏。
///
/// 抽到这里之后，应用与测试共用同一份代码：改坏实现，测试立刻变红。
///
/// ## 为什么是头文件而不是 .cpp
///
/// 应用只有一个翻译单元（`main.cpp`），这些函数都很短、无外部状态。
/// 放头文件里 `inline` 掉，省一份构建清单条目，也让测试无需链接应用。
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace xiaobai::tool {

/// 去首尾空白（`edit` 的相似行探测用）。
[[nodiscard]] inline auto trim_ws(std::string_view text) -> std::string {
  const auto is_space = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
  };
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  return std::string(text.substr(begin, end - begin));
}

/// 文本里是否含裸 NUL（前 `probe` 字节）。
///
/// **二进制判据**：文本文件（UTF-8/GBK/UTF-16）不含裸 NUL，而几乎所有二进制
/// 格式都有。比“扩展名白名单”可靠（`.dat`/`.bin` 枚举不完），也比“可打印字符
/// 占比”便宜（只看前 8KB）。
[[nodiscard]] inline auto looks_binary(const std::string& content,
                                       std::size_t probe = 8192) -> bool {
  const std::size_t window = std::min(content.size(), probe);
  return content.substr(0, window).find('\0') != std::string::npos;
}

/// 把路径归一化成可比较的形态：统一分隔符、消解 `.`/`..`、统一小写盘符、
/// 去尾分隔符。
///
/// 为何不做真实路径规约（`weakly_canonical` 之类）：写目标可能**还不存在**，
/// 真实的规约会失败；而工具只关心“字面上会不会跑到根外”。
[[nodiscard]] inline auto normalize_path(std::string_view text) -> std::string {
  std::vector<std::string> parts;
  bool absolute = false;
  std::string current;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    const char ch = i < text.size() ? text[i] : '/';
    if (ch == '/' || ch == '\\') {
      if (current.empty()) {
        if (parts.empty() && i == 0) absolute = true;
      } else if (current == ".") {
        // 无操作
      } else if (current == "..") {
        if (!parts.empty()) parts.pop_back();
      } else {
        parts.push_back(current);
      }
      current.clear();
    } else {
      current += ch;
    }
  }
  std::string out = absolute ? "/" : "";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out += '/';
    out += parts[i];
  }
  // Windows 盘符与整体比较都按小写：路径大小写不敏感
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return out;
}

/// 解析后的路径是否跑到工具根目录之外。
///
/// **判据是提醒而不是拦拦**：往系统目录写日志/临时文件可能是合理需求，
/// 硬拦会让模型陷入“怎么做都不对”的死局；但**静默**让模型写到意料之外的地方
/// 同样危险（可能覆盖用户的项目还以为写的是沙箱副本）。
[[nodiscard]] inline auto path_escapes_root(const std::string& root,
                                            const std::string& resolved) -> bool {
  if (root.empty()) return false;
  const std::string norm_root = normalize_path(root);
  const std::string norm_path = normalize_path(resolved);
  if (norm_root.empty()) return false;
  if (norm_path.size() < norm_root.size()) return true;
  if (norm_path.compare(0, norm_root.size(), norm_root) != 0) return true;
  // 必须是分隔符边界：`/a/bc` 不算在 `/a/b` 之内
  return norm_path.size() > norm_root.size() && norm_path[norm_root.size()] != '/';
}

/// 在 `haystack` 里找与 `needle` 首行“相近”的行，返回 `<行号, 原文>`（最多 `max` 条）。
///
/// 用途：`edit` 命不中时给模型定位线索。行首空白差异是最常见的不匹配原因，
/// 所以按**trim 后包含**比对——只回“未找到”会让模型盲猜重试，每猜一次
/// 就是一轮完整 API 调用（实测这类重试常要 3~5 轮）。
[[nodiscard]] inline auto similar_lines(const std::string& haystack, const std::string& needle,
                                        int max = 3) -> std::vector<std::pair<std::size_t, std::string>> {
  std::vector<std::pair<std::size_t, std::string>> out;
  const std::string first_line = needle.substr(0, needle.find('\n'));
  const std::string probe = trim_ws(first_line);
  if (probe.empty()) return out;
  // 只拿前 40 字符去比：长行里后段常因换行/空白差异而不同，前 40 字足以定位。
  const std::string key = probe.substr(0, std::min<std::size_t>(probe.size(), 40));
  std::size_t line_no = 0;
  std::size_t pos = 0;
  while (pos <= haystack.size() && static_cast<int>(out.size()) < max) {
    const auto end = haystack.find('\n', pos);
    const std::string line = end == std::string::npos ? haystack.substr(pos)
                                                      : haystack.substr(pos, end - pos);
    ++line_no;
    if (trim_ws(line).find(key) != std::string::npos) {
      out.emplace_back(line_no, line);
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return out;
}

}  // namespace xiaobai::tool
