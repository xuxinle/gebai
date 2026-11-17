/// 分支/远程的**分类与命名**（纯函数，独立编译单元）。
///
/// ## 为什么单独成文件（而不是留在 `git_service.cpp` 里）
///
/// 这些函数是**纯数据变换**（给一个 `BranchInfo`，回答“它属于哪个 remote、
/// 短名是什么”），既有应用在用、也要被测试直接断言。
///
/// 而 `git_service.cpp` 被 gbcode 目标编译，测试目标再直传一次就是
/// **duplicate symbol**（实测踩过）。测试目标能直接链接的只有“没人引用过”
/// 的编译单元——所以“测试要直接调用的纯函数”必须住在独立文件里。
/// 这也是 `git_graph.cpp` 的同一个道理。

#include "apps/gbcode/git_service.hpp"

namespace gbcode {

auto remote_of_branch(const BranchInfo& branch) -> std::string {
  if (!branch.is_remote) return {};
  // 远程名本身**不含** `?`/`/`：`origin`、`github`、`upstream`。
  // 而分支名**可以**含 `/`（`feature/x`）⇒ 只能按第一个 `/` 切。
  const std::size_t slash = branch.name.find('/');
  if (slash == std::string::npos) return {};
  return branch.name.substr(0, slash);
}

auto short_branch_name(const BranchInfo& branch) -> std::string_view {
  if (!branch.is_remote) return branch.name;
  const std::size_t slash = branch.name.find('/');
  if (slash == std::string::npos) return branch.name;
  return std::string_view(branch.name).substr(slash + 1);
}

}  // namespace gbcode
