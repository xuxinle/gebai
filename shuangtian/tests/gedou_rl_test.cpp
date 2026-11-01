/// 格斗 RL 链路的单测：**契约一致性 + 推理正确性**。
///
/// 这些用例的存在理由都很具体（每条都对应一次真实翻车）：
///
/// - **观测维度/顺序**：Python 侧按 schema 读维度；一旦 C++ 加了观测分量而
///   schema 没跟上，训练会按错误 stride 切数据 —— 得到的"乱数"看起来正常，
///   只是学不会东西。
/// - **前向一致性**：训练在 torch、推理在 C++。任何一处实现差异都会让
///   "训练收敛、进游戏不会打"，而曲线完全正常。这里用**手算的定值**钉住
///   单层/两层的输出（不依赖 Python 侧对照）。
/// - **动作→意图映射**：策略能出场的前提是"8 个动作都能变成真实操作"。
/// - **奖励符号**：打人给正、挨打给负、贴身硬推给负 —— 符号错了训练会
///   朝相反方向收敛，且很难从曲线上看出来。

#include <cmath>
#include <string>
#include <vector>

#include "examples/gedou/policy.cpp"  // NOLINT(bugprone-suspicious-include) —— 同 gedou_test
#include "examples/gedou/rl_env.cpp"  // NOLINT(bugprone-suspicious-include)
#include "st/test/test.hpp"

namespace {

using gedou::AiController;
using gedou::Arena;
using gedou::Difficulty;
using gedou::FighterState;
using gedou::LinearLayer;
using gedou::Match;
using gedou::MoveId;
using gedou::Observation;
using gedou::PolicyNet;
using gedou::RlAction;
using gedou::RlEnv;

[[nodiscard]] auto near(float a, float b, float epsilon = 1e-5f) -> bool {
  return std::abs(a - b) <= epsilon;
}

}  // namespace

ST_TEST(rl_schema_matches_observation_dim) {
  // schema 里的维度必须与 `observe()` 实际产出的长度一致，
  // 且各分量名不能重复（重复 = 两份定义已经漂移）。
  const auto& fields = gedou::observation_fields();
  ST_CHECK(fields.size() == gedou::observation_dim());
  Match match{0, 1, Arena{}};
  ST_CHECK(gedou::observe(match, 0).size() == fields.size());
  for (std::size_t left = 0; left < fields.size(); ++left) {
    for (std::size_t right = left + 1; right < fields.size(); ++right) {
      ST_CHECK(fields[left].name != fields[right].name);
    }
  }
}

ST_TEST(rl_schema_json_is_parseable_and_complete) {
  auto parsed = st::json_parse(gedou::rl_schema_json());
  ST_CHECK(parsed.has_value());
  const st::Json& root = *parsed;
  ST_CHECK(st::json_get_i64(root, "observation_dim", -1) ==
           static_cast<std::int64_t>(gedou::observation_dim()));
  ST_CHECK(st::json_get_i64(root, "action_count", -1) ==
           static_cast<std::int64_t>(gedou::kRlActionCount));
  const st::Json* actions = st::json_find(root, "actions");
  ST_CHECK(actions != nullptr && actions->is_array());
  ST_CHECK(actions->size() == gedou::kRlActionCount);
  // 动作名必须唯一（策略输出的是下标，名字是给人看的，重名会让 --constant 选错）
  for (std::size_t left = 0; left < actions->size(); ++left) {
    for (std::size_t right = left + 1; right < actions->size(); ++right) {
      ST_CHECK((*actions)[left] != (*actions)[right]);
    }
  }
}

