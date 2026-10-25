/// 增量构建的内容哈希判定回归测试。
///
/// ## 为什么这批用例必须存在
///
/// 原来判定只比 `(mtime, size)`，于是**同长度改动 + 保留 mtime** 必然判"没变"。
/// 实测（2026-10-25）把 `render.hpp` 改成 `rendEr.hpp`（等长）+ `os.utime` 回原值：
///
/// - `st build` 报「编译 111ms / 链接已跳过」
/// - `git status` 显示无改动（它也用 stat 缓存）
/// - 而产物仍是**旧代码**
///
/// 三方一起被骗，排查代价极高（我先怀疑布局、再怀疑缓存、最后才查 mtime）。
///
/// ## 判错的两个方向都有代价，所以两个方向都钉
///
/// | 方向 | 后果 |
/// |---|---|
/// | 漏报（该重编没重编） | 跑了旧产物、看到旧结果 → 去查被测代码，白烧几轮 |
/// | 误报（不该重编却重编） | 每次构建全量重编，人开始习惯性加 `--force` |
///
/// ## 为什么用真实文件
///
/// 被测的是"文件的 mtime/size/内容三者组合"。桩掉文件系统就把**被测的那件事**
/// 本身抽掉了（桩回一个恒等值 → 错实现也判绿，本仓库踩过这种假绿）。
/// 所以这里用真实文件、真实写入，并且**显式把 mtime 改回原值**来构造那个窗口。
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>

#include <fcntl.h>
#endif

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

/// 把文件的 mtime 精确改回给定纳秒值。
///
/// ## 为什么不用 `std::filesystem::last_write_time`
///
/// 它收 `file_time_type`，而那个类型的精度**不保证**，实测在 Windows/MinGW 上
/// 落到整秒（设 `...69018570900` 回读成 `...8000000000`）——于是用例在
/// "mtime 是否真的没变"这条前提断言上就红了，根本走不到被测逻辑。
///
/// 这里走平台 API 的单点封装：`st::fs` 里没有"设置 mtime"的公开接口
/// （只有读），所以直接用带 `_WIN32` 分支的最小实现。这不是业务代码，
/// 是测试夹具——平台分支收在用例文件内，不外溢。
void force_mtime_ns(const std::string& path, std::int64_t unix_ns) {
#if defined(_WIN32)
  // Windows 纪元是 1601-01-01，与 Unix 差 11644473600 秒。
  // FILETIME 是 100ns 单位 —— 正好能表达纳秒的前两位，精度够用。
  constexpr std::int64_t kEpochDiff100ns = 11644473600LL * 10'000'000LL;
  const std::int64_t filetime = unix_ns / 100LL + kEpochDiff100ns;
  const std::wstring wide(path.begin(), path.end());   // 测试路径是 ASCII
  const HANDLE handle = ::CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return;
  FILETIME ft{};
  ft.dwLowDateTime = static_cast<DWORD>(filetime & 0xFFFFFFFFLL);
  ft.dwHighDateTime = static_cast<DWORD>((filetime >> 32) & 0xFFFFFFFFLL);
  (void)::SetFileTime(handle, nullptr, nullptr, &ft);
  (void)::CloseHandle(handle);
#else
  (void)path;
  (void)unix_ns;
#endif
}

}  // namespace

/// 同长度改动 + mtime/size 都不变时，**必须**判为不一致。
///
/// 这是那个真实事故的最小复现：内容从 `AAAA` 改成 `BBBB`（等长），
/// mtime 强制改回原值。只比 (mtime, size) 的实现会判"一致"→ 漏报。
ST_TEST(deps_hash_detects_equal_length_change_with_preserved_mtime) {
  TempFile file("content.txt");
  file.write("AAAAAAAA");

  const auto before_time = *st::fs::modified_ns(file.path);
  const auto before_size = *st::fs::file_size(file.path);
  const auto before_hash = st::pkg::file_content_hash(file.path);
  ST_CHECK(before_hash.has_value());

  // 等长替换，然后把 mtime 改回去
  file.write("BBBBBBBB");
  force_mtime_ns(file.path, before_time);

  // 前提校验：若 mtime 或 size 其实变了，这条用例就没测到它该测的东西
  ST_CHECK_EQ(*st::fs::file_size(file.path), before_size);
  ST_CHECK_EQ(*st::fs::modified_ns(file.path), before_time);

  // 被测：带哈希的记录必须判"不一致"
  const bool same = st::pkg::deps_entry_matches(file.path, before_time, before_size, before_hash);
  ST_CHECK(!same);
}

