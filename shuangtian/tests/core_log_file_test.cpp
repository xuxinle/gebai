/// 日志落盘与崩溃尾部采样。
///
/// 这几条测的是**出事后才有价值**的能力，所以每条都要钉住一个具体的失效方式，
/// 而不是"调了没崩"。两个真实踩到的坑都在下面：
///
/// ① **文件名不能含 `:`**——Windows 上 `open` 直接失败，且失败发生在启动时、
///    用户看不到，后果是"崩溃报告永远不生成"而毫无提示。
/// ② **尾部采样必须能"放弃"**——崩溃时若尾部锁被别的线程占着，阻塞等锁
///    就等于永远拿不到（`try_recent_tail` 存在的全部理由）。

#include "st/test/test.hpp"

#include <filesystem>
#include <string>

#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/log_file.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"

namespace {

/// 日志级别作用域（离开时复原，避免影响同进程的其它用例）。
class LevelScope {
 public:
  explicit LevelScope(st::log::Level level) : previous_(st::log::level()) {
    st::log::set_level(level);
  }
  ~LevelScope() { st::log::set_level(previous_); }
  LevelScope(const LevelScope&) = delete;
  auto operator=(const LevelScope&) -> LevelScope& = delete;

 private:
  st::log::Level previous_;
};

/// 每条用例独立的临时目录（用 pid + 名字，避免并行分片互相踩）。
[[nodiscard]] auto scratch_dir(const std::string& name) -> std::string {
  const auto base = std::filesystem::temp_directory_path() /
                    std::format("st_logfile_{}_{}", static_cast<int>(st::process::current_id()),
                                name);
  std::error_code error;
  std::filesystem::remove_all(base, error);
  std::filesystem::create_directories(base, error);
  return base.string();
}

}  // namespace

ST_TEST(log_file_writes_and_flushes_on_close) {
  LevelScope scope(st::log::Level::Info);
  const std::string dir = scratch_dir("basic");
  const std::string path = std::format("{}/app.log", dir);
  st::log::FileOptions options{};
  options.path = path;
  ST_REQUIRE(st::log::open_file(options));
  ST_CHECK(st::log::file_active());

  st::log::info("落盘探针 {}", 42);
  st::log::close_file();
  ST_CHECK(!st::log::file_active());

  // 真读回磁盘——只看 `file_active()` 不能证明"内容到了文件里"。
  const auto text = st::fs::read_text(path);
  ST_REQUIRE(text.has_value());
  ST_CHECK(text->find("落盘探针 42") != std::string::npos);
}

ST_TEST(log_file_rotates_and_keeps_bounded_files) {
  LevelScope scope(st::log::Level::Info);
  const std::string dir = scratch_dir("rotate");
  const std::string path = std::format("{}/app.log", dir);
  st::log::FileOptions options{};
  options.path = path;
  options.max_bytes = 512;   // 故意小：几条就滚一次
  options.keep_files = 2;
  ST_REQUIRE(st::log::open_file(options));
  for (int index = 0; index < 60; ++index) st::log::info("填充 {} {}", index, std::string(40, 'x'));
  st::log::close_file();

  // 滚动后的三档都在（当前 + 2 档历史），且**当前那档仍是最新路径**——
  // 这是"脚本不必猜文件名"的前提。
  ST_CHECK(st::fs::is_regular_file(path));
  ST_CHECK(st::fs::is_regular_file(std::format("{}/app.1.log", dir)));
  ST_CHECK(st::fs::is_regular_file(std::format("{}/app.2.log", dir)));
  // 最老一档应被丢弃（keep_files=2 → 不该出现 app.3.log）。
  ST_CHECK(!st::fs::is_regular_file(std::format("{}/app.3.log", dir)));
}

