/// 格斗示例的姿态与战斗逻辑单测（`examples/gedou`）。
///
/// 为什么这些必须有单测（而不是全靠 e2e）：
/// - **对称性**：`pose_for` 只算一个朝向，镜像靠 `to_world` 的 sign 翻转——
///   一旦有人"顺手"在 `pose_for` 里加一个朝右的偏移，e2e 看不出来（它只验左打右）。
///   单测直接断言"镜像后的 x 关于脚下对称"，一次就能钉死。
/// - **单调性**：攻击动作的"手/脚伸出去"必须是**单调**的（起手 → 命中），
///   否则画面会看到拳头来回抽动。e2e 只看命中帧，看不见路径。
/// - **不变量**：血条不会变负、必杀槽不越界、同种子可复现、站立姿态脚在地面。
///
/// 注意：`examples/` 下的实现不参与测试目标的链接（`CONVENTIONS` §测试纪律），
/// 因此这里按本仓既有做法**直接 include 实现**（同 `tests/caiyun_carry_test.cpp`）。

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "examples/gedou/ai.cpp"     // NOLINT(bugprone-suspicious-include) —— 见文件头
#include "examples/gedou/fight.cpp"  // NOLINT(bugprone-suspicious-include)
#include "examples/gedou/fighter.cpp"  // NOLINT(bugprone-suspicious-include)
#include "examples/gedou/pose.cpp"     // NOLINT(bugprone-suspicious-include)
#include "st/test/test.hpp"

namespace {

using gedou::AiController;
using gedou::Arena;
using gedou::FighterState;
using gedou::Intent;
using gedou::kBodyScale;
using gedou::Match;
using gedou::MoveId;
using gedou::Pose;

[[nodiscard]] auto with_padding(const Pose& pose) -> bool {
  // 所有骨架点必须落在合理范围内（局部单位）：
  // 负 x 到 +110、y 从 0 到 200。超出说明某个姿态写错了数量级。
  const std::vector<st::math::Point> points{
      pose.hip,        pose.chest,      pose.head,        pose.shoulder_front,
      pose.shoulder_back, pose.elbow_front, pose.hand_front, pose.elbow_back,
      pose.hand_back,  pose.knee_front, pose.foot_front,  pose.knee_back,
      pose.foot_back};
  for (const auto& point : points) {
    if (point.x < -70.0f || point.x > 120.0f) return false;
    if (point.y < -5.0f || point.y > 210.0f) return false;
  }
  return true;
}

}  // namespace

ST_TEST(gedou_pose_all_moves_stay_in_range) {
  // 每个动作的**每一帧**都必须是合法骨架。这条抓的是"某一帧手飞到天上"这类
  // 插值错误——它在 e2e 里只会表现为"某一帧看着怪"，无法断言。
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(MoveId::Count); ++index) {
    const auto move = static_cast<MoveId>(index);
    const int total = gedou::timing_for(move).total() + 30;
    for (int frame = 0; frame < total; ++frame) {
      const Pose pose = gedou::pose_for(move, frame, static_cast<float>(frame % 20) / 20.0f);
      ST_CHECK(with_padding(pose));
    }
  }
}

ST_TEST(gedou_pose_mirror_is_exact) {
  // 镜像对称：同一个姿态朝左与朝右，**相对脚下的偏移必须完全对称**。
  // 这条抓的是"在 pose_for 里偷偷加了朝右的世界偏移"——e2e 只打一个方向，看不出来。
  const Pose local = gedou::pose_for(MoveId::Heavy, 10);
  const float feet = 400.0f;
  const float ground = 500.0f;
  const Pose right = local.to_world(feet, ground, true);
  const Pose left = local.to_world(feet, ground, false);
  const auto near = [](float a, float b) { return std::abs(a - b) < 0.001f; };
  ST_CHECK(near(right.hip.x - feet, feet - left.hip.x));
  ST_CHECK(near(right.hand_front.x - feet, feet - left.hand_front.x));
  ST_CHECK(near(right.foot_front.x - feet, feet - left.foot_front.x));
  ST_CHECK(near(right.head.y, left.head.y));   // 垂直方向不受镜像影响
}

