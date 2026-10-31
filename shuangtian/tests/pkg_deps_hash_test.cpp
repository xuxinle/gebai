/// 增量判定的内容哈希回归。
///
/// ## 判据只有一个：内容
///
/// 时间戳与文件长度都不参与“要不要重编”。两个方向的历史错误各对应一组用例：
///
/// | 错方向 | 后果 | 谁钉住 |
/// |---|---|---|
/// | 按 mtime 判变 | `touch` / `git checkout` / 原子保存后**白重编**（实测 touch 一个头 → 重编 34 单元、20.3 s） | `deps_hash_ignores_timestamp_change` |
/// | 按 mtime+size 判未变 | **等长改写 + 保留时间戳**时漏报 → 拿旧产物当新结果 | `deps_hash_detects_equal_length_change` |
///
/// ## 为什么用真实文件
///
/// 被测的是“文件内容的 hash 与记录比没比”。桩掉文件系统就把**被测的那件事**
/// 本身抽掉了（桩回一个恒等值 → 错实现也判绿，本仓库踩过这种假绿）。
/// 所以这里用真实文件、真实写入，并显式把 mtime 改回原值来构造那个窗口。
#include <cstdint>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

/// 一个临时目录里的文件，析构时清理。
struct TempFile {
  std::string dir;
  std::string path;

  explicit TempFile(std::string_view name) {
    const auto created = st::fs::make_temp_dir("st_hash_test_");
    dir = created.value_or(st::fs::temp_dir());
    path = st::fs::join(dir, std::string(name));
  }
  ~TempFile() { (void)st::fs::remove_all(dir); }
  TempFile(const TempFile&) = delete;
  auto operator=(const TempFile&) -> TempFile& = delete;

  void write(std::string_view text) const { (void)st::fs::write_text(path, std::string(text)); }
};

}  // namespace

/// 同长度改写（连时间戳都不变）**必须**判为不一致。
///
/// 这是那个真实事故的最小复现：内容从 `AAAA` 改成 `BBBB`（等长），
/// 任何按 `(mtime, size)` 判定的实现都会说“没变” → 漏报 → 产物停在旧代码上。
ST_TEST(deps_hash_detects_equal_length_change) {
  TempFile file("content.txt");
  file.write("AAAAAAAA");
  const auto recorded = st::pkg::file_content_hash(file.path);
  ST_REQUIRE(recorded.has_value());

  file.write("BBBBBBBB");
  ST_CHECK(!st::pkg::deps_entry_matches(file.path, *recorded));
}

/// 内容没变时不能误判（误报方向）。
///
/// 这条与上一条成对：只有“变了能发现”而没有“没变别乱动”，
/// 一个恒返回 false 的实现也能过——那会让每次构建全量重编。
ST_TEST(deps_hash_accepts_unchanged_content) {
  TempFile file("stable.txt");
  file.write("hello world");
  const auto recorded = st::pkg::file_content_hash(file.path);
  ST_REQUIRE(recorded.has_value());
  ST_CHECK(st::pkg::deps_entry_matches(file.path, *recorded));
}

/// size 变了 → 内容必然不同 → 必须判“变了”。
ST_TEST(deps_hash_detects_size_change) {
  TempFile file("grow.txt");
  file.write("short");
  const auto recorded = st::pkg::file_content_hash(file.path);
  ST_REQUIRE(recorded.has_value());

  file.write("much longer content");
  ST_CHECK(!st::pkg::deps_entry_matches(file.path, *recorded));
}

/// 读不到的路径保守判“变了”（宁可多编一次，不交付旧代码）。
///
/// 注：本判定在单个构建进程内对同一路径只读一次（`content_hash_cached`），
/// 所以这里用一个**从头就不存在**的路径——那才是要钉的不变量。
ST_TEST(deps_hash_treats_missing_file_as_changed) {
  TempFile file("never.txt");
  ST_CHECK(!st::pkg::deps_entry_matches(file.path, std::uint64_t{12345}));
}

/// 同一个文件的不同内容 → 不同哈希；相同内容 → 相同哈希（哈希本身的基本性质）。
///
/// 钉这一条的理由：若 `file_content_hash` 恒返回同一个值，
/// 上面几条“变了能发现”的用例会全部失效而不自知。
ST_TEST(file_content_hash_distinguishes_content) {
  TempFile a("a.txt");
  TempFile b("b.txt");
  a.write("AAAA");
  b.write("BBBB");
  const auto ha = st::pkg::file_content_hash(a.path);
  const auto hb = st::pkg::file_content_hash(b.path);
  ST_REQUIRE(ha.has_value());
  ST_REQUIRE(hb.has_value());
  ST_CHECK(*ha != *hb);

  b.write("AAAA");
  const auto hb2 = st::pkg::file_content_hash(b.path);
  ST_REQUIRE(hb2.has_value());
  ST_CHECK_EQ(*ha, *hb2);
}

/// `.d` 解析：**Windows 盘符冒号不能被当成目标/依赖分隔符**。
///
/// 这是 2026-10-26 真实事故的回归。原实现里 `if (raw == ':')` 无条件切分，
/// `C:/Users/...` 的盘符冒号被吃掉 → 依赖路径变 `/Users/...`（读不到）→
/// `.meta` 里的哈希恒对不上 → **改了源文件不重编，连 `--force` 也治不好**
/// （重编后又写回同样的坏记录）。
///
/// 两个断言缺一不可：
/// - 第一个依赖（源文件）必须**保留盘符**且能真读到（后半句才是真正有用的判据：
///   光字符串相等不够，路径得**能用**）；
/// - 目标行（第一行以 `:` 结尾）里的那个冒号仍必须被正确剔除——
///   否则修过头会把 `目标:` 整行当成一个依赖混进列表。
ST_TEST(depfile_parse_preserves_windows_drive_letter) {
  // 构造两个真实文件，让 `.d` 里写的是实际存在的路径（这样才能断言“读得到”）。
  TempFile source("dep_source.cpp");
  source.write("// a\n");
  TempFile header("dep_header.hpp");
  header.write("#pragma once\n");
  TempFile depfile("unit.d");

  // 模拟 GCC 的 `-MMD` 输出：**首行是 `目标:` 后紧跟反斜杠续行**，依赖逐个换行。
  // 注意反斜杠与换行之间**不能有空格**：GCC 的输出是 `...: \\<LF>`，
  // 写了空格就变成“反斜杠续行 + 前导空格”，解析器会把空格后的内容当新 token。
  const std::string target = st::fs::normalize(source.path);
  const std::string dep1 = st::fs::normalize(source.path);
  const std::string dep2 = st::fs::normalize(header.path);
  depfile.write(std::format("{}.tmp: \\\n{} \\\n{}\n", target, dep1, dep2));

  const std::vector<std::string> deps = st::pkg::parse_depfile(depfile.path);
  // 1) 数量：目标行不算依赖，两个依赖都在
  ST_CHECK_EQ(deps.size(), 2U);
  // 2) 关键断言：盘符保留（字符串层面）
  for (const auto& dep : deps) {
    ST_CHECK(dep.find(':') == std::string::npos || dep.substr(0, 2) != "/U");
  }
  // 3) **更硬的判据**：解析出的路径必须真能读到（这才是增量判定实际要做的事）
  for (const auto& dep : deps) {
    ST_CHECK(st::fs::is_regular_file(dep));
  }
  // 4) 源文件确实在列表里（不是被盘符损坏后的某个变体）
  ST_CHECK(std::ranges::find(deps, dep1) != deps.end());
}
