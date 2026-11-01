#pragma once

/// 格斗示例 · 对战视图（自绘组件）。
///
/// 这是**唯一**把 `Match` 画到画布上的地方：骨架姿态、地面透视、打击特效、
/// 血条/必杀槽 HUD 全在这里。它与 `Match` 的分工很硬——
/// 模拟层不知道画布存在（所以能被单测/e2e 直接断言），
/// 视图层不改战斗状态（所以"画面看着打到但血量没动"这类问题不会互相掩盖）。
///
/// 帧推进：视图自己按 `RenderContext::time_seconds` 累积 60Hz 逻辑帧
/// （固定步长，见 `fight.hpp` 的时间口径），并 `request_animation()` 续帧；
/// 结算完毕且特效播完后**停止续帧**（静止画面不烧 CPU）。

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ai.hpp"
#include "fight.hpp"
#include "battle_select.hpp"
#include "fighter_art.hpp"
#include "fighter_rig.hpp"
#include "policy.hpp"
#include "rl_env.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/scene3d.hpp"
#include "st/ui/element.hpp"

namespace gedou {

/// 特效种类（都按帧倒计时，播完即删）。
enum class EffectKind : std::uint8_t { Spark, Block, Shake, Text, Dust };

struct Effect {
  EffectKind kind{EffectKind::Spark};
  math::Point at{};
  /// 已播帧数 / 总寿命（帧）。
  int age{0};
  int life{10};
  float scale{1.0f};
  bool facing_right{true};
  std::string label{};
};

/// 特效构造器：`Effect{kind, at, 0, 24, 1.8f, true}` 这种聚合初始化
/// 在混着 `std::string` 时很容易数错位置（实测漏一个字段就是一个
/// “float 缩窄成 int”的报错，而报错行是别的字段）。统一定义含义后在这里兜住。
[[nodiscard]] inline auto make_effect(EffectKind kind, math::Point at, int life, float scale = 1.0f,
                                      bool facing_right = true, std::string label = {})
    -> Effect {
  Effect effect{};
  effect.kind = kind;
  effect.at = at;
  effect.age = 0;
  effect.life = life;
  effect.scale = scale;
  effect.facing_right = facing_right;
  effect.label = std::move(label);
  return effect;
}

/// 视图自己的工作状态（与战斗状态分开：重开一局不必重建组件）。
struct ViewState {
  bool paused{false};
  bool show_hitbox{false};
  /// 玩家侧（0 或 1）。玩家在左侧时 AI 在右侧。
  int player_side{0};
  /// AI 是否接管对手侧。
  bool ai_enabled{true};
  Difficulty difficulty{Difficulty::Normal};
  /// 对手侧是否由**训练好的神经网络策略**接管（`--policy` 装载时置真）。
  bool policy_loaded{false};
  /// 累计的逻辑帧（HUD 与断言都读它）。
  std::int64_t logic_frames{0};
  /// 上一帧的命中事件数（HUD 演出用）。
  int last_hit_count{0};
  float shake{0.0f};
  float flash{0.0f};
  int slowmo{0};
  /// 回合结束时的横幅（"K.O." / "胜者"）。
  std::string banner{};
  int banner_age{0};
};

class GameView : public st::ui::Element {
 public:
  GameView();
  ~GameView() override;
  GameView(const GameView&) = delete;
  auto operator=(const GameView&) -> GameView& = delete;

  /// 换对局（选人条/重开都走它）。
  void start_match(std::size_t left_profile, std::size_t right_profile);

  // —— 选人 / 对战模式 ——
  /// 打开选人界面（开局默认打开；`--left/--right` 等命令行给了具体角色时也仍打开，
  /// 只是默认选中项不同）。
  void open_select();
  /// 关掉选人界面直接用给定配置开打（供 e2e / 命令行跳过选人）。
  void skip_select();
  [[nodiscard]] auto select_state() const -> const SelectState& { return select_; }
  /// 切换对战模式（会立刻应用到对手决策：机机模式下两侧都是 AI）。
  void set_battle_mode(BattleMode mode);
  [[nodiscard]] auto battle_mode() const noexcept -> BattleMode { return select_.mode; }
  /// 选人界面是否仍在显示。
  [[nodiscard]] auto selecting() const noexcept -> bool { return select_.active; }
  void restart();
  [[nodiscard]] auto match() noexcept -> Match& { return *match_; }
  [[nodiscard]] auto match() const noexcept -> const Match& { return *match_; }
  [[nodiscard]] auto view_state() const noexcept -> const ViewState& { return view_; }
  [[nodiscard]] auto view_state() noexcept -> ViewState& { return view_; }

