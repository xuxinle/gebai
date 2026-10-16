// st test 链接失败与用例失败分开报（BACKLOG 3837）的回归：
// is_link_failure 分类边界——哪类错误是「产物没出来」、哪类不是。
//
// 为什么值得测（实测事故，2026-10）：ld.lld「failed to write output … permission
// denied」时 st test 退出码 1，与「恰好 1 个用例失败」无法区分，演进循环把它
// 当测试失败追了一轮。分开报之后：链接失败固定退 125（POSIX「命令不可运行」
// 同款语义，不会与任何失败用例数碰撞），失败用例数仍从 1 起原样退。

#include <format>
#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

using st::ErrorCode;
using st::Error;

struct Case {
  std::string name;
  Error error;
  bool expected;
};

/// 等价于 run_tests 里 return unexpected(...) 的真实消息形态（抽样关键路径）。
/// 错误码保留原始值（link() 内部用 Io/Invalid），分类只看消息前缀。
[[nodiscard]] auto link_error(std::string detail) -> Error {
  return Error{ErrorCode::Io, std::format("测试构建失败（链接段）：{}", detail)};
}

}  // namespace

ST_TEST(pkg_test_link_split_classifies_link_failures) {
  const std::vector<Case> cases = {
      // —— 应判为链接失败（产物没出来/分片没跑完，用例零结果）——
      {"link-write-denied",
       link_error("ld.lld: error: failed to write output "
                  "'C:/…/build/dev/bin/st_tests.exe': permission denied"),
       true},
      {"link-undefined-ref", link_error("undefined reference to `main'"), true},
      {"compile-stage", Error{ErrorCode::Internal, "测试构建失败（编译段）：error: …"}, true},
      {"shard-not-started",
       Error{ErrorCode::Internal, "测试分片 2/16 未能启动: CreateProcess 失败"}, true},
      {"shard-aborted",
       Error{ErrorCode::Internal,
             "测试分片 5/16 异常退出（码 3）且未写出报告：片内用例可能直接 abort/崩溃…"},
       true},
      // —— 不应判为链接失败（非产物问题；退出码保持旧语义）——
      {"manifest-load", Error{ErrorCode::Invalid, "清单加载失败: st.pkg 不存在"}, false},
      {"toolchain-resolve", Error{ErrorCode::Unsupported, "工具链解析失败: 无 clang"}, false},
      {"cross-host", Error{ErrorCode::Unsupported,
                           "交叉编译产物无法在宿主执行: …（请在目标平台运行）"}, false},
      {"empty", Error{}, false},
  };
  for (const auto& item : cases) {
    ST_CHECK_EQ(st::pkg::is_link_failure(item.error), item.expected);
  }
}

ST_TEST(pkg_test_link_split_classifier_depends_on_prefix_not_code) {
  // 分类判据是消息前缀而非 ErrorCode：同是 Internal，分片未启动是链接类
  // （用例零结果），JUnit 合并失败不是（用例已跑完、只是报告没写出来）。
  ST_CHECK(st::pkg::is_link_failure(
      Error{ErrorCode::Internal, "测试分片 1/16 未能启动: 未知原因（进程未返回结果）"}));
  ST_CHECK(!st::pkg::is_link_failure(
      Error{ErrorCode::Internal, "JUnit 报告合并失败: shard-3.xml 损坏"}));
}
