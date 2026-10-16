// 环境合并纯函数的实现：语义契约见头文件（BACKLOG 315——POSIX 覆盖失效的对齐修复）。
#include "process_env.hpp"

#include <algorithm>
#include <format>

#include "st/core/string.hpp"   // ascii_lower：键归一（Windows 大小写不敏感匹配）

namespace st::process {

auto env_entry_key(std::string_view entry) -> std::string_view {
  const std::size_t position = entry.find('=');
  return position == std::string_view::npos ? entry : entry.substr(0, position);
}

auto merge_environment(const std::vector<std::string>& base,
                       const std::map<std::string, std::string>& overrides,
                       bool case_insensitive) -> std::vector<std::string> {
  // overrides 键先归一成"比较键"再比对——大小写不敏感时 `Path`/`PATH` 同键。
  // 归一放收集侧而不是逐条目侧：base 条目可能上百个，overrides 通常个位数，
  // 少的先算，多的复用（std::map 本身已去重，无需再防）。
  std::vector<std::string> replaced;
  replaced.reserve(overrides.size());
  for (const auto& [key, value] : overrides) {
    replaced.push_back(case_insensitive ? st::ascii_lower(key) : key);
  }
  std::vector<std::string> entries;
  entries.reserve(overrides.size() + base.size());
  for (const auto& [key, value] : overrides) {
    entries.push_back(std::format("{}={}", key, value));
  }
  for (const auto& entry : base) {
    const std::string key(env_entry_key(entry));
    const std::string probe = case_insensitive ? st::ascii_lower(key) : key;
    if (std::ranges::find(replaced, probe) != replaced.end()) continue;
    entries.push_back(entry);
  }
  return entries;
}

}  // namespace st::process
