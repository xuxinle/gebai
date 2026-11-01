#include "pose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace gedou {

namespace {

/// 骨架比例（局部单位，绘制前统一乘 `kBodyScale`）。
///
/// 这几个常量集中在这里，是因为"人物看起来对不对"几乎全由它们决定：
/// 腿太长像筷子、躯干太短像火柴人、头太大像萌系。集中成表之后，
/// 一次只改一个量就能看出影响（而不是在 11 个姿态里各改一遍）。
namespace body {
inline constexpr float kHipY = 88.0f;
inline constexpr float kChestY = 132.0f;
inline constexpr float kHeadY = 162.0f;
inline constexpr float kHeadRadius = 15.0f;
inline constexpr float kKneeY = 46.0f;
/// 脚踝高度。**必须接近 0**：这是“脚踩在地上”的定义；写 8 会让整个人悬在地面上
/// （实测代价：截图里角色脚下多出一条缝，而“谁会去量脚底”正是单测存在的理由）。
inline constexpr float kFootY = 0.0f;
/// 肩点相对躯干中线的横向偏移（正面 / 背面）。**必须有**：两条手臂都从
/// 躯干中线一个点伸出时，视觉上会糊成一坨（实测截图里"手臂不见了"）。
inline constexpr float kShoulderFrontX = 14.0f;
inline constexpr float kShoulderBackX = -9.0f;
}  // namespace body

/// 站立骨架（格斗架势）。所有姿态都从它出发，这样"某个关节飞出去"只会影响单点。
///
/// 为什么不是“直立站立”：角色默认得看得像个要打架的人——双脚前后开、膝微屈、
/// 双拳提到胸前。直立+双手垂下的姿态在截图里读起来像“排队”，不像格斗。
[[nodiscard]] auto neutral_pose() -> Pose {
  Pose pose{};
  pose.hip = math::Point{0.0f, body::kHipY};
  pose.chest = math::Point{1.0f, body::kChestY};
  pose.head = math::Point{3.0f, body::kHeadY};
  pose.shoulder_front = math::Point{body::kShoulderFrontX, body::kChestY - 2.0f};
  pose.shoulder_back = math::Point{body::kShoulderBackX, body::kChestY - 2.0f};
  // 双拳护在胸前（前手提得稍高）——这是格斗游戏“待机”该有的样子。
  // 手要落在**身体轮廓外侧**：否则两个拳套会叠在躯干上（截图里像胸前挂了两块牌子）。
  pose.elbow_front = math::Point{body::kShoulderFrontX + 6.0f, 104.0f};
  pose.hand_front = math::Point{body::kShoulderFrontX + 10.0f, 116.0f};
  pose.elbow_back = math::Point{body::kShoulderBackX - 7.0f, 102.0f};
  pose.hand_back = math::Point{body::kShoulderBackX - 9.0f, 112.0f};
  // 前后站位：前脚在前、后脚在后，两膝都微屈（腿不伸直就不像“僵立”）。
  pose.knee_front = math::Point{12.0f, body::kKneeY + 2.0f};
  pose.foot_front = math::Point{17.0f, body::kFootY};
  pose.knee_back = math::Point{-12.0f, body::kKneeY + 2.0f};
  pose.foot_back = math::Point{-17.0f, body::kFootY};
  pose.head_radius = body::kHeadRadius;
  pose.lean = 0.02f;
  return pose;
}

/// 在原地改一只手（肩不动；攻击时只动肘/手，肩由躯干带着走）。
void set_arm(Pose& pose, bool front, math::Point elbow, math::Point hand) {
  if (front) {
    pose.elbow_front = elbow;
    pose.hand_front = hand;
  } else {
    pose.elbow_back = elbow;
    pose.hand_back = hand;
  }
}

/// 移重心：整体平移髋/胸/头（腿脚由调用方按需另调）。
void shift_torso(Pose& pose, math::Point delta) {
  pose.hip.x += delta.x;
  pose.hip.y += delta.y;
  pose.chest.x += delta.x;
  pose.chest.y += delta.y;
  pose.head.x += delta.x;
  pose.head.y += delta.y;
  pose.shoulder_front.x += delta.x;
  pose.shoulder_front.y += delta.y;
  pose.shoulder_back.x += delta.x;
  pose.shoulder_back.y += delta.y;
  // 手臂跟着躯干走（相对肩的位置不变）。
  pose.elbow_front.x += delta.x;
  pose.elbow_front.y += delta.y;
  pose.hand_front.x += delta.x;
  pose.hand_front.y += delta.y;
  pose.elbow_back.x += delta.x;
  pose.elbow_back.y += delta.y;
  pose.hand_back.x += delta.x;
  pose.hand_back.y += delta.y;
}

/// 防守：重心后坐、双手抬起护住头，膝更弯。
[[nodiscard]] auto guard_pose() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-5.0f, -4.0f});
  set_arm(pose, true, math::Point{16.0f, 126.0f}, math::Point{13.0f, 146.0f});
  set_arm(pose, false, math::Point{-14.0f, 124.0f}, math::Point{-2.0f, 144.0f});
  pose.knee_front = math::Point{9.0f, 42.0f};
  pose.knee_back = math::Point{-17.0f, 40.0f};
  pose.foot_front = math::Point{16.0f, body::kFootY};
  pose.foot_back = math::Point{-25.0f, body::kFootY};
  pose.lean = -0.06f;
  return pose;
}

