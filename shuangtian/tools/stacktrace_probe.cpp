// `std::stacktrace` 在本仓两个编译器族上的**可用性**探测（不只是"能不能链接"）。
//
// 背景：GCC 把这个设施的实现在 **`libstdc++exp.a`** 里（`stacktrace.o` +
// 15 个 `std_stacktrace-*.o`，内嵌 libbacktrace 的 dwarf/pecoff 解析与 demangle），
// 而不是 Linux 上那个 `libstdc++_libbacktrace.a`。`exp` 库**不自动链接**，
// 所以要显式 `-lstdc++exp`。
//
// 实测（MinGW-w64 GCC 16.2）：加上 `-lstdc++exp` 后能链接、`current()` 也返回了帧，
// 但**所有帧同叫 `register_frame_ctor`**——符号化实际没有工作。由此得出本仓的结论：
// **不把 `std::stacktrace` 接进崩溃报告**，仍走自研路径（DbgHelp / addr2line）。
//
// 注意：同一个 `libstdc++exp` 里的 `std::print` 是**完全可用**的（只需同一个 `-l`）——
// 所以“加 `-lstdc++exp` 就完事”是错的推广，要逐设施验。
//
// 本探针的输出就是那个判据的证据，跑一次即可复核：
//   g++ -std=c++23 -O1 tools/stacktrace_probe.cpp -o /tmp/stp -lstdc++exp && /tmp/stp
//
// 退出码：0 = 可用且符号化正常；1 = 帧数为 0；2 = 帧数有但**全部同名**（符号化失效）；
//         3 = 该编译器族不提供 `<stacktrace>`。
//
// 输出用 `st::print`（框架口径，L7 禁 printf）：格式串在编译期校验，不会出现
// "占位符与实参类型不匹配"这类只有运行期才炸的缺陷。

#include <stacktrace>
#include <string>
#include <vector>

#include <version>

#include "st/core/print.hpp"

namespace {

[[nodiscard]] auto collect() -> std::vector<std::string> {
  std::vector<std::string> descriptions;
#if defined(__cpp_lib_stacktrace)
  for (const auto& entry : std::stacktrace::current()) {
    descriptions.push_back(entry.description());
  }
#endif
  return descriptions;
}

/// 内联一层，让回溯至少有两帧（单帧看不出"层数对不对"）。
[[nodiscard]] auto deepest() -> std::vector<std::string> { return collect(); }

}  // namespace

int main() {
#if defined(__cpp_lib_stacktrace)
  st::print("__cpp_lib_stacktrace = {}\n", static_cast<long>(__cpp_lib_stacktrace));
  const auto frames = deepest();
  for (std::size_t index = 0; index < frames.size(); ++index) {
    st::print("  #{} {}\n", index, frames[index]);
  }
  st::print("frames = {}\n", frames.size());
  if (frames.empty()) {
    st::print("结论：宏在但抓不到帧——不可用\n");
    return 1;
  }
  // 帧**全部同名**说明符号化没生效（帧地址有、名字是同一份误导值）。
  // 这是比"抓不到帧"更危险的状态：看上去有回溯，实际每一行都指同一个地方。
  std::size_t distinct = 0;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    bool seen = false;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (frames[earlier] == frames[index]) {
        seen = true;
        break;
      }
    }
    if (!seen) ++distinct;
  }
  st::print("distinct = {} / {}\n", distinct, frames.size());
  if (distinct <= 1U) {
    st::print("结论：能链接、能取帧，但**符号化失效**（所有帧同名）——不可用于定位缺陷\n");
    return 2;
  }
  st::print("结论：可用（{} 帧 / {} 个不同符号）\n", frames.size(), distinct);
  return 0;
#else
  st::print("__cpp_lib_stacktrace 未定义\n结论：该编译器族不提供 <stacktrace>\n");
  return 3;
#endif
}