ST_TEST(rl_observation_is_egocentric) {
  // 自我中心：把同一场对局摆到**不同尺寸**的场地上（相对位置相同），
  // 观测必须**完全相同**。否则策略会学到“世界坐标/绝对像素”这种与窗口
  // 绑定、换个尺寸就失效的东西。
  //
  // ⚠ 必须改**宽度**而不只是平移：只平移时，`arena.left + 40px` 这类
  // 容差在两个场地上算出的结果恰好相同，测试会**恒绿**（逆向验证实测过）。
  // 所以这里用 1040 → 400 的窄场，并把双方都摆在靠近边缘处（相对位置 8% / 92%）。
  Match a{0, 1, Arena{}};
  Match b{0, 1, Arena{}};
  Arena narrow{};
  narrow.left = 1000.0f;
  // 宽度要**足够放下两人 + 半身夹取**（每人留 48px 边距），否则 `place` 的
  // 夹取会把相对位置改掉，测出来的是“夹取生效”而不是“观测自我中心”。
  // 1038 → 700 已是能保持相对位置一致的最小量级。
  narrow.right = 1700.0f;   // 宽 700（a 是 1038）
  narrow.ground = 900.0f;
  narrow.start_gap = 200.0f;
  b.set_arena(narrow);
  const auto relative = [](const Match& match) {
    const Arena& arena = match.arena();
    return std::array<float, 2>{(match.state(0).x - arena.left) / arena.width(),
                                (match.state(1).x - arena.left) / arena.width()};
  };
  a.place(0, a.arena().left + a.arena().width() * 0.08f);
  a.place(1, a.arena().left + a.arena().width() * 0.92f);
  b.place(0, narrow.left + narrow.width() * 0.08f);
  b.place(1, narrow.left + narrow.width() * 0.92f);
  // ⚠ `place` 会做夹取（留半身宽），相对位置可能被改掉。
  // 所以先把**实际摆到的**相对位置取出来，再用它去查而不是用“我以为的 8%”。
  const auto relative_now = [](const Match& match, int side) {
    const Arena& arena = match.arena();
    return (match.state(side).x - arena.left) / arena.width();
  };
  // 直接把 b 的两人按 a 的**实际**相对位置摆好（夹取后会一致到浮点误差）。
  b.place(0, narrow.left + narrow.width() * relative_now(a, 0));
  b.place(1, narrow.left + narrow.width() * relative_now(a, 1));
  const auto rel_a = relative(a);
  const auto rel_b = relative(b);
  ST_CHECK(near(rel_a[0], rel_b[0], 1e-2f));
  ST_CHECK(near(rel_a[1], rel_b[1], 1e-2f));
  const Observation obs_a = gedou::observe(a, 0);
  const Observation obs_b = gedou::observe(b, 0);
  ST_CHECK(obs_a.size() == obs_b.size());
  for (std::size_t index = 0; index < obs_a.size(); ++index) {
    ST_CHECK(near(obs_a[index], obs_b[index], 1e-4f));
  }

  // 单独钉"靠墙"分量：判据必须是**相对场地宽度**，不是绝对像素。
  //
  // 为什么上面那条逐值对比抓不住：两个场地都"不靠墙"时该分量恒为 0，
  // 实现用比例还是用绝对像素都会得到 0（逆向验证实测证实）。
  //
  // 这里取一个**必然分岔**的摆位：窄场 1200×3% = 36px（若用 40px 阈值 → 靠墙），
  // 宽场 5000×3% = 150px（同阈值 → 不靠墙）。相对边距相同、绝对像素差 4 倍。
  const auto& fields = gedou::observation_fields();
  const auto corner_index = [&fields](std::string_view name) -> std::size_t {
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (fields[index].name == name) return index;
    }
    return fields.size();
  };
  const std::size_t self_corner = corner_index("self_cornered");
  ST_CHECK(self_corner < fields.size());

  // 这条要钉的是"判据用**相对**边距还是**绝对**像素"。
  //
  // 一个必须先想清的事实（我在这里绕了三轮）：夹取下限是半身宽 48px，
  // 而"绝对像素"实现的典型阈值（40px）**小于** 48 —— 于是只要角色靠边，
  // 绝对实现永远判"不靠墙"，根本不存在翻转点。
  // 所以不能用"两种宽度都扫翻转点"的写法；改成**只取一个足够宽的场地**，
  // 在它上面扫相对边距，翻转点应当落在实现真正使用的那个阈值上：
  //   · 相对实现：翻转点 ≈ 4%（`arena.width() * 0.04f`）
  //   · 绝对实现：阈值 40px 对 10000 宽场地 = 0.4%，而夹取把最小相对边距
  //     顶到 48/10000 = 0.48% > 0.4% ⇒ **永不翻转**（扫不到点）
  // 于是"有没有翻转点 + 落在哪"这两件事合起来，足以区分两种实现。
  const float width = 10000.0f;
  const auto corner_at_ratio = [self_corner, width](float ratio) {
    Match match{0, 1, Arena{}};
    Arena arena{};
    arena.left = 0.0f;
    arena.right = width;
    arena.ground = 900.0f;
    arena.start_gap = 200.0f;
    match.set_arena(arena);
    match.place(0, arena.left + width * ratio);
    // 对手摆远，避免它自己的状态影响 self_cornered（它只看 self）。
    match.place(1, arena.right - width * 0.30f);
    return gedou::observe(match, 0)[self_corner];
  };
  bool saw_cornered = false;
  bool saw_open = false;
  float flip_ratio = -1.0f;
  for (int step = 0; step <= 40; ++step) {
    const float ratio = static_cast<float>(step) * 0.005f;   // 0 ~ 20%
    const float marker = corner_at_ratio(ratio);
    if (marker > 0.5f) saw_cornered = true;
    if (marker < 0.5f) {
      if (saw_cornered && flip_ratio < 0.0f) flip_ratio = ratio;
      saw_open = true;
    }
  }
  // 必须同时观察到"靠墙"与"不靠墙"（否则这个分量根本没被覆盖到）。
  ST_CHECK(saw_cornered);
  ST_CHECK(saw_open);
  // 翻转点必须存在（相对实现有；绝对实现因 48px > 40px 而永远不翻）。
  ST_CHECK(flip_ratio > 0.0f);
  // 且必须落在 3%~5% 之间（实现用的是 `width * 0.04f`）。
  if (flip_ratio > 0.0f) {
    ST_CHECK(flip_ratio > 0.02f && flip_ratio < 0.06f);
  }
}

