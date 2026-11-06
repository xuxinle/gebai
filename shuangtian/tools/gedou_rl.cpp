/// 格斗 RL 采集 / 评测工具（`gedou_rl`）。
///
/// ## 它解决什么
///
/// PPO 需要**大量**（状态, 动作, 奖励）样本，而每帧都跨进程问一次游戏是不可能的。
/// 但把战斗逻辑抄到 Python 里又必然漂移（判定/帧数/伤害各算一遍）。
///
/// 所以：**逻辑留在 C++**，工具在这里跑成千上万局，把经验写成二进制，
/// Python 只负责训练（那部分用 torch 最合适）。评测同理——"胜率多少"
/// 由同一个 `Match` 算出来，训练侧不自己实现对局。
///
/// ## 用法
///
/// ```bash
/// gedou_rl --schema                          # 契约（观测/动作/奖励）
/// gedou_rl --selfplay --episodes 200 --out data.bin        # 采集（脚本 AI 当对手）
/// gedou_rl --eval --policy policy.json --episodes 200      # 评测已训练策略
/// gedou_rl --eval --random --episodes 200                  # 随机策略基线
/// gedou_rl --eval --script-ai --episodes 200               # 手写 AI 自身对同难度对手
/// ```

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "apps/gedou/ai.hpp"
#include "apps/gedou/fight.hpp"
#include "apps/gedou/policy.hpp"
#include "apps/gedou/rl_env.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"

namespace {

using gedou::AiController;
using gedou::Arena;
using gedou::Difficulty;
using gedou::Match;
using gedou::PolicyNet;
using gedou::RlAction;
using gedou::RlEnv;

/// 经验记录的**磁盘布局**（小端、紧密排列）。
///
/// 为什么不写 JSON：一次采集是几百 MB 的浮点数，JSON 会慢一个量级，
/// 而训练侧只需要"定长结构数组"。格式写在这里的注释里，Python 侧照读；
/// 头部带 magic + 维度，**不匹配就报错**（而不是读出一堆乱数去训练）。
///
/// ```
/// header: magic[8]="GDRL0001" | uint32 obs_dim | uint32 act_dim
///         | uint32 episode_count | uint32 step_count
/// record: float32 obs[obs_dim] | int32 action | float32 reward
///         | float32 value_target(当时留空=0) | uint8 done | uint8[3] pad
/// ```
constexpr std::string_view kMagic{"GDRL0001"};
constexpr std::size_t kHeaderSize = 8 + 4 + 4 + 4 + 4;
constexpr std::size_t kRecordPad = 3;

struct Options {
  bool show_help{false};
  bool schema{false};
  /// 前向自检（从 stdin 读请求 JSON，向 stdout 写 logits）。
  bool forward_probe{false};
  /// 动作分布探针：跑 N 局并统计策略选了哪些动作（判定“学到了什么”）。
  bool action_histogram{false};
  bool selfplay{false};
  bool eval{false};
  bool use_policy{false};
  bool use_random{false};
  bool use_script_ai{false};
  /// 固定动作基线（空 = 不用）。
  std::string constant_action{};
  /// 训练用采样：按策略分布采样（而不是 argmax）。
  bool sample_actions{false};
  /// 诊断：一个回合内**同一动作连续重复**的比例（发现“按住某键”式退化）。
  bool repeat_stats{false};
  /// 诊断：逐帧轨迹（前 N 帧）——看清“到底发生了什么”，而不是从胜率猜。
  bool trace{false};
  std::string policy_path{};
  std::string out_path{};
  int episodes{200};
  int max_frames{60 * 40};
  /// 策略决策间隔（逻辑帧）。**默认与手写 AI 同频**（=1）：
  ///
  /// 对比必须公平：手写 AI 每帧都在决策，若学习策略每 4 帧才被问一次，
  /// 那“胜率更高”可能只来自“手写 AI 被限制了反应速度”，而不是策略更强。
  int frame_skip{1};
  Difficulty difficulty{Difficulty::Normal};
  std::uint32_t seed{0x51ED5EEDU};
  /// 课程学习：对手难度在第 N 局后提升（0 = 不提升）。
  int curriculum{0};
  float epsilon{0.0f};
};

[[nodiscard]] auto parse(int argc, char** argv) -> std::optional<Options> {
  Options options{};
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    const auto next = [&]() -> std::string_view {
      if (index + 1 >= argc) return {};
      ++index;
      return std::string_view{argv[index]};
    };
    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
    } else if (argument == "--schema") {
      options.schema = true;
    } else if (argument == "--forward-probe") {
      options.forward_probe = true;
    } else if (argument == "--action-histogram") {
      options.action_histogram = true;
    } else if (argument == "--repeat-stats") {
      options.repeat_stats = true;
    } else if (argument == "--trace") {
      options.trace = true;
    } else if (argument == "--frame-skip") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.frame_skip = std::atoi(std::string{value}.c_str());
    } else if (argument == "--selfplay") {
      options.selfplay = true;
    } else if (argument == "--eval") {
      options.eval = true;
    } else if (argument == "--policy") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.policy_path = std::string{value};
      options.use_policy = true;
    } else if (argument == "--random") {
      options.use_random = true;
    } else if (argument == "--script-ai") {
      options.use_script_ai = true;
    } else if (argument == "--constant") {
      // 固定动作基线（如 `--constant guard`）：判定“不做任何事”能拿到多少分。
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.constant_action = std::string{value};
    } else if (argument == "--sample") {
      options.sample_actions = true;
    } else if (argument == "--out") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.out_path = std::string{value};
    } else if (argument == "--episodes") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.episodes = std::atoi(std::string{value}.c_str());
    } else if (argument == "--max-frames") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.max_frames = std::atoi(std::string{value}.c_str());
    } else if (argument == "--curriculum") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.curriculum = std::atoi(std::string{value}.c_str());
    } else if (argument == "--epsilon") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.epsilon = static_cast<float>(std::atof(std::string{value}.c_str()));
    } else if (argument == "--difficulty") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.difficulty = gedou::difficulty_from_name(std::string{value}.c_str());
    } else if (argument == "--seed") {
      const std::string_view value = next();
      if (value.empty()) return std::nullopt;
      options.seed = static_cast<std::uint32_t>(std::strtoul(std::string{value}.c_str(), nullptr, 10));
    } else {
      std::fprintf(stderr, "未知参数：%.*s\n", static_cast<int>(argument.size()), argument.data());
      return std::nullopt;
    }
  }
  return options;
}