/// 轻拳起手：后手收、前肩略沉。
[[nodiscard]] auto light_wind() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-2.0f, -2.0f});
  set_arm(pose, true, math::Point{12.0f, 122.0f}, math::Point{10.0f, 108.0f});
  set_arm(pose, false, math::Point{-14.0f, 116.0f}, math::Point{-8.0f, 110.0f});
  pose.lean = -0.04f;
  return pose;
}

/// 轻拳命中：前手直出到最远。
[[nodiscard]] auto light_strike() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{4.0f, -2.0f});
  set_arm(pose, true, math::Point{28.0f, 122.0f}, math::Point{52.0f, 124.0f});
  set_arm(pose, false, math::Point{-8.0f, 114.0f}, math::Point{-4.0f, 104.0f});
  pose.knee_front = math::Point{16.0f, 44.0f};
  pose.knee_back = math::Point{-16.0f, 44.0f};
  pose.foot_front = math::Point{24.0f, body::kFootY};
  pose.foot_back = math::Point{-24.0f, body::kFootY};
  pose.lean = 0.10f;
  return pose;
}

/// 重拳起手：大幅扭腰拉后手。
[[nodiscard]] auto heavy_wind() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-9.0f, -5.0f});
  set_arm(pose, true, math::Point{12.0f, 118.0f}, math::Point{18.0f, 128.0f});
  set_arm(pose, false, math::Point{-26.0f, 112.0f}, math::Point{-34.0f, 100.0f});
  pose.knee_front = math::Point{7.0f, 42.0f};
  pose.knee_back = math::Point{-22.0f, 40.0f};
  pose.foot_front = math::Point{14.0f, body::kFootY};
  pose.foot_back = math::Point{-30.0f, body::kFootY};
  pose.lean = -0.12f;
  return pose;
}

/// 重拳命中：后拳甩到最前、身体压上去。
///
/// 注意前手要**收在身后**（不能比站姿更靠前）：重拳是后手打的，
/// 前手如果跟着往前会“两拳同源”，既看不出主次也丢了反击空间。
/// 后拳必须伸得比重拳的轻拳（`light_strike`）更远——这是“重拳更远更重”的根据。
[[nodiscard]] auto heavy_strike() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{12.0f, -4.0f});
  set_arm(pose, true, math::Point{10.0f, 116.0f}, math::Point{2.0f, 104.0f});
  set_arm(pose, false, math::Point{34.0f, 128.0f}, math::Point{64.0f, 126.0f});
  pose.knee_front = math::Point{26.0f, 44.0f};
  pose.knee_back = math::Point{-14.0f, 42.0f};
  pose.foot_front = math::Point{40.0f, body::kFootY};
  pose.foot_back = math::Point{-22.0f, body::kFootY};
  pose.lean = 0.22f;
  return pose;
}

