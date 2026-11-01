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

// ══════════════════════════════════════════════════════════════════════
// 专属招式机制（甜甜圈 / 户圣）—— 详见本条上方的通用规则小节
// ══════════════════════════════════════════════════════════════════════
using gedou::AttackSpec;
using gedou::MoveMechanic;
using gedou::RoundPhase;
using gedou::StatusKind;

namespace {


constexpr auto near(float a, float b, float tol = 1e-3f) -> bool {
  return std::abs(a - b) <= tol;
}

/// 角色在档案表里的下标（按 key 找，不硬编码序号——加角色时序号会变）。
auto profile_index(std::string_view key) -> std::size_t {
  for (std::size_t index = 0; index < gedou::kFighterCount; ++index) {
    if (gedou::fighter_profile(index).key == key) return index;
  }
  return gedou::kFighterCount;   // 找不到：让调用方的 ST_CHECK 红
}

auto make_match(std::string_view left_key, std::string_view right_key) -> Match {
  Arena arena{};
  arena.left = 0.0f;
  arena.right = 1200.0f;
  arena.ground = 560.0f;
  return Match{profile_index(left_key), profile_index(right_key), arena};
}

/// 推进若干帧（双方都待机）。
void idle_for(Match& match, int frames) {
  static const Intent kIdle{};
  for (int i = 0; i < frames; ++i) (void)match.advance(kIdle, kIdle);
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════
// 角色与招式表的接线
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_new_characters_are_registered) {
  const std::size_t donut = profile_index("donut");
  const std::size_t husheng = profile_index("husheng");
  ST_CHECK(donut < gedou::kFighterCount);
  ST_CHECK(husheng < gedou::kFighterCount);
  ST_CHECK(gedou::fighter_profile(donut).name == "甜甜圈");
  ST_CHECK(gedou::fighter_profile(husheng).name == "户圣");
}

ST_TEST(gd_new_characters_have_their_own_moves) {
  const std::size_t donut = profile_index("donut");
  const std::size_t husheng = profile_index("husheng");
  // 三个技能位各自对上一个专属招式（不是共用的 light/heavy/special）。
  ST_CHECK(gedou::fighter_move(donut, 0) == MoveId::DonutToss);
  ST_CHECK(gedou::fighter_move(donut, 1) == MoveId::PuffSpray);
  ST_CHECK(gedou::fighter_move(donut, 2) == MoveId::WaistGrab);
  ST_CHECK(gedou::fighter_move(husheng, 0) == MoveId::AppleDrop);
  ST_CHECK(gedou::fighter_move(husheng, 1) == MoveId::AppleGuard);
  ST_CHECK(gedou::fighter_move(husheng, 2) == MoveId::AppleLife);
  // 技能名就是需求里给的名字（界面上要显示它们）。
  ST_CHECK(gedou::fighter_move_label(donut, 0) == "真好吃");
  ST_CHECK(gedou::fighter_move_label(donut, 2) == "摸腰");
  ST_CHECK(gedou::fighter_move_label(husheng, 0) == "苹果手机");
  ST_CHECK(gedou::fighter_move_label(husheng, 2) == "苹果人生");
}

ST_TEST(gd_every_new_move_is_an_attack) {
  // `is_attack` 漏加新招式 ⇒ 判定盒返回空矩形 ⇒ “能放但打不到”。
  // 这条直接钉住那个门。
  for (const MoveId move : {MoveId::DonutToss, MoveId::PuffSpray, MoveId::WaistGrab,
                            MoveId::AppleDrop, MoveId::AppleGuard, MoveId::AppleLife}) {
    ST_CHECK(gedou::is_attack(move));
    // 每个都有帧数与关键帧（否则姿态层拿不到东西，角色会“原地不动做完一招”）。
    ST_CHECK(gedou::timing_for(move).total() > 0);
    ST_CHECK(gedou::keyframes_for(move).has_value());
  }
}

ST_TEST(gd_move_specs_carry_their_mechanics) {
  ST_CHECK(gedou::attack_spec(MoveId::DonutToss).mechanic == MoveMechanic::Projectile);
  ST_CHECK(gedou::attack_spec(MoveId::PuffSpray).mechanic == MoveMechanic::FanSpray);
  ST_CHECK(gedou::attack_spec(MoveId::WaistGrab).mechanic == MoveMechanic::GripFreeze);
  ST_CHECK(gedou::attack_spec(MoveId::AppleDrop).mechanic == MoveMechanic::SkyDrop);
  ST_CHECK(gedou::attack_spec(MoveId::AppleGuard).mechanic == MoveMechanic::OrbitShield);
  ST_CHECK(gedou::attack_spec(MoveId::AppleLife).mechanic == MoveMechanic::Transform);
  // 摸腰的“扣 50% 血”必须真的写进表里。
  ST_CHECK(near(gedou::attack_spec(MoveId::WaistGrab).damage_ratio, 0.5f));
  // 冻结 2 秒 = 120 帧 @60Hz。
  ST_CHECK(gedou::attack_spec(MoveId::WaistGrab).status_frames == 120);
}

// ════════════════════════════════════════════════════════════════════════
// 真好吃：投射物
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_donut_toss_spawns_a_projectile) {
  Match match = make_match("donut", "husheng");
  match.place(0, 300.0f);
  match.place(1, 700.0f);
  match.force_move(0, MoveId::DonutToss);
  ST_CHECK(match.projectiles().empty() == false);   // 起手那一帧就该有甜甜圈
  const auto& shot = match.projectiles().front();
  ST_CHECK(shot.owner == 0);
  ST_CHECK(shot.velocity.x > 0.0f);                 // 朝右飞
  ST_CHECK(shot.life > 0);
}

