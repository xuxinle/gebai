#pragma once

/// 格斗示例 · 强化学习契约（观测 / 动作 / 奖励）——**唯一真源**。
///
/// ## 为什么契约必须住在 C++
///
/// 训练脚本（Python）与游戏内推理（C++）必须看到**同一份**观测与动作语义。
/// 两边各写一份时，最常见的失效是"偏移一个下标"或"少归一化一次"——
/// 训练照常收敛、损失照常下降，而进游戏后机器人行为莫名其妙。
/// 这种缺陷无法从曲线看出来，只能靠"只有一份定义"来排除。
///
/// 因此：C++ 定义 + `--schema` 导出 JSON（形状/顺序/归一化口径），
/// Python 侧**读 schema 校验维度**，不手抄任何下标。
///
/// ## 设计取舍
///
/// - **自我中心（ego-centric）**：观测里只用"相对脚下距离 + 朝向"，不含世界坐标。
///   一张网络因此能同时打左右两侧（格斗游戏里左右交换必须同强，否则
///   "AI 只会打一边"是最刺眼的失衡）。
/// - **离散 8 动作**：格斗的操作面本来就小（移动/防御/四招），留成连续量
///   只会让 PPO 在低维上更难学。
/// - **奖励以伤害为主轴**：格斗的胜负由血量决定，所以主项必须是"造成的伤害 −
///   承受的伤害"；命中奖励只是加速学习，不能替代主轴（否则学会"空挥骗奖励"）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ai.hpp"
#include "fight.hpp"

