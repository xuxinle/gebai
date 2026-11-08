/// 格斗示例（gedou）—— 类拳皇的 2D 对战游戏。
///
/// ## 这个示例检验什么
///
/// 别的大多数示例检验"控件正确"；本示例检验的是**连续动画 + 实时输入 +
/// 判定几何**这一路：自绘组件的每帧重绘、固定步长逻辑帧、按键按住/边沿的两种输入
/// 语义、以及"看得见的画面"与"算得准的判定"必须各归其位。
///
/// 分层（也是这个示例的主要教学点）：
///
/// | 层 | 文件 | 职责 | 能否被单测直接断言 |
/// |---|---|---|---|
/// | 姿态 | `pose.*` | 招式关键帧 → 骨架关节（纯函数） | 能（纯几何） |
/// | 数据 | `fighter.*` | 角色档案与招式数值表 | 能（常量表） |
/// | 逻辑 | `fight.*` | 60Hz 状态机：判定盒/伤害/硬直/回合 | 能（确定性推进） |
/// | 对手 | `ai.*` | 产出与键盘**同一个** `Intent` | 能（固定种子） |
/// | 视图 | `game_view.*` | 把以上画出来 + HUD/特效 | 只能看截图 |
///
/// ## 与其它示例的分工
///
/// - `gallery`：控件陈列（每样东西长什么样）；
/// - `gbcode`：大界面（多面板/编辑器/终端协同）；
/// - `caiyun` / `louyue`：离屏出片（结果是一张图/一段视频）；
/// - `gedou`：**实时**（结果是"每秒 60 次的判定 + 每帧重绘"）。
///
/// ## 操作
///
/// `A`/`D` 移动 · `S` 防御 · `J` 轻拳 · `K` 重拳 · `L` 蹬腿 · `U` 必杀 ·
/// `P` 暂停 · `H` 显示判定盒 · `R` 重开。

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ai.hpp"
#include "fight.hpp"
#include "game_view.hpp"
#include "../app_icon.hpp"   // 图标两件套：PNG（窗口/任务栏）+ SVG（标题栏品牌）
#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/window_frame.hpp"