ST_TEST(gd_projectile_travels_and_hits) {
  Match match = make_match("donut", "husheng");
  match.place(0, 300.0f);
  match.place(1, 760.0f);
  const float before = match.state(1).hp;
  match.force_move(0, MoveId::DonutToss);
  // 飞到对方身上（给足帧数：速度 9 × 1.6 = 14.4 px/帧，460px 要 ~32 帧）。
  idle_for(match, 90);
  ST_CHECK(match.state(1).hp < before);             // 真掉血了
  ST_CHECK(match.projectiles().empty());            // 命中后消失
}

ST_TEST(gd_projectile_does_not_hit_its_owner) {
  // 投射物只打对方——否则“抛出去先炸自己”这种荒谬行为会静默存在。
  Match match = make_match("donut", "donut");
  match.place(0, 300.0f);
  match.place(1, 900.0f);   // 对方很远：投射物会飞完全程
  const float self_hp = match.state(0).hp;
  match.force_move(0, MoveId::DonutToss);
  idle_for(match, 40);
  ST_CHECK(near(match.state(0).hp, self_hp));       // 自己一滴没掉
}

ST_TEST(gd_projectile_expires_when_nobody_is_hit) {
  Match match = make_match("donut", "husheng");
  match.place(0, 200.0f);
  match.place(1, 1150.0f);   // 站在最边上，但仍会被飞到的位置之外
  match.force_move(0, MoveId::DonutToss);
  // 打空：投射物必须有寿命，否则它会永远留在场上来回穿。
  idle_for(match, 200);
  ST_CHECK(match.projectiles().empty());
}