/// 蹬腿起手：抬前膝（膝比脚高但仍在脚上方，避免“折叠”）。
[[nodiscard]] auto kick_wind() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-3.0f, 0.0f});
  set_arm(pose, true, math::Point{10.0f, 120.0f}, math::Point{4.0f, 112.0f});
  set_arm(pose, false, math::Point{-16.0f, 116.0f}, math::Point{-10.0f, 104.0f});
  pose.knee_front = math::Point{16.0f, 62.0f};
  pose.foot_front = math::Point{8.0f, 30.0f};
  pose.knee_back = math::Point{-13.0f, body::kKneeY + 3.0f};
  pose.foot_back = math::Point{-18.0f, body::kFootY};
  pose.lean = -0.10f;
  return pose;
}

/// 蹬腿命中：前腿伸展、后腿支撑。
///
/// 几何要点（实测改过三轮）：
/// ① **胯 → 膝 → 足单调向外**（x 递增）：反过来会得到一个“折起来的三角”，
///    读起来不是蹬腿而是“抬着膝盖”；
/// ② 足必须**明显超出拳的最远点**（踢腿的攻击距离必须大于拳，
///    这是玩法上“用长距离换起手帧”的根据）；
/// ③ 大腿短于小腿（膝不能太远，否则成了直腿撑地）。
[[nodiscard]] auto kick_strike() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{0.0f, 4.0f});
  set_arm(pose, true, math::Point{4.0f, 124.0f}, math::Point{-6.0f, 112.0f});
  set_arm(pose, false, math::Point{-22.0f, 120.0f}, math::Point{-28.0f, 106.0f});
  pose.knee_front = math::Point{26.0f, 58.0f};
  pose.foot_front = math::Point{70.0f, 62.0f};
  pose.knee_back = math::Point{-8.0f, body::kKneeY + 2.0f};
  pose.foot_back = math::Point{-14.0f, body::kFootY};
  pose.lean = -0.18f;
  return pose;
}

/// 必杀起手：深蹲蓄力、双臂后收。
[[nodiscard]] auto special_wind() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-12.0f, -10.0f});
  set_arm(pose, true, math::Point{-2.0f, 112.0f}, math::Point{-16.0f, 96.0f});
  set_arm(pose, false, math::Point{-28.0f, 108.0f}, math::Point{-38.0f, 92.0f});
  pose.knee_front = math::Point{13.0f, 34.0f};
  pose.knee_back = math::Point{-23.0f, 32.0f};
  pose.foot_front = math::Point{22.0f, body::kFootY};
  pose.foot_back = math::Point{-33.0f, body::kFootY};
  pose.head_radius = body::kHeadRadius - 1.0f;
  pose.lean = -0.22f;
  return pose;
}

/// 必杀命中：双掌前推、身体腾起（足离地）。
/// 必杀必须比其它招式都远（它是“耗槽换距离 + 破防”的那一招）。
[[nodiscard]] auto special_strike() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{12.0f, 8.0f});
  set_arm(pose, true, math::Point{38.0f, 122.0f}, math::Point{76.0f, 122.0f});
  set_arm(pose, false, math::Point{26.0f, 116.0f}, math::Point{52.0f, 108.0f});
  pose.knee_front = math::Point{24.0f, 50.0f};
  pose.foot_front = math::Point{44.0f, 22.0f};
  pose.knee_back = math::Point{-4.0f, 48.0f};
  pose.foot_back = math::Point{-24.0f, 14.0f};
  pose.lean = 0.16f;
  return pose;
}

