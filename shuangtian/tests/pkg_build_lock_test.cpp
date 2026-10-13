// 链接诊断「产物被占用」判定的回归：三套工具链文案各命中、近似文案不误报。
//
// 为什么值得测（实测事故，2026-10-13）：MinGW 宿主上产物被残留进程占用时
// ld.lld 报 `failed to write output '…exe': permission denied`——不在旧的检测
// 清单里（只有 MSVC 的 LNK1168/1104 与 POSIX 的 Text file busy），于是用户
// 看到的是一句裸「链接失败」，想不到要去结束还在跑的 gbcode 无头实例。
// 判定漏一套文案 = 排障多绕一轮，这就是护栏要拦的东西。
//
// 逆向验证已做：把 MinGW 分支临时去掉 → mingw_cases 三条断言变红。

#include <string>
#include <vector>

#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

struct Case {
  std::string name;
  std::string diagnostics;
  bool expected;
};

}  // namespace

ST_TEST(pkg_link_lock_detects_all_toolchains) {
  const std::vector<Case> cases = {
      // —— MSVC（LNK1168/LNK1104）——
      {"msvc-1168",
       "main.obj : fatal error LNK1168: 无法打开 …gbcode.exe 进行写入",
       true},
      {"msvc-1104", "fatal error LNK1104: 无法打开文件“app.exe”", true},
      // —— POSIX ——
      {"posix-text-busy", "/usr/bin/ld: cannot open output file app: Text file busy", true},
      // —— MinGW（ld/lld，实测文案）——
      {"mingw-lld",
       "ld.lld: error: failed to write output "
       "'C:/…/build/dev/bin/gbcode.exe': permission denied",
       true},
      {"mingw-bfd",
       "c:/mingw/bin/ld.exe: cannot open output file gbcode.exe: Permission denied",
       false},   // BFD ld 的「cannot open … Permission denied」——保守不判（见下）
      // —— 不应误报的近似文案 ——
      {"plain-permission-denied", "cc1plus.exe: error: permission denied", false},
      {"unrelated", "undefined reference to `main'", false},
      {"empty", "", false},
  };
  for (const auto& item : cases) {
    ST_CHECK_EQ(st::pkg::artifact_locked(item.diagnostics), item.expected);
  }
}

ST_TEST(pkg_link_lock_requires_both_mingw_keywords) {
  // MinGW 判定要求「failed to write output」与「permission denied」**同时**出现：
  // 单独的 permission denied 更可能是目录权限问题（下一步不是「结束实例」，误导）。
  ST_CHECK(!st::pkg::artifact_locked("failed to write output 'x.exe'"));
  ST_CHECK(!st::pkg::artifact_locked("error: permission denied somewhere"));
  ST_CHECK(st::pkg::artifact_locked(
      "failed to write output 'x.exe': permission denied"));
  // 顺序无关（诊断里两段可以分开出现）。
  ST_CHECK(st::pkg::artifact_locked(
      "warning: something\npermission denied …\nld: failed to write output 'x.exe'"));
}