ST_TEST(rl_observation_values_are_bounded) {
  // 所有分量必须落在 `[-1, 1]`：策略的输入分布一旦越界，
  // 训练时的标准化假设就失效（而那是在 Python 侧看不见的）。
  RlEnv::Config config{};
  config.max_frames = 600;
  RlEnv env{config};
  (void)env.reset();
  const RlAction script[] = {RlAction::Forward, RlAction::Kick, RlAction::Guard,
                             RlAction::Heavy};
  for (int frame = 0; frame < 600; ++frame) {
    const RlAction action = script[(frame / 7) % 4];
    const gedou::Observation observation = env.observation();
    for (float value : observation) {
      ST_CHECK(value >= -1.0001f && value <= 1.0001f);
      ST_CHECK(!std::isnan(value));
    }
    const auto result = env.step(action);
    if (result.done) break;
  }
}

ST_TEST(rl_action_intent_covers_all_actions) {
  // 每个动作都必须映射到真实操作——否则策略"学会了"但游戏里什么也没发生。
  for (int index = 0; index < static_cast<int>(gedou::kRlActionCount); ++index) {
    const RlAction action = gedou::rl_action_from_index(index);
    const gedou::Intent intent = gedou::action_intent(action);
    switch (action) {
      case RlAction::Idle:
        ST_CHECK(!intent.attack.has_value() && !intent.guard && near(intent.move, 0.0f));
        break;
      case RlAction::Forward: ST_CHECK(intent.move > 0.0f); break;
      case RlAction::Backward: ST_CHECK(intent.move < 0.0f); break;
      case RlAction::Guard: ST_CHECK(intent.guard); break;
      case RlAction::Light:
      case RlAction::Heavy:
      case RlAction::Kick:
      case RlAction::Special: ST_CHECK(intent.attack.has_value()); break;
      case RlAction::Count: break;
    }
  }
}

