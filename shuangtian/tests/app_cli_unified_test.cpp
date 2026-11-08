/// 「框架通用开关 + 应用自有开关」统一解析的回归测试。
///
/// 为什么要有这一层（两次真实事故，方向相反）：
///
/// 1. **重复实现让新开关被吃掉**：应用自己写 `argv` 循环，于是框架给
///    `parse_common_options` 新加的开关它不认识 → 静默丢弃。gbcode 的注释里记着
///    那次代价：三档渲染出来的图几乎一样（实测只差 20 像素），差点被当成
///    「三档观感接近」的结论；
/// 2. **反向缺口让诊断能力只在一个应用里存在**：`--perf-report` / `--log-file`
///    在未声明它们的应用里直接报「未知参数」，于是「只有一个应用能产出性能报告」。
///
/// 本文件钉四件事（比“能解析就行”更重要）：
/// - 应用参数与通用参数**混排**时都能落到正确的字段/取值；
/// - **两边都不认识的仍要报错**并点名（不能为“都好过”而关掉错误检测）；
/// - 白名单只对**声明了白名单**的参数生效（枚举必须报错，路径类不该被拦）；
/// - `common_option_takes_value` 与实现**不分家**（漏登一个通用开关就变红）。
///
/// ⚠ 故意没写的一条：**不测“顺序无关”以外的语义**。`--` 结尾符、重复参数
/// 后者覆盖前者这类行为目前没有产品口径，测了会把它固化成一个没人要求的契约。

#include <string>
#include <vector>

#include "st/app/cli.hpp"
#include "st/test/test.hpp"
#include "st/ui/theme.hpp"

namespace {

using st::app::AppOption;
using st::app::AppOptionValues;
using st::app::CommonOptions;
using st::app::flag;
using st::app::takes_value;
using st::app::takes_values;

/// 把 `{"--headless", "--width", "800"}` 拼成 `argc/argv` 并走统一入口。
///
/// 底层存储由 `std::vector<std::string>` 持有（`argv` 指向的缓冲必须活着），
/// 指针数组单独维护——不能拿临时 `c_str()` 去拼数组。
[[nodiscard]] auto parse(const std::vector<std::string>& arguments,
                         const std::vector<AppOption>& app_options, CommonOptions& common,
                         AppOptionValues& values) -> st::Status {
  std::vector<const char*> pointers;
  pointers.reserve(arguments.size() + 1);
  pointers.push_back("app");
  for (const std::string& argument : arguments) pointers.push_back(argument.c_str());
  return st::app::parse_options(static_cast<int>(pointers.size()),
                                const_cast<char**>(pointers.data()), app_options, common, values);
}

/// 一个典型应用的表：一个布尔开关 + 一个路径参数 + 一个枚举参数。
[[nodiscard]] auto sample_table() -> std::vector<AppOption> {
  return {
      AppOption{.name = "--workspace", .value_count = 1, .help = "工作区目录"},
      AppOption{.name = "--paused", .value_count = 0, .help = "开局暂停"},
      AppOption{.name = "--mode",
                .value_count = 1,
                .help = "模式",
                .allowed = {"fast", "slow"}},
  };
}

}  // namespace

/// ① 混排：应用参数与通用参数交替出现，各自落到正确的地方。
///
/// 只断言“解析成功”不够——`--workspace` 被吸收却没写进 `values` 时，症状是
/// “工作区永远是当前目录”，比直接报错更难查。
ST_TEST(cli_unified_parses_interleaved_app_and_common) {
  CommonOptions common;
  AppOptionValues values;
  const auto status = parse({"--headless", "--workspace", "/tmp/ws", "--width", "800", "--paused",
                             "--theme", "dark"},
                            sample_table(), common, values);
  ST_CHECK(status.has_value());
  // 通用侧
  ST_CHECK(common.app.headless);
  ST_CHECK_EQ(common.app.width, 800);
  ST_CHECK(common.app.theme == st::ui::ThemeMode::Dark);
  // 应用侧
  ST_CHECK(values.has("--workspace"));
  ST_CHECK_EQ(values.get("--workspace"), std::string("/tmp/ws"));
  ST_CHECK(values.has("--paused"));       // 布尔开关以“存在”表示打开
  ST_CHECK_EQ(values.get("--paused"), std::string{});
}

/// ② 两边都不认识 → 报错并点名。
///
/// 反例（故意破坏）：把 `parse_options` 的未知分支改成静默忽略，本用例当场变红。
/// 这条比①更重要：写成“参数能过就行”会诱导后来者用“兜底吸收一切 `--xxx`”
/// 让测试变绿，那等于把 CLI 的错误检测关掉。
ST_TEST(cli_unified_rejects_unknown_flag) {
  CommonOptions common;
  AppOptionValues values;
  const auto status = parse({"--workspce", "/tmp/ws"}, sample_table(), common, values);
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().code == st::ErrorCode::Invalid);
  ST_CHECK(status.error().message.find("--workspce") != std::string::npos);
}