/// 采集用的随机源（确定性：同种子同数据）。
struct Rng {
  std::uint64_t state{1};

  [[nodiscard]] auto next() noexcept -> std::uint64_t {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
  }
  [[nodiscard]] auto next_float() noexcept -> float {
    return static_cast<float>((next() >> 40U) & 0xFFFFFFU) / 16777216.0f;
  }
  [[nodiscard]] auto next_action(std::size_t count) noexcept -> int {
    return static_cast<int>(next() % count);
  }
};

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

void append_f32(std::vector<std::uint8_t>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  append_u32(out, bits);
}

auto collect(const Options& options) -> int {
  RlEnv::Config config{};
  config.opponent = options.difficulty;
  config.max_frames = options.max_frames;
  config.seed = options.seed;
  RlEnv env{config};

  PolicyNet policy{};
  if (options.use_policy) {
    if (auto error = policy.load_file(options.policy_path); error.has_value()) {
      std::fprintf(stderr, "权重加载失败：%s\n", error->c_str());
      return 1;
    }
  }

  Rng rng{static_cast<std::uint64_t>(options.seed) | 1U};
  std::vector<std::uint8_t> buffer;
  buffer.reserve(64U * 1024U * 1024U);
  // 头部先占位，最后回填计数。
  for (char character : kMagic) buffer.push_back(static_cast<std::uint8_t>(character));
  append_u32(buffer, static_cast<std::uint32_t>(gedou::observation_dim()));
  append_u32(buffer, static_cast<std::uint32_t>(gedou::kRlActionCount));
  append_u32(buffer, 0);   // episodes
  append_u32(buffer, 0);   // steps

  std::uint32_t steps = 0;
  std::uint32_t wins = 0;
  std::uint32_t losses = 0;
  std::uint32_t draws = 0;
  const std::int64_t started = st::time::now_ms();

  for (int episode = 0; episode < options.episodes; ++episode) {
    // 课程学习：随局数推进提高对手难度（训练早期少挨打，后期学得更细）。
    if (options.curriculum > 0) {
      const int stage = episode / std::max(1, options.curriculum);
      config.opponent = stage == 0 ? Difficulty::Easy
                                   : (stage == 1 ? Difficulty::Normal : Difficulty::Hard);
      env = RlEnv{config};
    }
    gedou::Observation observation = env.reset();
    bool done = false;
    while (!done) {
      int action_index = 0;
      if (policy.valid()) {
        action_index = static_cast<int>(options.sample_actions
                                            ? policy.sample(observation, rng.next_float())
                                            : policy.act(observation));
      } else {
        // 随机基线：均匀采样（对照用）。
        action_index = rng.next_action(gedou::kRlActionCount);
      }
      if (options.epsilon > 0.0f && rng.next_float() < options.epsilon) {
        action_index = rng.next_action(gedou::kRlActionCount);
      }
      const auto action = gedou::rl_action_from_index(action_index);
      const gedou::StepResult result = env.step(action);

      for (float value : observation) append_f32(buffer, value);
      append_u32(buffer, static_cast<std::uint32_t>(action_index));
      append_f32(buffer, result.reward);
      append_f32(buffer, 0.0f);   // 留空（旧版 value target；保持记录定长）
      buffer.push_back(result.done ? 1U : 0U);
      for (std::size_t pad = 0; pad < kRecordPad; ++pad) buffer.push_back(0U);
      ++steps;

      observation = env.observation();
      done = result.done;
      if (done) {
        if (result.won) ++wins;
        else if (result.lost) ++losses;
        else ++draws;
      }
    }
  }

  // 回填计数（偏移固定：magic 8 字节 + obs_dim 4 + act_dim 4）。
  const auto write_at = [&buffer](std::size_t offset, std::uint32_t value) {
    buffer[offset + 0] = static_cast<std::uint8_t>(value & 0xFFU);
    buffer[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    buffer[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    buffer[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  };
  write_at(kHeaderSize - 8, static_cast<std::uint32_t>(options.episodes));
  write_at(kHeaderSize - 4, steps);

  if (!options.out_path.empty()) {
    if (auto status = st::fs::write_bytes(options.out_path, buffer); !status) {
      std::fprintf(stderr, "写经验文件失败：%s\n", status.error().message.c_str());
      return 1;
    }
  }
  const double seconds = static_cast<double>(st::time::now_ms() - started) / 1000.0;
  st::log::out(
      "采集完成：{} 局 / {} 步 / 胜 {} 负 {} 平 {} · {:.1f}s（{:.0f} 步/秒）· {} 字节\n",
      options.episodes, steps, wins, losses, draws, seconds,
      seconds > 0.0 ? static_cast<double>(steps) / seconds : 0.0, buffer.size());
  if (!options.out_path.empty()) st::log::out("经验落盘：{}\n", options.out_path);
  return 0;
}

auto evaluate(const Options& options) -> int {
  PolicyNet policy{};
  if (options.use_policy) {
    if (auto error = policy.load_file(options.policy_path); error.has_value()) {
      std::fprintf(stderr, "权重加载失败：%s\n", error->c_str());
      return 1;
    }
  }
  Rng rng{static_cast<std::uint64_t>(options.seed) | 1U};

  RlEnv::Config config{};
  config.opponent = options.difficulty;
  config.max_frames = options.max_frames;
  config.seed = options.seed;
  config.frame_skip = std::max(1, options.frame_skip);
  RlEnv env{config};

  int wins = 0;  int losses = 0;
  int draws = 0;
  float total_return = 0.0f;
  int total_frames = 0;
  int total_damage = 0;
  for (int episode = 0; episode < options.episodes; ++episode) {
    if (options.use_script_ai) {
      // 手写 AI 基线：**走同一个 RlEnv**，只是把“策略决策”换成 `AiController::think`。
      //
      // 为何不能单独手搓一场：两边必须吃同样的**接口约束**（决策节流、
      // 攻击冷却、观测时机），否则“谁更弱”里混着“谁被接口限制得更狠”。
      AiController script_ai{options.seed + static_cast<std::uint32_t>(episode) + 1U,
                             options.difficulty};
      gedou::Observation observation = env.reset();
      bool done = false;
      const float opp_start_hp = env.match().state(1).max_hp;
      while (!done) {
        const gedou::Intent intent = script_ai.think(env.match(), 0);
        // 把意图折回动作枚举（手写 AI 的动作就在这 8 个里）。
        int action_index = static_cast<int>(gedou::RlAction::Idle);
        if (intent.attack.has_value()) {
          switch (*intent.attack) {
            case gedou::MoveId::Light: action_index = static_cast<int>(gedou::RlAction::Light); break;
            case gedou::MoveId::Heavy: action_index = static_cast<int>(gedou::RlAction::Heavy); break;
            case gedou::MoveId::Kick: action_index = static_cast<int>(gedou::RlAction::Kick); break;
            case gedou::MoveId::Special: action_index = static_cast<int>(gedou::RlAction::Special); break;
            default: break;
          }
        } else if (intent.guard) {
          action_index = static_cast<int>(gedou::RlAction::Guard);
        } else if (intent.move > 0.01f) {
          action_index = static_cast<int>(gedou::RlAction::Forward);
        } else if (intent.move < -0.01f) {
          action_index = static_cast<int>(gedou::RlAction::Backward);
        }
        const gedou::StepResult result =
            env.step(gedou::rl_action_from_index(action_index));
        observation = env.observation();
        (void)observation;
        done = result.done;
        if (done) {
          if (result.won) ++wins;
          else if (result.lost) ++losses;
          else ++draws;
        }
      }
      total_frames += env.episode_frames();
      total_damage += static_cast<int>(opp_start_hp - env.match().state(1).hp);
      continue;
    }

    gedou::Observation observation = env.reset();
    bool done = false;
    while (!done) {
      int action_index = 0;
      if (policy.valid()) {
        action_index = static_cast<int>(policy.act(observation));
      } else if (!options.constant_action.empty()) {
        // 固定动作基线：遍历动作表按名字匹配（不手写映射，避免两边漂移）。
        for (int index = 0; index < static_cast<int>(gedou::kRlActionCount); ++index) {
          if (std::string(gedou::rl_action_name(gedou::rl_action_from_index(index))) ==
              options.constant_action) {
            action_index = index;
            break;
          }
        }
      } else if (options.use_random) {
        action_index = rng.next_action(gedou::kRlActionCount);
      }
      const auto action = gedou::rl_action_from_index(action_index);
      const gedou::StepResult result = env.step(action);
      observation = env.observation();
      done = result.done;
      if (done) {
        if (result.won) ++wins;
        else if (result.lost) ++losses;
        else ++draws;
      }
    }
    total_return += env.episode_return();
    total_frames += env.episode_frames();
    total_damage += static_cast<int>(env.match().state(1).max_hp - env.match().state(1).hp);
  }

  const double games = static_cast<double>(std::max(1, options.episodes));
  st::log::out("评测：{} 局 · 胜 {} / 负 {} / 平 {} · 胜率 {:.1f}% · 平均回报 {:.3f} · "
               "平均帧数 {:.0f} · 平均伤害 {:.0f}\n",
               options.episodes, wins, losses, draws,
               100.0 * static_cast<double>(wins) / games, static_cast<double>(total_return) / games,
               static_cast<double>(total_frames) / games,
               static_cast<double>(total_damage) / games);
  // 机器可读的一行（训练脚本解析它做曲线）。
  st::log::out("RESULT winrate={:.4f} wins={} losses={} draws={} return={:.4f} frames={:.1f} "
               "damage={:.1f}\n",
               static_cast<double>(wins) / games, wins, losses, draws,
               static_cast<double>(total_return) / games,
               static_cast<double>(total_frames) / games,
               static_cast<double>(total_damage) / games);
  return 0;
}

auto action_histogram(const Options& options) -> int {
  PolicyNet policy{};
  if (options.use_policy) {
    if (auto error = policy.load_file(options.policy_path); error.has_value()) {
      std::fprintf(stderr, "权重加载失败：%s\n", error->c_str());
      return 1;
    }
  }
  RlEnv::Config config{};
  config.opponent = options.difficulty;
  config.max_frames = options.max_frames;
  config.seed = options.seed;
  RlEnv env{config};
  Rng rng{static_cast<std::uint64_t>(options.seed) | 1U};
  std::array<std::uint64_t, gedou::kRlActionCount> counts{};
  std::uint64_t total = 0;
  std::uint64_t repeated = 0;
  int wins = 0;
  // 连续重复统计：诊断“策略退化成按住一个键”。
  // 为什么这个指标重要：动作在**每个可行动帧**都会被消费，
  // 所以“一直按 kick”等价于“无限连出腿”——它看起来是高分策略，
  // 实际是在利用环境接口，而不是学会格斗。
  int previous_action = -1;
  for (int episode = 0; episode < options.episodes; ++episode) {
    gedou::Observation observation = env.reset();
    bool done = false;
    while (!done) {
      int action_index = 0;
      if (policy.valid()) {
        action_index = static_cast<int>(policy.act(observation));
      } else {
        action_index = rng.next_action(gedou::kRlActionCount);
      }
      ++counts[static_cast<std::size_t>(action_index)];
      if (action_index == previous_action) ++repeated;
      previous_action = action_index;
      ++total;
      const gedou::StepResult result = env.step(gedou::rl_action_from_index(action_index));
      observation = env.observation();
      done = result.done;
      if (done && result.won) ++wins;
    }
  }
  st::log::out("动作分布（{} 局 / {} 步 / 胜 {}）· 连续重复率 {:.1f}%：\n", options.episodes, total,
               wins, total > 0 ? 100.0 * static_cast<double>(repeated) /
                                     static_cast<double>(total)
                               : 0.0);
  for (std::size_t index = 0; index < gedou::kRlActionCount; ++index) {
    const double share = total > 0 ? 100.0 * static_cast<double>(counts[index]) /
                                         static_cast<double>(total)
                                   : 0.0;
    st::log::out("  {:8s} {:6.2f}%  ({})\n", std::string(gedou::rl_action_name(
                                       gedou::rl_action_from_index(static_cast<int>(index)))),
                 share, counts[index]);
  }
  return 0;
}

auto trace_episode(const Options& options) -> int {
  PolicyNet policy{};
  if (options.use_policy) {
    if (auto error = policy.load_file(options.policy_path); error.has_value()) {
      std::fprintf(stderr, "权重加载失败：%s\n", error->c_str());
      return 1;
    }
  }
  RlEnv::Config config{};
  config.opponent = options.difficulty;
  config.max_frames = options.max_frames;
  config.seed = options.seed;
  config.frame_skip = std::max(1, options.frame_skip);
  RlEnv env{config};
  Rng rng{static_cast<std::uint64_t>(options.seed) | 1U};
  gedou::Observation observation = env.reset();
  bool done = false;
  // 固定动作基线的下标（在**循环外**解析一次）：
  // 之前在 trace 里只写了随机回退，于是 `--constant kick` 实际跑的是随机策略——
  // 诊断工具用错输入得出的结论会把人带到反方向（实测踩过：据此把平衡调错了一轮）。
  int constant_index = -1;
  if (!options.constant_action.empty()) {
    for (int index = 0; index < static_cast<int>(gedou::kRlActionCount); ++index) {
      if (std::string(gedou::rl_action_name(gedou::rl_action_from_index(index))) ==
          options.constant_action) {
        constant_index = index;
        break;
      }
    }
    if (constant_index < 0) {
      std::fprintf(stderr, "未知动作：%s\n", options.constant_action.c_str());
      return 2;
    }
  }
  st::log::out("# 帧  动作      己方HP 对手HP 距离 己方动作  对手动作  奖励   事件\n");
  while (!done) {
    int action_index = 0;
    if (policy.valid()) action_index = static_cast<int>(policy.act(observation));
    else if (constant_index >= 0) action_index = constant_index;
    else action_index = rng.next_action(gedou::kRlActionCount);
    const auto action = gedou::rl_action_from_index(action_index);
    const auto before = env.match().state(0);
    const auto before_opp = env.match().state(1);
    const gedou::StepResult result = env.step(action);
    const auto after = env.match().state(0);
    const auto after_opp = env.match().state(1);
    const float gap = std::abs(after_opp.x - after.x);
    if (env.episode_frames() <= 40 || result.done || result.hits > 0) {
      std::string event;
      if (result.hits > 0) event += std::format("HIT({}) ", result.hits);
      if (result.done) event += result.won ? "WIN" : (result.lost ? "LOSS" : "DRAW");
      st::log::out("#{:4d} {:8s} {:6.0f} {:6.0f} {:5.0f} {:9s} {:9s} {:+.3f} {}\n",
                   env.episode_frames(), std::string(gedou::rl_action_name(action)), after.hp,
                   after_opp.hp, gap, std::string(gedou::move_name(after.move)),
                   std::string(gedou::move_name(after_opp.move)), static_cast<double>(result.reward),
                   event);
    }
    observation = env.observation();
    done = result.done;
    (void)before;
    (void)before_opp;
  }
  return 0;
}

auto run(int argc, char** argv) -> int {
  const auto parsed = parse(argc, argv);
  if (!parsed.has_value()) {
    std::fprintf(stderr, "%s\n",
                 "参数错误（--help 查看用法）");
    return 2;
  }
  const Options& options = *parsed;
  if (options.show_help || argc <= 1) {
    st::log::out(
        "gedou_rl —— 格斗强化学习采集 / 评测工具\n"
        "  --schema                     打印观测/动作/奖励契约（JSON）\n"
        "  --selfplay --episodes N --out data.bin   采集经验（二进制）\n"
        "  --eval --episodes N          评测：--policy FILE / --random / --script-ai\n"
        "  --difficulty easy|normal|hard  对手难度（默认 normal）\n"
        "  --curriculum N               课程学习：每 N 局提一档难度\n"
        "  --sample                     采集时按策略分布采样（训练用）\n"
        "  --seed N                     随机种子（决定对手与采样，可复现）\n"
        "  --max-frames N               单回合最大帧数（默认 2400 = 40 秒）\n");
    return 0;
  }
  if (options.schema) {
    st::log::out("{}\n", gedou::rl_schema_json());
    return 0;
  }
  if (options.forward_probe) {
    // 从 stdin 读完整请求（训练脚本会把权重 + 一批输入塞进来）。
    std::string request;
    std::string line;
    while (std::getline(std::cin, line)) {
      request += line;
    }
    // 注意：**只输出 JSON**（stdout 还要给脚本解析，不能掺日志）。
    std::fputs(gedou::PolicyNet::forward_json(request).c_str(), stdout);
    std::fputc('\n', stdout);
    return 0;
  }
  if (options.selfplay) return collect(options);
  if (options.trace) return trace_episode(options);
  if (options.action_histogram || options.repeat_stats) return action_histogram(options);
  if (options.eval) return evaluate(options);
  std::fprintf(stderr, "需要 --selfplay 或 --eval\n");
  return 2;
}

}  // namespace

ST_MAIN(run)