ST_TEST(log_file_open_failure_is_not_fatal) {
  LevelScope scope(st::log::Level::Info);
  // 目标是一个**目录**：两个平台上 `fopen(..., "ab")` 都不能把目录当文件打开。
  //（最初写的 `/proc/self/...` 在 Linux 上必败、在 Windows 上却被当成相对路径
  //  正常建了出来——用例于是成了“环境依赖”而不是契约断言。）
  const std::string dir = scratch_dir("openfail");
  st::log::FileOptions options{};
  options.path = dir;
  const bool opened = st::log::open_file(options);
  ST_CHECK(!opened);
  ST_CHECK(!st::log::file_active());
  // 打一条日志仍要能走通（降级到 stderr，不因为没文件而丢日志）。
  st::log::info("落盘失败时仍可记录");
  st::log::close_file();
}

ST_TEST(log_tail_capacity_is_bounded_and_returns_latest) {
  LevelScope scope(st::log::Level::Info);
  st::log::set_tail_capacity(8);
  for (int index = 0; index < 40; ++index) st::log::info("尾部探针 {}", index);

  const std::string tail = st::log::detail::try_recent_tail(8);
  // 最新一条在（尾部是"最近若干行"）。
  ST_CHECK(tail.find("尾部探针 39") != std::string::npos);
  // 超出容量的老行已被丢弃——这是"环形缓冲有界"的可观测后果。
  ST_CHECK(tail.find("尾部探针 0\n") == std::string::npos);
  // 行数不超过容量（否则崩溃报告会被无界内容撑爆）。
  std::size_t lines = 0;
  for (const char ch : tail) {
    if (ch == '\n') ++lines;
  }
  ST_CHECK(lines <= 8);

  st::log::set_tail_capacity(512);   // 复原（全局状态，影响后续用例）
}

ST_TEST(diag_default_log_follows_build_mode) {
  // 契约：**非 release 默认落盘，release 默认不落盘**（见 `entry.hpp` 的
  // `default_log_to_file`）。判据是编译器自己带的 `NDEBUG`（release 档带 `-DNDEBUG`）。
  //
  // 为何用 `#if defined(NDEBUG)` 双向断言而不是只断言一个值：这个契约**两边都要成立**，
  // 而测试可能在任何档位下被构建——写成单向就会在另一半档位里变成假绿。
#if defined(NDEBUG)
  ST_CHECK(!st::default_log_to_file());
#else
  ST_CHECK(st::default_log_to_file());
#endif
}
ST_TEST(log_crash_report_filename_has_no_colon) {
  // 回归：崩溃报告文件名曾直接用 `iso8601_now()`（形如 `2026-10-10T14:18:33.362Z`），
  // 其中的 `:` 在 Windows 文件名里不合法 ⇒ `open` 失败 ⇒ **崩溃报告永不生成**
  //（实测：`--crash-dir` 指了，目录里却一个 `crash-*.log` 都没有，且毫无提示）。
  //
  // ⚠ 这条**必须调生产函数**：早先写成“测试内自己做一遍替换再验证”，回退实现后
  // 依然全绿（测的是测试自己）——`CONVENTIONS` §7.2 那个“桩把差异抹平”的翻版。
  const std::string name = st::crash_report_file_name(1791641913362LL, 1234);
  ST_CHECK(name.find(':') == std::string::npos);   // 核心判据
  ST_CHECK(name.rfind("crash-", 0) == 0);
  ST_CHECK(name.ends_with("-1234.log"));
  // 时间戳部分仍在（不是把整个时间戳删掉蒙混过关）。
  ST_CHECK(name.find("2026") != std::string::npos);
  // 而原串确实是带冒号的——否则上面那条判据自动成立，测不出任何东西。
  ST_CHECK(st::time::iso8601_utc(1791641913362LL).find(':') != std::string::npos);

  // 并且这个名字真的能在本平台建出文件（“没有 `:`”与“能用”是两件事）。
  const std::string path = std::format("{}/{}", scratch_dir("crashname"), name);
  ST_CHECK(st::fs::write_text(path, "probe").has_value());
  ST_CHECK(st::fs::is_regular_file(path));
}