ST_TEST(rl_action_names_unique_and_roundtrip) {
  for (int index = 0; index < static_cast<int>(gedou::kRlActionCount); ++index) {
    const RlAction action = gedou::rl_action_from_index(index);
    ST_CHECK(static_cast<int>(action) == index);
    ST_CHECK(!gedou::rl_action_name(action).empty());
  }
}

ST_TEST(policy_single_layer_matches_hand_computation) {
  // 一层、无激活：out = W·x + b，手算可验。
  // 取 identity 权重时输出必须等于输入平移；这里用 2→2 的显式矩阵。
  LinearLayer layer{};
  layer.in = 2;
  layer.out = 2;
  layer.tanh = false;
  layer.weight = {1.0f, 2.0f,    // out0 = 1*x0 + 2*x1
                  3.0f, 4.0f};   // out1 = 3*x0 + 4*x1
  layer.bias = {0.5f, -0.5f};
  auto net = PolicyNet::from_layers({layer});
  const std::vector<float> out = net.forward({1.0f, 1.0f});
  ST_CHECK(out.size() == 2);
  ST_CHECK(near(out[0], 3.5f));    // 1+2+0.5
  ST_CHECK(near(out[1], 6.5f));    // 3+4-0.5
}

ST_TEST(policy_tanh_layer_applies_activation) {
  LinearLayer layer{};
  layer.in = 1;
  layer.out = 1;
  layer.tanh = true;
  layer.weight = {2.0f};
  layer.bias = {0.0f};
  auto net = PolicyNet::from_layers({layer});
  ST_CHECK(near(net.forward({1.0f})[0], std::tanh(2.0f)));
  ST_CHECK(near(net.forward({0.0f})[0], 0.0f));
  // tanh 有界：大输入不能溢出成 inf（训练里出现 NaN 权重时这里会先炸）。
  ST_CHECK(near(net.forward({50.0f})[0], 1.0f, 1e-5f));
}

ST_TEST(policy_two_layers_compose) {
  LinearLayer first{};
  first.in = 1;
  first.out = 1;
  first.tanh = true;
  first.weight = {1.0f};
  first.bias = {0.0f};
  LinearLayer second{};
  second.in = 1;
  second.out = 1;
  second.tanh = false;
  second.weight = {3.0f};
  second.bias = {1.0f};
  auto net = PolicyNet::from_layers({first, second});
  ST_CHECK(near(net.forward({0.5f})[0], 3.0f * std::tanh(0.5f) + 1.0f));
}

ST_TEST(policy_act_is_argmax_and_deterministic) {
  // 评测与游戏内都走 `act()`：同一输入必须给同一动作（否则"可复现"是假的）。
  LinearLayer layer{};
  layer.in = 1;
  layer.out = 3;
  layer.tanh = false;
  layer.weight = {0.0f, 1.0f, 5.0f};
  layer.bias = {0.0f, 0.0f, 0.0f};
  auto net = PolicyNet::from_layers({layer});
  const int expected = 2;   // 最大 logit 在第三维
  for (int repeat = 0; repeat < 5; ++repeat) {
    ST_CHECK(static_cast<int>(net.act({1.0f})) == expected);
  }
}

ST_TEST(policy_json_roundtrip_preserves_output) {
  LinearLayer layer{};
  layer.in = 3;
  layer.out = 2;
  layer.tanh = true;
  layer.weight = {0.1f, -0.2f, 0.3f, 0.4f, 0.5f, -0.6f};
  layer.bias = {0.01f, -0.02f};
  auto original = PolicyNet::from_layers({layer});
  const std::string text = original.to_json(false);
  auto reloaded = PolicyNet::from_layers({});
  const auto error = reloaded.load_json(text);
  ST_CHECK(!error.has_value());
  const std::vector<float> a = original.forward({0.5f, -0.5f, 1.0f});
  const std::vector<float> b = reloaded.forward({0.5f, -0.5f, 1.0f});
  ST_CHECK(a.size() == b.size());
  for (std::size_t index = 0; index < a.size(); ++index) ST_CHECK(near(a[index], b[index]));
}