ST_TEST(gedou_pose_attack_reach_is_monotonic) {
  // 攻击动作：**判定段内**"最前点"必须单调向外伸（不许来回抽）。
  //
  // 两个容易写错的地方（都实测踩过）：
  // ① 只看起手→命中的全段会把"蓄力时把手收回身后"当成回缩（那里回缩是对的）；
  // ② 只看前手会把蹬腿误判为回缩（踢腿时最前点是脚）。
  const auto reach = [](const Pose& pose) {
    return std::max({pose.hand_front.x, pose.hand_back.x, pose.foot_front.x, pose.foot_back.x});
  };
  for (const MoveId move : {MoveId::Light, MoveId::Heavy, MoveId::Kick, MoveId::Special}) {
    const auto timing = gedou::timing_for(move);
    float previous = -1.0e9f;
    for (int frame = timing.startup; frame <= timing.startup + timing.active; ++frame) {
      const float current = reach(gedou::pose_for(move, frame));
      ST_CHECK(current >= previous - 0.001f);
      previous = current;
    }
    // 命中帧的"最前点"必须明显比站姿更远，否则"伸出去了"只是错觉。
    const float strike_reach = reach(gedou::pose_for(move, timing.startup + timing.active - 1));
    const float idle_reach = reach(gedou::pose_for(MoveId::Idle, 0));
    ST_CHECK(strike_reach > idle_reach + 10.0f);
    // 且必须真的够到这一招的判定盒远端（否则"盒到了、人没到"——
    // 看起来打中了，实际上是手臂凭空伸出一截）。招式表用局部单位，这里直接比。
    ST_CHECK(strike_reach >= gedou::attack_spec(move).reach_far - 1.0f);
  }
}

ST_TEST(gedou_pose_feet_on_ground_when_idle) {
  // 站姿/走路/防御必须**脚在地面**（局部 y ≈ 0），否则角色会飘或者陷进地面。
  // 允许小容差：走路有抬脚（另一只脚仍须在地面）。
  for (const MoveId move : {MoveId::Idle, MoveId::Guard, MoveId::Walk}) {
    const Pose pose = gedou::pose_for(move, 3, 0.0f);
    const float lowest = std::min(pose.foot_front.y, pose.foot_back.y);
    ST_CHECK(lowest <= 1.0f);
  }
}

ST_TEST(gedou_to_world_applies_body_scale) {
  // 缩放口径：世界坐标 = 局部 × `kBodyScale`（招式表与骨架同源的前提）。
  const Pose local = gedou::pose_for(MoveId::Idle, 0);
  const Pose world = local.to_world(0.0f, 0.0f, true);
  ST_CHECK(std::abs(world.hip.y - (-local.hip.y * kBodyScale)) < 0.001f);
  ST_CHECK(std::abs(world.hip.x - local.hip.x * kBodyScale) < 0.001f);
  // 地面线对齐：局部 y=0 的点必须正好落在 ground 上。
  const Pose ground = local.to_world(120.0f, 700.0f, true);
  ST_CHECK(std::abs(ground.foot_front.y - (700.0f - local.foot_front.y * kBodyScale)) < 0.001f);
}

ST_TEST(gedou_match_hp_never_negative_and_meter_bounded) {
  Match match{0, 1, Arena{}};
  const Intent idle{};
  for (int frame = 0; frame < 600; ++frame) {
    // 双方不停出招：把"血量会不会变负/槽会不会越界"在真实推进里跑出来。
    Intent left{};
    left.attack = (frame % 9 == 0) ? std::optional<MoveId>{MoveId::Heavy} : std::nullopt;
    (void)match.advance(left, idle);
    for (int side = 0; side < 2; ++side) {
      const FighterState& fighter = match.state(side);
      ST_CHECK(fighter.hp >= 0.0f);
      ST_CHECK(fighter.hp <= fighter.max_hp + 0.001f);
      ST_CHECK(fighter.meter >= 0.0f && fighter.meter <= gedou::kMeterMax + 0.001f);
    }
  }
}

