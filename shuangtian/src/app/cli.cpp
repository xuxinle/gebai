#include "st/app/cli.hpp"

#include <format>
#include <string>
#include <string_view>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/perf/clock.hpp"
#include "st/perf/report.hpp"

namespace st::app {
namespace {

/// 取 `--name value` 形式的下一个参数（越界或缺失即报错）。
[[nodiscard]] auto next_value(int argc, char** argv, int& index, std::string_view name)
    -> Result<std::string> {
  if (index + 1 >= argc) {
    return unexpected(ErrorCode::Invalid, std::format("{} 缺少取值", name));
  }
  ++index;
  return std::string(argv[index]);
}

[[nodiscard]] auto parse_int(std::string_view text, std::string_view name) -> Result<int> {
  auto parsed = parse_i64(text);
  if (!parsed.has_value()) {
    return unexpected(ErrorCode::Invalid, std::format("{} 的取值不是整数: {}", name, text));
  }
  return static_cast<int>(*parsed);
}

[[nodiscard]] auto parse_float(std::string_view text, std::string_view name) -> Result<float> {
  auto parsed = parse_f64(text);
  if (!parsed.has_value() || *parsed <= 0.0) {
    return unexpected(ErrorCode::Invalid, std::format("{} 的取值不是正数: {}", name, text));
  }
  return static_cast<float>(*parsed);
}

}  // namespace

auto common_options_usage(std::string_view program) -> std::string {
  return std::format(
      "用法: {} [选项]\n"
      "  --headless            不创建窗口（无显示环境的默认姿态）\n"
    "  --no-splash           不开启动卡（截图/基准要确定性首帧）\n"
      "  --backend NAME        headless / win32 / x11 / wayland\n"
      "  --width N --height N  逻辑尺寸（默认 {}x{}）\n"
      "  --scale F             DPI 缩放（物理像素 = 逻辑 × scale；缺省跟随系统显示缩放）\n"
      "  --title TEXT          窗口标题\n"
      "  --theme MODE          light / dark / system\n"
      "  --theme-file PATH     自定义主题 JSON（稀疏覆盖；也看 ST_THEME_FILE）\n"
              "  --renderer MODE       渲染器：auto（按实测帧耗时选更快；无头缺省不探测，直接 software）/ gpu / software\n"
        "  --text-lcd MODE       文字抗锯齿：auto（默认亚像素 LCD）/ on / off（灰度基准）\n"
        "  --text-fit MODE       字形网格拟合：auto（默认 off）/ off / light / normal\n"
        "  --text-fit-max-size N 拟合的适用字号上限（物理 px，0 = 不限）\n"
        "  --text-digit-gamma V  数字类的覆盖率 gamma（auto = 内置默认）\n"
        "  --text-letter-gamma V 字母类的覆盖率 gamma（auto = 内置默认）\n"
        "  --text-han-gamma V    汉字类的覆盖率 gamma（auto = 内置默认）\n"
        "  --control-port N      控制通道端口（0 = 自动选空闲端口）\n"
      "  --control-file PATH   把端口等写入该文件（自动化流程据此连接）\n"
      "  --shots DIR           无头截图的落盘目录（控制通道 `encode=file` 且未给 path 时用）\n"
      "  --enable-script       开启进程内脚本能力（默认关闭）\n"
        "  --frames N            跑够 N 帧后退出\n"
        "  --ms N                跑够 N 毫秒后退出\n"
              "  --log-file PATH       日志落盘（父目录自动建；也看 ST_LOG_FILE）\n"
      "  --log-level NAME      trace / debug / info / warn / error / off\n"
      "  --crash-dir DIR       崩溃报告落盘目录（也看 ST_CRASH_DIR）\n"
      "  --perf-frames N       开启帧剖析并保留 N 帧的逐帧序列（默认不剖析）\n"
      "  --perf-report PATH    退出时把性能诊断报告写成 JSON（供 st perf --compare 或智能体读取）\n",
      program, 1280, 720);
}

auto parse_common_options(int argc, char** argv, CommonOptions& options) -> Status {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    const auto value = [&](std::string_view name) { return next_value(argc, argv, index, name); };

    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
    } else if (argument == "--no-splash") {
      // 关掉启动卡。为何需要它：截图/基准要**确定性首帧**（启动卡会多一次绘制+上屏，
      // 且多一帧上屏会让“首帧时刻”这个指标不可比）。
      options.app.splash = false;
    } else if (argument == "--headless") {
      options.app.headless = true;
    } else if (argument == "--backend") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.backend = *parsed;
    } else if (argument == "--width") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.app.width = *number;
    } else if (argument == "--height") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.app.height = *number;
    } else if (argument == "--scale") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_float(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.app.scale = *number;
    } else if (argument == "--renderer") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      const std::string mode = *parsed;
      // 白名单：拼错时**报错**而不是静默落回 auto——静默会让"我明明指定了 GPU"变成谜案。
      if (mode != "auto" && mode != "gpu" && mode != "software") {
        return unexpected(ErrorCode::Invalid,
                          std::format("--renderer 只接受 auto / gpu / software，收到「{}」", mode));
      }
      options.app.renderer = mode;
    } else if (argument == "--text-lcd") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      const std::string mode = *parsed;
      // 与 `--renderer` 同口径：拼错就报错，不静默落回 auto——
      // “我明明开了亚像素”变成谜案，比直接报错贵得多。
      if (mode != "auto" && mode != "on" && mode != "off") {
        return unexpected(ErrorCode::Invalid,
                          std::format("--text-lcd 只接受 auto / on / off，收到「{}」", mode));
      }
      options.app.text_lcd = mode;
    } else if (argument == "--text-fit") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      const std::string mode = *parsed;
      // 白名单同口径：拼错就报错（静默落回 auto 会把“我明明开了”变成谜案）。
      if (mode != "auto" && mode != "off" && mode != "light" && mode != "normal") {
        return unexpected(
            ErrorCode::Invalid,
            std::format("--text-fit 只接受 auto / off / light / normal，收到「{}」", mode));
      }
      options.app.text_fit = mode;
    } else if (argument == "--text-fit-max-size") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      // 连续量：只查"能不能解析出一个数"（白名单不成立）。
      try {
        std::size_t consumed = 0;
        (void)std::stof(*parsed, &consumed);
      } catch (const std::exception&) {
        return unexpected(ErrorCode::Invalid,
                          std::format("--text-fit-max-size 需要一个数值（物理 px，0 = 不限），"
                                      "收到「{}」",
                                      *parsed));
      }
      options.app.text_fit_max_size = *parsed;
    } else if (argument == "--text-digit-gamma" || argument == "--text-letter-gamma" ||
               argument == "--text-han-gamma") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      // 连续量：只查可解析性（1.35 也是合法值），解析不了就报错——
      // 静默回落会让"我明明设了 gamma=2"变成谜案（与 `--text-gamma` 同一姿态）。
      if (*parsed != "auto") {
        try {
          std::size_t consumed = 0;
          (void)std::stof(*parsed, &consumed);
        } catch (const std::exception&) {
          return unexpected(
              ErrorCode::Invalid,
              std::format("{} 需要 auto 或数值，收到「{}」", argument, *parsed));
        }
      }
      if (argument == "--text-digit-gamma") {
        options.app.text_digit_gamma = *parsed;
      } else if (argument == "--text-letter-gamma") {
        options.app.text_letter_gamma = *parsed;
      } else {
        options.app.text_han_gamma = *parsed;
      }
    } else if (argument == "--text-gamma") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      // 与 `--text-lcd`/`--text-fit` 的“拼错就报错”不同：这是连续量，
      // 白名单不成立（1.35 也是合法值）。因此只查“能不能解析出一个数”，
      // 解析不了就报错——静默回落会让“我明明设了 gamma=2”变成谜案。
      const std::string mode = *parsed;
      if (mode != "auto" && mode != "off") {
        try {
          std::size_t consumed = 0;
          (void)std::stof(mode, &consumed);
          if (consumed != mode.size()) {
            return unexpected(ErrorCode::Invalid,
                              std::format("--text-gamma 只接受 auto / off / 数值，收到「{}」", mode));
          }
        } catch (const std::exception&) {
          return unexpected(ErrorCode::Invalid,
                            std::format("--text-gamma 只接受 auto / off / 数值，收到「{}」", mode));
        }
      }
      options.app.text_gamma = mode;
    } else if (argument == "--ui-font-scale") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      // 与 `--text-gamma` 同口径：连续量没有白名单，只查“能不能整串解析成一个数”——
      // 拼错就报错，静默回落会让“我明明设了 1.2”变成谜案。范围校验在
      // `resolve_ui_font_scale`（夹到 [0.5, 3]，那是可用性边界而非合法性边界）。
      const std::string mode = *parsed;
      if (mode != "auto") {
        try {
          std::size_t consumed = 0;
          (void)std::stof(mode, &consumed);
          if (consumed != mode.size()) {
            return unexpected(ErrorCode::Invalid,
                              std::format("--ui-font-scale 只接受 auto / 数值，收到「{}」",
                                          mode));
          }
        } catch (const std::exception&) {
          return unexpected(ErrorCode::Invalid,
                            std::format("--ui-font-scale 只接受 auto / 数值，收到「{}」", mode));
        }
      }
      options.app.ui_font_scale = mode;
    } else if (argument == "--title") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.title = *parsed;
    } else if (argument == "--theme") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.theme = *parsed == "dark" ? ui::ThemeMode::Dark : ui::ThemeMode::Light;
      // ⚠ **必须同时记下名字**（2026-11-08 修的真缺陷）。
      //
      // 旧版只设枚举 `theme`，`theme_name` 一直是空串。而“用户显式给过主题没有”
      // 这个判据是用 `theme_name.empty()` 判的（见相期的 `theme_explicit`），
      // 于是 `--theme dark` 被当成“没给”，被持久化的偏好（`home/theme.txt`）
      // 或应用默认的 light 盖掉——实测：`xiangqi --theme dark` 启动后日志
      // 写的是“已应用内置主题 light”，而没有任何告警。这是“我设了没生效”那一类。
      //
      // 名字还兼作**内置主题库的 id**（qinhan/nebula/…）：只设枚举会让那些
      // 名字完全不可用（否则得靠应用自己再解析一遍 argv）。
      options.app.theme_name = *parsed;
    } else if (argument == "--theme-file") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.theme_file = *parsed;
    } else if (argument == "--control-port") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      // **范围校验，不静默截断**（2026-11-08 修）。
      //
      // 旧写法是裸 `static_cast<std::uint16_t>(*number)`：`--control-port 99999`
      // 被截成 **34463**，于是应用监听在用户完全没要求的端口上，而外部驱动方
      // 按控制文件里的端口去连——「连上了、但连的是意料之外的一个端口」。
      // 静默截断在这里没有任何好处理由：越界就是用户写错了，报错比猜一个端口好。
      if (*number < 0 || *number > 65535) {
        return unexpected(ErrorCode::Invalid,
                          std::format("--control-port 超出端口范围 0~65535，收到「{}」",
                                      *number));
      }
      options.app.control_port = static_cast<std::uint16_t>(*number);
    } else if (argument == "--control-file") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.control_file = *parsed;
    } else if (argument == "--shots") {
      // 无头截图目录。**必须吸收**：这是**外部驱动方**（歌白桌面子Agent、各 e2e 脚本）
      // 启动应用时的固定契约（`--headless --control-port 0 --control-file X --shots DIR`），
      // 而独立工程走的是应用的 `main` → 本函数——不认它就直接「未知参数」退出，
      // 表现是「应用起不来、等不到控制通道就绪」，排查时看的是应用日志而不是参数表。
      // 语义与「不影响行为」不同：它是**真实转发的配置**（截图落盘位置），只是可选。
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.screenshot_dir = *parsed;
    } else if (argument == "--enable-script") {
      options.app.enable_script = true;
    } else if (argument == "--frames") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.max_frames = static_cast<std::uint32_t>(*number < 0 ? 0 : *number);
    } else if (argument == "--ms") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.max_ms = static_cast<std::uint32_t>(*number < 0 ? 0 : *number);
    } else if (argument == "--perf-frames") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.app.profile_frames = static_cast<std::uint32_t>(*number < 0 ? 0 : *number);
      // 显式要剖析帧就必须真的开探针（即使 `ST_PERF` 没设）：用户已经说“我要数据”。
      if (options.app.profile_frames > 0) perf::set_enabled(true);
    } else if (argument == "--perf-report") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.perf_report = *parsed;
      options.perf_on_exit = true;
    } else if (argument == "--perf-exit") {
      options.perf_on_exit = true;
    } else if (argument == "--log-file" || argument == "--log-level" || argument == "--crash-dir") {
    // 诊断开关：**已在 `startup_configure_diagnostics` 里生效**（它比这里跑得更早，
    // 才能盖住启动早期的日志）。这里只做“认识它”——否则会被下面那句
    // “未知参数”报错退出，而应用里写死的解析器也可能因为不认识它而拒掉整个命令行。
    if (auto parsed = value(argument); !parsed) return forward_error(parsed.error());
  } else {
    return unexpected(ErrorCode::Invalid,
                      std::format("未知参数: {}（--help 查看用法）", argument));
    }
  }
  return ok();
}

