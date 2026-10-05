/// 通用命令行解析（`st::app::parse_common_options`）回归测试。
///
/// 为什么必须有这一层测试（真实事故）：应用被外部驱动方启动的**固定契约**是
///
///   <app> --headless --control-port 0 --control-file <ctl.json> --shots <dir>
///
/// 而独立工程走的是「模板 `main` → `parse_common_options`」。此前共享 CLI 不认
/// `--shots`，于是它落进最后的 `else` 分支 → `ErrorCode::Invalid` → 模板 `main`
/// 打印错误并 **return 1**。现象是「应用起不来、等不到控制通道就绪」——
/// 排查时看的是应用日志（只有一句「未知参数」），而参数表本身没有任何测试拦着。
///
/// 所以本文件钉两件事：
/// 1. **驱动方实际会传的参数必须被吸收**（尤其 `--shots`；漏一个就整条自动化流程起不来）；
/// 2. **未知参数仍然拒绝**（不能为了「都吸收」而把拼错的参数静默吞掉——
///    「我明明指定了 `--them`」变成谜案的代价比多一条错误信息高得多）。
///
/// 第 2 条比第 1 条更重要：把契约测试写成「参数能过就行」会诱导后来者
/// 用「兜底吸收一切 `--xxx`」来让测试变绿，那等于把 CLI 的错误检测关掉。

#include <string>
#include <vector>

#include "st/app/cli.hpp"
#include "st/test/test.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::app::CommonOptions;

/// 把 `{"--headless", "--width", "800"}` 这类字面量拼成 `argc/argv` 并解析。
///
/// 用 `std::vector<std::string>` 持有底层存储（`argv` 是 `char**`，指向的缓冲必须活着），
/// 再单独维护一个指针数组——不能拿 `c_str()` 的临时结果去拼数组。
[[nodiscard]] auto parse(const std::vector<std::string>& arguments, CommonOptions& options)
    -> st::Status {
  std::vector<const char*> pointers;
  pointers.reserve(arguments.size() + 1);
  pointers.push_back("app");
  for (const std::string& argument : arguments) pointers.push_back(argument.c_str());
  return st::app::parse_common_options(static_cast<int>(pointers.size()),
                                       const_cast<char**>(pointers.data()), options);
}

}  // namespace

/// ① 驱动方的完整参数组合必须全部被吸收，且**落到正确的字段**。
///
/// 只断言「解析成功」不够：`--shots` 若被吸收却没写进 `screenshot_dir`，
/// 症状会变成「截图落到了临时目录」——比直接报错更难查。
ST_TEST(cli_accepts_shots_and_keeps_control) {
  CommonOptions options;
  const std::vector<std::string> arguments{
      "--headless", "--control-port", "0",   "--control-file", "/tmp/ctl.json",
      "--shots",    "/tmp/shots",   "--theme", "dark",
  };
  const auto status = parse(arguments, options);
  ST_CHECK(status.has_value());
  ST_CHECK(options.app.headless);
  ST_CHECK_EQ(static_cast<int>(options.app.control_port), 0);
  ST_CHECK_EQ(options.app.control_file, std::string("/tmp/ctl.json"));
  // `--shots` 的落点：控制通道 `encode=file` 且未给 path 时的目录
  ST_CHECK_EQ(options.app.screenshot_dir, std::string("/tmp/shots"));
  ST_CHECK(options.app.theme == st::ui::ThemeMode::Dark);
}

/// ② `--shots` 缺取值 → 报错（而不是把下一个参数吃掉当目录）。
///
/// 这条防的是「宽松解析」：`--shots --headless` 若把 `--headless` 当成目录名，
/// 应用会带着一个名为 `--headless` 的截图目录启动，而 headless 态静默失效。
ST_TEST(cli_rejects_shots_without_value) {
  CommonOptions options;
  const auto status = parse({"--shots"}, options);
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().code == st::ErrorCode::Invalid);
}

/// ③ 未知参数必须拒绝（错误检测不能为了「都能过」而关掉）。
///
/// 反例（故意破坏）：把 `parse_common_options` 最后的 `else` 改成静默忽略
/// unknown（或加一条「`--` 开头一律吞掉」的兜底），本用例当场变红。
ST_TEST(cli_rejects_unknown_flag) {
  CommonOptions options;
  const auto status = parse({"--them", "dark"}, options);   // 拼错的 --theme
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().code == st::ErrorCode::Invalid);
  // 错误信息必须点名那个参数（否则用户只能靠猜）
  ST_CHECK(status.error().message.find("--them") != std::string::npos);
}

/// ④ 用法文本必须列出驱动方依赖的每一个参数。
///
/// `--help` 是用户/智能体发现契约的唯一入口；参数实现了却没进 usage，
/// 就等于契约只存在于某些调用方的源码里。这里只钉「驱动方实际会传的」几个。
ST_TEST(cli_usage_documents_driver_contract) {
  const std::string usage = st::app::common_options_usage("myapp");
  for (const std::string_view flag : {"--headless", "--control-port", "--control-file", "--shots",
                                      "--theme", "--enable-script"}) {
    ST_CHECK(usage.find(flag) != std::string::npos);
  }
  ST_CHECK(usage.find("myapp") != std::string::npos);
}
