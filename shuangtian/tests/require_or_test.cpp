/// `ST_REQUIRE_OR` 的回归用例（BACKLOG 3801-3806）。
///
/// ## 被钉的契约
///
/// `ST_REQUIRE` 的宏体是裸 `return;`——在**值返回函数**（helper/lambda）里直接
/// 编译失败（-fpermissive 也救不了），2026-10-12 写单测亲历被迫手写
/// `if (!ok) return "<failed>"`（失败记录全丢）。`ST_REQUIRE_OR(expr, fallback)`
/// 补齐该形态：失败时**记录失败 + 早退当前函数 + 回 fallback**。
///
/// ## 为什么失败路径必须走子进程（哨兵范式，与 shard_parity/skip_visibility 同款）
///
/// REQUIRE_OR 的失败记录落在**调用方用例**的失败栈里——在套件内直测「失败路径」
/// 必然把承载它的用例自己弄红（那是行为本身，不是缺陷）。所以：哨兵用例在
/// `ST_REQUIRE_OR_PROBE=1` 时故意触发失败路径（预期 FAIL），主用例自启动本二进制
/// 跑哨兵，从 stdout 与退出码取证三件事：
/// 1. 失败真的记上了（「前置断言失败」详情行——亲历丑写法丢掉的正是这一步）；
/// 2. 早退时回的是 fallback（哨兵里若拿不到 fallback 会打出专属败诉消息，
///    主用例断言 stdout 不含它）；
/// 3. 退出码 = 失败用例数（REQUIRE_OR 的早退不会把进程带走，run_all 继续）。
///
/// 通过路径无此问题，主用例内直测（不记失败、返回实际值）。
///
/// ## 与既有惯例的关系
///
/// 仓库里值返回 helper 的既有主流是 `expect()` 型 **void 化 helper**
/// （`ui_terminal_palette_test.cpp` 等 5+ 文件：把断言收进 void helper，值逻辑
/// 留在用例体）。`ST_REQUIRE_OR` 不是替代它，而是给「helper 早退必须带回值」的
/// 场景（如解包、夹具懒加载）一个不丢失败记录的写法。

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/process.hpp"
#include "st/test/test.hpp"

namespace {

/// 哨兵身份开关：只有自启动的子进程才带它（空值条目不当「已设置」）。
[[nodiscard]] bool require_or_probe_armed() {
  const char* value = std::getenv("ST_REQUIRE_OR_PROBE");
  return value != nullptr && *value != '\0';
}

/// 被测形态①：解包 helper——上游值不合法就记失败并回 fallback。
[[nodiscard]] auto probe_unwrap_or(int value, std::string_view fallback) -> std::string_view {
  ST_REQUIRE_OR(value > 0, fallback);
  return "positive";
}

/// 被测形态②：bool helper——早退时回 false 让调用方的后续逻辑短路。
[[nodiscard]] auto probe_positive_or_false(int value) -> bool {
  ST_REQUIRE_OR(value > 0, false);
  return true;
}

/// 自启动本二进制（带哨兵开关），跑哨兵失败路径。
[[nodiscard]] auto run_self_probe() -> st::Result<st::process::RunResult> {
  auto exe = st::process::executable_path();
  if (!exe) return st::unexpected(exe.error());
  st::process::Options options;
  options.env["ST_REQUIRE_OR_PROBE"] = "1";
  return st::process::run(*exe, {"require_or_probe"}, options);
}

}  // namespace

/// 哨兵：故意触发失败路径（预期 FAIL——失败记录落在本用例身上，这正是被测行为）。
ST_TEST(require_or_probe_fails) {
  if (!require_or_probe_armed()) return;
  const std::string_view got = probe_unwrap_or(-1, "<failed>");
  if (got != std::string_view{"<failed>"}) {
    ST_FAIL("REQUIRE_OR 未回 fallback（主用例以 stdout 不含本消息为判据）");
  }
}

/// 主用例：子进程取证失败路径 + 直测通过路径。
ST_TEST(require_or_records_failure_and_returns_fallback) {
  if (require_or_probe_armed()) return;  // 哨兵运行里空转（防重入）

  // —— 失败路径（子进程取证，本用例保持绿）——
  auto ran = run_self_probe();
  ST_REQUIRE(ran.has_value());
  // ① 失败真的记上了：消息前缀「前置断言失败」是 REQUIRE_OR 的 record_failure 落款。
  ST_CHECK(ran->stdout_text.find("前置断言失败") != std::string::npos);
  // ② 哨兵用例被正确判 FAIL（而非静默早退成 PASS）。行级扫描：结果行是
  //    `FAIL<色码> 名字 耗时`——色码夹在标记词与名字之间，不能拼串 find。
  bool saw_fail_line = false;
  bool saw_pass_line = false;
  std::size_t cursor = 0;
  const std::string& text = ran->stdout_text;
  while (cursor < text.size()) {
    const std::size_t end = text.find('\n', cursor);
    const std::size_t line_end = end == std::string::npos ? text.size() : end;
    const std::string_view line(text.data() + cursor, line_end - cursor);
    if (line.find("require_or_probe_fails") != std::string_view::npos) {
      saw_fail_line = saw_fail_line || line.find("FAIL") != std::string_view::npos;
      saw_pass_line = saw_pass_line || line.find("PASS") != std::string_view::npos;
    }
    cursor = end == std::string::npos ? text.size() : end + 1;
  }
  ST_CHECK(saw_fail_line);
  ST_CHECK(!saw_pass_line);
  // ③ 回的是 fallback：哨兵的专属败诉消息没出现（出现了说明早退值不对）。
  ST_CHECK(ran->stdout_text.find("未回 fallback") == std::string::npos);
  // ④ 退出码 = 失败用例数（1）：早退没把进程带走，run_all 正常走完。
  ST_CHECK_EQ(ran->exit_code, 1);

  // —— 通过路径（直测：不记失败、不早退、返回实际值）——
  ST_CHECK_EQ(probe_unwrap_or(1, "<failed>"), std::string_view{"positive"});
  ST_CHECK_EQ(probe_positive_or_false(1), true);
  ST_CHECK(st::test::Registry::instance().failures().empty());
}