namespace {

using st::app::flag;
using st::app::takes_value;

using gedou::Difficulty;
using gedou::GameView;
using gedou::MoveId;

/// 应用自己的命令行（在通用选项之外）：只在示例内部用，不进框架。
struct Options {
  st::app::AppOptions app{};
  std::uint32_t frames{0};
  std::uint32_t max_ms{0};
  bool show_help{false};
  /// 训练模式（对手打桩）。
  bool dummy{false};
  /// `--record <目录>`：把每一帧存成 PNG（供 ffmpeg 合成视频）。
  /// 空 = 不录。
  std::string record_dir{};
  /// `--ai-vs-ai`：跳过选人界面、直接进机机对战（录制/观战用）。
  bool ai_vs_ai{false};
  /// 直接以暂停态启动（截图/素材用）。
  bool paused{false};
  /// 显示判定盒。
  bool hitbox{false};
  /// 难度。
  Difficulty difficulty{Difficulty::Normal};
  /// 双方角色档（0 甜甜圈 / 1 户圣）。
  std::size_t left_profile{0};
  std::size_t right_profile{1};
  /// 用**训练好的神经网络策略**接管对手侧（`--policy 文件`）。
  /// 空 = 用内置手写 AI。这是“训练出来的机器人真的能上场打”的入口。
  std::string policy_path{};
};

/// 本应用特有参数表（框架通用开关**不在这里**）。
[[nodiscard]] auto gedou_options() -> std::vector<st::app::AppOption> {
  return {
      takes_value("--record", "把对战录到指定目录"),
      flag("--ai-vs-ai", "双方都交给 AI"),
      flag("--dummy", "木桩（对手不还手）"),
      flag("--paused", "开局暂停"),
      flag("--hitbox", "显示判定框"),
      takes_value("--policy", "用训练好的神经网络策略接管对手侧"),
      takes_value("--difficulty", "难度：easy / normal / hard"),
      takes_value("--left", "左侧角色档（0~1）"),
      takes_value("--right", "右侧角色档（0~1）"),
  };
}

[[nodiscard]] auto parse_options(int argc, char** argv) -> st::Result<Options> {
  Options options{};
  st::app::CommonOptions common;
  st::app::AppOptionValues values;
  auto parsed = st::app::parse_options(argc, argv, gedou_options(), common, values);
  if (!parsed) return st::forward_error(parsed.error());
  if (common.show_help) {
    st::log::out("{}", st::app::options_usage("gedou", gedou_options()));
    std::exit(0);
  }

  options.record_dir = values.get("--record");
  options.ai_vs_ai = values.has("--ai-vs-ai");
  options.dummy = values.has("--dummy");
  options.paused = values.has("--paused");
  options.hitbox = values.has("--hitbox");
  options.policy_path = values.get("--policy");
  if (const std::string difficulty = values.get("--difficulty"); !difficulty.empty()) {
    options.difficulty = gedou::difficulty_from_name(difficulty.c_str());
  }

  // 角色档：范围校验留在应用侧（0~kFighterCount-1 是领域知识，框架不该知道）。
  const auto profile = [](std::string_view text, std::string_view flag,
                          std::size_t& target) -> st::Status {
    std::int64_t number = 0;
    try {
      number = std::stoll(std::string{text});
    } catch (const std::exception&) {
      return st::unexpected(st::ErrorCode::Invalid,
                            std::format("{} 只接受整数，收到「{}」", flag, text));
    }
    if (number < 0 || static_cast<std::size_t>(number) >= gedou::kFighterCount) {
      return st::unexpected(st::ErrorCode::Invalid,
                            std::format("{} 超出角色范围（0~{}）", flag,
                                        gedou::kFighterCount - 1));
    }
    target = static_cast<std::size_t>(number);
    return st::ok();
  };
  if (values.has("--left")) {
    if (auto status = profile(values.value_at("--left", 0), "--left", options.left_profile);
        !status) {
      return st::forward_error(status.error());
    }
  }
  if (values.has("--right")) {
    if (auto status = profile(values.value_at("--right", 0), "--right", options.right_profile);
        !status) {
      return st::forward_error(status.error());
    }
  }

  options.app = common.app;
  options.frames = common.max_frames;
  options.max_ms = common.max_ms;
  return options;
}

auto run_app(int argc, char** argv) -> int {
  auto parsed = parse_options(argc, argv);
  if (!parsed) {
    std::fprintf(stderr, "%s\n", parsed.error().to_string().c_str());
    return 1;
  }
  const Options options = *parsed;
  if (options.show_help) {
    st::log::out("{}", st::app::common_options_usage("gedou"));
    st::log::out(
        "格斗示例专有选项：\n"
        "  --difficulty easy|normal|hard  电脑强度（默认 normal）\n"
        "  --left N --right N             双方角色档（0 甜甜圈 / 1 户圣）\n"
        "  --dummy                        训练模式：对手不动\n"
        "  --record <目录>                逐帧存 PNG（供 ffmpeg 合成视频）\n"
        "  --ai-vs-ai                     跳过选人，直接机机对战（录制/观战）\n"
        "  --paused                       以暂停态启动（截图用）\n"
        "  --hitbox                       显示判定盒\n");
    return 0;
  }

  st::app::AppOptions app_options = options.app;
  app_options.width = 1180;
  app_options.height = 680;
  // 游戏窗口是"固定比例的舞台"：跟着工作区铺满会让场地比例失真，
  // 所以关掉自动铺开（`fill_work_area=false` 的语义见 AppOptions）。
  app_options.fill_work_area = false;
  app_options.title = "格斗 · 霜天应用";
  // 深色是格斗场景的默认观感（浅底会让打击特效失去对比）。
  // 用户显式传 `--theme light` 时已由通用解析置成 Light，这里只在**默认 Light**
  // 且没指定主题文件时改成 Dark——两者无法从 API 区分，所以改用环境变量开关：
  // 想要浅色就跑 `--theme-file <浅色主题>`（主题文件的优先级高于这里的默认）。
  app_options.theme = st::ui::ThemeMode::Dark;

  // 窗口/任务栏图标 + 标题栏品牌标记（两份素材，用途不同——见 app_icon.hpp）。
  st::app::icon::attach_png(app_options.window_icon_png,
                            b::embed<"apps/gedou/assets/gedou-icon.png">());
  st::app::icon::register_svg("gedou-logo",
                              b::embed<"apps/gedou/assets/gedou-logo.svg">());

  st::app::Application app("gedou", "0.1.0", app_options);
  
  auto frame = std::make_unique<st::ui::WindowFrame>("格斗 · 霜天应用");
  frame->set_id("window-frame");
  frame->set_window_control(&app);
  if (frame->title_bar() != nullptr) {
    frame->title_bar()->set_id("titlebar");
    // 品牌标记（本应用自己的 SVG 记号，不是通用图标）。
    st::app::icon::apply_brand(frame->title_bar(), "gedou-logo");
    frame->title_bar()->set_brand("格斗");
  }

  // 舞台：整块内容区就是一个自绘视图（它自己画背景/HUD），外面不再套面板——
  // 套一层会让背景被面板底色盖住，也会多出一圈没人要的内边距。
  auto view = std::make_unique<GameView>();
  view->start_match(options.left_profile, options.right_profile);
  view->set_difficulty(options.difficulty);
  view->set_dummy(options.dummy);
  view->set_show_hitbox(options.hitbox);
  if (!options.policy_path.empty()) {
    // 策略装载失败要**当场报出来**：静默回退到手写 AI 会让“机器人变傻了”
    // 被误归因为“训练没效果”。
    if (auto error = view->load_policy(options.policy_path); error.has_value()) {
      st::log::out("策略装载失败：{}\n", *error);
      return 2;
    }
    st::log::out("对手由神经网络策略接管：{}\n", options.policy_path);
  }
  if (options.paused) view->set_paused(true);
  // 默认**先进选人界面**；`--ai-vs-ai` 才跳过（录制/观战用）。
  //
  // ⚠ 这里曾经只有 `--ai-vs-ai` 分支调用 `skip_select()`，而 `open_select()`
  // 从头到尾**没人调用**——于是默认启动直接进对战，选人界面等于不存在。
  // 用户报"还没开始，机机对战怎么就进行了"，根因有两层：
  // ① 选人界面没被打开过（本处）；
  // ② 就算打开了，`paint_content` 推逻辑帧时也不判 `select_.active`（另修）。
  if (options.ai_vs_ai) {
    view->set_battle_mode(gedou::BattleMode::AiVsAi);
    view->skip_select();
  } else {
    view->open_select();
  }
  if (!options.record_dir.empty()) {
    // 录制时跳帧：60Hz 全存会出 60 张/秒，几十秒就上万张。
    // 默认每 2 帧存一张（30fps 的成片节奏），够顺滑也不至于爆盘。
    view->set_record_dir(options.record_dir, 2);
  }
  GameView* view_ptr = view.get();
  if (frame->content() != nullptr) frame->content()->add_child(std::move(view));

  app.set_content(std::move(frame));
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败：%s\n", status.error().to_string().c_str());
    return 1;
  }

  // 键盘要能落到舞台上：游戏是"焦点在内容区"的形态（与编辑器的焦点在编辑器同理）。
  (void)app.root().set_focus(view_ptr);

  st::log::out("格斗就绪：{} vs {} · 难度 {} · 后端 {} · 控制通道 127.0.0.1:{}\n",
               gedou::fighter_profile(options.left_profile).name,
               gedou::fighter_profile(options.right_profile).name,
               gedou::difficulty_name(options.difficulty), app.backend_name(),
               app.control_port());

  std::uint64_t frames = 0;
  const std::int64_t started_ms = st::time::now_ms();
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    app.tick();
    ++frames;
    if (options.frames > 0 && frames >= options.frames) break;
    if (options.max_ms > 0 && st::time::now_ms() - started_ms >= options.max_ms) break;
    // 节拍交给框架：有活 → 帧预算；空闲 → 短等（控制通道响应）。
    // 舞台在打的时候会持续 `request_animation()`，静止后自然停下。
    app.pace_loop(frame_start_ms);
  }
  st::log::out("格斗退出：{} 帧，逻辑帧 {}\n", frames, view_ptr->view_state().logic_frames);
  return 0;
}

}  // namespace

ST_MAIN(run_app)
