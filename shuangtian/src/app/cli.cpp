#include "st/app/cli.hpp"

#include <format>
#include <string>
#include <string_view>

#include "st/core/string.hpp"

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
      "  --backend NAME        headless / win32 / x11 / wayland\n"
      "  --width N --height N  逻辑尺寸（默认 {}x{}）\n"
      "  --scale F             DPI 缩放（物理像素 = 逻辑 × scale；缺省跟随系统显示缩放）\n"
      "  --title TEXT          窗口标题\n"
      "  --theme MODE          light / dark / system\n"
              "  --renderer MODE       渲染器：auto（按实测帧耗时选更快）/ gpu / software\n"
        "  --text-lcd MODE       文字抗锯齿：auto（默认亚像素 LCD）/ on / off（灰度基准）\n"
        "  --text-fit MODE       字形网格拟合：auto（默认 normal）/ off / light / normal\n"
    "  --control-port N      控制通道端口（0 = 自动选空闲端口）\n"
      "  --control-file PATH   把端口等写入该文件（自动化流程据此连接）\n"
      "  --enable-script       开启进程内脚本能力（默认关闭）\n"
      "  --frames N            跑够 N 帧后退出\n"
      "  --ms N                跑够 N 毫秒后退出\n",
      program, 1280, 720);
}

auto parse_common_options(int argc, char** argv, CommonOptions& options) -> Status {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    const auto value = [&](std::string_view name) { return next_value(argc, argv, index, name); };

    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
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
    } else if (argument == "--control-port") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      auto number = parse_int(*parsed, argument);
      if (!number) return forward_error(number.error());
      options.app.control_port = static_cast<std::uint16_t>(*number);
    } else if (argument == "--control-file") {
      auto parsed = value(argument);
      if (!parsed) return forward_error(parsed.error());
      options.app.control_file = *parsed;
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
    } else {
      return unexpected(ErrorCode::Invalid,
                        std::format("未知参数: {}（--help 查看用法）", argument));
    }
  }
  return ok();
}

}  // namespace st::app
