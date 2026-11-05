// 诊断 bpy_watch.py 的失败位置：逐步截断脚本，二分找第一次失败的行。
//
// ## 为什么要这个
//
// MicroPython MINIMUM 档报 `NameError: name not defined` **不说是哪个名字**。
// 面对 400 行脚本，唯一可靠的定位手段就是截断/二分——按行截断比按空行
// 切块可靠（不会切坏 docstring 或多行表达式）。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "st/python/runtime.hpp"

namespace {

auto read_file(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

/// 在**顶层语句边界**处截断（跳过 docstring / 多行字符串内部）。
///
/// ## 为何不能按行硬截（踩过两次）
///
/// 直接切在第 N 行会把 docstring、括号续行、多行表达式切成两半 →
/// `SyntaxError: invalid syntax`，而那个错**与真实问题无关**。
///
/// 更隐蔽的是：docstring 内部的文字行缩进是 0（"它是 bpy 相对……"），
/// 所以"缩进 0"这个判据**也会误判**——它把 docstring 中间当成了语句边界。
/// 必须真的跟踪引号状态。
auto truncate_at_statement(const std::string& source, std::size_t line_no) -> std::string {
  std::istringstream stream(source);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(stream, line)) lines.push_back(line);

  // 跟踪三引号状态：只有"不在多行字符串里"且缩进 0 的行才是切点
  std::size_t cut = 0;
  bool in_triple = false;
  char triple_char = 0;
  for (std::size_t i = 0; i < lines.size() && i < line_no; ++i) {
    const std::string& text = lines[i];
    if (text.empty()) continue;
    // 统计本行出现的三引号（粗略：只看 """ 与 '''）
    const std::size_t dq = text.find("\"\"\"");
    const std::size_t sq = text.find("\'\'\'");
    const bool has_dq = dq != std::string::npos;
    const bool has_sq = sq != std::string::npos;
    if (!in_triple) {
      if (has_dq || has_sq) {
        const char ch = has_dq ? '"' : '\'';
        // 同一行内出现两次 = 开+闭，不进多行状态
        const std::string token = has_dq ? "\"\"\"" : "\'\'\'";
        const std::size_t first = text.find(token);
        const std::size_t second = text.find(token, first + 3);
        if (second == std::string::npos) {
          in_triple = true;
          triple_char = ch;
        }
        continue;
      }
    } else {
      const std::string token = (triple_char == '"') ? "\"\"\"" : "\'\'\'";
      if (text.find(token) != std::string::npos) {
        in_triple = false;
      }
      continue;
    }
    // 不在字符串里，且缩进 0 → 这是一个语句的开始
    if (text[0] != ' ' && text[0] != '\t') cut = i;
  }
  std::string out;
  for (std::size_t i = 0; i <= cut && i < lines.size(); ++i) {
    out += lines[i];
    out += '\n';
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string path =
      argc > 1 ? argv[1] : "examples/jianmu/scenes/bpy_watch.py";
  const auto source = read_file(path);
  if (source.empty()) {
    std::printf("读不到：%s\n", path.c_str());
    return 1;
  }
  if (!st::mp::Runtime::available()) {
    std::printf("MicroPython 不可用\n");
    return 1;
  }
  auto& runtime = st::mp::Runtime::instance();

  std::size_t total = 0;
  {
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) ++total;
  }
  std::printf("脚本 %zu 行\n\n", total);

  // 线性步进：每 5 行一个探测点，找到**第一次失败**。
  //
  // 为什么不用二分：切点必须落在顶层语句边界（否则得到无关的 SyntaxError），
  // 而二分探到的位置多半落在某个函数体中间。403 行的脚本线性扫一遍也就几秒，
  // 准比快重要。
  std::size_t first_fail = 0;
  for (std::size_t probe = 5; probe <= total; probe += 5) {
    const auto slice = truncate_at_statement(source, probe);
    if (slice.empty()) continue;
    const auto result = runtime.run(slice, "<bisect>");
    if (!result.has_value()) {
      first_fail = probe;
      break;
    }
  }

  if (first_fail == 0) {
    std::printf("全部 %zu 行执行无错（那问题不在脚本语法/名字，而在别处）\n", total);
    return 0;
  }

  // 失败行之前的最后一个"顶层完整语句"位置更准，所以再往前找到缩进 0 的行
  std::printf("首次失败在 ~第 %zu 行\n\n", first_fail);
  const auto slice = truncate_at_statement(source, first_fail);
  const auto result = runtime.run(slice, "<bisect>");
  std::printf("错误：%s\n\n",
              result.has_value() ? "(无，说明边界不准)" : result.error().message.c_str());

  std::istringstream stream(source);
  std::string line;
  std::size_t index = 0;
  const std::size_t from = first_fail > 8 ? first_fail - 8 : 1;
  const std::size_t to = first_fail + 3;
  while (std::getline(stream, line)) {
    ++index;
    if (index >= from && index <= to) {
      std::printf("%4zu%s %s\n", index, index == first_fail ? " >>" : "   ", line.c_str());
    }
    if (index > to) break;
  }
  return 0;
}
