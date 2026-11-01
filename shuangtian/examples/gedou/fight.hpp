#pragma once

/// 格斗示例 · 战斗模拟（**纯逻辑，不含任何绘制**）。
///
/// 分层的理由：格斗游戏最容易坏的地方是"判定/帧数/朝向"这些**看不见的量**，
/// 而它们既不能在截图里断言（画面看着都在动），也不该由绘制代码承担。
/// 所以本层是确定的逐帧状态机：给定同一串输入，跑出同一串事件——
/// `tools/gedou_e2e.py` 可以直接断言"3 帧后命中""血量掉了 42"。
///
/// 时间口径：**固定 60Hz 逻辑帧**，与渲染帧解耦（`advance()` 由主循环按
/// 累积时间调用固定次数）。这样低帧率机器上判定不会变慢，也让测试完全可复现。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fighter.hpp"
#include "pose.hpp"
#include "st/math/geometry.hpp"

namespace gedou {

/// 战斗场地（逻辑坐标；`ground` 是地面线 `y`）。
struct Arena {
  float left{90.0f};
  float right{1110.0f};
  float ground{560.0f};
  /// 两名角色的初始间距（各自离场地中心线多远）。
  float start_gap{210.0f};
  float width() const noexcept { return right - left; }
  float center() const noexcept { return (left + right) * 0.5f; }
};

/// 一帧的输入意图（键盘与 AI 都折算成它——两条控制源走同一个入口，
/// 于是"AI 能做的事玩家都能做"是结构保证，而不是靠两边各写一遍）。
struct Intent {
  float move{-0.0f};   ///< -1 后退 / +1 前进（相对自己朝向）
  bool guard{false};
  std::optional<MoveId> attack{};
  bool operator==(const Intent& other) const {
    return move == other.move && guard == other.guard && attack == other.attack;
  }
};

/// 单侧角色的运行时状态。
struct FighterState {
  std::size_t profile{0};
  float x{0.0f};              ///< 双脚之间的地面点（世界 x）
  bool facing_right{true};
  float hp{1000.0f};
  float max_hp{1000.0f};
  float meter{0.0f};          ///< 必杀槽 0~kMeterMax
  MoveId move{MoveId::Idle};
  int move_frame{0};          ///< 当前动作已过帧数
  MoveTiming timing{};        ///< 当前动作的阶段帧数（攻击招生效）
  /// 受击硬直剩余帧（>0 时不能行动）。
  int hitstun{0};
  /// 本次动作是否**已经**命中过（禁止一招收两次血）。
  bool connected{false};
  /// 击退惯性（每帧衰减）。
  float push_velocity{0.0f};
  /// 走路相位（0~1 循环；`Walk` 姿态用）。
  float walk_phase{0.0f};
  /// 倒地/胜利/败北这类"退场态"的剩余计时（>0 时压住其它一切）。
  int pose_lock{0};
  /// 连击计数（同一次压制里连续命中）。
  int combo{0};
  int comboTimer{0};
  /// 防御削韧累积（0~`kGuardBreakMax`）：满了就破防。
  /// 只在防御挨打时增长，否则缓慢回落。
  float guard_break{0.0f};