// ════════════════════════════════════════════════════════════════════════
// 泡芙：扇形
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_fan_spray_has_range_limit) {
  // 扇形的核心是“有射程、有张角”。这里测**射程**：
  // 贴脸能打到，退到射程外就打不到。
  //
  // ⚠ 不测“背后打不到”：角色每帧会自动转向对手（`face_opponent`），
  // 出招时永远是面向对手的——"背后"这个局面在正常对局里不存在，
  // 为它写断言只会得到一条永远不可能失败的用例。
  const auto damage_at = [](float gap) {
    Match match = make_match("donut", "husheng");
    match.place(0, 400.0f);
    match.place(1, 400.0f + gap);
    const float before = match.state(1).hp;
    match.force_move(0, MoveId::PuffSpray);
    idle_for(match, 40);
    return before - match.state(1).hp;
  };
  const AttackSpec spec = gedou::attack_spec(MoveId::PuffSpray);
  const float range = spec.fan_range * gedou::kBodyScale;
  const float near_damage = damage_at(range * 0.55f);   // 射程中段
  const float far_damage = damage_at(range + 120.0f);   // 远超射程
  ST_CHECK(near_damage > 0.0f);
  ST_CHECK(near(far_damage, 0.0f));
  ST_CHECK(near_damage > 0.0f && far_damage == 0.0f);
}

ST_TEST(gd_fan_spray_is_multi_hit) {
  // 扇形的定位是“压着人打”，所以它必须**多段**——
  // 单段的话它就只是一发伤害很低的近战，那还不如直接出拳。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 520.0f);
  const float before = match.state(1).hp;
  match.force_move(0, MoveId::PuffSpray);
  idle_for(match, 40);
  const float total = before - match.state(1).hp;
  const AttackSpec spec = gedou::attack_spec(MoveId::PuffSpray);
  ST_CHECK(total > spec.damage * 1.5f);   // 至少打中两段
}

// ════════════════════════════════════════════════════════════════════════
// 摸腰：冻结 + 半血
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_waist_grab_freezes_and_burns_half_hp) {
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 460.0f);   // 必须贴近（这一招射程很短）
  const float before = match.state(1).hp;
  const float max_hp = match.state(1).max_hp;
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 40);

  const FighterState& victim = match.state(1);
  ST_CHECK(victim.hp < before);
  // **半血**：固定 40 + 50% × max_hp —— 这是需求里最硬的一条数值。
  const float expected = 40.0f + 0.5f * max_hp;
  ST_CHECK(near(before - victim.hp, expected, 2.0f));
  // 冻结：2 秒 = 120 帧（刚命中时几乎是满的）。
  ST_CHECK(victim.frozen > 100);
}

ST_TEST(gd_frozen_victim_cannot_act) {
  // 冻结的**后果**：不能行动。只看 `frozen > 0` 不够——
  // 若 `busy()` 没把它算进去，角色被冻着却还能走能打。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 460.0f);
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 40);
  ST_CHECK(match.state(1).frozen > 0);

  // 等击退惯性衰减完再取样（被摸腰那一下会被推开一段，那是**击退**不是走动）。
  idle_for(match, 20);
  ST_CHECK(match.state(1).frozen > 0);                // 还在冻着
  const float x_before = match.state(1).x;
  // 被冻期间持续按“前进 + 出招”
  Intent walk{};
  walk.move = 1.0f;
  walk.attack = gedou::fighter_move(1, 0);
  for (int i = 0; i < 60; ++i) (void)match.advance(Intent{}, walk);
  // 判据一：**一步没挪**（走动被禁止）。
  ST_CHECK(near(match.state(1).x, x_before));
  // 判据二：**没出招**（`move` 没变成攻击招式）。
  ST_CHECK(!gedou::is_attack(match.state(1).move));
}

ST_TEST(gd_freeze_expires) {
  // 冻结必须**会过期**——不会过期的控制 = 永久封印对手。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 460.0f);
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 40);
  ST_CHECK(match.state(1).frozen > 0);
  idle_for(match, 200);   // 远超 120 帧
  ST_CHECK(match.state(1).frozen == 0);
}

ST_TEST(gd_waist_grab_can_be_blocked) {
  // 能挡是它不成为“无脑必胜”的关键（前摇 22 帧，看见了就能按防）。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 460.0f);
  const float before = match.state(1).hp;
  // 对方全程按住防御
  Intent guard{};
  guard.guard = true;
  match.force_move(0, MoveId::WaistGrab);
  for (int i = 0; i < 40; ++i) (void)match.advance(Intent{}, guard);
  // 挡住时只吃削韧，不该掉半血；也不该被冻。
  ST_CHECK(before - match.state(1).hp < 100.0f);
  ST_CHECK(match.state(1).frozen == 0);
}

