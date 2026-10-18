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
#include "st/pkg/manifest.hpp"
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

/// **Windows 目标可以声明 GUI 子系统与 exe 图标**（回归：用户报「release 版本要
/// WinMain 链接、不要控制台窗口、exe 图标也要有」）。
///
/// 缺陷形态（2026-10-18 实测）：`build/dev/bin/gbcode.exe` 的 PE 头
/// `Subsystem = 3 (CONSOLE)`——双击先弹一个黑框；且全仓没有任何 `.rc`/`.ico`，
/// 资源管理器里显示的是空白默认图标。
///
/// 这一层只能测**清单解析**（真正的链接结果要看 PE 头）。但解析错了一样会让
/// 上游静默走 console：字段名少一个字母、默认值写反，链接器就会照旧产出
/// Subsystem=3 的产物，**不报任何错**。所以把解析口径钉住。
namespace {

[[nodiscard]] auto parse_target_spec(std::string_view json_text)
    -> st::Result<st::pkg::Manifest> {
  const auto json = st::json_parse(json_text);
  if (!json) return st::unexpected(st::ErrorCode::Parse, "测试内 JSON 写错了");
  return st::pkg::Manifest::parse_json(*json, ".");
}

}  // namespace

ST_TEST(manifest_target_system_and_icon_are_parsed) {
  // ① 缺省是 console（保持既有行为不变——工具链/探测程序不该被静默改成 GUI）。
  {
    const auto parsed = parse_target_spec(R"({
      "name": "demo", "version": "0.1.0", "kind": "static_library",
      "targets": { "tool": { "kind": "executable", "sources": ["a.cpp"] } }
    })");
    ST_REQUIRE(parsed.has_value());
    ST_REQUIRE(parsed->targets.size() == 1);
    ST_CHECK_EQ(parsed->targets[0].system, std::string{"console"});
    ST_CHECK(parsed->targets[0].icon.empty());
  }

  // ② 显式 gui + icon 要原样读进来（链接层据此加 `-mwindows` 与资源对象）。
  {
    const auto parsed = parse_target_spec(R"({
      "name": "demo", "version": "0.1.0", "kind": "static_library",
      "targets": { "app": { "kind": "executable", "sources": ["a.cpp"],
                            "system": "gui", "icon": "assets/app.ico" } }
    })");
    ST_REQUIRE(parsed.has_value());
    ST_REQUIRE(parsed->targets.size() == 1);
    ST_CHECK_EQ(parsed->targets[0].system, std::string{"gui"});
    ST_CHECK_EQ(parsed->targets[0].icon, std::string{"assets/app.ico"});
  }

  // ③ 拼错的 system 必须**报错**而不是静默回退到 console。
  //    为何这条重要：写成 "GUI"（大写）或 "windows" 都很自然，而静默回退的
  //    表现是“我明明配了 gui、怎么还弹黑框”——排查方向会被完全带偏。
  {
    const auto parsed = parse_target_spec(R"({
      "name": "demo", "version": "0.1.0", "kind": "static_library",
      "targets": { "app": { "kind": "executable", "sources": ["a.cpp"],
                            "system": "GUI" } }
    })");
    ST_CHECK(!parsed.has_value());
  }
}