/// 内容没变时不能误判（误报方向）。
///
/// 这条与上一条成对：只有"变了能发现"而没有"没变别乱动"，
/// 一个恒返回 false 的实现也能过 —— 那会让每次构建全量重编。
ST_TEST(deps_hash_accepts_unchanged_content) {
  TempFile file("stable.txt");
  file.write("hello world");
  const auto time = *st::fs::modified_ns(file.path);
  const auto size = *st::fs::file_size(file.path);
  const auto hash = st::pkg::file_content_hash(file.path);
  ST_CHECK(hash.has_value());

  ST_CHECK(st::pkg::deps_entry_matches(file.path, time, size, hash));
}

/// 记录里**没有哈希**（旧格式 `.meta`）时退回 (mtime, size) 判定。
///
/// 这条保证升级兼容：仓库里已有的 `.o` 与 `.meta` 不该因这次改动全部失效
/// （那会让所有人都要全量重编一次，且无法回滚）。
ST_TEST(deps_hash_legacy_entry_falls_back_to_mtime_and_size) {
  TempFile file("legacy.txt");
  file.write("content");
  const auto time = *st::fs::modified_ns(file.path);
  const auto size = *st::fs::file_size(file.path);

  // mtime/size 都一致 + 没有哈希 → 认作一致（与改动前的行为相同）
  ST_CHECK(st::pkg::deps_entry_matches(file.path, time, size, std::nullopt));

  // 但 mtime 不一致时仍然要判"变了"（旧路径的判据不能丢）
  ST_CHECK(!st::pkg::deps_entry_matches(file.path, time + 1'000'000'000, size, std::nullopt));
}

/// size 变了就必须判"变了"，**且不必去读内容**。
///
/// 这一条同时钉住性能前提：日常改动（改了文件 → size 或 mtime 变）走快速路径，
/// 不触发内容哈希。所以"加哈希"不会让正常构建变慢。
ST_TEST(deps_hash_size_change_short_circuits) {
  TempFile file("grow.txt");
  file.write("short");
  const auto time = *st::fs::modified_ns(file.path);
  const auto size = *st::fs::file_size(file.path);
  const auto hash = st::pkg::file_content_hash(file.path);

  file.write("much longer content");
  // size 已变 —— 即使哈希记录是旧的（甚至可以是胡编的）也判"变了"
  ST_CHECK(!st::pkg::deps_entry_matches(file.path, time, size, hash));
  ST_CHECK(!st::pkg::deps_entry_matches(file.path, time, size, std::uint64_t{12345}));
}

/// 同一个文件的不同内容 → 不同哈希；相同内容 → 相同哈希（哈希本身的基本性质）。
///
/// 钉这一条的理由：若 `file_content_hash` 恒返回同一个值，
/// 上面几条"变了能发现"的用例会全部失效而不自知。
ST_TEST(file_content_hash_distinguishes_content) {
  TempFile a("a.txt");
  TempFile b("b.txt");
  a.write("AAAA");
  b.write("BBBB");
  const auto ha = st::pkg::file_content_hash(a.path);
  const auto hb = st::pkg::file_content_hash(b.path);
  ST_CHECK(ha.has_value());
  ST_CHECK(hb.has_value());
  ST_CHECK(*ha != *hb);

  b.write("AAAA");
  const auto hb2 = st::pkg::file_content_hash(b.path);
  ST_CHECK(hb2.has_value());
  ST_CHECK_EQ(*ha, *hb2);
}
