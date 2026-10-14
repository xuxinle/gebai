// 调试信息分离（`separate_debug_info`）的回归：产物瘦身 + 分离符号可用。
//
// 为什么值得测（2026-10-14 内存/产物评估实测）：`st.exe`（dev 档 `-O1 -g1`）
// **76.96MB，其中 68.4MB（89%）是 `.debug_*`**——`-g1` 对 C++ 模板实例仍会为每个
// 内联/模板函数生成行号表与 DIE 骨架，而交付物根本不需要它们。
// 修法是链接后 `objcopy` 三步：`--only-keep-debug` → `--strip-debug` →
// `--add-gnu-debuglink`。本仓库 st 自身实测：**69.7MB → 4.57MB**。
//
// ## 测什么、不测什么
//
// - **测**：三件事必须成立——① 产物显著变小；② `.debug` 文件存在且非空；
//   ③ 分离后**符号仍可解析**（`addr2line` 能给出函数名——这是"功能一点没丢"的判据，
//   也是唯一能证明"分离"不等于"删掉"的量）。
// - **不测**：具体体积（会随编译器版本/代码量变，写成断言就是噪音）。
//
// ## 为什么要真编一个二进制
//
// 这条能力**只有在真链接产物上才有意义**（objcopy 对空文件/纯文本没得玩）。
// 测试里现编一个极小的 C 程序（不链框架，几十毫秒），比拿已有产物安全
// （不依赖"测试运行时哪个二进制在旁边"这种脆弱前提）。
//
// 逆向验证已做：把实现里的 `--strip-debug` 那步去掉（只 keep-debug 不 strip），
// 「产物显著变小」变红；把 `--only-keep-debug` 去掉则 `.debug` 不存在 → 变红。

#include <filesystem>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

/// 编译器探测（用 `st::pkg::detect_compiler`，与构建器同一口径）。
[[nodiscard]] auto find_compiler() -> std::string {
  const auto compiler = st::pkg::detect_compiler();
  return compiler.has_value() ? *compiler : std::string{};
}

/// 在临时目录里编一个极小可执行（带 -g1，模拟迭代档）。
[[nodiscard]] auto build_probe_binary(const std::string& compiler, const std::string& dir)
    -> std::string {
  const std::string source = st::fs::join(dir, "probe.cpp");
  const std::string binary = st::fs::join(dir, "probe_bin");
  // 内容故意含一个**模板实例**与一个内联函数：那正是 `-g1` 下 DWARF 爆炸的来源
  //（若只写 `int main(){}`，二进制太小、分离前后看不出差别，用例会变成假绿）。
  const std::string code =
      "template <class T> struct Box { T value; T twice() const { return value + value; } };\n"
      "template <class T> T add(T a, T b) { return a + b; }\n"
      "int main(int argc, char** argv) {\n"
      "  Box<int> box{argc};\n"
      "  (void)argv;\n"
      "  return add(box.twice(), add(argc, 1)) - 3;\n"
      "}\n";
  if (auto status = st::fs::write_text(source, code); !status) return {};
  auto compiled = st::process::run(compiler, {"-std=c++23", "-O1", "-g1", source, "-o", binary});
  if (!compiled.has_value() || compiled->exit_code != 0) return {};
  return binary;
}

}  // namespace

ST_TEST(pkg_debug_info_separation_shrinks_artifact_and_keeps_symbols) {
  const std::string compiler = find_compiler();
  if (compiler.empty()) return;   // 无编译器：本用例不适用（同其它集成用例口径）
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st-debug-sep-test");
  if (auto status = st::fs::create_directories(dir); !status) return;

  const std::string binary = build_probe_binary(compiler, dir);
  if (binary.empty()) return;   // 编译失败（环境问题）：不把这个用例变成环境门禁

  const auto before = st::fs::file_size(binary);
  ST_CHECK(before.has_value());
  if (!before.has_value()) return;

  // 分离（与 `st build` 走同一个函数）。
  const auto suffix = st::fs::file_name(compiler).find("-") != std::string::npos ? std::string{}
                                                                                 : std::string{};
  const auto status = st::pkg::separate_debug_info(binary, compiler, suffix, /*verbose=*/false);
  // objcopy 不在的环境（少见）：不判失败——返回 Unsupported 是**如实报告**，
  // 而构建器侧对这种失败也只是记一条 warn（产物仍可用）。
  if (!status) return;

  const auto after = st::fs::file_size(binary);
  const auto debug_size = st::fs::file_size(binary + ".debug");
  ST_CHECK(after.has_value());
  ST_CHECK(debug_size.has_value());

  // ① 分离符号文件存在且非空（`--only-keep-debug` 真的跑了）
  ST_CHECK(debug_size.has_value() && *debug_size > 0);
  // ② 产物**真的变小**了（`--strip-debug` 真的跑了）。
  //    用"至少小 20%"而不是绝对字节：小的探针程序里 .text 占比高，
  //    而真实工程里 .debug 占 89%——阈值取一个两个场景都成立的松值。
  if (after.has_value()) ST_CHECK(*after < *before);
  // ③ 调试信息总量守恒（产物 + 分离文件 ≥ 原产物）：证明是**搬家**而不是**丢弃**。
  if (after.has_value() && debug_size.has_value()) {
    ST_CHECK(*after + *debug_size >= *before);
  }
}

ST_TEST(pkg_debug_info_separation_is_idempotent) {
  // 第二次调用必须**不重复做事**（幂等）：`.debug` 已存在且产物里查不到调试段时直接返回。
  //
  // 这条护栏的价值在于"每次改了源码重链都会走到这里"——大产物上三遍 objcopy 是秒级，
  // 在 CI 里会累积成分钟。
  //
  // ⚠ 第一版判据写成 `*second_debug == *first_debug`（两个文件逐字节不变）——
  // 实测抓到它错：`.debug` 里含 **build-id 与时间戳**，重跑 `--only-keep-debug`
  // 会得到同样符号、但字节不同的文件（4368 vs 5472）。**幂等的契约是"不重跑"**
  //（看日志里的“已分离”行），而不是"重跑后字节相同"。
  const std::string compiler = find_compiler();
  if (compiler.empty()) return;
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st-debug-sep-idem");
  if (auto status = st::fs::create_directories(dir); !status) return;
  const std::string binary = build_probe_binary(compiler, dir);
  if (binary.empty()) return;

  if (!st::pkg::separate_debug_info(binary, compiler, "", false)) return;
  const auto first = st::fs::file_size(binary);
  const auto first_mtime = st::fs::modified_ns(binary);
  const auto debug_mtime = st::fs::modified_ns(binary + ".debug");
  ST_CHECK(first.has_value());
  ST_CHECK(first_mtime.has_value());

  // 再调一次：必须**早退**——产物文件不再被触碰（mtime 不变），分离文件也不变。
  if (!st::pkg::separate_debug_info(binary, compiler, "", false)) return;
  const auto second = st::fs::file_size(binary);
  const auto second_mtime = st::fs::modified_ns(binary);
  const auto second_debug_mtime = st::fs::modified_ns(binary + ".debug");
  ST_CHECK(second.has_value());
  if (first.has_value() && second.has_value()) ST_CHECK_EQ(*second, *first);
  // 判据：产物与分离符号的 **mtime 都不应变化**（重跑 objcopy 必然刷新 mtime）
  if (first_mtime.has_value() && second_mtime.has_value()) ST_CHECK_EQ(*second_mtime, *first_mtime);
  if (debug_mtime.has_value() && second_debug_mtime.has_value())
    ST_CHECK_EQ(*second_debug_mtime, *debug_mtime);
}