// ════════════════════════════════════════════════════════════════════════
// 苹果手机：天降
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_apple_drop_warns_before_falling) {
  // 有预警才有“躲”这个选项——否则就是无法反制的瞬发伤害。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 700.0f);
  match.force_move(0, MoveId::AppleDrop);
  ST_CHECK(!match.sky_warnings().empty());
  const int warn = match.sky_warnings().front().frames_left;
  ST_CHECK(warn > 0);   // 起手时还在倒计时，没落下
}

ST_TEST(gd_apple_drop_can_be_dodged) {
  // 落点是**生成那一刻**的对手位置：跑开就落空。这是它有互动性的证明。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 700.0f);
  const float hp_before = match.state(1).hp;
  match.force_move(0, MoveId::AppleDrop);
  // 预警期间跑远（往右跑 40 帧）
  Intent flee{};
  flee.move = 1.0f;
  for (int i = 0; i < 60; ++i) (void)match.advance(Intent{}, flee);
  ST_CHECK(near(match.state(1).hp, hp_before));   // 一滴没掉（躲掉了）
  ST_CHECK(match.sky_warnings().empty());          // 预警已消费
}

ST_TEST(gd_apple_drop_hits_if_you_stay) {
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 700.0f);
  const float hp_before = match.state(1).hp;
  match.force_move(0, MoveId::AppleDrop);
  idle_for(match, 80);   // 站着不动
  ST_CHECK(match.state(1).hp < hp_before);
}

// ════════════════════════════════════════════════════════════════════════
// 苹果小区：护盾 + 反弹
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_apple_guard_grants_shield_to_self) {
  // 护盾挂在**自己**身上（不是对手）——这是它与其它招最容易被写反的一点。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 700.0f);
  match.force_move(0, MoveId::AppleGuard);
  ST_CHECK(match.state(0).shielded > 0);
  ST_CHECK(match.state(1).shielded == 0);
}

ST_TEST(gd_shield_blocks_and_reflects) {
  // 核心机制：护盾挡下伤害，并把一部分**弹回攻击方**。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 480.0f);
  match.force_move(0, MoveId::AppleGuard);
  idle_for(match, 6);   // 护盾成形
  ST_CHECK(match.state(0).shielded > 0);

  const float shielded_hp = match.state(0).hp;
  const float attacker_hp = match.state(1).hp;
  // 对方出重拳（正面来）
  match.force_move(1, MoveId::Heavy);
  idle_for(match, 40);

  // ① 被护盾的人几乎不掉血（护盾完全格挡）
  ST_CHECK(match.state(0).hp > shielded_hp - 1.0f);
  // ② 攻击方反而掉血（反弹生效）
  ST_CHECK(match.state(1).hp < attacker_hp);
}

ST_TEST(gd_shield_expires) {
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 700.0f);
  match.force_move(0, MoveId::AppleGuard);
  idle_for(match, 6);
  ST_CHECK(match.state(0).shielded > 0);
  idle_for(match, 200);
  ST_CHECK(match.state(0).shielded == 0);
}

// ════════════════════════════════════════════════════════════════════════
// 苹果人生：变身回血 + 减伤 + 不能动
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_apple_life_regenerates_hp) {
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 900.0f);
  match.apply_damage(0, 400.0f);   // 先掉血，才有得回
  const float before = match.state(0).hp;
  match.force_move(0, MoveId::AppleLife);
  idle_for(match, 120);   // 2 秒
  ST_CHECK(match.state(0).hp > before);
}

ST_TEST(gd_apple_life_cannot_move) {
  // 回血的代价：**不能动**。否则“一边跑一边回血”就是无敌。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 900.0f);
  match.force_move(0, MoveId::AppleLife);
  idle_for(match, 10);
  ST_CHECK(match.state(0).apple_form > 0);
  const float x_before = match.state(0).x;
  Intent walk{};
  walk.move = 1.0f;
  for (int i = 0; i < 40; ++i) (void)match.advance(walk, Intent{});
  ST_CHECK(near(match.state(0).x, x_before));
}

