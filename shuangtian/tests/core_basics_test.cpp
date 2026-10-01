/// 基础工具补齐测试（vsedit P2-3）：`st::lower/upper`、`Result::value_or`、
/// `time::format_now`，以及 capture 落盘白名单的控制文件目录项（P2-2）。

#include "st/control/control.hpp"
#include "st/core/error.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/test/test.hpp"

#include <string>

// ————————————————————————————————————————————————————————————————————————————
// st::lower / upper
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(string_lower_upper_normalize_ascii) {
  ST_CHECK_EQ(st::lower("Hello.MD"), std::string("hello.md"));
  ST_CHECK_EQ(st::upper("readme.md"), std::string("README.MD"));
  // 非 ASCII 字节原样保留（不做 Unicode 折叠）
  ST_CHECK_EQ(st::lower("路径/文件.TXT"), std::string("路径/文件.txt"));
  ST_CHECK_EQ(st::lower(""), std::string(""));
}

// ————————————————————————————————————————————————————————————————————————————
// Result::value_or
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(result_value_or_suppresses_boilerplate) {
  const st::Result<std::string> ok_value{std::string("real")};
  ST_CHECK_EQ(ok_value.value_or(std::string("fallback")), std::string("real"));

  const st::Result<std::string> failure{st::unexpected(st::ErrorCode::Io, "读失败")};
  ST_CHECK_EQ(failure.value_or(std::string("fallback")), std::string("fallback"));

  // 右值形态（移动后取值）
  st::Result<std::string> moved{std::string("carried")};
  ST_CHECK_EQ(std::move(moved).value_or(std::string("fb")), std::string("carried"));
}

// ————————————————————————————————————————————————————————————————————————————
// time::format_now
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(time_format_now_templates) {
  const std::string date_time = st::time::format_now(st::time::kFormatDateTime);
  // 形如 2026-01-31 04:05:06：长度固定、分隔符在位
  ST_CHECK_EQ(date_time.size(), std::size_t{19});
  ST_CHECK_EQ(date_time[4], '-');
  ST_CHECK_EQ(date_time[10], ' ');
  ST_CHECK_EQ(date_time[13], ':');
  for (const char c : date_time) {
    const bool ok = (c >= '0' && c <= '9') || c == '-' || c == ' ' || c == ':';
    ST_CHECK(ok);
    if (!ok) break;
  }

  const std::string time_only = st::time::format_now(st::time::kFormatTime);
  ST_CHECK_EQ(time_only.size(), std::size_t{8});
  ST_CHECK_EQ(time_only[2], ':');

  // 未知占位与字面文本原样透出
  ST_CHECK_EQ(st::time::format_now("x{Q}y"), std::string("x{Q}y"));
  ST_CHECK_EQ(st::time::format_now("abc"), std::string("abc"));
}

// ————————————————————————————————————————————————————————————————————————————
// capture 白名单默认项（P2-2）：控制文件所在目录
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(capture_default_dirs_include_control_file_directory) {
  st::control::ServerOptions options;
  options.control_file = "/tmp/agent-session-42/control.json";
  const auto dirs = options.default_capture_dirs();

  bool has_control_dir = false;
  for (const auto& dir : dirs) {
    if (dir == "/tmp/agent-session-42") has_control_dir = true;
  }
  ST_CHECK(has_control_dir);  // 截图可直落会话目录，免二次搬运

  // 不设控制文件：不含该项（不凭空放行）
  st::control::ServerOptions bare;
  for (const auto& dir : bare.default_capture_dirs()) {
    ST_CHECK(dir.find("agent-session") == std::string::npos);
  }

  // 相对路径无分隔符：放行当前目录
  st::control::ServerOptions relative;
  relative.control_file = "control.json";
  const auto relative_dirs = relative.default_capture_dirs();
  bool has_dot = false;
  for (const auto& dir : relative_dirs) {
    if (dir == ".") has_dot = true;
  }
  ST_CHECK(has_dot);
}