ST_TEST(policy_rejects_malformed_weights) {
  // 载入必须**明确报错**而不是静默降级：静默降级会让"权重载错了"
  // 表现为"机器人很傻"，从而被误归因为"训练没效果"。
  PolicyNet net{};
  ST_CHECK(net.load_json("not json").has_value());
  ST_CHECK(net.load_json("{}").has_value());

  // 维度与观测不符
  st::Json root = st::Json::object();
  st::Json layers = st::Json::array();
  st::Json layer = st::Json::object();
  layer["in"] = static_cast<std::int64_t>(gedou::observation_dim() + 3);
  layer["out"] = static_cast<std::int64_t>(gedou::kRlActionCount);
  layer["tanh"] = false;
  layer["weight"] = st::Json::array();
  layer["bias"] = st::Json::array();
  layers.push_back(std::move(layer));
  root["layers"] = std::move(layers);
  auto error = net.load_json(st::json_dump(root, false));
  ST_CHECK(error.has_value());
  // 报错里要提到"维度"，否则读的人不知道是形状问题还是数值问题。
  ST_CHECK(error->find("维度") != std::string::npos);
}

ST_TEST(rl_reward_signs) {
  // 符号检查：**打人给正分、挨打给负分**。
  //
  // ⚠ 早期版本只断言“什么都不做时奖励 ≤ 0”，回退符号后依然绿
  // （逆向验证实测：因为挨打项本来就是负的，总和仍为负）。
  // 所以这里必须**单独隔离每一项**：把伤害项单独拿出来看符号。
  const gedou::RewardConfig reward{};
  ST_CHECK(reward.damage_dealt > 0.0f);   // 打人有赏
  ST_CHECK(reward.damage_taken > 0.0f);   // 挨打有罚（系数为正，实际取负）
  ST_CHECK(reward.win_bonus > 0.0f);
  ST_CHECK(reward.lose_penalty < 0.0f);
  ST_CHECK(reward.whiff_penalty < 0.0f);
  ST_CHECK(reward.passive_range_penalty < 0.0f);
  ST_CHECK(reward.push_penalty < 0.0f);
  ST_CHECK(reward.step_penalty < 0.0f);

  // 端到端符号：**必须测 `RlEnv::step()` 真实产出的奖励**，不能只测 `RewardConfig`
  // 里的系数——后者是"桩"：系数符号对了而实现里写反了，断言照样绿
  // （CONVENTIONS §7.2 记的正是这类假绿）。
  //
  // 判据也不是"命中帧平均为正"：`hit_bonus`(0.35) 远大于单帧伤害(≈0.04)，
  // 把 `damage_dealt` 反号后平均仍然为正（实测：这条假绿过一次）。
  // 可靠判据：**伤害更大的招式命中时，单帧奖励必须更高**。
  // 两局对照（一局只轻拳、一局只重拳），避免同局交错出招时重拳抢不到判定帧。
  const auto hit_reward_of = [](MoveId move, float& damage_out) -> float {
    RlEnv::Config cfg{};
    cfg.max_frames = 900;
    cfg.idle_opponent_ratio = 1.0f;   // 对手不动（排除挨打项）
    cfg.seed = 777U;
    RlEnv env{cfg};
    (void)env.reset();
    const RlAction attack = (move == MoveId::Heavy) ? RlAction::Heavy : RlAction::Light;
    float best = 0.0f;
    float biggest = 0.0f;
    for (int frame = 0; frame < 900; ++frame) {
      const FighterState& self = env.match().state(0);
      const FighterState& other = env.match().state(1);
      const float gap = std::abs(other.x - self.x);
      const float reach = gedou::attack_spec(move).reach_far * gedou::kBodyScale +
                          30.0f * gedou::kBodyScale;
      const auto result = env.step(gap > reach ? RlAction::Forward : attack);
      if (result.damage_dealt > biggest) {
        biggest = result.damage_dealt;
        best = result.reward;
      }
      if (result.done) break;
    }
    damage_out = biggest;
    return best;
  };
  float light_damage = 0.0f;
  float heavy_damage = 0.0f;
  const float light_reward = hit_reward_of(MoveId::Light, light_damage);
  const float heavy_reward = hit_reward_of(MoveId::Heavy, heavy_damage);
  ST_CHECK(light_damage > 0.0f);    // 前提：两种招都真打中过
  ST_CHECK(heavy_damage > 0.0f);
  ST_CHECK(heavy_damage > light_damage * 1.5f);   // 重拳伤害本来就更大
  // 核心断言：伤害更大的那一招，单帧奖励更高。符号反了立刻被抓住。
  ST_CHECK(heavy_reward > light_reward);

  // 反向：纯挨打（对手会动、自己不动）的帧必须是负分。
  RlEnv::Config taken_cfg{};
  taken_cfg.max_frames = 300;
  taken_cfg.idle_opponent_ratio = 0.0f;
  RlEnv taken_env{taken_cfg};
  (void)taken_env.reset();
  bool got_hit = false;
  float taken_reward_sum = 0.0f;
  int taken_frames = 0;
  for (int frame = 0; frame < 300; ++frame) {
    const auto result = taken_env.step(RlAction::Idle);
    if (result.damage_taken > 0.0f) {
      got_hit = true;
      taken_reward_sum += result.reward;
      ++taken_frames;
    }
    if (result.done) break;
  }
  ST_CHECK(got_hit);
  ST_CHECK(taken_frames > 0);
  ST_CHECK(taken_reward_sum / static_cast<float>(std::max(1, taken_frames)) < 0.0f);
}