namespace gedou {

/// 动作空间（离散）。数值即下标，**不可重排**（与训练权重绑定）。
enum class RlAction : std::uint8_t {
  Idle = 0,
  Forward,
  Backward,
  Guard,
  Light,
  Heavy,
  Kick,
  Special,
  Count,
};

inline constexpr std::size_t kRlActionCount = static_cast<std::size_t>(RlAction::Count);

[[nodiscard]] auto rl_action_name(RlAction action) -> std::string_view;
[[nodiscard]] auto rl_action_from_index(int index) -> RlAction;

/// 观测的一个分量（名字 + 归一化口径）。schema 导出与文档都从这里取。
struct ObservationField {
  std::string_view name;
  std::string_view meaning;
  /// 取值范围（归一化后）：schema 里如实上报，训练侧据此做 clip/标准化。
  float low{0.0f};
  float high{1.0f};
};

/// 观测维度与各分量语义（顺序即向量下标）。
[[nodiscard]] auto observation_fields() -> const std::vector<ObservationField>&;
[[nodiscard]] auto observation_dim() -> std::size_t;

/// 单帧观测（定长数组；`observation_dim()` 个 float）。
using Observation = std::vector<float>;

/// 把对局渲染成 `side` 视角的观测。
[[nodiscard]] auto observe(const Match& match, int side) -> Observation;

/// 动作 → 输入意图。返回的 `Intent` 直接喂给 `Match::advance`。
[[nodiscard]] auto action_intent(RlAction action) -> Intent;

/// 奖励配置（训练侧不允许魔改语义，只允许调权重）。
struct RewardConfig {
  /// 造成伤害的系数（damage 按 `max_hp` 归一化后再乘）。
  float damage_dealt{1.0f};
  /// 承受伤害的系数（>1 = 更怕挨打，鼓励保守；<1 = 更敢换血）。
  float damage_taken{0.85f};
  /// 命中一次的固定奖励（blocked 时为 `blocked_bonus`）。
  float hit_bonus{0.35f};
  float blocked_bonus{0.05f};
  /// 赢/输的终局奖励。
  float win_bonus{6.0f};
  float lose_penalty{-6.0f};
  /// 平局（时间耗尽且血量相同）的奖励。
  float draw_bonus{0.0f};
  /// 每逻辑帧的微小惩罚（鼓励结束战斗而不是绕圈）。
  float step_penalty{-0.002f};
  /// 距离过近时的微小惩罚（学会保持出招距离，而不是贴脸）。
  float crowding_penalty{-0.004f};
  float crowding_distance{70.0f};
  /// **主动惩罚**：本帧按了防御或后退且没挨打。
  ///
  /// 为何必需（实测代价）：不加它时，"一直按防御"就能让血量比不被拉开，
  /// 而时间到时按血量比判胜——于是**随机策略的胜率达 99.5%**，学习曲线完全失去意义。
  /// 格斗游戏的"龟缩必胜"是规则漏洞，不是策略；奖励必须把它压下去。
  float passive_penalty{-0.02f};
  /// **空挥惩罚**：在**肯定够不到**的距离上出招（出招即浪费起手时间）。
  ///
  /// 为何必需（实测代价）：进游戏验证时策略每帧都出腿，而双方距离 200px
  /// 超过腿长 160px ⇒ 永远打空、也从不靠近，现场看起来像“机器人卡住了”。
  /// 训练里这个行为不被惩罚（出招本身不扣分），于是它只学会了“出招”。
  float whiff_penalty{-0.03f};
  /// **消极惩罚**：距离超出自己所有招式的触及范围，而本帧没有向前压。
  ///
  /// 为何必需（实测代价，本项目最隐蔽的一个）：训练对手是手写 AI，它会**主动靠近**；
  /// 于是策略学成了“等对手走过来再打”的反应型打法。评测（对手也是脚本 AI）
  /// 拿到 80% 胜率，但**进游戏后玩家不动**，它就永远站在 400px 外选“待机”——
  /// 看起来像“机器人坏了”。奖励必须直接奖励“主动拉近距离”。
  float passive_range_penalty{-0.02f};
  /// **贴身硬推惩罚**：已经在所有招式的触及范围内，却还在往前压。
  ///
  /// 为何必需（实测代价）：策略把“前进”当成安全动作（无成本、永不被拒），
  /// 于是它 70% 的帧在前进、只在冷却间隙偶尔出招，胜率虽高但**看起来像在顶人**。
  /// 贴身再推既没有进攻收益，又让自己长期处于起手前的无效帧。
  float push_penalty{-0.02f};
};

/// 一帧的推进结果（环境侧用；奖励在这里算）。
struct StepResult {
  float reward{0.0f};
  bool done{false};
  /// 本帧是否赢了（`done` 且赢）。
  bool won{false};
  bool lost{false};
  bool draw{false};
  float damage_dealt{0.0f};
  float damage_taken{0.0f};
  int hits{0};
  int blocked{0};
};

/// RL 环境：一层薄壳，把 `Match` 包成"单智能体对脚本 AI"的回合制环境。
///
/// **它不是第二个战斗实现**：所有判定/帧数/伤害都来自 `Match`（与游戏里完全相同）。
/// 这层只做三件事——决定对手怎么动、把状态转成观测、把帧事件折成奖励。
///
/// ## 动作节流（**没有它训练必然退化**）
///
/// 策略在**每个可行动帧**都会被问一次，而攻击动作在游戏里是"按一次出一招"。
/// 若允许每帧都触发新攻击，"一直按蹬腿"就等于"无限连蹬腿"——实测该策略
/// 拿到 100% 胜率、连续重复率 **98.1%**（只按 kick、从不防御、从不移动）。
/// 那不是学会了格斗，而是利用了接口。所以环境里加两道约束：
///
/// - **动作保持**：策略每 `frame_skip` 帧才被真正问一次，其余帧复用上次动作；
/// - **攻击冷却**：同一招触发后一段时间内不重复触发（模拟起手/收招占用）。
///
/// ## 场地尺寸与观测的耦合（已知限制）
///
/// 观测里的 `gap` 是**按固定 300px 归一**的，所以策略学到的距离感与
/// 场地宽度无关（它看的是像素），但会与**窗口尺寸**耦合：训练用 1040 宽，
/// 实际窗口不同时双方初始距离不同，策略需要“走近”的帧数也不同。
/// 这是真实存在的限制（进游戏验证时就撞到了：宽窗口下机器人停在远远超出
/// 腿长的位置）。缓解方式见 `GameView::load_policy` 的注释（游戏内固定场地宽）。
class RlEnv {
 public:
  struct Config {
    std::size_t self_profile{0};
    std::size_t opp_profile{1};
    /// 对手难度（课程学习：从易到难）。
    Difficulty opponent{Difficulty::Normal};
    /// 对手有概率**完全不动**（打桩）。
    ///
    /// 为何必需（实测代价）：训练对手若总是主动靠近的脚本 AI，策略会学成
    /// “等对手过来再打”的反应型打法——评测（对手也是脚本 AI）有 80% 胜率，
    /// 但进游戏后玩家不动，它就站在远处选“待机”。训练分布里必须有“对手不动”。
    float idle_opponent_ratio{0.3f};
    /// 单回合最大帧数（60Hz）；到点按血量比判定。
    int max_frames{60 * 40};
    std::uint32_t seed{0x51ED5EEDU};
    RewardConfig reward{};
    /// 策略每多少逻辑帧被问一次（其余帧复用上次动作）。
    /// 4 帧 = 15 次/秒的决策频率：人能操作的量级，也接近格斗游戏的输入频率。
    int frame_skip{4};
    /// 一次攻击动作触发后的**最小间隔**（逻辑帧）。
    ///
    /// 按招式的**完整时长**算，而不是一个统一小数：否则短招（轻拳）与长招
    /// （必杀）被同一把尺子卡，结果是“只按最短/最远的那个”变成最优解。
    /// 实测代价：统一 12 帧时，“一直按蹬腿”仍能 100% 胜（腿长 112px、硬直 20 帧，
    /// 而冷却只 12 帧 ⇒ 硬直还没完下一腿已经接上了）。
    int attack_cooldown{0};
    /// 冷却的计算口径：`true` = 按招式总帧数 × `cooldown_ratio`。
    bool cooldown_from_move{true};
    float cooldown_ratio{1.0f};
    /// 开局间距随机范围（像素）。
    ///
    /// 为何要随机化（实测代价）：固定 200px 时策略只在这个距离附近学会了
    /// “能打到”，进游戏遇到更宽窗口（实际基线 428px）就**永远不出招**，
    /// 像卡住了。训练分布覆盖不到的量，策略就学不会。
    float start_gap_min{90.0f};
    float start_gap_max{430.0f};
  };