/// ③ 声明了白名单的参数，取值越界要报错（不静默回落）。
///
/// 若静默回落，“我明明指定了 fast”就变成谜案——本仓在 `--renderer` / `--text-lcd`
/// 上已经按这个口径写过注释，这里把同一条规矩钉进统一入口。
ST_TEST(cli_unified_rejects_value_outside_allowlist) {
  CommonOptions common;
  AppOptionValues values;
  const auto status = parse({"--mode", "turbo"}, sample_table(), common, values);
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().code == st::ErrorCode::Invalid);
  ST_CHECK(status.error().message.find("--mode") != std::string::npos);
  ST_CHECK(status.error().message.find("turbo") != std::string::npos);
  // 合法值要能过（否则上一条可能只是“什么都不接受”而假绿）
  CommonOptions ok_common;
  AppOptionValues ok_values;
  ST_CHECK(parse({"--mode", "fast"}, sample_table(), ok_common, ok_values).has_value());
  ST_CHECK_EQ(ok_values.get("--mode"), std::string("fast"));
}

/// ④ 没声明白名单的路径类参数：任意取值都该放行。
///
/// 这条防的是“白名单机制被误用成全局校验”——`--workspace` 的取值空间是任意路径，
/// 列不出来；若把空白名单当成“不许任何值”，路径参数就全废了。
ST_TEST(cli_unified_allows_free_form_value_when_no_allowlist) {
  CommonOptions common;
  AppOptionValues values;
  ST_CHECK(parse({"--workspace", "C:/some/odd dir/名"}, sample_table(), common, values)
               .has_value());
  ST_CHECK_EQ(values.get("--workspace"), std::string("C:/some/odd dir/名"));
}

/// ⑤ 应用参数缺取值 → 报错（而不是把下一个开关吃掉当值）。
///
/// `--workspace --headless` 若把 `--headless` 当成目录名，应用会带着一个名为
/// `--headless` 的工作区启动，而 headless 态静默失效。
ST_TEST(cli_unified_rejects_app_option_without_value) {
  CommonOptions common;
  AppOptionValues values;
  const auto status = parse({"--workspace"}, sample_table(), common, values);
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().code == st::ErrorCode::Invalid);
  ST_CHECK(status.error().message.find("--workspace") != std::string::npos);
}

/// ⑥ 通用开关缺取值同样要报错（这条走的是 `parse_options` 自己的判据，
/// 而不是靠 `parse_common_options` 的内部报错兜住——两者都要能红）。
ST_TEST(cli_unified_rejects_common_option_without_value) {
  CommonOptions common;
  AppOptionValues values;
  const auto status = parse({"--width"}, sample_table(), common, values);
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().code == st::ErrorCode::Invalid);
}

/// ⑦ `common_option_takes_value` 必须与 `parse_common_options` **不分家**。
///
/// 这是本设计里唯一的“两份知识”风险点：那张表若漏登一个带值的开关，
/// `--width 800` 就会被拆开解析（`800` 成了未知参数），症状极其隐蔽。
///
/// 判据：对表里每个开关，`parse_common_options` 单独喂它必须报「缺值」；
/// 对它**不在**表里的通用布尔开关，单独喂它必须成功。任一条不成立即变红。
ST_TEST(cli_value_flag_table_matches_parser) {
  const std::vector<std::string_view> value_flags{
      "--backend",           "--width",             "--height",
      "--scale",             "--renderer",          "--text-lcd",
      "--text-fit",          "--text-fit-max-size", "--text-digit-gamma",
      "--text-letter-gamma", "--text-han-gamma",    "--text-gamma",
      "--ui-font-scale",     "--title",             "--theme",
      "--theme-file",        "--control-port",      "--control-file",
      "--shots",             "--frames",            "--ms",
      "--perf-frames",       "--perf-report",       "--log-file",
      "--log-level",         "--crash-dir",
  };
  for (const std::string_view flag : value_flags) {
    // 表说吃值 → 实现也必须说缺值（否则表多登了）
    ST_CHECK(st::app::common_option_takes_value(flag));
    CommonOptions alone;
    std::string text(flag);
    char* argv[2]{const_cast<char*>("app"), text.data()};
    const auto status = st::app::parse_common_options(2, argv, alone);
    ST_CHECK(!status.has_value());
    ST_CHECK(status.error().message.find("缺少取值") != std::string::npos);
  }
  // 布尔开关：表说 false，实现也必须成功
  for (const std::string_view flag : {"--headless", "--enable-script", "--perf-exit", "--help"}) {
    ST_CHECK(!st::app::common_option_takes_value(flag));
    CommonOptions alone;
    std::string text(flag);
    char* argv[2]{const_cast<char*>("app"), text.data()};
    ST_CHECK(st::app::parse_common_options(2, argv, alone).has_value());
  }
}

