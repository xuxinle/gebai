// 编译器族差异层测试：族判定、PCH 消费标志、链接参数。
//
// 为什么这些必须有测试：它们错了不会让构建失败，而是让构建**静默用错口径**——
// 例如把 GCC 的 `-include` 漏给另一族、或把 `-ldl` 发给 Windows 目标。
// 这类错误在编译/链接期才现形，且报错位置离真正原因很远。

#include "st/pkg/compiler.hpp"
#include "st/test/test.hpp"

namespace {

using st::pkg::CompilerKind;
using st::pkg::SourceLanguage;

[[nodiscard]] auto has(const std::vector<std::string>& args, std::string_view flag) -> bool {
  return std::ranges::find(args, flag) != args.end();
}

}  // namespace

ST_TEST(compiler_kind_detection) {
  ST_CHECK(st::pkg::compiler_kind_of("g++").value() == CompilerKind::Gcc);
  ST_CHECK(st::pkg::compiler_kind_of("x86_64-w64-mingw32-g++").value() == CompilerKind::Gcc);
  ST_CHECK(st::pkg::compiler_kind_of("clang++").value() == CompilerKind::Clang);
  ST_CHECK(st::pkg::compiler_kind_of("C:/program/clang+llvm-23.1.2/bin/clang++.exe").value() ==
           CompilerKind::Clang);
}

ST_TEST(compiler_kind_rejects_clang_cl) {
  // `clang-cl` 是 MSVC 口径（`/std:c++20`、`/I`、`/WX`）：本层不支持，**必须明确拒绝**。
  // 为什么不能"当成 Clang 凑合"：会拿 `-std=c++20` 去喂它，报一堆与真正原因
  // （用错了家族）相隔很远的错——这正是本框架反复踩过的那类"报错位置骗人"。
  ST_CHECK(!st::pkg::compiler_kind_of("clang-cl").has_value());
  ST_CHECK(!st::pkg::compiler_kind_of("C:/LLVM/bin/clang-cl.exe").has_value());
}

ST_TEST(compiler_kind_name_covers_every_family) {
  // 名字进缓存键与日志：漏一族会让两个不同的族共享同一个键（对象不兼容）
  ST_CHECK_EQ(std::string(st::pkg::compiler_kind_name(CompilerKind::Gcc)), std::string("gcc"));
  ST_CHECK_EQ(std::string(st::pkg::compiler_kind_name(CompilerKind::Clang)), std::string("clang"));
}

// PCH 消费端标志：GCC 与 Clang 同一拼法（`-I` 指向 .gch 所在目录 + `-include` 代理头）
ST_TEST(pch_consume_args_uses_gcc_style) {
  const auto gcc = st::pkg::pch_consume_args("build/dev/pch", "prefix.hpp");
  ST_CHECK(has(gcc, "-Ibuild/dev/pch/"));
  ST_CHECK(has(gcc, "-include"));
  ST_CHECK(has(gcc, "prefix.hpp"));
  // `-include` 与头名必须是**相邻两项**：拆开就成了两个参数，`prefix.hpp` 会被当成源文件
  const auto include_at = std::ranges::find(gcc, "-include");
  ST_REQUIRE(include_at != gcc.end());
  ST_REQUIRE(std::next(include_at) != gcc.end());
  ST_CHECK_EQ(*std::next(include_at), std::string("prefix.hpp"));

  // 目录已带尾分隔符时不双写（`-Ibuild/dev/pch//` 是合法但难看、且会让缓存键与日志发散）
  const auto trailing = st::pkg::pch_consume_args("build/dev/pch/", "prefix.hpp");
  ST_CHECK(has(trailing, "-Ibuild/dev/pch/"));
  ST_CHECK(!has(trailing, "-Ibuild/dev/pch//"));

  // 反斜杠分隔也认（Windows 上清单/拼接可能给出反斜杠）
  const auto backslash = st::pkg::pch_consume_args("build\\dev\\pch\\", "prefix.hpp");
  ST_CHECK(has(backslash, "-Ibuild\\dev\\pch\\"));
}

ST_TEST(compiler_link_libraries_drops_posix_on_windows) {
  // Windows 目标：mingw 没有 libdl/libm/libpthread，照传就是 `cannot find -ldl`
  const auto windows =
      st::pkg::link_library_arguments("windows", {"pthread", "dl", "m", "ws2_32", "user32"});
  ST_CHECK(!has(windows, "-lpthread"));
  ST_CHECK(!has(windows, "-ldl"));
  ST_CHECK(!has(windows, "-lm"));
  ST_CHECK(has(windows, "-lws2_32"));
  ST_CHECK(has(windows, "-luser32"));

  // 反向：Windows 专属库不能发给非 Windows 目标
  const auto linux = st::pkg::link_library_arguments("linux", {"ws2_32", "pthread", "dl", "m"});
  ST_CHECK(!has(linux, "-lws2_32"));
  ST_CHECK(has(linux, "-lpthread"));
  ST_CHECK(has(linux, "-ldl"));
  ST_CHECK(has(linux, "-lm"));
}

ST_TEST(dialect_flags_empty_for_gcc_and_clang) {
  // 两族都不需要恒定追加标志：`-std=c++20` 由清单给，编码按 UTF-8 读源码。
  // 若将来某族需要（如 `/utf-8`），这里必须有测试跟着改——"空"是一个**结论**，不是缺省。
  ST_CHECK(st::pkg::dialect_flags(CompilerKind::Gcc, SourceLanguage::Cxx).empty());
  ST_CHECK(st::pkg::dialect_flags(CompilerKind::Clang, SourceLanguage::Cxx).empty());
  ST_CHECK(st::pkg::dialect_flags(CompilerKind::Gcc, SourceLanguage::C).empty());
  ST_CHECK(st::pkg::dialect_flags(CompilerKind::Clang, SourceLanguage::C).empty());
}

ST_TEST(strip_sanitizers_removes_instrumentation_flags) {
  const std::vector<std::string> flags{"-O1", "-g1", "-fsanitize=address,undefined",
                                       "-fno-sanitize-recover=all", "-Wall", "-fsanitize-recover"};
  const auto stripped = st::pkg::strip_sanitizers(flags);
  ST_CHECK(!has(stripped, "-fsanitize=address,undefined"));
  ST_CHECK(!has(stripped, "-fno-sanitize-recover=all"));
  ST_CHECK(!has(stripped, "-fsanitize-recover"));
  // 其余一个不能丢（丢了就改变了那个单元的语言/告警口径）
  ST_CHECK(has(stripped, "-O1"));
  ST_CHECK(has(stripped, "-g1"));
  ST_CHECK(has(stripped, "-Wall"));
  ST_CHECK_EQ(stripped.size(), std::size_t{3});
}