  explicit RlEnv(Config config);

  /// 重置一回合，返回己方初始观测。
  auto reset() -> Observation;
  /// 推进一步：`action` 由策略给出，对手由脚本 AI 给出。
  auto step(RlAction action) -> StepResult;
  [[nodiscard]] auto observation() const -> Observation;
  [[nodiscard]] auto episode_frames() const noexcept -> int { return frames_; }
  [[nodiscard]] auto match() const noexcept -> const Match& { return match_; }
  [[nodiscard]] auto config() const noexcept -> const Config& { return config_; }
  /// 本回合累计奖励（评测时读它）。
  [[nodiscard]] auto episode_return() const noexcept -> float { return episode_return_; }

  /// 本侧（智能体）是哪一边：固定 0（左侧），对手固定 1。
  static constexpr int kSelfSide{0};

  /// 本帧是否被"真正问到决策"（`step` 内部用 `frame_skip` 节流）。
  [[nodiscard]] auto awaiting_action() const noexcept -> bool {
    return frames_ % std::max(1, config_.frame_skip) == 0;
  }

 private:
  Config config_{};
  /// `Match` 没有默认构造（它要两份角色档）：用成员初始化器配上默认档位。
  Match match_{0, 1, Arena{}};
  AiController opponent_ai_{};
  int frames_{0};
  float episode_return_{0.0f};
  std::uint64_t rng_{1};
  /// 上一次推进后己方的血量/槽（算差分用）。
  float last_self_hp_{0.0f};
  float last_opp_hp_{0.0f};
  /// 动作保持：上次动作与攻击冷却计时。
  RlAction held_action_{RlAction::Idle};
  int attack_cooldown_left_{0};
  /// 局计数（用来给每局的对手换种子，见 `reset` 里的说明）。
  std::uint32_t episode_index_{0};
  /// 本局对手是否“打桩不动”（见 `Config::idle_opponent_ratio`）。
  bool opponent_idle_{false};

  [[nodiscard]] auto next_random() noexcept -> float;
};

/// 契约的 JSON 描述（`--schema` 输出；Python 侧据此校验维度与顺序）。
[[nodiscard]] auto rl_schema_json() -> std::string;

}  // namespace gedou