  /// 玩家这一帧的输入意图（键盘/控制通道都经由它）。
  void set_player_intent(Intent intent);
  /// 让 AI 接管/交还对手侧（也用于"双人同机"）。
  void set_ai_enabled(bool enabled);
  /// 按“对手侧是否由 AI 接管”**重建** AI：
  ///
  /// 为何不在 `think()` 里自己初始化：AI 的内部状态（上一次决策、反应计时）
  /// 会随着“先跑一场、再重开”而跨局残留，于是同一份初始状态会跑出两种结果。
  /// 重置点只该有一个：这里。
  void reset_ai();

  /// 装载**训练好的神经网络策略**接管对手侧（空路径 = 交还内置手写 AI）。
  /// 失败返回原因（**不静默回退**：那会让“权重载错了”表现为“机器人很傻”）。
  [[nodiscard]] auto load_policy(std::string_view path) -> std::optional<std::string>;
  [[nodiscard]] auto has_policy() const -> bool { return policy_.valid(); }
  /// 读观测（供外部断言“策略看到的是同一份观测”）。
  [[nodiscard]] auto opponent_observation() const -> Observation {
    return observe(*match_, 1 - view_.player_side);
  }
  /// 诊断：策略是否正处在“它没见过的距离”上（见实现的注释）。
  [[nodiscard]] auto policy_missing_pressure() const -> bool;
  /// 诊断：策略对当前帧给出的动作下标（-1 = 未装载策略）。
  [[nodiscard]] auto policy_action_index() const -> int;
  /// 诊断：策略的输入/输出摘要（JSON），用于与评测环境逐值对比。
  [[nodiscard]] auto policy_probe_json() const -> std::string;
  void set_difficulty(Difficulty difficulty);
  void set_paused(bool paused);
  void set_show_hitbox(bool show);
  /// 训练模式：对手不动（打桩）。给"看招式/拍素材"用。
  void set_dummy(bool dummy);
  [[nodiscard]] auto dummy() const noexcept -> bool { return dummy_; }

  // —— Element ——
  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void arrange(const st::ui::RenderContext& context, math::Rect rect) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  auto on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool override;
  void apply_theme(const st::ui::Theme& theme) override;
  [[nodiscard]] auto semantics_text() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action, std::string_view argument)
      -> bool override;

  /// 手动推进 N 个逻辑帧（控制通道/e2e 用来"快进"，不依赖真实墙钟）。
  auto step_logic(int frames) -> std::string;
  /// 当前帧快照（JSON；控制通道 `get state` 直接给出去）。
  [[nodiscard]] auto snapshot() const -> std::string;

 private:
  /// 逻辑推进（**可在 `paint` 里调**：`paint` 是 const 路径，而帧时间轴必须每帧推进——
  /// 与框架里悬浮过渡/`Switch` 的逐帧推进同一套做法，状态成员声明为 `mutable`）。
  ///
  /// `force = true` 时忽略暂停：手动步进是**显式驱动**（e2e/剧本），
  /// 与“墙钟不停下来”是两件事——旧的实现把两者混为一谈，
  /// 结果是 `--paused` 下 `invoke step` 完全不动（看起来像步进坏了）。
  void advance_logic(bool force = false) const;
  void expire_effects() const;
  void push_effect(Effect effect) const;