ST_TEST(gd_apple_life_reduces_damage) {
  // 减伤必须真的生效：同一招在“变身中”与“平时”造成的伤害要明显不同。
  const auto damage_taken = [](bool in_apple_form) {
    Match match = make_match("donut", "husheng");
    match.place(0, 400.0f);
    match.place(1, 480.0f);
    if (in_apple_form) {
      match.force_move(0, MoveId::AppleLife);
      idle_for(match, 12);
    }
    const float before = match.state(0).hp;
    match.force_move(1, MoveId::Heavy);
    idle_for(match, 40);
    return before - match.state(0).hp;
  };
  const float normal = damage_taken(false);
  const float reduced = damage_taken(true);
  ST_CHECK(normal > 0.0f);
  ST_CHECK(reduced < normal * 0.7f);   // 至少减掉三成
}

ST_TEST(gd_apple_life_ends_and_returns_to_idle) {
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 900.0f);
  match.force_move(0, MoveId::AppleLife);
  idle_for(match, 12);
  ST_CHECK(match.state(0).apple_form > 0);
  idle_for(match, 220);
  ST_CHECK(match.state(0).apple_form == 0);
  // 变身结束后必须**能动**（卡在苹果态出不来就是最糟的 bug）。
  ST_CHECK(match.state(0).move != MoveId::AppleLife);
  const float x_before = match.state(0).x;
  Intent walk{};
  walk.move = 1.0f;
  for (int i = 0; i < 20; ++i) (void)match.advance(walk, Intent{});
  ST_CHECK(std::abs(match.state(0).x - x_before) > 1.0f);
}

// ════════════════════════════════════════════════════════════════════════
// 与既有规则的相处
// ════════════════════════════════════════════════════════════════════════

ST_TEST(gd_new_moves_do_not_break_round_flow) {
  // 新角色打完一局仍要正常结束（倒地 + 胜者姿态）。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 470.0f);
  match.apply_damage(1, 990.0f);   // 把对手打到残血
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 60);
  ST_CHECK(match.state(1).hp <= 0.0f);
  ST_CHECK(match.phase() == RoundPhase::Over);
  ST_CHECK(match.winner().has_value());
  ST_CHECK(*match.winner() == 0);
}

ST_TEST(gd_two_new_characters_can_fight) {
  // 两个技能型角色互打不能崩，且**真的能互相造成伤害**（投射物/护盾/冻结同时在飞）。
  //
  // ⚠ 输入节奏要像真人：**空闲时才发招**。
  // 每帧都设 `intent.attack` 会得到“招式永远在起手第一帧被打断”——
  // 屏幕上看不出异常（动作确实在演），但一次都打不中（实测踩过：
  // 这条断言写错时判红，而实现其实完全正常）。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 600.0f);
  int slot[2] = {0, 0};
  bool any_hit = false;
  for (int frame = 0; frame < 900; ++frame) {
    Intent inputs[2];
    for (int side = 0; side < 2; ++side) {
      const FighterState& me = match.state(side);
      const FighterState& foe = match.state(1 - side);
      const float gap = std::abs(foe.x - me.x);
      if (gap > 150.0f) {
        inputs[side].move = 1.0f;   // 先靠近
      } else if (me.can_act() && me.move == MoveId::Idle && frame % 40 == side * 17) {
        inputs[side].attack = gedou::fighter_move(static_cast<std::size_t>(side),
                                                  static_cast<std::size_t>(slot[side] % 3));
        ++slot[side];
      }
    }
    const auto events = match.advance(inputs[0], inputs[1]);
    if (!events.hits.empty()) any_hit = true;
    if (match.phase() != RoundPhase::Fight) break;
  }
  ST_CHECK(any_hit);                                     // 至少打中过一次
  ST_CHECK(match.state(0).hp >= 0.0f);
  ST_CHECK(match.state(1).hp >= 0.0f);
  ST_CHECK(match.state(0).hp < match.state(0).max_hp ||
           match.state(1).hp < match.state(1).max_hp);
}