/// ⑧ 登记表写错要在入口处就报出来（漏 `--`、缺说明、重复登记）。
///
/// 这些错误的症状是“这个开关永远不生效”或“后者静默覆盖前者”，都不报错；
/// 在入口一次性拦住，比事后翻三个应用入口便宜得多。
ST_TEST(cli_unified_validates_option_table) {
  CommonOptions common;
  AppOptionValues values;
  // 漏前导 `--`
  const auto no_prefix =
      parse({}, {AppOption{.name = "workspace", .value_count = 1, .help = "x"}}, common, values);
  ST_CHECK(!no_prefix.has_value());
  ST_CHECK(no_prefix.error().message.find("--") != std::string::npos);
  // 缺说明 → `--help` 列不出来
  const auto no_help =
      parse({}, {AppOption{.name = "--x", .value_count = 0, .help = {}}}, common, values);
  ST_CHECK(!no_help.has_value());
  // 重复登记
  const auto duplicated = parse({},
                                {AppOption{.name = "--dup", .value_count = 0, .help = "a"},
                                 AppOption{.name = "--dup", .value_count = 0, .help = "b"}},
                                common, values);
  ST_CHECK(!duplicated.has_value());
  ST_CHECK(duplicated.error().message.find("--dup") != std::string::npos);
}

/// ⑨ `--help` 要列出应用自己的参数，且与通用段分开。
///
/// 用户要能一眼分清“哪些是所有霜天应用都有的、哪些是这一个特有的”。
/// 分开列也是“参数的归属与谁在用一致”这条设计在文档上的体现。
ST_TEST(cli_usage_separates_app_options_from_common) {
  const std::string usage = st::app::options_usage("myapp", sample_table());
  ST_CHECK(usage.find("myapp") != std::string::npos);
  // 通用段
  ST_CHECK(usage.find("--headless") != std::string::npos);
  ST_CHECK(usage.find("--control-port") != std::string::npos);
  // 应用段（含说明文字）
  ST_CHECK(usage.find("--workspace") != std::string::npos);
  ST_CHECK(usage.find("工作区目录") != std::string::npos);
  ST_CHECK(usage.find("本应用特有") != std::string::npos);
  // 通用开关不该被当成应用参数重复列一遍（两份事实必分家）
  const std::size_t app_section = usage.find("本应用特有");
  ST_CHECK(app_section != std::string::npos);
  ST_CHECK(usage.find("--headless", app_section) == std::string::npos);
}

/// ⑫ 多值参数（`--pack A B`）必须按声明吃掉恰好 N 个值。
///
/// 为何需要这条：裁云的 `--snapshot <帧目录> <输出目录>` / `--diff-snap <A> <B>` /
/// `--export-usda <源> <目标>` 天然是**一对**值。把它们拆成两个开关会改变命令行
/// 语法（脚本与文档早就在用双值形式），而让应用自己解析又会把“切令牌”的知识
/// 拆成两份——那正是本设计要消灭的东西。
///
/// 反例（故意破坏）：把 `value_count` 当 1 处理时，`--diff-snap a.png b.png` 的
/// `b.png` 会落成未知参数→报错；若只吃 1 个却不报，`b.png` 就被静默当成位置参数。
ST_TEST(cli_supports_multi_value_app_options) {
  const std::vector<AppOption> table{
      takes_values("--pair", 2, "一对值"),
      takes_value("--single", "单值"),
      flag("--toggle", "布尔"),
  };
  CommonOptions common;
  AppOptionValues values;
  ST_CHECK(parse({"--pair", "left", "right", "--single", "one", "--toggle"}, table, common,
                  values)
               .has_value());
  ST_CHECK_EQ(values.value_at("--pair", 0), std::string("left"));
  ST_CHECK_EQ(values.value_at("--pair", 1), std::string("right"));
  ST_CHECK_EQ(values.value_at("--pair", 2), std::string{});        // 越界为空，不抛
  ST_CHECK_EQ(values.get("--pair"), std::string("left"));     // get 取第一个值
  ST_CHECK_EQ(values.value_at("--single", 0), std::string("one"));
  ST_CHECK(values.has("--toggle"));

  // 缺一个值 → 报错（不能把下一个开关当成第二个值）
  CommonOptions short_common;
  AppOptionValues short_values;
  const auto status = parse({"--pair", "only-one"}, table, short_common, short_values);
  ST_CHECK(!status.has_value());
  ST_CHECK(status.error().message.find("--pair") != std::string::npos);
}