/// 受击：上身后仰、双手向外散。
[[nodiscard]] auto hurt_pose() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-7.0f, -4.0f});
  set_arm(pose, true, math::Point{16.0f, 128.0f}, math::Point{30.0f, 142.0f});
  set_arm(pose, false, math::Point{-24.0f, 126.0f}, math::Point{-38.0f, 136.0f});
  pose.head.x -= 8.0f;
  pose.knee_front = math::Point{6.0f, 44.0f};
  pose.knee_back = math::Point{-20.0f, 42.0f};
  pose.foot_front = math::Point{12.0f, body::kFootY};
  pose.foot_back = math::Point{-27.0f, body::kFootY};
  pose.lean = -0.28f;
  return pose;
}

/// 倒地：整体压到地面（骨架点仍在局部空间，靠 y 很小表达"躺着"）。
[[nodiscard]] auto knockdown_pose() -> Pose {
  Pose pose{};
  pose.hip = math::Point{-4.0f, 24.0f};
  pose.chest = math::Point{-30.0f, 28.0f};
  pose.head = math::Point{-52.0f, 32.0f};
  pose.shoulder_front = math::Point{-28.0f, 34.0f};
  pose.shoulder_back = math::Point{-34.0f, 22.0f};
  pose.elbow_front = math::Point{-26.0f, 46.0f};
  pose.hand_front = math::Point{-38.0f, 54.0f};
  pose.elbow_back = math::Point{-18.0f, 40.0f};
  pose.hand_back = math::Point{-12.0f, 52.0f};
  pose.knee_front = math::Point{20.0f, 22.0f};
  pose.foot_front = math::Point{48.0f, 14.0f};
  pose.knee_back = math::Point{12.0f, 16.0f};
  pose.foot_back = math::Point{40.0f, 6.0f};
  pose.head_radius = body::kHeadRadius;
  pose.lean = -1.2f;
  return pose;
}

/// 胜利：举拳。
[[nodiscard]] auto victory_pose() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{0.0f, 2.0f});
  set_arm(pose, true, math::Point{22.0f, 148.0f}, math::Point{16.0f, 180.0f});
  set_arm(pose, false, math::Point{-16.0f, 122.0f}, math::Point{-20.0f, 100.0f});
  pose.lean = 0.04f;
  return pose;
}

/// 败北：低头垂手。
[[nodiscard]] auto defeat_pose() -> Pose {
  Pose pose = neutral_pose();
  shift_torso(pose, math::Point{-2.0f, -8.0f});
  set_arm(pose, true, math::Point{13.0f, 108.0f}, math::Point{15.0f, 84.0f});
  set_arm(pose, false, math::Point{-13.0f, 106.0f}, math::Point{-14.0f, 82.0f});
  pose.knee_front = math::Point{9.0f, 40.0f};
  pose.knee_back = math::Point{-11.0f, 40.0f};
  pose.lean = 0.28f;
  return pose;
}

/// 走路：在站立骨架上加一条正弦步态（抬腿 + 手臂反向摆动）。
[[nodiscard]] auto walk_pose(float phase) -> Pose {
  Pose pose = neutral_pose();
  const float turn = 6.2831853f * phase;
  const float swing = std::sin(turn);
  const float lift = std::cos(turn);
  pose.foot_front = math::Point{17.0f + 12.0f * swing, body::kFootY + std::max(0.0f, 8.0f * lift)};
  pose.knee_front = math::Point{12.0f + 6.0f * swing, body::kKneeY + 2.0f + 4.0f * lift};
  pose.foot_back = math::Point{-17.0f - 11.0f * swing, body::kFootY + std::max(0.0f, -8.0f * lift)};
  pose.knee_back = math::Point{-12.0f - 6.0f * swing, body::kKneeY + 2.0f - 4.0f * lift};
  pose.head.y += 1.5f * std::sin(turn * 2.0f);
  return pose;
}

/// `t` 的缓入缓出（起手慢 → 命中快 → 收招慢的手感靠它）。
[[nodiscard]] auto ease(float t) noexcept -> float {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  return clamped * clamped * (3.0f - 2.0f * clamped);
}

}  // namespace