  // —— 绘制分解（每块一个函数：观感出问题时能直接定位到哪一层画的） ——
  void paint_backdrop(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  void paint_arena(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  /// 3D 角色批量渲染（两个角色必须在同一次 begin/end_frame 里）。
  void paint_fighters_3d(const st::ui::RenderContext& context,
                         st::raster::Surface& canvas) const;
  /// 2D 矢量角色绘制（取代 3D；两条路径都在，方便对比）。
  void paint_fighters_2d(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  /// 角色的 2D 覆盖层（影子/防御罩/必杀环/连击数）。
  void paint_fighter_overlays(const st::ui::RenderContext& context,
                              st::raster::Surface& canvas, int side) const;
  /// 危险物：飞行中的投射物与地面落点预警（画在角色**之下**）。
  void paint_hazards(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  /// 包在角色**身上**的状态效果（冰壳/护盾环/苹果形态）。
  void paint_status_overlays(const st::ui::RenderContext& context,
                             st::raster::Surface& canvas) const;
  /// 一颗甜甜圈（飞行中）。
  void paint_donut(st::raster::Surface& canvas, const Projectile& shot) const;
  /// 一颗苹果（护盾绕身件与变大后的本体共用）。
  void paint_apple(st::raster::Surface& canvas, math::Point center, float radius) const;
  void paint_effects(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  void paint_hud(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  void paint_hitboxes(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;
  void paint_banner(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;

  std::unique_ptr<Match> match_{};

  // —— 3D 角色渲染 ——
  //
  // 两个角色各一份装配（glTF 本体 + 服装，建一次就缓存），
  // 每帧只改变换。`scene3d_` 是离屏渲染目标，尺寸随窗口变化。
  //
  // 为何放在视图层而不是逻辑层：三维只是**表现**，`Match` 不该知道它的存在
  // （否则单测/E2E/RL 全部要拖上一个 GPU 上下文）。
  std::shared_ptr<const FighterRig> fighter_rig_[2]{};
  /// 离屏三维场景（尺寸变化时重建）。
  mutable std::unique_ptr<st::raster::Scene3D> scene3d_{};
  mutable int scene3d_width_{0};
  mutable int scene3d_height_{0};
  /// 上一帧相机用的"米→像素"比例（角色位置换算必须与相机同尺度）。
  mutable float pixels_per_meter_3d_{160.0f};
  /// 三维不可用的告警只记一次（每帧刷屏会把日志淹掉）。
  mutable bool scene3d_warned_{false};
  /// 相机诊断日志只记一次。
  mutable bool scene3d_logged_{false};
  /// 确保 `scene3d_` 与当前画布尺寸一致；返回是否可用。
  auto ensure_scene3d(st::raster::Surface& canvas) const -> bool;
  /// 下面这些在 `paint_content()`（const）里被推进，因此是 `mutable`：
  /// 动画状态必须能被绘制路径推进（否则动画停在首帧），而 `paint` 的 const
  /// 契约不能破——框架里各组件动画都是这一套（见 `Element::RenderExtras`）。
  mutable AiController ai_{};
  /// 训练好的策略（未装载时 `valid() == false`，此时用 `ai_`）。
  PolicyNet policy_{};
  /// 策略侧的攻击冷却（与训练环境同公式；见 `opponent_decision` 的说明）。
  mutable int policy_cooldown_{0};
  /// 对手决策（策略与手写 AI 的唯一分岔点；见实现里的说明）。
  [[nodiscard]] auto opponent_decision() const -> Intent;
  /// 选人界面状态（绘制与命中都读它）。
  mutable SelectState select_{};
  /// 选人界面缓存的命中区（`draw` 时按当前视口填好，点击复用同一份）。
  mutable SelectHitAreas select_areas_{};
  /// 画选人界面（覆盖在对战画面之上）。
  void paint_select(const st::ui::RenderContext& context, st::raster::Surface& canvas) const;

  mutable ViewState view_{};
  mutable Intent player_intent_{};
  mutable Intent opponent_intent_{};
  bool dummy_{false};
  /// 键盘按住状态（KeyDown 置位 / KeyUp 清位）：
  /// **不能靠 KeyDown 重复事件**推“按住”——控制通道的 `press` 只发一对 down/up。
  bool key_left_{false};
  bool key_right_{false};
  bool key_guard_{false};
  /// 边沿触发的攻击（按下时置位，折算进意图后立刻清掉），以及按住类键位。
  /// 这三个在 `on_event`（非 const）里改，但 `advance_logic()` 会在画的时候
  /// 把攻击意图折算给模拟层——那边会读它，所以保持非 mutable 也不影响。
  mutable std::optional<MoveId> pending_attack_{};
  /// 最近一次动作失败的原因（`invoke attack` 返回 false 时给调用方看）。
  mutable std::string last_error_{};
  /// 特效列表（在 `paint_content` 里推进：`mutable` 同上）。
  mutable std::vector<Effect> effects_{};
  /// 定时推进用：上次绘制时刻与累积余量（秒）。
  mutable double last_time_{-1.0};
  mutable double accumulator_{0.0};
  /// 本帧的开火节拍（每 6 帧一次的呼吸光，靠它驱动，不额外续帧）。
  mutable float hue_phase_{0.0f};
};

}  // namespace gedou