ST_TEST(gedou_attack_reach_comes_from_pose) {
  // 招式表的 `reach_far` 必须**由命中帧姿态反算**（不是手填的手数字）。
  //
  // 为何重要：两者一分岔就是两种反向的假命中——盒比手远（空挥也命中）、
  // 手比盒远（看着打中了却没判上）。这条把“表 = 姿态”钉成不变量。
  for (const MoveId move : {MoveId::Light, MoveId::Heavy, MoveId::Kick, MoveId::Special}) {
    const Pose strike = gedou::keyframes_for(move)->strike;
    const float pose_reach = std::max({strike.hand_front.x, strike.hand_back.x,
                                       strike.foot_front.x, strike.foot_back.x});
    const float spec_far = gedou::attack_spec(move).reach_far;
    ST_CHECK(std::abs(spec_far - pose_reach) < 0.001f);
  }
  // 并且四个招式的触及距离必须递增（轻 < 重 < 踢 < 必杀）：
  // 这是玩法上“用起手帧换距离”的根据，也是 e2e 第 1 步断的东西。
  const std::vector<float> reaches{gedou::attack_spec(MoveId::Light).reach_far,
                                   gedou::attack_spec(MoveId::Heavy).reach_far,
                                   gedou::attack_spec(MoveId::Kick).reach_far,
                                   gedou::attack_spec(MoveId::Special).reach_far};
  for (std::size_t index = 1; index < reaches.size(); ++index) {
    ST_CHECK(reaches[index] > reaches[index - 1]);
  }
}

ST_TEST(gedou_ai_actually_lands_hits) {
  // AI 必须**真的打到人**。
  //
  // 这条坑的实测代价：AI 最初拿“世界像素间距”去比招式表的**局部单位**距离，
  // 于是永远认为“还差得远”——一路逼近、永不出手，而画面看起来完全像
  // “AI 在走位”（287 帧一次未命中）。从现象无法反推原因，只能靠这条断言。
  for (const auto difficulty :
       {gedou::Difficulty::Easy, gedou::Difficulty::Normal, gedou::Difficulty::Hard}) {
    Match match{0, 1, Arena{}};
    AiController ai{0x1234ABCDU, difficulty};
    float damage_taken = 0.0f;
    const Intent idle{};
    for (int frame = 0; frame < 1200; ++frame) {
      const Intent opponent = ai.think(match, 1);
      (void)match.advance(idle, opponent);
      damage_taken = match.state(0).max_hp - match.state(0).hp;
      if (damage_taken > 0.0f) break;
    }
    ST_CHECK(damage_taken > 0.0f);   // 哪个难度失败由外层循环的打印指出
  }
}

ST_TEST(gedou_ai_has_no_unit_mismatch) {
  // 钉住“AI 的进攻阈值 = 招式表触及距离 + 半身体宽”（**单位一致**）。
  //
  // 为何必须直接对拍这个数：单位错（局部 vs 世界）不会让 AI 失效，
  // 只让“出手距离”悄悄偏小 40%——画面上看不出来，而行为断言（“能打中人”
  // 或“够不着时不出手”）都会被 AI 自己的走位掩盖掉。
  const AiController ai{0x55AA55AAU, gedou::Difficulty::Hard};
  const float expected =
      gedou::attack_spec(MoveId::Heavy).reach_far * kBodyScale + 30.0f * kBodyScale;
  ST_CHECK(std::abs(ai.aggression_range() - expected) < 0.001f);
  // 顺便硬编码校验：换算后的数值必须落在“人体尺度”里（≈150~220 逻辑像素），
  // 防止“两边一起算错”（例如把 kBodyScale 改成 0.1）。
  ST_CHECK(ai.aggression_range() > 120.0f && ai.aggression_range() < 320.0f);
}