auto move_name(MoveId move) -> std::string_view {
  switch (move) {
    case MoveId::Idle: return "idle";
    case MoveId::Walk: return "walk";
    case MoveId::Guard: return "guard";
    case MoveId::Light: return "light";
    case MoveId::Heavy: return "heavy";
    case MoveId::Kick: return "kick";
    case MoveId::Special: return "special";
    case MoveId::Hurt: return "hurt";
    case MoveId::KnockDown: return "knockdown";
    case MoveId::Victory: return "victory";
    case MoveId::Defeat: return "defeat";
    case MoveId::Count: break;
  }
  return "idle";
}

auto move_label(MoveId move) -> std::string_view {
  switch (move) {
    case MoveId::Idle: return "待机";
    case MoveId::Walk: return "移动";
    case MoveId::Guard: return "防御";
    case MoveId::Light: return "轻拳";
    case MoveId::Heavy: return "重拳";
    case MoveId::Kick: return "蹬腿";
    case MoveId::Special: return "必杀";
    case MoveId::Hurt: return "受击";
    case MoveId::KnockDown: return "倒地";
    case MoveId::Victory: return "胜利";
    case MoveId::Defeat: return "败北";
    case MoveId::Count: break;
  }
  return "待机";
}

auto move_from_name(std::string_view name) -> std::optional<MoveId> {
  for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(MoveId::Count); ++index) {
    const auto move = static_cast<MoveId>(index);
    if (move_name(move) == name) return move;
  }
  return std::nullopt;
}

auto Pose::to_world(float feet_x, float ground_y, bool facing_right) const -> Pose {
  const float sign = facing_right ? 1.0f : -1.0f;
  const float scale = kBodyScale;
  const auto map = [&](math::Point local) {
    return math::Point{feet_x + local.x * sign * scale, ground_y - local.y * scale};
  };
  Pose out{};
  out.hip = map(hip);
  out.chest = map(chest);
  out.head = map(head);
  out.shoulder_front = map(shoulder_front);
  out.shoulder_back = map(shoulder_back);
  out.elbow_front = map(elbow_front);
  out.hand_front = map(hand_front);
  out.elbow_back = map(elbow_back);
  out.hand_back = map(hand_back);
  out.knee_front = map(knee_front);
  out.foot_front = map(foot_front);
  out.knee_back = map(knee_back);
  out.foot_back = map(foot_back);
  out.head_radius = head_radius * scale;
  out.lean = lean * sign;
  return out;
}

auto lerp_pose(const Pose& from, const Pose& to, float t) -> Pose {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const auto mix = [clamped](math::Point a, math::Point b) {
    return math::Point{math::lerp(a.x, b.x, clamped), math::lerp(a.y, b.y, clamped)};
  };
  Pose out{};
  out.hip = mix(from.hip, to.hip);
  out.chest = mix(from.chest, to.chest);
  out.head = mix(from.head, to.head);
  out.shoulder_front = mix(from.shoulder_front, to.shoulder_front);
  out.shoulder_back = mix(from.shoulder_back, to.shoulder_back);
  out.elbow_front = mix(from.elbow_front, to.elbow_front);
  out.hand_front = mix(from.hand_front, to.hand_front);
  out.elbow_back = mix(from.elbow_back, to.elbow_back);
  out.hand_back = mix(from.hand_back, to.hand_back);
  out.knee_front = mix(from.knee_front, to.knee_front);
  out.foot_front = mix(from.foot_front, to.foot_front);
  out.knee_back = mix(from.knee_back, to.knee_back);
  out.foot_back = mix(from.foot_back, to.foot_back);
  out.head_radius = math::lerp(from.head_radius, to.head_radius, clamped);
  out.lean = math::lerp(from.lean, to.lean, clamped);
  return out;
}