auto finish_with_report(Application& app, const CommonOptions& options) -> bool {
  if (!options.perf_on_exit) return false;
  const auto report = app.perf_report();
  if (!report.has_value()) {
    // 剖析没开就没什么可写的。**不静默**：调用方给了 `--perf-report` 却没拿到文件，
    // 那种“以为采到了、其实没采”的空白最难查（与 ST_SKIP 同一姿态：说清楚没测）。
    st::eprint("未生成性能报告：剖析未开启（需要 --perf-frames N 或 ST_PERF=1）\n");
    return false;
  }
  const std::string json = perf::to_json(*report);
  if (options.perf_report.empty()) {
    // 没给路径：打到 stdout（自动化流程直接读管道）。
    st::print("{}\n", json);
    return true;
  }
  if (const auto parent = fs::parent(options.perf_report); !parent.empty()) {
    (void)fs::create_directories(parent);
  }
  if (auto written = fs::write_text(options.perf_report, json); !written) {
    st::eprint("性能报告写入失败（{}）: {}\n", options.perf_report,
               written.error().to_string());
    return false;
  }
  st::print("性能报告: {}\n", options.perf_report);
  return true;
}

auto common_option_takes_value(std::string_view name) noexcept -> bool {
  // 单一事实源：这张表与 `parse_common_options` 里的分支**必须同步**。
  // 不把两边合并的理由：那边的分支还管“取值怎么验证”（枚举白名单 / 数值 / 路径），
  // 而这里是纯“吃不吃值”的判据。把两者合成一张大表会把验证逻辑塞进数据，
  // 反而更难改。
  //
  // ⚠ 同步由 `cli_every_value_flag_is_declared` 那条用例拦住：它遍历这张表，
  // 逐个验证“带值的开关真能吃掉一个值、布尔的开关真的不吃”——
  // 新增一个通用开关却漏登这里，那条用例当场变红。
  static constexpr std::string_view kValueFlags[] = {
      "--backend",         "--width",         "--height",
      "--scale",           "--renderer",      "--text-lcd",
      "--text-fit",        "--text-fit-max-size", "--text-digit-gamma",
      "--text-letter-gamma", "--text-han-gamma", "--text-gamma",
      "--ui-font-scale",   "--title",         "--theme",
      "--theme-file",      "--control-port",  "--control-file",
      "--shots",           "--frames",        "--ms",
      "--perf-frames",     "--perf-report",   "--log-file",
      "--log-level",       "--crash-dir",
  };
  for (const std::string_view flag : kValueFlags) {
    if (flag == name) return true;
  }
  return false;
}

