/// 子进程 env 覆盖语义的纯函数回归（BACKLOG 315——POSIX 侧追加式覆盖失效的修复）。
///
/// ## 被钉的契约
///
/// `st::process::merge_environment(base, overrides, case_insensitive)`：
/// 1. **overrides 优先**：base 中键被命中的条目不进结果（同名键至多一份）——
///    旧 POSIX 分支把 overrides 追加在 environ 之后，`getenv` 取首个同名条目，
///    覆盖静默失效；修复后 POSIX `run()` 与 Windows `build_environment` 同语义；
/// 2. **键匹配的平台差异由参数钉住**：Windows 不敏感（`Path`/`PATH` 同键一并剔除）、
///    POSIX 敏感（两个键，`PATH` 被覆盖时 `Path` 原样保留）——这不是“哪边更对”，
///    是两平台环境块的真实语义，测试两侧都跑、各自断言；
/// 3. 未命中的 base 条目原样保留（含无 `=` 的畸形条目——POSIX 允许，不能误删）。
///
/// ## 为什么是纯函数测试而不是 `process::run` 自启动
///
/// `run()` 的 env 注入在 POSIX 分支（Windows 本机编不到/跑不了）；提炼成纯函数后
/// **两平台本机都能跑本用例**（Windows 全量 st test 覆盖语义，Linux/WSL 轮验时同用例
/// 覆盖同语义）。`run()` 到 `merge_environment` 的接线（POSIX 传 false）靠静态核对 +
/// 将来 WSL 轮验——见 BACKLOG 条目验证记录。
///
/// ## 逆向验证口径（CONVENTIONS §7.1：修复回退必须变红）
///
/// 把 `merge_environment` 的剔除行（`if (find...) continue;`）注释掉即回到追加式——
/// 下述“覆盖生效”与“大小写不敏感剔除”两组断言必红；恢复后回绿。（本仓纪律：
/// 落码不跑，逆向验证由控制面执行。）

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../src/core/process_env.hpp"
#include "st/test/test.hpp"

namespace {

/// 便捷断言：结果里键为 `key` 的条目**恰好一条**且值等于 `expected`。
/// “恰好一条”是被测语义本身（追加式的病根就是两份同名键），比“存在一条”强。
void check_single_entry(const std::vector<std::string>& merged, const std::string& key,
                        const std::string& expected) {
  std::size_t count = 0;
  for (const auto& entry : merged) {
    if (st::process::env_entry_key(entry) == key) ++count;
  }
  ST_CHECK_EQ(count, std::size_t{1});
  if (count == 1) {
    ST_CHECK(std::ranges::find(merged, key + "=" + expected) != merged.end());
  }
}

}  // namespace

/// 覆盖生效（BACKLOG 315 的病灶）：同名键只留 overrides 的值，旧条目被剔除。
ST_TEST(env_merge_override_wins_over_base) {
  const std::vector<std::string> base{"PATH=/usr/bin", "HOME=/home/dev", "LANG=C"};
  const std::map<std::string, std::string> overrides{{"PATH", "/opt/toolchain/bin"}};
  const auto merged = st::process::merge_environment(base, overrides, /*case_insensitive=*/false);
  check_single_entry(merged, "PATH", "/opt/toolchain/bin");
  // 未命中的条目原样保留。
  ST_CHECK(std::ranges::find(merged, "HOME=/home/dev") != merged.end());
  ST_CHECK(std::ranges::find(merged, "LANG=C") != merged.end());
  ST_CHECK_EQ(merged.size(), std::size_t{3});
}

/// 新增键（两平台旧实现就对）：overrides 里 base 没有的键直接进入结果。
ST_TEST(env_merge_appends_new_keys) {
  const std::vector<std::string> base{"PATH=/usr/bin"};
  const std::map<std::string, std::string> overrides{{"ST_JUNIT_XML", "/tmp/report.xml"}};
  const auto merged = st::process::merge_environment(base, overrides, /*case_insensitive=*/false);
  check_single_entry(merged, "ST_JUNIT_XML", "/tmp/report.xml");
  check_single_entry(merged, "PATH", "/usr/bin");
  ST_CHECK_EQ(merged.size(), std::size_t{2});
}

/// Windows 语义：键大小写不敏感——base 里 `Path` 被 overrides 的 `PATH` 命中剔除。
ST_TEST(env_merge_case_insensitive_matches_windows) {
  const std::vector<std::string> base{"Path=C:\\Windows", "INCLUDE=C:\\VC\\include"};
  const std::map<std::string, std::string> overrides{{"PATH", "C:\\Toolchain\\bin"}};
  const auto merged = st::process::merge_environment(base, overrides, /*case_insensitive=*/true);
  check_single_entry(merged, "PATH", "C:\\Toolchain\\bin");
  ST_CHECK(std::ranges::find(merged, "INCLUDE=C:\\VC\\include") != merged.end());
  ST_CHECK_EQ(merged.size(), std::size_t{2});
}

/// POSIX 语义：键大小写敏感——`PATH` 被覆盖时 `Path` 是另一个键，原样保留。
ST_TEST(env_merge_case_sensitive_matches_posix) {
  const std::vector<std::string> base{"PATH=/usr/bin", "Path=/usr/local/bin"};
  const std::map<std::string, std::string> overrides{{"PATH", "/opt/toolchain/bin"}};
  const auto merged = st::process::merge_environment(base, overrides, /*case_insensitive=*/false);
  check_single_entry(merged, "PATH", "/opt/toolchain/bin");
  check_single_entry(merged, "Path", "/usr/local/bin");
  ST_CHECK_EQ(merged.size(), std::size_t{2});
}

/// 边界：无 `=` 的畸形条目按整条当键参与匹配（不误删、不崩溃）；空 overrides 原样透传。
ST_TEST(env_merge_handles_malformed_and_empty_overrides) {
  const std::vector<std::string> base{"MALFORMED_NO_EQUALS", "K=v"};
  // 无 `=` 的条目：env_entry_key 返回整条；overrides 里有同名“键”时命中剔除。
  const std::map<std::string, std::string> hit{{"MALFORMED_NO_EQUALS", "now=formed"}};
  const auto merged = st::process::merge_environment(base, hit, /*case_insensitive=*/false);
  check_single_entry(merged, "MALFORMED_NO_EQUALS", "now=formed");
  check_single_entry(merged, "K", "v");
  // 空 overrides：全量透传（POSIX run() 的空 env 走更早的 fast path——直接继承
  // environ 不进本函数；这里钉函数自身的透传语义，与该 fast path 行为一致）。
  const auto passthrough =
      st::process::merge_environment(base, {}, /*case_insensitive=*/false);
  ST_CHECK_EQ(passthrough.size(), std::size_t{2});
  ST_CHECK(passthrough[0] == "MALFORMED_NO_EQUALS");
  ST_CHECK(passthrough[1] == "K=v");
}

/// 键提取：首个 `=` 之前（值里可以再有 `=`——`A=b=c` 的值是 `b=c`）。
ST_TEST(env_entry_key_splits_at_first_equals) {
  ST_CHECK(st::process::env_entry_key("A=b=c") == "A");
  ST_CHECK(st::process::env_entry_key("PLAIN") == "PLAIN");
  ST_CHECK(st::process::env_entry_key("=VALUE") == "");
}