// ════════════════════════════════════════════════════════════════════════
// 平衡结构：强招必须有抗衡手段
// ════════════════════════════════════════════════════════════════════════

/// 摸腰不能构成「永久封印」——控制反制必须有喘息窗口。
///
/// 为何要这条：摸腰是"冻结 2 秒 + 扣 50% 血"的全表最强单招。
/// 而"同一个键一直按"最容易变成无解：若冻结能被**反复刷新**，
/// 对手就永远动不了。
///
/// 实测代价（本轮真撞到）：摸腰方连按 37 次，对手一次都没还手——
/// 屏幕上双方都"在动"，但一方完全无法行动。修法是：**已冻结时不再刷新**，
/// 2 秒到就必须放人出来（这 2 秒足够对手挨两下，但也给了他反打的窗口）。
ST_TEST(gd_freeze_cannot_be_refreshed_while_active) {
  // 要测的是「**冻结仍然生效时**再次命中，会不会把剩余帧重置回满值」。
  //
  // ⚠ 时序必须算准：摸腰有 22 帧前摇，所以"第二次命中"实际发生在起手后 ~23 帧。
  // 若此时冻结已经自然走完（120 帧），那 `frozen <= 0` 成立、写入满值**完全正确**
  // ——早先版本就栽在这里：断言红了两边（真实现与回退版都红），说明测的不是刷新。
  // 正确做法：从**第 1 帧就开始起手**（不等），让命中落在冻结还剩很多的时候。
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 440.0f);
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 40);
  ST_CHECK(match.state(1).frozen > 0);
  const int after_first = match.state(1).frozen;
  ST_CHECK(after_first > 60);   // 刚冻上，剩很多

  // 立刻再摸一次（前摇 22 帧 ⇒ 命中时冻结还剩约 120-22-5 ≈ 90 帧，仍然生效）
  match.place(0, 400.0f);
  match.place(1, 440.0f);
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 30);
  const int after_second = match.state(1).frozen;

  // 正确实现（不刷新）：剩余帧只会在原基础上**继续减少**，
  // 绝不会回到"满值区间"（>100）。
  // 刷新实现（max）：会在这里跳回 ~110+ ⇒ 红。
  ST_CHECK(after_second < after_first - 15);
}

/// 摸腰的伤害必须是**半血**（这是需求里最硬的一条数值）。
ST_TEST(gd_waist_grab_deals_half_max_hp) {
  Match match = make_match("donut", "husheng");
  match.place(0, 400.0f);
  match.place(1, 440.0f);   // 必须贴脸（这一招射程很短）
  const float before = match.state(1).hp;
  const float max_hp = match.state(1).max_hp;
  match.force_move(0, MoveId::WaistGrab);
  idle_for(match, 40);
  const float dealt = before - match.state(1).hp;
  // 固定 40 + 50% × max_hp
  ST_CHECK(near(dealt, 40.0f + 0.5f * max_hp, 2.0f));
}

/// 摸腰的**代价**必须真实存在：前摇 + 收招远超其它招。
///
/// 这是"长前摇平衡"方案的核心断言——数值一旦被改短，这条立刻红。
ST_TEST(gd_waist_grab_is_the_slowest_move) {
  const gedou::MoveTiming grab = gedou::timing_for(MoveId::WaistGrab);
  const gedou::MoveTiming heavy = gedou::timing_for(MoveId::Heavy);
  const gedou::MoveTiming special = gedou::timing_for(MoveId::Special);
  // 起手比必杀还长：它是"看得见就能躲"的那一类。
  ST_CHECK(grab.startup > heavy.startup);
  ST_CHECK(grab.startup >= special.startup);
  // 总帧数全场最长（代价最重）。
  ST_CHECK(grab.total() > heavy.total());
  ST_CHECK(grab.total() >= special.total());
}