ST_TEST(rl_step_is_deterministic_for_same_seed) {
  // 同种子 → 同一局。这是"评测可复现"的地基（训练脚本靠它比较不同轮次）。
  const auto play = [](std::uint32_t seed) {
    RlEnv::Config config{};
    config.seed = seed;
    config.max_frames = 240;
    RlEnv env{config};
    (void)env.reset();
    float total = 0.0f;
    for (int frame = 0; frame < 240; ++frame) {
      const auto result = env.step(static_cast<RlAction>(frame % 4));
      total += result.reward;
      if (result.done) break;
    }
    return total;
  };
  ST_CHECK(near(play(12345U), play(12345U), 1e-4f));
  // 不同种子应当给出不同结果（否则"每局换对手种子"的机制没生效）。
  ST_CHECK(!near(play(12345U), play(54321U), 1e-6f));
}

ST_TEST(rl_attack_cooldown_blocks_repeat) {
  // “每帧重按攻击”不能被原样执行 —— 这是"一直按踢腿 == 无限连"的直接原因。
  //
  // ⚠ 走过的弯路（都是逆向验证实测出来的假绿）：
  //  ① 断言"900 帧命中次数 < 40"——去掉冷却后角色受招式动画限制，次数照样上不去；
  //  ② 断言"连续两帧请求不会起新招"——角色本就在起手中，`move_frame` 不为 0，测不出；
  //  ③ 数"攻击姿态被重新起始的次数"——同样被动画掩盖。
  // 结论：**从外部行为反推不出冷却是否生效**，必须直接观测它（所以 RlEnv 暴露了
  // `attack_cooldown()`：这是可测试性要求，不是测试在偷看实现细节）。
  RlEnv::Config cfg{};
  cfg.max_frames = 200;
  cfg.idle_opponent_ratio = 1.0f;
  RlEnv env{cfg};
  (void)env.reset();
  ST_CHECK(env.attack_cooldown() == 0);          // 开局无冷却
  (void)env.step(RlAction::Light);               // 出一招
  ST_CHECK(env.attack_cooldown() > 0);           // 立刻进入冷却（关键断言）
  // 冷却期间再请求攻击：冷却应当**继续倒数**而不是被重置/绕过
  const int after_first = env.attack_cooldown();
  (void)env.step(RlAction::Light);
  ST_CHECK(env.attack_cooldown() <= after_first);
  // 等足够久（远超轻拳总帧数）后冷却归零
  for (int i = 0; i < 60; ++i) (void)env.step(RlAction::Idle);
  ST_CHECK(env.attack_cooldown() == 0);
}
