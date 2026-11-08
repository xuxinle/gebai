/// @file component_registry.cpp
/// @brief 组件注册表（进程级；导入期写、之后只读）。
///
/// 设计说明见头文件。这里只记实现上的两个要点。

#include "st/ui/component_registry.hpp"

#include <algorithm>
#include <utility>

#include "st/core/log.hpp"
#include "st/ui/element.hpp"

namespace st::ui::component_registry {
namespace {

/// 注册表本体。
///
/// ## 为什么是函数内 static + `std::vector` + 线性查找
///
/// - **函数内 static**：跨翻译单元的初始化顺序未定是老问题。放在函数内保证
///   "第一次用时已构造"——而第一次用总在 `main` 之后（应用显式导入时）。
/// - **vector 而不是 map**：组件是几十个量级，而造元素的频率是"建界面时"而不是
///   每帧。线性查找更快（无哈希、缓存友好），且**保持登记顺序**（诊断可读）。
// lint-allow: L8 导入期写、之后只读的注册表（不含业务态）
auto entries() -> std::vector<Entry>& {
  static std::vector<Entry> table;
  return table;
}

/// 类型 token → 注册名（同上：导入期写、之后只读）。
auto type_names() -> std::vector<std::pair<const void*, std::string>>& {
  static std::vector<std::pair<const void*, std::string>> table;
  return table;
}

/// 待跑的导入函数（组件头登记；`run_imports` 跑完清空）。
// lint-allow: L8 同上（导入期写、之后只读）
auto imports() -> std::vector<void (*)()>& {
  static std::vector<void (*)()> table;
  return table;
}

}  // namespace

auto add(Entry entry) -> bool {
  if (entry.name.empty() || entry.create == nullptr) return false;
  auto& table = entries();
  for (const auto& existing : table) {
    if (existing.name != entry.name) continue;
    // 已存在：**保留先登记的**，如实报一句（重名是需要修的缺陷，但不该静默丢数据）。
    st::log::warn("[组件注册] 重名 '{}'（保留先登记的那一个）", entry.name);
    return false;
  }
  table.push_back(std::move(entry));
  return true;
}

auto create(std::string_view name) -> std::unique_ptr<Element> {
  for (const auto& entry : entries()) {
    if (entry.name == name) return entry.create();
  }
  return nullptr;
}

auto names() -> std::vector<std::string> {
  std::vector<std::string> result;
  const auto& table = entries();
  result.reserve(table.size());
  for (const auto& entry : table) result.push_back(entry.name);
  return result;
}

auto contains(std::string_view name) -> bool {
  return std::ranges::any_of(entries(), [name](const Entry& entry) {
    return entry.name == name;
  });
}

void add_type_name(const void* token, std::string_view name) {
  if (token == nullptr || name.empty()) return;
  auto& table = type_names();
  for (const auto& [existing, slot] : table) {
    if (existing == token) return;  // 同一类型：保留第一次（那是它与组件头的绑定）
  }
  table.emplace_back(token, std::string(name));
}

auto type_name_of(const void* token) -> std::string {
  if (token == nullptr) return {};
  for (const auto& [existing, name] : type_names()) {
    if (existing == token) return name;
  }
  return {};
}

void add_import(void (*run)()) {
  if (run == nullptr) return;
  auto& table = imports();
  // 幂等：同一个登记函数（多个 TU 都 include 了组件头并导入）只登一次
  if (std::ranges::find(table, run) != table.end()) return;
  table.push_back(run);
}

void run_imports() {
  auto& table = imports();
  // 快照 + 清空：避免导入过程中又登记（那会是递归导入）导致迭代器失效
  const std::vector<void (*)()> snapshot = table;
  table.clear();
  for (auto run : snapshot) {
    if (run != nullptr) run();
  }
}

}  // namespace st::ui::component_registry