auto timing_for(MoveId move) -> MoveTiming {
  switch (move) {
    case MoveId::Light: return MoveTiming{4, 4, 8};
    case MoveId::Heavy: return MoveTiming{9, 6, 15};
    case MoveId::Kick: return MoveTiming{8, 6, 14};
    case MoveId::Special: return MoveTiming{14, 8, 22};
    case MoveId::Hurt: return MoveTiming{0, 0, 14};
    case MoveId::KnockDown: return MoveTiming{0, 0, 26};
    case MoveId::Victory: return MoveTiming{0, 0, 40};
    case MoveId::Defeat: return MoveTiming{0, 0, 40};
    default: return MoveTiming{};
  }
}

auto keyframes_for(MoveId move) -> std::optional<Keyframes> {
  switch (move) {
    case MoveId::Light: return Keyframes{light_wind(), light_strike()};
    case MoveId::Heavy: return Keyframes{heavy_wind(), heavy_strike()};
    case MoveId::Kick: return Keyframes{kick_wind(), kick_strike()};
    case MoveId::Special: return Keyframes{special_wind(), special_strike()};
    default: return std::nullopt;
  }
}

auto pose_for(MoveId move, int frame, float walk_phase) -> Pose {
  const Pose idle = neutral_pose();
  const int at = frame < 0 ? 0 : frame;
  switch (move) {
    case MoveId::Idle: {
      // 呼吸：躯干 1.5px 上下浮动（周期 90 帧）。
      Pose pose = idle;
      const float bob = std::sin(static_cast<float>(at) * 0.0698f);
      pose.chest.y += bob * 1.5f;
      pose.head.y += bob * 1.5f;
      pose.shoulder_front.y += bob * 1.5f;
      pose.shoulder_back.y += bob * 1.5f;
      pose.hand_front.y += bob * 1.2f;
      pose.hand_back.y += bob * 1.2f;
      return pose;
    }
    case MoveId::Walk: return walk_pose(walk_phase);
    case MoveId::Guard: return guard_pose();
    case MoveId::Hurt: {
      // 受击：前两帧猛然后仰，然后回弹（一次性冲击，不循环）。
      const float t = std::clamp(static_cast<float>(at) / 6.0f, 0.0f, 1.0f);
      return lerp_pose(idle, hurt_pose(), std::sin(t * 3.14159265f));
    }
    case MoveId::KnockDown: {
      const float t = std::clamp(static_cast<float>(at) / 8.0f, 0.0f, 1.0f);
      return lerp_pose(hurt_pose(), knockdown_pose(), ease(t));
    }
    case MoveId::Victory: {
      const float t = std::clamp(static_cast<float>(at) / 10.0f, 0.0f, 1.0f);
      return lerp_pose(idle, victory_pose(), ease(t));
    }
    case MoveId::Defeat:
      return lerp_pose(idle, defeat_pose(),
                       std::clamp(static_cast<float>(at) / 12.0f, 0.0f, 1.0f));
    default: break;
  }
  const auto keys = keyframes_for(move);
  if (!keys.has_value()) return idle;
  const MoveTiming timing = timing_for(move);
  // 起手段：静止 → 蓄力（ease：抬手前慢后快）；
  // 判定段：蓄力 → 出招（线性，命中瞬间要有速度感）；
  // 收招段：出招 → 静止（ease 收回来）。
  if (at < timing.startup) {
    const float t = timing.startup > 0
                        ? ease(static_cast<float>(at) / static_cast<float>(timing.startup))
                        : 1.0f;
    return lerp_pose(idle, keys->wind, t);
  }
  if (at < timing.startup + timing.active) {
    const float t = static_cast<float>(at - timing.startup) /
                    static_cast<float>(std::max(1, timing.active));
    return lerp_pose(keys->wind, keys->strike, std::min(1.0f, t + 0.34f));
  }
  const float t = ease(static_cast<float>(at - timing.startup - timing.active) /
                       static_cast<float>(std::max(1, timing.recovery)));
  return lerp_pose(keys->strike, idle, t);
}

}  // namespace gedou