// ————————————————————————————————————————————————————————————————————————————
// 应用自有参数：声明式登记 + 统一解析（设计理由见头文件）
// ————————————————————————————————————————————————————————————————————————————

auto AppOptionValues::has(std::string_view name) const -> bool {
  for (const auto& [key, _value] : values) {
    if (key == name) return true;
  }
  return false;
}

auto AppOptionValues::get(std::string_view name, std::string_view fallback) const -> std::string {
  for (const auto& [key, value] : values) {
    if (key == name && !value.empty()) return value.front();
  }
  return std::string(fallback);
}

auto AppOptionValues::last(std::string_view name) const -> std::vector<std::string> {
  for (auto entry = values.rbegin(); entry != values.rend(); ++entry) {
    if (entry->first == name) return entry->second;
  }
  return {};
}

auto AppOptionValues::value_at(std::string_view name, std::size_t index) const -> std::string {
  for (const auto& [key, value] : values) {
    if (key == name) return index < value.size() ? value[index] : std::string{};
  }
  return {};
}

namespace {

/// 已登记的开关定义（找不到 = 不是应用参数）。
[[nodiscard]] auto find_app_option(const std::vector<AppOption>& table, std::string_view name)
    -> const AppOption* {
  for (const AppOption& entry : table) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

/// 登记表自身的一致性：返回第一条有问题的条目说明（空 = 表合法）。
///
/// 为何要在解析前先查一次：参数表是**手写常量**，写错（漏 `--`、重复登记）
/// 的症状是“这个开关永远不生效”或“后者静默覆盖前者”——两者都不报错，
/// 而排雷要翻三个应用入口。在入口处一次性报出来，代价是一次 O(n²) 扫描。
[[nodiscard]] auto validate_app_options(const std::vector<AppOption>& table) -> std::string {
  for (std::size_t index = 0; index < table.size(); ++index) {
    const AppOption& entry = table[index];
    if (entry.name.size() < 3 || entry.name.substr(0, 2) != "--") {
      return std::format("应用参数名必须以 -- 开头: 「{}」", entry.name);
    }
    if (entry.help.empty()) {
      return std::format("应用参数 {} 缺少说明（--help 会列不出来）", entry.name);
    }
    for (std::size_t other = index + 1; other < table.size(); ++other) {
      if (table[other].name == entry.name) {
        return std::format("应用参数重复登记: {}", entry.name);
      }
    }
  }
  return {};
}

}  // namespace

auto parse_options(int argc, char** argv, const std::vector<AppOption>& app_options,
                   CommonOptions& common, AppOptionValues& values) -> Status {
  if (const std::string problem = validate_app_options(app_options); !problem.empty()) {
    return unexpected(ErrorCode::Invalid, problem);
  }
  values.values.clear();

  // **一次循环同时处理两类参数**，而不是“先摘应用参数、剩下的交给通用”。
  //
  // 为何不沿用 gd 那个“先过滤再转交”的写法（它现在能跑通，但只因为它自己是那个特例）：
  // 两次遍历意味着**两份“未知参数”判据**——过滤阶段放行的东西通用阶段可能不认，
  // 反之亦然；而错位的症状是“参数有时被吃掉、有时报错”，取决于顺序。
  // 一次遍历只有一个“认不认识”的判据，不存在两边不一致的可能。
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (const AppOption* matched = find_app_option(app_options, argument); matched != nullptr) {
      std::vector<std::string> collected{};
      collected.reserve(matched->value_count);
      for (std::size_t slot = 0; slot < matched->value_count; ++slot) {
        if (index + 1 >= argc) {
          return unexpected(ErrorCode::Invalid, std::format("{} 缺少取值", argument));
        }
        ++index;
        collected.emplace_back(argv[index]);
      }
      // 白名单只看**第一个**值（本仓的枚举类参数都是单值；多值参数是路径/文件对）。
      if (!matched->allowed.empty() && !collected.empty()) {
        bool allowed = false;
        for (const std::string_view candidate : matched->allowed) {
          if (candidate == collected.front()) {
            allowed = true;
            break;
          }
        }
        if (!allowed) {
          std::string list{};
          for (std::size_t slot = 0; slot < matched->allowed.size(); ++slot) {
            if (slot != 0) list += " / ";
            list += matched->allowed[slot];
          }
          return unexpected(ErrorCode::Invalid,
                            std::format("{} 只接受 {}，收到「{}」", argument, list,
                                        collected.front()));
        }
      }
      values.values.emplace_back(std::string(argument), std::move(collected));
      continue;
    }
    // 不是应用参数 → 交给框架通用开关。
    //
    // 把**单个**参数拼成 `argc = 2` 的 argv 交给 `parse_common_options`：
    // 这样“通用开关的取值规则”只有一份实现。把那一大堆 `else if` 在两边各写
    // 一遍，就会重现本仓反复踩过的“两处默认值不一致”类缺陷。
    // 代价：每个参数多一次函数调用（命令行只有几十项，不在热路径上）。
    //
    // 它吃值（`--width 800`）时把下一位一并交给它——**判据问实现**，不在这里
    // 另写一份“哪些开关带值”的局面（两份必分家）。
    const bool takes_value = common_option_takes_value(argument);
    auto status = ok();
    if (takes_value) {
      if (index + 1 >= argc) {
        return unexpected(ErrorCode::Invalid, std::format("{} 缺少取值", argument));
      }
      char* pair[3]{const_cast<char*>("app"), const_cast<char*>(argv[index]),
                    const_cast<char*>(argv[index + 1])};
      status = parse_common_options(3, pair, common);
      if (status) ++index;
    } else {
      char* single[2]{const_cast<char*>("app"), const_cast<char*>(argv[index])};
      status = parse_common_options(2, single, common);
    }
    if (!status) return forward_error(status.error());
  }
  return ok();
}

auto options_usage(std::string_view program, const std::vector<AppOption>& app_options)
    -> std::string {
  std::string text = common_options_usage(program);
  if (app_options.empty()) return text;
  // 应用段单独列出：用户要能分清“哪些是所有霜天应用都有的、哪些是这一个特有的”。
  // 不存在的开关就别占版面——旧写法里每个应用都把通用开关在自家表里再拄一遍，
  // 结果两份“事实”注定会分家（多一个少一个都看不出来）。
  text += "  —— 本应用特有 ——\n";
  for (const AppOption& entry : app_options) {
    text += std::format("  {:<22}{}{}\n", entry.name,
                        entry.value_count > 0 ? "VALUE " : "      ", entry.help);
  }
  return text;
}

}  // namespace st::app
