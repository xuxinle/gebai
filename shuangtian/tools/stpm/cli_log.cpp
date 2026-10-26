#include "cli_log.hpp"

#include <format>
#include <string>

#include "st/core/console_log.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"

// lint-allow: L8 日志路径与起点是**进程级配置**：`st log` 的输出属于“本次运行的全局事实”，
// 而它要被分发层（`run_app` 的 finish 收尾）与写入方（`begin`）两处共享。
// 没有 Context 可注入——它们属于整个 CLI 进程而非某个业务对象，与 `st::log` 的
// sink/listener 同一类（那些已在 §8 登记为进程级例外）。
namespace st::cli_log {
namespace {

/// 文件名里用的时间戳：本地时间 `YYYYmmdd-HHMMSS`。
///
/// 用本地时间而不是 UTC：看日志的人关心的是"我刚才那次跑"，
/// 而他读到的 `ls -l` 也是本地时间——两者对齐才不用心算时区。
auto file_stamp() -> std::string {
  const std::string now = st::time::format_now("{Y}{m}{d}-{H}{M}{S}");
  return now;
}

/// 文件名片段清洗：只留 `A-Za-z0-9._-`，其余替换成 `-`。
///
/// 目标名/档位名可能带路径分隔符（`--toolchain` 的产物目录带后缀），
/// 而一个带 `/` 的文件名会把日志写到**另一个目录**去——静默、且看起来"没落盘"。
auto sanitize_piece(std::string_view text) -> std::string {
  if (text.empty()) return "-";
  std::string out(text);
  for (auto& ch : out) {
    const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!keep) ch = '-';
  }
  return out;
}

std::string g_path{};
std::int64_t g_start_ns{0};

}  // namespace

auto suppressed() -> bool {
  const auto flag = st::fs::read_env("ST_NO_CONSOLE_LOG");
  return flag.has_value() && !flag->empty() && *flag != "0";
}

auto begin(std::string_view root, std::string_view action, std::string_view target,
           std::string_view profile) -> std::string {
  if (suppressed()) return {};
  const std::string file = st::fs::join(
      st::fs::join(std::string(root), std::string(kDirectory)),
      std::format("{}-{}-{}-{}-{}.log", sanitize_piece(action),
                  sanitize_piece(target.empty() ? std::string_view("lib") : target),
                  sanitize_piece(profile.empty() ? std::string_view("-") : profile), file_stamp(),
                  st::process::current_id()));
  if (!st::console_log::open(file)) return {};
  g_path = file;
  g_start_ns = st::time::now_ns();
  // 这行本身也会进日志（它由 print 写出，而镜像此时已打开）——所以日志的
  // 第一行就是它自己的路径：拿到一个日志文件就能反查"这是哪一次运行的"。
  st::print("日志：{}\n", file);
  return file;
}

void end(int exit_code) {
  if (g_path.empty()) return;
  const std::string elapsed = st::time::format_duration_ns(st::time::now_ns() - g_start_ns);
  const std::size_t bytes = st::console_log::bytes_written();
  // 收尾行必须**在关闭镜像之前**写：否则它只出现在终端，日志文件缺最后一行。
  // 这行是"事后判断这次跑成没成"的第一眼信息。
  st::print("── {} ── 耗时 {} · 退出码 {} · 日志 {} 字节\n",
            exit_code == 0 ? "完成" : "失败", elapsed, exit_code, bytes);
  st::console_log::close();
  g_path.clear();
}

auto current_path() -> std::string { return g_path; }

}  // namespace st::cli_log