ST_TEST(gedou_match_hitbox_only_during_active_frames) {
  Match match{0, 1, Arena{}};
  const auto timing = gedou::scaled_timing(MoveId::Light, gedou::fighter_profile(0).speed);
  ST_CHECK(match.force_move(0, MoveId::Light));
  const Intent idle{};
  for (int frame = 0; frame < timing.total() + 4; ++frame) {
    const FighterState& me = match.state(0);
    const bool box = !match.active_hitbox(0).is_empty();
    const bool expected = me.move == MoveId::Light && me.move_frame >= timing.startup &&
                          me.move_frame < timing.startup + timing.active;
    ST_CHECK(box == expected);
    (void)match.advance(idle, idle);
  }
}

ST_TEST(gedou_match_special_requires_meter) {
  Match match{0, 1, Arena{}};
  // 槽不满：force_move 必须**如实拒绝**（而不是放出去但不扣槽）。
  ST_CHECK(!match.force_move(0, MoveId::Special));
  match.charge_meter(0);
  ST_CHECK(match.force_move(0, MoveId::Special));
  const Intent idle{};
  (void)match.advance(idle, idle);
  // 放出去就扣槽（且只扣一次）。
  ST_CHECK(match.state(0).meter < gedou::kMeterMax);
  const float after = match.state(0).meter;
  for (int frame = 0; frame < 10; ++frame) (void)match.advance(idle, idle);
  ST_CHECK(std::abs(match.state(0).meter - after) < 0.001f);
}

ST_TEST(gedou_match_ai_is_deterministic) {
  // 同种子 + 同输入序列 → 完全相同的对局。AI 若用了 `std::rand`/时间/未初始化读，
  // 这条会随机变红——它是"AI 可复现"的唯一机械保证。
  const auto play = [] {
    Match match{0, 1, Arena{}};
    AiController ai{0xA1B2C3D4U, gedou::Difficulty::Normal};
    Intent player{};
    player.move = 1.0f;
    for (int frame = 0; frame < 240; ++frame) {
      Intent mine = player;
      if (frame % 17 == 0) mine.attack = MoveId::Light;
      (void)match.advance(mine, ai.think(match, 1));
    }
    return std::pair<float, float>{match.state(0).x, match.state(1).hp};
  };
  const auto first = play();
  const auto second = play();
  ST_CHECK(std::abs(first.first - second.first) < 0.001f);
  ST_CHECK(std::abs(first.second - second.second) < 0.001f);
}

ST_TEST(gedou_match_knockdown_ends_round) {
  Match match{0, 1, Arena{}};
  const Intent idle{};
  ST_CHECK(match.apply_damage(1, 1.0e6f));
  ST_CHECK(match.state(1).hp == 0.0f);
  ST_CHECK(match.phase() == gedou::RoundPhase::Over);
  ST_CHECK(match.winner() == 0);
  // 结算态下胜者必须停在**胜利姿态**（不是待机）——这是"看不出谁赢了"的根因。
  for (int frame = 0; frame < 60; ++frame) (void)match.advance(idle, idle);
  ST_CHECK(match.state(0).move == MoveId::Victory);
  ST_CHECK(match.state(1).move == MoveId::Defeat || match.state(1).move == MoveId::KnockDown);
}

ST_TEST(gedou_arena_resize_keeps_fight_state) {
  // 窗口缩放（`set_arena`）只该动几何：血量/槽/招式帧都必须原样保留。
  // 旧写法是"重建 Match 再补差"，那条路会静默清空战况。
  Match match{0, 1, Arena{}};
  const Intent idle{};
  ST_CHECK(match.apply_damage(1, 300.0f));
  match.charge_meter(0);
  for (int frame = 0; frame < 5; ++frame) (void)match.advance(idle, idle);
  const float hp = match.state(1).hp;
  const float meter = match.state(0).meter;

  Arena wider{};
  wider.left = 0.0f;
  wider.right = 2000.0f;
  wider.ground = 620.0f;
  wider.start_gap = 300.0f;
  match.set_arena(wider);
  ST_CHECK(std::abs(match.state(1).hp - hp) < 0.001f);
  ST_CHECK(std::abs(match.state(0).meter - meter) < 0.001f);
  ST_CHECK(match.arena().right == 2000.0f);
  // 位置必须仍在场地内（夹取生效）。
  ST_CHECK(match.state(0).x >= wider.left && match.state(0).x <= wider.right);
}