  [[nodiscard]] auto alive() const noexcept -> bool { return hp > 0.0f; }
  [[nodiscard]] auto busy() const noexcept -> bool { return hitstun > 0 || pose_lock > 0; }
  [[nodiscard]] auto can_act() const noexcept -> bool { return !busy(); }
};

/// 命中事件（一帧内一次命中一条）。
struct HitEvent {
  int victim{0};            ///< 0 = 左侧角色，1 = 右侧角色
  MoveId move{MoveId::Light};
  float damage{0.0f};       ///< 实际扣血（已含防御/削减）
  bool blocked{false};
  bool knockdown{false};
  int combo{1};
  /// 命中点（世界坐标；绘制火花/震屏用）。
  math::Point at{};
};

/// 防御削韧上限（帧单位）：满了即破防。
/// 取值参考：踢腿 40 × 3 次 > 100 ⇒ 连续三次踢腿打防一定破防。
inline constexpr float kGuardBreakMax = 100.0f;

/// 一帧的推进结果（绘制与外部断言都读它）。
struct FrameEvents {
  std::vector<HitEvent> hits{};
  /// 本帧有角色**刚刚**倒下（血条清零的第一次帧）。
  std::vector<int> knocked_out{};
  /// 震屏强度（0 = 不震）。
  float shake{0.0f};
  /// 本帧是否发生了"必杀释放"（用于光效与音效）。
  bool special_fired{false};
  /// 本帧发生破防的受害者侧（`guard_break` 满了）。绘制层据此播破防特效。
  std::vector<int> broken_guard{};
  bool empty() const noexcept {
    return hits.empty() && knocked_out.empty() && broken_guard.empty() && shake <= 0.0f &&
           !special_fired;
  }
};

/// 回合结论。
enum class RoundPhase : std::uint8_t {
  Fight,   ///< 对战中
  Over,    ///< 有人倒下，播结算姿态
};

/// 一场对局。
///
/// 职责边界：**只推演**。它不认识画布、不认识键盘、不认识主题——
/// 因此同一份逻辑被单元测试、e2e、以及屏幕上的对局共用。
class Match {
 public:
  Match(std::size_t left_profile, std::size_t right_profile, Arena arena);

  /// 重置到开局（再来一局）。
  void reset();

  /// 推进**一个逻辑帧**；`left`/`right` 是这一帧双方的输入意图。
  auto advance(const Intent& left, const Intent& right) -> FrameEvents;

  [[nodiscard]] auto state(int side) const -> const FighterState&;
  [[nodiscard]] auto arena() const noexcept -> const Arena& { return arena_; }
  [[nodiscard]] auto phase() const noexcept -> RoundPhase { return phase_; }
  [[nodiscard]] auto frame_count() const noexcept -> std::int64_t { return frame_index_; }
  [[nodiscard]] auto winner() const -> std::optional<int>;
  /// 结算态的剩余帧（`phase()==Over` 时倒数）。
  [[nodiscard]] auto end_timer() const noexcept -> int { return end_timer_; }

  /// 换场地（窗口缩放/重排）：只改几何，**不动**血量/槽/招式帧。
  /// 调用方随后用 `place()` 把角色位置搬到新坐标（按比例）。
  void set_arena(const Arena& arena);
  /// 直接摆位（夹到场地内）。供重排与剧本化驱动用。
  void place(int side, float x);
  /// 充满某一侧的必杀槽（演示/截图用；正常对局靠命中与受击攒）。
  void charge_meter(int side);

  /// 当前帧的姿态（世界坐标；绘制与"命中盒可视化"都读它）。
  [[nodiscard]] auto world_pose(int side) const -> Pose;
  /// 当前招式判定的世界矩形（不可判定的帧返回空矩形）。
  [[nodiscard]] auto active_hitbox(int side) const -> math::Rect;
  /// 角色可被击中的身体矩形（躯干 + 头；下蹲/倒地时变矮）。
  [[nodiscard]] auto hurtbox(int side) const -> math::Rect;

  /// 手动设一条指令（供控制通道/e2e"剧本化"用：直接让某侧做某个动作）。
  /// 返回 false = 该动作当下不可发起（正在硬直/槽不足），**不静默吞掉**。
  auto force_move(int side, MoveId move) -> bool;
  /// 直接结算伤害（e2e 想造"残血"这类场景时不必打满整场）。
  auto apply_damage(int side, float damage) -> bool;

  // —— 属性面（控制通道 `get` 用；键名稳定） ——
  [[nodiscard]] auto snapshot() const -> std::string;

 private:
  void begin_move(FighterState& fighter, MoveId move);
  void update_fighter(FighterState& fighter, int side, const Intent& intent, FrameEvents& events);
  void resolve_attack(FighterState& attacker, FighterState& defender, int attacker_side,
                      FrameEvents& events);
  void apply_hit(FighterState& attacker, FighterState& defender, int attacker_side, MoveId move,
                 FrameEvents& events);
  void clamp_to_arena(FighterState& fighter) const;

  Arena arena_{};
  std::array<FighterState, 2> fighters_{};  RoundPhase phase_{RoundPhase::Fight};
  std::int64_t frame_index_{0};
  int end_timer_{0};
};

}  // namespace gedou
