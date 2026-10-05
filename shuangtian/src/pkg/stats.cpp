#include "st/pkg/stats.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <set>

#include "st/core/fs.hpp"
#include "st/core/string.hpp"

namespace st::pkg {
namespace {

/// 逐字符剥离注释与字符串内容（**保持行数不变**：换行原样保留）。
///
/// 为什么必须做（实测教训）：注释里出现的 `{`/`}`/`(` 会把括号匹配带偏——一次结构评审里
/// 就把一个 42 行的函数算成了 1536 行；而字符串里的花括号（markdown 解析器满地图）
/// 会让深度计数整个错位。剥离后得到的"净代码"才适合做结构判定。
[[nodiscard]] auto strip_noncode(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  const std::size_t size = text.size();
  for (std::size_t index = 0; index < size; ++index) {
    const char current = text[index];
    if (current == '/' && index + 1 < size && text[index + 1] == '/') {
      while (index < size && text[index] != '\n') {
        out.push_back(' ');
        ++index;
      }
      if (index < size) out.push_back('\n');
      continue;
    }
    if (current == '/' && index + 1 < size && text[index + 1] == '*') {
      out.append("  ");
      index += 2;
      while (index < size && !(text[index] == '*' && index + 1 < size && text[index + 1] == '/')) {
        out.push_back(text[index] == '\n' ? '\n' : ' ');
        ++index;
      }
      out.append("  ");
      index += 1;  // for 循环还会 +1，合计跳过 `*/`
      continue;
    }
    if (current == '"' || current == '\'') {
      const char quote = current;
      // 原始字符串 `R"delim(...)delim"` 单独处理：它内部**允许**换行与各种括号。
      const bool raw = current == '"' && index > 0 && text[index - 1] == 'R';
      out.push_back(' ');
      ++index;
      if (raw) {
        std::string delimiter;
        while (index < size && text[index] != '(') {
          delimiter.push_back(text[index]);
          ++index;
        }
        ++index;  // 跳过 '('
        const std::string terminator = ")" + delimiter + "\"";
        while (index < size) {
          if (text.compare(index, terminator.size(), terminator) == 0) {
            out.append(terminator.size(), ' ');
            index += terminator.size();
            break;
          }
          out.push_back(text[index] == '\n' ? '\n' : ' ');
          ++index;
        }
        --index;  // 交给 for 的 +1
        continue;
      }
      while (index < size) {
        if (text[index] == '\\') {
          out.append("  ");
          index += 2;
          continue;
        }
        if (text[index] == '\n') {
          out.push_back('\n');
          break;
        }
        if (text[index] == quote) {
          out.push_back(' ');
          ++index;
          break;
        }
        out.push_back(' ');
        ++index;
      }
      --index;
      continue;
    }
    out.push_back(current);
  }
  return out;
}

/// 关键字行首（这些不是函数定义）。
[[nodiscard]] auto is_keyword_start(std::string_view line) noexcept -> bool {
  static constexpr std::array<std::string_view, 19> kKeywords{
      "if",        "for",     "while",   "switch",   "else",     "do",      "namespace",
      "struct",    "class",   "enum",    "union",    "using",    "return",  "template",
      "public",    "private", "protected", "friend", "try"};
  for (const std::string_view keyword : kKeywords) {
    if (line.size() < keyword.size()) continue;
    if (line.substr(0, keyword.size()) != keyword) continue;
    // 后面必须是非标识符字符（`format(` 不该被 `for` 命中）。
    if (line.size() == keyword.size()) return true;
    const char next = line[keyword.size()];
    return !(std::isalnum(static_cast<unsigned char>(next)) != 0 || next == '_');
  }
  return false;
}

/// 从签名文本里抠出函数名（尽力而为；抠不出返回空）。
[[nodiscard]] auto function_name_of(std::string_view signature) -> std::string {
  // 取最后一个 `(` 之前的一段标识符。
  const std::size_t open = signature.find('(');
  if (open == std::string_view::npos) return {};
  std::size_t end = open;
  while (end > 0 && std::isspace(static_cast<unsigned char>(signature[end - 1])) != 0) --end;
  std::size_t begin = end;
  while (begin > 0) {
    const char previous = signature[begin - 1];
    if (std::isalnum(static_cast<unsigned char>(previous)) == 0 && previous != '_' &&
        previous != ':') {
      break;
    }
    --begin;
  }
  std::string name(signature.substr(begin, end - begin));
  // 去掉 `::` 前缀（保留最后一段），`operator()` 之类保持原样。
  const std::size_t separator = name.rfind("::");
  return separator == std::string::npos ? name : name.substr(separator + 2);
}

}  // namespace

auto collect_stats(std::string_view root, std::size_t top) -> Result<ProjectStats> {
  ProjectStats stats;
  std::vector<FunctionStat> all_functions;
  std::map<std::string, int> header_includers;
  /// 包含图：相对路径 → 它直接包含的项目内头（相对路径，`include/` 前缀已剥）。
  std::map<std::string, std::vector<std::string>> include_graph;
  std::vector<std::string> unit_files;   ///< 所有 `.cpp`（爆炸半径以它们为分母）

  const std::array<std::string_view, 5> roots{"include", "src", "tests", "examples", "tools"};
  for (const std::string_view directory : roots) {
    const std::string base = fs::join(root, directory);
    if (!fs::is_directory(base)) continue;
    auto entries = fs::walk(base);
    if (!entries) return forward_error(entries.error());
    for (const auto& entry : *entries) {
      if (entry.is_dir) continue;
      const std::string extension = fs::extension(entry.path);
      if (extension != ".cpp" && extension != ".hpp") continue;
      const std::string relative = fs::join(directory, entry.path);
      const std::string full = fs::join(base, entry.path);
      auto text = fs::read_text(full);
      if (!text) return forward_error(text.error());

      const std::string code = strip_noncode(*text);
      FileStat file;
      file.path = relative;

      const auto raw_lines = split(*text, '\n');
      file.lines = static_cast<int>(raw_lines.size());
      for (const std::string_view line : raw_lines) {
        if (!trim(line).empty()) ++stats.lines;
      }

      // —— 单遍扫描：逐行推进，维护「当前函数栈」——不做回溯 ——
      struct Frame {
        std::string name;
        int start_line{0};
        int depth_at_open{0};
        int body_lines{0};
        int complexity{0};
      };
      std::vector<Frame> stack;
      int depth = 0;
      int pending_start = -1;   // 已看到签名、等待 `{` 的行号
      std::string pending_name;
      std::string pending_signature;
      const auto code_lines = split(code, '\n');

      const auto flush = [&](int end_line) {
        Frame& frame = stack.back();
        const std::string at = std::format("{}:{}", relative, frame.start_line);
        FunctionStat stat;
        stat.file = relative;
        stat.name = frame.name;
        stat.line = frame.start_line;
        stat.lines = end_line - frame.start_line + 1;
        stat.complexity = frame.complexity;
        all_functions.push_back(std::move(stat));
        if (stack.size() == 1) {
          ++file.functions;
          if (end_line - frame.start_line + 1 > file.max_function) {
            file.max_function = end_line - frame.start_line + 1;
            file.max_at = at;
          }
        }
        stack.pop_back();
      };

      for (std::size_t index = 0; index < code_lines.size(); ++index) {
        const std::string_view raw_line = code_lines[index];
        const std::string_view body = trim(raw_line);
        const int line_number = static_cast<int>(index) + 1;

        if (!body.empty()) {
          if (body.front() != '#' && body.front() != '}' && body.back() != ')' &&
              (body.back() == '{' || body.ends_with("){") || body.ends_with(") {"))) {
            // 形如 `... ) {` 的行 = 函数定义的开头（也可能只是块，见下方判定）
            const bool looks_like_block =
                is_keyword_start(body) || body.starts_with("} else") ||
                body.starts_with("catch") || body.ends_with("= {");
            if (!looks_like_block) {
              pending_start = line_number;
              pending_signature = std::string(body);
              pending_name = function_name_of(body);
            }
          }
        }

        // 复杂度：只在**函数体内**计数。
        if (!stack.empty()) {
          Frame& frame = stack.back();
          for (std::size_t position = 0; position + 1 < body.size(); ++position) {
            const char current = body[position];
            if ((current == 'i' && body.substr(position, 3) == "if ") ||
                (current == 'f' && body.substr(position, 5) == "for (") ||
                (current == 'w' && body.substr(position, 6) == "while ") ||
                (current == 'c' && (body.substr(position, 5) == "case " ||
                                    body.substr(position, 6) == "catch ")) ||
                (current == '&' && body[position + 1] == '&') ||
                (current == '|' && body[position + 1] == '|')) {
              ++frame.complexity;
            }
          }
        }

        for (const char glyph : raw_line) {
          if (glyph == '{') {
            if (!stack.empty() && pending_start < 0) {
              // 函数体内的普通块：只加深深度。
              ++depth;
              continue;
            }
            if (pending_start >= 0) {
              Frame frame;
              frame.name = pending_name;
              frame.start_line = pending_start;
              frame.depth_at_open = depth;
              stack.push_back(std::move(frame));
              pending_start = -1;
              pending_name.clear();
              ++depth;
              continue;
            }
            ++depth;
          } else if (glyph == '}') {
            if (!stack.empty() && depth - 1 == stack.back().depth_at_open) {
              --depth;
              flush(line_number);
              continue;
            }
            if (depth > 0) --depth;
          }
        }
      }
      // 文件结束仍在栈里的（未闭合）：也计入，避免静默丢失。
      while (!stack.empty()) flush(static_cast<int>(code_lines.size()));

      for (const std::string_view line : code_lines) {
        if (!trim(line).empty()) ++file.code_lines;
      }

      stats.biggest_files.push_back(file);
      ++stats.files;
      stats.code_lines += file.code_lines;

      // 包含关系：同时供"热点头"与"爆炸半径"两处用（一份数据两个视图）。
      if (relative.ends_with(".cpp")) unit_files.push_back(relative);
      auto& edges = include_graph[relative];
      for (const std::string_view line : raw_lines) {
        const std::string_view trimmed = trim(line);
        if (!trimmed.starts_with("#include")) continue;
        const std::size_t open = trimmed.find('"');
        const std::size_t close = trimmed.find('"', open + 1);
        if (open == std::string_view::npos || close == std::string_view::npos) continue;
        const std::string target(trimmed.substr(open + 1, close - open - 1));
        header_includers[target] += 1;
        edges.push_back(target);
      }
    }
  }

  stats.functions = static_cast<int>(all_functions.size());
  for (const FunctionStat& function : all_functions) {
    if (function.lines >= 200) {
      ++stats.functions_over_200;
    } else if (function.lines >= 150) {
      ++stats.functions_over_150;
    } else if (function.lines >= 100) {
      ++stats.functions_over_100;
    }
  }
  std::ranges::sort(stats.biggest_files, [](const FileStat& a, const FileStat& b) {
    return a.lines > b.lines;
  });
  if (stats.biggest_files.size() > top) stats.biggest_files.resize(top);

  std::ranges::sort(all_functions, [](const FunctionStat& a, const FunctionStat& b) {
    return a.lines > b.lines;
  });
  for (std::size_t index = 0; index < std::min(top, all_functions.size()); ++index) {
    stats.biggest_functions.push_back(all_functions[index]);
  }
  std::ranges::sort(all_functions, [](const FunctionStat& a, const FunctionStat& b) {
    return a.complexity > b.complexity;
  });
  for (std::size_t index = 0; index < std::min(top, all_functions.size()); ++index) {
    stats.complex_functions.push_back(all_functions[index]);
  }

  for (auto& [header, count] : header_includers) {
    stats.hot_headers.push_back(IncludeStat{header, count});
  }
  std::ranges::sort(stats.hot_headers, [](const IncludeStat& a, const IncludeStat& b) {
    return a.includers > b.includers;
  });
  if (stats.hot_headers.size() > top) stats.hot_headers.resize(top);

  // —— 爆炸半径：对每个「项目内头」算传递闭包命中多少个 `.cpp` ——
  //
  // 为什么需要它（与"热点头"的区别）：`hot_headers` 只数**直接** include 的文件数，
  // 而真正决定"改一个头要重编多少"的是**传递**闭包（如 `element.hpp` 直接被 56 个文件
  // 包含，但经 `ui_root.hpp` 等中转，实际波及面更大）。这是增构改动前必须知道的数。
  //
  // 实现：先对每个 `.cpp` 做一次 DFS 求它的传递包含集（记忆化到 `closure_of`），
  // 再反向累计。**按 `.cpp` 序遍历一次**，复杂度 O(单元数 × 平均闭包大小)——亚秒级。
  std::map<std::string, std::vector<std::string>> closure_of;   // .cpp → 传递包含的头
  const auto resolve = [](const std::string& token) -> std::string {
    // `st/xxx.hpp` → `include/st/xxx.hpp`；`tests/...` 之类的相对引用原样保留。
    if (token.starts_with("st/")) return "include/" + token;
    return token;
  };
  const auto closure = [&](const std::string& unit) {
    const auto cached = closure_of.find(unit);
    if (cached != closure_of.end()) return cached->second;
    std::vector<std::string> reached;
    std::vector<std::string> pending{unit};
    std::set<std::string> seen{unit};
    while (!pending.empty()) {
      const std::string current = pending.back();
      pending.pop_back();
      const auto edges = include_graph.find(current);
      if (edges == include_graph.end()) continue;
      for (const std::string& raw : edges->second) {
        const std::string target = resolve(raw);
        if (include_graph.find(target) == include_graph.end()) continue;   // 系统头/第三方
        if (seen.insert(target).second) {
          reached.push_back(target);
          pending.push_back(target);
        }
      }
    }
    closure_of[unit] = reached;
    return reached;
  };
  std::map<std::string, int> blast;
  for (const std::string& unit : unit_files) {
    for (const std::string& header : closure(unit)) blast[header] += 1;
  }
  for (auto& [header, count] : blast) {
    stats.blast_radius.push_back(IncludeStat{header, count});
  }
  std::ranges::sort(stats.blast_radius, [](const IncludeStat& a, const IncludeStat& b) {
    return a.includers > b.includers;
  });
  if (stats.blast_radius.size() > top) stats.blast_radius.resize(top);
  return stats;
}

}  // namespace st::pkg