/// ⑬ 同一参数出现多次时 `values.back()` 就是“最后一次”，供“后写胜”判定。
///
/// `--watch` / `--no-watch` 这种成对反向开关靠它表达顺序——用“哪个在表里存在”
/// 判不出顺序，必须能把“最后一次出现的是哪个”取出来。
ST_TEST(cli_preserves_switch_order_for_last_wins) {
  const std::vector<AppOption> table{flag("--on", "开"), flag("--off", "关")};
  CommonOptions common;
  AppOptionValues values;
  ST_CHECK(parse({"--on", "--off", "--on"}, table, common, values).has_value());
  ST_CHECK(values.has("--on"));
  // 三组都在：按出现顺序存，调用方从后往前找即得“最后一次生效的是哪个”
  ST_CHECK_EQ(static_cast<int>(values.values.size()), 3);
  ST_CHECK_EQ(values.values.back().first, std::string("--on"));
  ST_CHECK_EQ(values.last("--on").size(), static_cast<std::size_t>(0));
}

/// ⑭ 声明式写法要与字段写法等价（三件套：`takes_value`/`takes_values`/`flag`）。
ST_TEST(cli_declarative_helpers_match_fields) {
  const AppOption one = takes_value("--x", "单值");
  ST_CHECK_EQ(static_cast<int>(one.value_count), 1);
  ST_CHECK_EQ(one.help, std::string_view("单值"));
  const AppOption two = takes_values("--y", 2, "双值");
  ST_CHECK_EQ(static_cast<int>(two.value_count), 2);
  const AppOption none = flag("--z", "布尔");
  ST_CHECK_EQ(static_cast<int>(none.value_count), 0);
}

/// ⑩ 空表 = 纯通用应用：行为要与直接调 `parse_common_options` 一致。
/// 这条保证“迁移不改变老行为”——`gedou` 之外的应用在接入新入口时，
/// 通用参数部分的语义必须**逐字**保持。
ST_TEST(cli_unified_with_no_app_options_matches_common_parser) {
  CommonOptions through_unified;
  AppOptionValues values;
  ST_CHECK(parse({"--headless", "--width", "1024", "--ms", "500"}, {}, through_unified, values)
               .has_value());
  CommonOptions direct;
  std::vector<std::string> storage{"--headless", "--width", "1024", "--ms", "500"};
  std::vector<const char*> pointers{"app"};
  for (const auto& text : storage) pointers.push_back(text.c_str());
  ST_CHECK(st::app::parse_common_options(static_cast<int>(pointers.size()),
                                         const_cast<char**>(pointers.data()), direct)
               .has_value());
  ST_CHECK_EQ(through_unified.app.headless, direct.app.headless);
  ST_CHECK_EQ(through_unified.app.width, direct.app.width);
  ST_CHECK_EQ(through_unified.max_ms, direct.max_ms);
  ST_CHECK(values.values.empty());
  // 未知参数在空表下同样要拒（不能因为“没有应用参数”就放下）
  CommonOptions loose;
  AppOptionValues loose_values;
  ST_CHECK(!parse({"--nope"}, {}, loose, loose_values).has_value());
}

/// ⑪ `--control-port` 越界必须报错，**不得静默截断**。
///
/// 反例（真实回归）：旧实现裸 `static_cast<std::uint16_t>(99999)` → 截成 34463，
/// 应用监听在一个用户完全没要求的端口上；而外部驱动方按控制文件去连，
/// 得到的是「连上了、但连的是意料之外的一个端口」——比直接报错危险得多。
///
/// 这条是迁 `gbcode` 时发现的：它原来自带范围校验，接进框架后那层校验没了。
/// 所以校验补在**框架侧**（一处修，所有应用受益），而不是让每个应用自己再写一遍。
ST_TEST(cli_rejects_control_port_out_of_range) {
  for (const std::string_view bad : {"99999", "65536", "-1"}) {
    CommonOptions common;
    AppOptionValues values;
    std::string text(bad);
    const auto status = parse({"--control-port", text}, {}, common, values);
    ST_CHECK(!status.has_value());
    ST_CHECK(status.error().code == st::ErrorCode::Invalid);
    ST_CHECK(status.error().message.find("control-port") != std::string::npos);
  }
  // 边界值合法（否则上一条可能只是“什么都不接受”而假绿）
  for (const std::string_view good : {"0", "65535", "1"}) {
    CommonOptions common;
    AppOptionValues values;
    std::string text(good);
    ST_CHECK(parse({"--control-port", text}, {}, common, values).has_value());
  }
}
