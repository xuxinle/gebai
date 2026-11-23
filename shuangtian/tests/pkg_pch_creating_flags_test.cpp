// ============================================================================
// PCH 创建标志的剔宏规则——回归用例
//
// ## 钉住什么
//
// PCH 能跨目标复用的**唯一前提**：创建时剔掉逐目标宏（`ST_NO_WEBVIEW` /
// `ST_NO_SCRIPT` / `ST_NO_THREE`）。GCC 对 PCH 的匹配是严格的——创建与消费
// 标志不一致就**静默丢弃**，不报错、不变慢之外的任何现象，只是"PCH 开了但完全
// 没用上"。
//
// 这正是 2026-11-22 那次错误结论的成因：那批测量里 PCH **从头到尾没被采纳**
// （`not used because 'NTDDI_VERSION' not defined`），量到的是"白读 108 MB 再扔掉"，
// 于是得出"PCH 是净损失 +13%"的错误结论，还据此把默认值改成了关。
//
// 所以这里逐条钉：
//   ① 三个能力宏必须被剔；
//   ② **不能误剔**全局宏（`ST_VERSION` / `ST_ENABLE_LINT`）——
//      用 `ST_` 前缀猜会连它们一起剔掉；
//   ③ 不能剔影响语义的项（`-std=` / `-O1` / `-DNTDDI_VERSION` / `-I`）；
//   ④ 剔后顺序保持、其余标志一个不少。
// ============================================================================

#include <string>
#include <vector>

#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::pch_creating_flags;

[[nodiscard]] auto contains(const std::vector<std::string>& flags, const std::string& needle)
    -> bool {
  for (const auto& flag : flags) {
    if (flag == needle || flag.starts_with(needle + "=")) return true;
  }
  return false;
}

}  // namespace

ST_TEST(pkg_pch_creating_flags_strips_capability_macros) {
  // ① 三个能力拆链宏都是"逐目标不同"的（gallery 有、xiangqi 没有），必须剔。
  const std::vector<std::string> flags{
      "-std=c++23",
      "-DST_NO_WEBVIEW=1",
      "-DST_NO_SCRIPT=1",
      "-DST_NO_THREE=1",
      "-O1",
  };
  const auto result = pch_creating_flags(flags);
  ST_CHECK(!contains(result, "-DST_NO_WEBVIEW"));
  ST_CHECK(!contains(result, "-DST_NO_SCRIPT"));
  ST_CHECK(!contains(result, "-DST_NO_THREE"));
  // 非能力项必须原样保留
  ST_CHECK(contains(result, "-std=c++23"));
  ST_CHECK(contains(result, "-O1"));
  ST_CHECK_EQ(result.size(), std::size_t{2});
}

ST_TEST(pkg_pch_creating_flags_keeps_global_macros) {
  // ② **不能误剔**：这两个是全局宏（所有目标一致），剔了会改变 PCH 内容。
  // 早期用 `ST_` 前缀猜的实现会连它们一起剔——这条就是防那个回归。
  const std::vector<std::string> flags{
      "-DST_VERSION=\"0.1.0\"",
      "-DST_ENABLE_LINT=1",
  };
  const auto result = pch_creating_flags(flags);
  ST_CHECK_EQ(result.size(), std::size_t{2});
  ST_CHECK(contains(result, "-DST_VERSION"));
  ST_CHECK(contains(result, "-DST_ENABLE_LINT"));
}

ST_TEST(pkg_pch_creating_flags_keeps_semantic_flags) {
  // ③ 这些决定 PCH 里头看到什么声明/API，剔了会产生**错误的 PCH**：
  // `_WIN32_WINNT` 影响 `<windows.h>` 可见的 APIs，`-std=` 影响语言模式，
  // `-I` 影响头搜索路径。
  const std::vector<std::string> flags{
      "-std=c++23",
      "-D_WIN32_WINNT=0x0A00",
      "-DNTDDI_VERSION=0x0A000006",
      "-Iinclude",
      "-O1",
      "-g1",
      "-ffunction-sections",
  };
  const auto result = pch_creating_flags(flags);
  ST_CHECK_EQ(result.size(), flags.size());  // 一个都不该少
  ST_CHECK(contains(result, "-D_WIN32_WINNT"));
  ST_CHECK(contains(result, "-DNTDDI_VERSION"));
  ST_CHECK(contains(result, "-Iinclude"));
}

ST_TEST(pkg_pch_creating_flags_preserves_order_and_empties) {
  // ④ 顺序必须保持（编译标志的顺序即语义：后者覆盖前者），空输入返回空。
  const std::vector<std::string> flags{"-std=c++23", "-DST_NO_THREE=1", "-O2", "-Wall"};
  const auto result = pch_creating_flags(flags);
  ST_CHECK_EQ(result.size(), std::size_t{3});
  ST_CHECK_EQ(result[0], std::string("-std=c++23"));
  ST_CHECK_EQ(result[1], std::string("-O2"));
  ST_CHECK_EQ(result[2], std::string("-Wall"));

  const std::vector<std::string> empty{};
  ST_CHECK(pch_creating_flags(empty).empty());
}

ST_TEST(pkg_pch_creating_flags_handles_valueless_define) {
  // `-DNAME`（无 `=`）与 `-DNAME=VALUE` 两种写法都要正确识别。
  // 能力宏实际总是带 `=1`，但没带等号时也必须剔——否则会出现
  // "改了个写法 PCH 就失效"的隐性耦合。
  const std::vector<std::string> flags{"-DST_NO_SCRIPT", "-DST_NO_THREE=1", "-O1"};
  const auto result = pch_creating_flags(flags);
  ST_CHECK_EQ(result.size(), std::size_t{1});
  ST_CHECK_EQ(result[0], std::string("-O1"));
}

ST_TEST(pkg_pch_creating_flags_is_idempotent) {
  // 剔两次等于剔一次（`stamp` 计算里会被重复调用，不能因此漂移）。
  const std::vector<std::string> flags{"-std=c++23", "-DST_NO_WEBVIEW=1", "-O1"};
  const auto once = pch_creating_flags(flags);
  const auto twice = pch_creating_flags(once);
  ST_CHECK_EQ(once.size(), twice.size());
  for (std::size_t index = 0; index < once.size(); ++index) ST_CHECK_EQ(once[index], twice[index]);
}
