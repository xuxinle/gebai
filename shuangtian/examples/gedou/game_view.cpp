#include "game_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>

#include "st/math/path.hpp"
#include "st/core/json.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/line_layout.hpp"
#include "st/ui/theme.hpp"

namespace gedou {

namespace {

using st::math::Color;
using st::math::Point;
using st::math::Rect;
using st::raster::Paint;
/// 画布类型别名**不能**叫 `Surface`：`Element` 里有个同名嵌套枚举（语义表面档位），
/// 在类成员定义的参数里名字查找会先命中它（报错内容是“类型不匹配”而不是“重名”）。
using Canvas = st::raster::Surface;

/// 逻辑帧率。**固定步长**：低帧率机器上判定不变慢，测试也可复现。
constexpr double kLogicHz = 60.0;
/// 单次绘制最多补的逻辑帧（防止长时间挂起后一次性补上万帧把界面卡死）。
constexpr int kMaxCatchUpFrames = 4;

constexpr Color kInk{0x11, 0x14, 0x1B, 0xFF};
constexpr Color kPale{0xF4, 0xF7, 0xFF, 0xFF};

/// 沿一条线段画粗线（骨架用）。
void draw_bone(Canvas& canvas, Point from, Point to, Color color, float width) {
  st::math::Path path;
  path.move_to(from);
  path.line_to(to);
  canvas.stroke_path(path, Paint::solid(color), width, st::raster::StrokeStyle{
                                                     st::raster::LineCap::Round,
                                                     st::raster::LineJoin::Round});
}

/// 在两点之间画一个"胶囊"（比描边更结实的四肢观感；半径随宽度走）。
void draw_limb(Canvas& canvas, Point from, Point to, Color color, float width) {
  // 用三条粗线叠出圆头：中段直线 + 两端圆点。比 `stroke_path` 圆帽更稳
  // （圆帽在极短线段上会退化成一个点，关节处会露缝）。
  draw_bone(canvas, from, to, color, width);
  canvas.fill_circle(from, width * 0.5f, Paint::solid(color));
  canvas.fill_circle(to, width * 0.5f, Paint::solid(color));
}

[[nodiscard]] auto mix_alpha(Color color, float alpha) -> Color {
  return color.with_alpha_f(std::clamp(alpha, 0.0f, 1.0f));
}

}  // namespace

GameView::GameView() {
  set_id("arena");
  set_focusable(true);   // 键盘要能落到这里（否则方向键被 Tab 焦点环吃掉）
  style().background = Color{0x0B, 0x0E, 0x16, 0xFF};
  match_ = std::make_unique<Match>(0, 1, Arena{});
}

GameView::~GameView() = default;

void GameView::start_match(std::size_t left_profile, std::size_t right_profile) {
  match_ = std::make_unique<Match>(left_profile, right_profile, Arena{});
  effects_.clear();
  view_ = ViewState{};
  view_.difficulty = ai_.difficulty();
  ai_.reseed(0x5EED1234U);
  last_time_ = -1.0;
  accumulator_ = 0.0;
  mark_dirty();
}

void GameView::restart() {
  match_->reset();
  effects_.clear();
  view_.banner.clear();
  view_.banner_age = 0;
  view_.shake = 0.0f;
  view_.flash = 0.0f;
  view_.logic_frames = 0;
  view_.last_hit_count = 0;
  // AI 也要回到初始种子：
  // “再开一局”与“重新启动”必须得到同一场对战——否则“同种子可复现”只在
  // 进程刚起来时成立，而重开一局就漂了（测试里表现为“两跑位置不同”）。
  reset_ai();
  player_intent_ = Intent{};
  last_time_ = -1.0;
  accumulator_ = 0.0;
  mark_dirty();
}

void GameView::set_player_intent(Intent intent) { player_intent_ = intent; }

void GameView::set_ai_enabled(bool enabled) {
  view_.ai_enabled = enabled;
  ai_.set_enabled(enabled);
  mark_dirty();
}

void GameView::reset_ai() {
  // AI 的所有内部状态都在这里重建（种子/难度/启用位）：
  // “重开一局”与“重新启动”必须得到同一场对战。
  ai_ = AiController(0x5EED1234U, view_.difficulty);
  ai_.set_enabled(view_.ai_enabled && !dummy_);
  opponent_intent_ = Intent{};
  policy_cooldown_ = 0;
}

auto GameView::load_policy(std::string_view path) -> std::optional<std::string> {
  PolicyNet loaded{};
  if (auto error = loaded.load_file(path); error.has_value()) return error;
  policy_ = std::move(loaded);
  view_.policy_loaded = true;
  mark_dirty();
  return std::nullopt;
}

void GameView::set_difficulty(Difficulty difficulty) {
  view_.difficulty = difficulty;
  ai_.set_difficulty(difficulty);
}

void GameView::set_paused(bool paused) {
  view_.paused = paused;
  mark_dirty();
}

void GameView::set_show_hitbox(bool show) {
  view_.show_hitbox = show;
  mark_dirty();
}

void GameView::set_dummy(bool dummy) {
  dummy_ = dummy;
  // 训练桩会**关掉 AI 的动作输出**；但 AI 内部的反应计时仍可能在跑。
  // 切回来时重建 AI，避免“关桩后第一帧用的是旧决策”。
  reset_ai();
  mark_dirty();
}

void GameView::measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) {
  (void)context;
  const float width = constraints.max_width > 0.0f && constraints.max_width < st::ui::kUnbounded
                          ? constraints.max_width
                          : 1200.0f;
  const float height = constraints.max_height > 0.0f && constraints.max_height < st::ui::kUnbounded
                           ? constraints.max_height
                           : 640.0f;
  measured_ = st::math::Size{width, height};
}

void GameView::arrange(const st::ui::RenderContext& context, Rect rect) {
  Element::arrange(context, rect);
  // 场地随视口重排：地面线固定在下方 80%，左右各留 6% 边距。
  //
  // 角色位置按**相对比例**保留：只改 `Arena`（不重建 `Match`）——
  // 战斗状态（血量/槽/招式帧）不能因为拖一下窗口就清空。
  //
  // ⚠ 神经网络策略装载时把初始间距固定成 `kPolicyStartGap`：
  // 策略是在固定场地（1040 宽、200 间距）上学出来的，而窗口尺寸各不同。
  // 若初始间距随窗口缩放（这里原本是 `rect.width * 0.17`），策略可能一开局
  // 就处于自己**从未见过**的距离分布里——进游戏验证时实测：宽窗口下它停在
  // 远远超出腿长的位置不动。
  constexpr float kPolicyStartGap = 200.0f;
  Arena arena{};
  arena.left = rect.x + rect.width * 0.06f;
  arena.right = rect.right() - rect.width * 0.06f;
  arena.ground = rect.y + rect.height * 0.80f;
  arena.start_gap = policy_.valid() ? kPolicyStartGap : rect.width * 0.17f;
  const Arena previous = match_->arena();
  if (std::abs(previous.left - arena.left) < 0.5f && std::abs(previous.right - arena.right) < 0.5f &&
      std::abs(previous.ground - arena.ground) < 0.5f) {
    return;
  }
  const float old_half = std::max(1.0f, previous.width() * 0.5f);
  const std::array<float, 2> relative{(match_->state(0).x - previous.center()) / old_half,
                                      (match_->state(1).x - previous.center()) / old_half};
  match_->set_arena(arena);
  match_->place(0, arena.center() + relative[0] * arena.width() * 0.5f);
  match_->place(1, arena.center() + relative[1] * arena.width() * 0.5f);
  if (policy_.valid() && view_.logic_frames == 0) {
    // 策略接管时，**开局的那一次布局**按绝对间距摆位。
    //
    // 为何必须覆盖掉上面的“相对比例”摆法：相对摆位会把两个角色按窗口宽度
    // 等比拉开（实测 1600 宽窗口开局间距变成 428px，而策略只在 200px 附近
    // 学过“能打到”），于是它一开局就处在从未见过的距离分布里、站住不动。
    match_->place(0, arena.center() - kPolicyStartGap * 0.5f);
    match_->place(1, arena.center() + kPolicyStartGap * 0.5f);
  }
}

void GameView::advance_logic(bool force) const {
  if (view_.paused && !force) return;
  FrameEvents events = match_->advance(player_intent_, dummy_ ? Intent{} : opponent_intent_);
  ++view_.logic_frames;
  view_.last_hit_count = static_cast<int>(events.hits.size());

  for (const HitEvent& hit : events.hits) {
    const FighterState& victim = match_->state(hit.victim);
    push_effect(make_effect(hit.blocked ? EffectKind::Block : EffectKind::Spark, hit.at,
                            hit.blocked ? 8 : 12, hit.knockdown ? 1.6f : 1.0f,
                            !victim.facing_right));
    if (hit.combo >= 2) {
      push_effect(make_effect(EffectKind::Text, Point{hit.at.x, hit.at.y - 52.0f}, 34, 1.0f,
                              !hit.blocked, std::format("{} 连击", hit.combo)));
    }
  }
  for (std::size_t index = 0; index < events.knocked_out.size(); ++index) {
    push_effect(make_effect(EffectKind::Shake, Point{}, 26));
    view_.banner = "K.O.";
    view_.banner_age = 0;
  }
  if (events.special_fired) {
    view_.flash = 1.0f;
    push_effect(make_effect(EffectKind::Dust,
                            Point{match_->arena().center(), match_->arena().ground}, 24, 1.8f));
  }
  view_.shake = std::max(view_.shake, events.shake);
  // 地面粒子：走路与落地时给一点尘土（纯观感，不影响逻辑）。
  if (match_->state(0).move == MoveId::Walk && view_.logic_frames % 9 == 0) {
    push_effect(make_effect(EffectKind::Dust,
                            Point{match_->state(0).x, match_->arena().ground}, 16, 0.6f,
                            match_->state(0).facing_right));
  }
  if (match_->state(1).move == MoveId::Walk && view_.logic_frames % 9 == 0) {
    push_effect(make_effect(EffectKind::Dust,
                            Point{match_->state(1).x, match_->arena().ground}, 16, 0.6f,
                            match_->state(1).facing_right));
  }
  // 结算横幅：有人倒下后进入"K.O."演出。
  if (match_->phase() == RoundPhase::Over && view_.banner.empty()) {
    view_.banner = "K.O.";
    view_.banner_age = 0;
  }
  ++view_.banner_age;
  view_.shake *= 0.86f;
  view_.flash *= 0.88f;
  if (view_.shake < 0.2f) view_.shake = 0.0f;
  if (view_.flash < 0.02f) view_.flash = 0.0f;
  expire_effects();
}

void GameView::push_effect(Effect effect) const {
  effects_.push_back(std::move(effect));
  // 上限：爆打时特效不设上限会一直涨（每帧都在 push），最终拖慢绘制。
  constexpr std::size_t kMaxEffects = 96;
  if (effects_.size() > kMaxEffects) {
    effects_.erase(effects_.begin(), effects_.begin() + static_cast<std::ptrdiff_t>(effects_.size() - kMaxEffects));
  }
}

void GameView::expire_effects() const {
  std::erase_if(effects_, [](const Effect& effect) { return effect.age >= effect.life; });
}

auto GameView::step_logic(int frames) -> std::string {
  const int count = std::clamp(frames, 0, 6000);
  for (int index = 0; index < count; ++index) {
    if (view_.ai_enabled && !dummy_) opponent_intent_ = opponent_decision();
    advance_logic(true);
    for (Effect& effect : effects_) ++effect.age;
    expire_effects();
  }
  mark_dirty();
  return snapshot();
}

auto GameView::opponent_decision() const -> Intent {
  // 对手由**神经网络策略**接管时，把观测喂给它，再把动作折回 `Intent`。
  //
  // ## 必须与训练环境同步的“动作节流”
  //
  // 训练环境里，攻击受到**冷却**约束（`RlEnv::Config::attack_cooldown`）；
  // 若游戏内不做同样的事，策略会每帧输出“踢腿”，而角色一直接到新的出招请求、
  // 永远停在起手段——表现就是“机器人站着不动/轮流摆姿势”而永远打不到人
  // （实测：进游戏验证时正好如此）。
  //
  // 这里把同一套节流复现一遍，**用与训练完全相同的公式**（招式总帧数）。
  if (policy_.valid()) {
    const FighterState& self = match_->state(1 - view_.player_side);
    Intent intent = action_intent(policy_.act(observe(*match_, 1 - view_.player_side)));
    if (policy_cooldown_ > 0) {
      --policy_cooldown_;
      if (intent.attack.has_value()) intent.attack.reset();
      return intent;
    }
    if (const auto move = intent.attack; move.has_value() && self.can_act()) {
      const FighterProfile& profile = fighter_profile(self.profile);
      policy_cooldown_ = scaled_timing(*move, profile.speed).total();
    }
    return intent;
  }
  return ai_.think(*match_, 1 - view_.player_side);
}

auto GameView::policy_missing_pressure() const -> bool {
  // 策略“学到的进攻距离”是训练场地里的（腿长 112px + 半身 ~48px）。
  // 若真实现场里距离超过这个量，策略可能停在原地——那是因为它**没见过**
  // 这么大的距离（训练场地 1040 宽、开局间距 90~430px）。
  // 这里只做诊断暴露，**不替它改行为**（替它改就会掩盖训练/现场分布不一致）。
  if (!policy_.valid()) return false;
  const FighterState& self = match_->state(1 - view_.player_side);
  const FighterState& other = match_->state(view_.player_side);
  const float gap = std::abs(other.x - self.x);
  const float reach = attack_spec(MoveId::Kick).reach_far * kBodyScale + 30.0f * kBodyScale;
  return gap > reach * 1.6f;
}

auto GameView::policy_action_index() const -> int {
  if (!policy_.valid()) return -1;
  return static_cast<int>(policy_.act(observe(*match_, 1 - view_.player_side)));
}

auto GameView::policy_probe_json() const -> std::string {
  using st::Json;
  Json root = Json::object();
  root["policy_loaded"] = policy_.valid();
  if (!policy_.valid()) return st::json_dump(root, false);
  const Observation observation = observe(*match_, 1 - view_.player_side);
  Json values = Json::array();
  for (float value : observation) values.push_back(value);
  root["observation"] = std::move(values);
  const std::vector<float> logits = policy_.forward(observation);
  Json logits_json = Json::array();
  for (float value : logits) logits_json.push_back(value);
  root["logits"] = std::move(logits_json);
  root["action"] = static_cast<std::int64_t>(policy_.act(observation));
  root["side"] = static_cast<std::int64_t>(1 - view_.player_side);
  root["cooldown"] = static_cast<std::int64_t>(policy_cooldown_);
  return st::json_dump(root, false);
}

auto GameView::snapshot() const -> std::string {
  return match_->snapshot();
}

void GameView::paint_content(const st::ui::RenderContext& context, Canvas& canvas) const {
  const Rect area = bounds();
  if (area.is_empty()) return;

  // —— 帧推进：固定步长累积 ——
  const double now = context.time_seconds;
  if (last_time_ >= 0.0 && now > last_time_ && !view_.paused) {
    accumulator_ += now - last_time_;
    const double step = 1.0 / kLogicHz;
    int steps = 0;
    while (accumulator_ >= step && steps < kMaxCatchUpFrames) {
      accumulator_ -= step;
      ++steps;
    }
    if (steps > 0) {
      if (view_.ai_enabled && !dummy_) {
        // 与 `step_logic` 走**同一个**决策入口（策略/手写 AI 在这里分流）。
        opponent_intent_ = opponent_decision();
      }
      for (int index = 0; index < steps; ++index) {
        // 每帧重新问策略：神经网络策略是**逐帧**的（而手写 AI 自带反应计时，
        // 所以两者都走这里不会给任一方多/少决策机会）。
        if (view_.ai_enabled && !dummy_) opponent_intent_ = opponent_decision();
        advance_logic();
        for (Effect& effect : effects_) ++effect.age;
        expire_effects();
      }
      request_animation();
    } else if (match_->phase() == RoundPhase::Fight || !effects_.empty() || view_.shake > 0.0f) {
      // 仍在打/特效没播完 → 下一帧还要画（否则会停在半截动画）。
      request_animation();
    }
  } else if (last_time_ < 0.0) {
    // 首帧：不前进（保证静态首帧可复现），但要请求下一帧让动画起头。
    request_animation();
  }
  last_time_ = now;
  hue_phase_ = static_cast<float>(now) * 0.6f;

  canvas.push_clip_rect(area);
  paint_backdrop(context, canvas);
  paint_arena(context, canvas);
  paint_effects(context, canvas);
  paint_fighter(context, canvas, 0);
  paint_fighter(context, canvas, 1);
  if (view_.show_hitbox) paint_hitboxes(context, canvas);
  paint_hud(context, canvas);
  paint_banner(context, canvas);
  canvas.pop_clip();
}

void GameView::paint_backdrop(const st::ui::RenderContext& context, Canvas& canvas) const {
  (void)context;
  const Rect area = bounds();
  // 夜空：竖向线性渐变（竖向有逐行 SIMD 快路径；斜向/径向要贵一个量级）。
  const auto gradient = st::raster::Gradient::linear(
      Point{area.x, area.y}, Point{area.x, area.y + area.height * 0.80f},
      {{0.0f, Color{0x0A, 0x0E, 0x1C, 0xFF}},
       {0.45f, Color{0x18, 0x1F, 0x38, 0xFF}},
       {0.78f, Color{0x33, 0x28, 0x3E, 0xFF}},
       {1.0f, Color{0x46, 0x30, 0x3A, 0xFF}}});
  canvas.fill_rect(area, Paint::with_gradient(gradient));

  // 星空：**确定性的**小点（黄金分割序列，无需随机源、跨帧不动）。
  // 为何不用多圈同心圆造“月晕”：环与环之间会留下可见的色阶台阶
  // （旧实现叠了 5 圈，截图上一眼就是同心圆环）。
  const float sky_height = area.height * 0.62f;
  for (int index = 0; index < 46; ++index) {
    const float u = std::fmod(0.6180339887f * static_cast<float>(index), 1.0f);
    const float v = std::fmod(0.7548776662f * static_cast<float>(index), 1.0f);
    const float x = area.x + u * area.width;
    const float y = area.y + v * sky_height * 0.85f;
    const auto alpha = static_cast<std::uint8_t>(40 + (index * 37) % 90);
    canvas.fill_circle(Point{x, y}, (index % 5 == 0) ? 1.6f : 1.0f,
                       Paint::solid(Color{0xD8, 0xE4, 0xFF, alpha}));
  }
  // 月亮：实心盘 + 一道描边（**不加外圈光晕**：见上），位置靠黄金分割偏右。
  const Point moon{area.x + area.width * 0.74f, area.y + area.height * 0.20f};
  canvas.fill_circle(moon, 54.0f, Paint::solid(Color{0xF2, 0xE6, 0xC8, 0xE6}));
  canvas.fill_circle(Point{moon.x - 16.0f, moon.y - 12.0f}, 9.0f,
                     Paint::solid(Color{0xD9, 0xC9, 0xA4, 0x8C}));
  canvas.fill_circle(Point{moon.x + 14.0f, moon.y + 16.0f}, 13.0f,
                     Paint::solid(Color{0xD9, 0xC9, 0xA4, 0x76}));

  // 远景城市剪影：两层矩形（远层更暗更矮），比任何渐变都便宜且读得清。
  const float horizon = area.y + area.height * 0.62f;
  for (int layer = 0; layer < 2; ++layer) {
    const Color tone = layer == 0 ? Color{0x14, 0x19, 0x2A, 0xFF} : Color{0x1D, 0x23, 0x38, 0xFF};
    float x = area.x - (layer == 0 ? 20.0f : 0.0f);
    int step = 0;
    while (x < area.right()) {
      const float width = 34.0f + static_cast<float>((step * 53 + layer * 17) % 5) * 16.0f;
      const float height = (layer == 0 ? 40.0f : 62.0f) +
                           static_cast<float>((step * 29 + layer * 11) % 6) * 12.0f;
      canvas.fill_rect(Rect{x, horizon - height, width - 6.0f, height}, Paint::solid(tone));
      // 窗户：每栋楼一排两三个小黄点（只给近层画，远层画了反而脏）。
      if (layer == 1 && step % 2 == 0) {
        for (int row = 0; row < 3; ++row) {
          if ((step + row) % 3 != 0) continue;
          canvas.fill_rect(Rect{x + 10.0f, horizon - height + 12.0f + static_cast<float>(row) * 16.0f,
                                6.0f, 6.0f},
                           Paint::solid(Color{0xFF, 0xD9, 0x8A, 0x8C}));
        }
      }
      x += width;
      ++step;
    }
  }
}

void GameView::paint_arena(const st::ui::RenderContext& context, Canvas& canvas) const {
  (void)context;
  const Rect area = bounds();
  const Arena& arena = match_->arena();
  const float ground = arena.ground;
  const float center = area.center().x;

  // 地面：一条亮线 + 向远处收拢的透视线（让“这是 3D 地面”不需要真 3D）。
  canvas.fill_rect(Rect{area.x, ground, area.width, area.height - (ground - area.y)},
                   Paint::with_gradient(st::raster::Gradient::linear(
                       Point{area.x, ground}, Point{area.x, area.bottom()},
                       {{0.0f, Color{0x3A, 0x2C, 0x38, 0xFF}}, {1.0f, Color{0x12, 0x12, 0x1A, 0xFF}}})));
  st::math::Path horizon;
  horizon.move_to(Point{area.x, ground});
  horizon.line_to(Point{area.right(), ground});
  canvas.stroke_path(horizon, Paint::solid(Color{0xE8, 0xC7, 0x4C, 0x60}), 2.0f);

  // 透视格线：从灭点（场地中心上方 240px）向下散开的竖线 + 越远越密的横线。
  const Point vanish{center, ground - 240.0f};
  for (int index = -6; index <= 6; ++index) {
    const float offset = static_cast<float>(index) * (area.width * 0.11f);
    st::math::Path line;
    line.move_to(Point{vanish.x + offset * 0.18f, ground});
    line.line_to(Point{vanish.x + offset * 1.9f, area.bottom()});
    canvas.stroke_path(line, Paint::solid(Color{0xC9, 0xDB, 0xFF, 0x1C}), 1.0f);
  }
  for (int row = 0; row < 5; ++row) {
    const float t = static_cast<float>(row) / 5.0f;
    const float y = ground + (area.bottom() - ground) * t * t;
    st::math::Path line;
    line.move_to(Point{area.x, y});
    line.line_to(Point{area.right(), y});
    canvas.stroke_path(line, Paint::solid(Color{0xC9, 0xDB, 0xFF, static_cast<std::uint8_t>(26 - row * 4)}),
                       1.0f);
  }

  // 场地边界柱（左右各一，帮助判断"被逼到墙角"）。
  for (const float edge : {arena.left - 26.0f, arena.right + 26.0f}) {
    canvas.fill_rect(Rect{edge - 5.0f, ground - 150.0f, 10.0f, 150.0f},
                     Paint::solid(Color{0x44, 0x4C, 0x66, 0xFF}), 5.0f);
    canvas.fill_rect(Rect{edge - 7.0f, ground - 158.0f, 14.0f, 14.0f},
                     Paint::solid(Color{0xE8, 0xC7, 0x4C, 0xCC}), 7.0f);
  }
}

void GameView::paint_fighter(const st::ui::RenderContext& context, Canvas& canvas, int side) const {
  const FighterState& fighter = match_->state(side);
  const FighterProfile& profile = fighter_profile(fighter.profile);
  // 震屏：整体偏移（只影响绘制，不影响判定——判定在逻辑层，绝不跟着抖）。
  const float shake_x = view_.shake * std::sin(hue_phase_ * 21.0f) * 0.5f;
  const float shake_y = view_.shake * std::cos(hue_phase_ * 17.0f) * 0.35f;
  Pose pose = match_->world_pose(side);
  const auto shift = [&](Point point) {
    return Point{point.x + shake_x, point.y + shake_y};
  };
  pose = [&] {
    Pose out = pose;
    out.hip = shift(out.hip);
    out.chest = shift(out.chest);
    out.head = shift(out.head);
    out.shoulder_front = shift(out.shoulder_front);
    out.shoulder_back = shift(out.shoulder_back);
    out.elbow_front = shift(out.elbow_front);
    out.hand_front = shift(out.hand_front);
    out.elbow_back = shift(out.elbow_back);
    out.hand_back = shift(out.hand_back);
    out.knee_front = shift(out.knee_front);
    out.foot_front = shift(out.foot_front);
    out.knee_back = shift(out.knee_back);
    out.foot_back = shift(out.foot_back);
    return out;
  }();

  const bool down = fighter.move == MoveId::KnockDown;
  const float ground_y = match_->arena().ground + shake_y;

  // 影子：地面上的椭圆（强度随"离地高度"变化——必杀腾空时影子变小变淡）。
  //
  // 必杀腾空时影子**跟着身体走**（水平向腾跃方向偏）：上一版固定在脚下，
  // 结果画面是“人往前飞、影子原地不动”——最容易被解读成“地上有个黑点”。
  const float lowest_foot = std::max(pose.foot_front.y, pose.foot_back.y);
  const float air = std::clamp((ground_y - lowest_foot) / 60.0f, 0.0f, 1.0f);
  const float body_dx = (pose.hip.x - fighter.x) * 0.75f;
  st::math::Path shadow;
  shadow.add_ellipse(Rect{fighter.x + body_dx - 42.0f * (1.0f - air * 0.3f) + shake_x,
                          ground_y - 9.0f, 84.0f * (1.0f - air * 0.3f), 18.0f});
  canvas.fill_path(shadow,
                   Paint::solid(Color{0x00, 0x00, 0x00, static_cast<std::uint8_t>(90 - air * 45)}));

  // 无敌帧/受击闪光：受击那一侧整体提亮（读起来像"被打了"）。
  Color gi = down ? profile.gi.darken(0.25f) : profile.gi;
  if (fighter.hitstun > 0) gi = gi.mix(Color{0xFF, 0xE2, 0xE2, 0xFF}, 0.45f);

  // —— 骨架 ——
  //
  // 绘制顺序 = 遮挡顺序：后肢（更暗）→ 躯干 → 前肢 → 头。
  // 手臂从**肩点**出发（不是躯干中线）：两条臂同源会糊成一坠。
  const Color line = gi;
  const float bone = 26.0f;   // 世界像素；骨架已按 `kBodyScale` 缩放过，这里是最终粗细
  const Color back = line.darken(0.3f);
  draw_limb(canvas, pose.hip, pose.knee_back, back, bone * 1.0f);
  draw_limb(canvas, pose.knee_back, pose.foot_back, back, bone * 0.88f);
  draw_limb(canvas, pose.shoulder_back, pose.elbow_back, back, bone * 0.86f);
  draw_limb(canvas, pose.elbow_back, pose.hand_back, back, bone * 0.74f);
  // 躯干：髋→胸为主体，再用一个稍宽的梯形肩线把“胸膛”读出来
  draw_limb(canvas, pose.hip, pose.chest, line, bone * 1.7f);
  draw_limb(canvas, pose.shoulder_back, pose.shoulder_front, line, bone * 1.2f);
  // 前肢
  draw_limb(canvas, pose.hip, pose.knee_front, line, bone * 1.06f);
  draw_limb(canvas, pose.knee_front, pose.foot_front, line, bone * 0.92f);
  draw_limb(canvas, pose.shoulder_front, pose.elbow_front, line, bone * 0.92f);
  draw_limb(canvas, pose.elbow_front, pose.hand_front, line, bone * 0.78f);

  /// 腰带（配色带）+ 拳套（强调色圆点）+ 脚
  // 腰带是**横向**的带子：沿髋→胸的垂直方向取两个端点，
  // 而不是沿躯干方向（沿躯干画会得到一个“大圆盘”——实测截图里像肚子肻了个球）。
  {
    const float dx = pose.chest.x - pose.hip.x;
    const float dy = pose.chest.y - pose.hip.y;
    const float length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    const Point unit{dx / length, dy / length};
    const Point perpendicular{-unit.y, unit.x};
    const Point belt_center{pose.hip.x + dx * 0.40f, pose.hip.y + dy * 0.40f};
    const float half_span = bone * 0.72f;
    draw_bone(canvas, Point{belt_center.x - perpendicular.x * half_span,
                            belt_center.y - perpendicular.y * half_span},
              Point{belt_center.x + perpendicular.x * half_span,
                    belt_center.y + perpendicular.y * half_span},
              profile.accent, bone * 0.5f);
  }
  // 拳套用**比道服亮**的配色（不是更暗）：暗拳套贴在暗道服上会糊成一个色块，
  // 看截图根本分不出“这是手”（实测改过两轮）。亮一阶 + 落在身体轮廓外侧才可读。
  canvas.fill_circle(pose.hand_front, bone * 0.44f,
                     Paint::solid(profile.accent.lighten(0.10f)));
  canvas.fill_circle(pose.hand_back, bone * 0.4f, Paint::solid(profile.accent.darken(0.10f)));
  canvas.fill_circle(pose.foot_front, bone * 0.44f, Paint::solid(kInk));
  canvas.fill_circle(pose.foot_back, bone * 0.4f, Paint::solid(mix_alpha(kInk, 0.82f)));

  // 头：颈 → 脸 → 头带 → 朝向标记
  // 颈必须画（否则头看起来是飘的：胸腔上缘与头部之间有 4~6px 空白）。
  draw_bone(canvas, pose.chest, pose.head, line.darken(0.12f), bone * 0.7f);
  canvas.fill_circle(pose.head, pose.head_radius, Paint::solid(profile.skin));
  {
    // 头带（配色）+ 面朝方向的短须（用两条短线拼出"视线"）。
    const float band = pose.head_radius * 0.42f;
    canvas.fill_rect(Rect{pose.head.x - pose.head_radius, pose.head.y - band * 1.55f,
                          pose.head_radius * 2.0f, band},
                     Paint::solid(profile.accent), band * 0.5f);
    const float sign = fighter.facing_right ? 1.0f : -1.0f;
    draw_bone(canvas, Point{pose.head.x + sign * pose.head_radius * 0.96f, pose.head.y},
              Point{pose.head.x + sign * pose.head_radius * 1.42f, pose.head.y + 3.0f}, kInk, 3.0f);
    draw_bone(canvas, Point{pose.head.x + sign * pose.head_radius * 0.5f, pose.head.y - 3.0f},
              Point{pose.head.x + sign * pose.head_radius * 0.72f, pose.head.y - 3.0f},
              Color{0x1A, 0x1A, 0x1A, 0xFF}, 3.0f);
  }

  // 防御：身前一道弧形护盾（读起来就是"挡住了"）。
  if (fighter.move == MoveId::Guard) {
    st::math::Path shield;
    shield.add_arc(Point{pose.chest.x, pose.chest.y + 6.0f}, 56.0f,
                   fighter.facing_right ? -70.0f : 200.0f,
                   fighter.facing_right ? 70.0f : 340.0f);
    canvas.stroke_path(shield, Paint::solid(Color{0x7F, 0xD8, 0xFF, 0xB4}), 5.0f,
                       st::raster::StrokeStyle{st::raster::LineCap::Round,
                                               st::raster::LineJoin::Round});
  }

  // 必杀槽满：身周一层发光环（"能放了"的直接提示）。
  if (fighter.meter >= kMeterMax && fighter.alive()) {
    const float pulse = 0.5f + 0.5f * std::sin(hue_phase_ * 4.0f);
    st::math::Path ring;
    ring.add_ellipse(Rect{pose.hip.x - 46.0f, ground_y - 24.0f, 92.0f, 30.0f});
    canvas.stroke_path(ring,
                       Paint::solid(Color{0xFF, 0xD5, 0x66, static_cast<std::uint8_t>(90 + 90 * pulse)}),
                       3.0f);
  }

  // 连击数：头顶的小标（只在打中期间显示）。
  if (fighter.combo >= 2 && fighter.comboTimer > 0 && context.text != nullptr) {
    const std::string label = std::format("{} 连", fighter.combo);
    const float width = context.text->measure_width(label, 15.0f);
    const Point at{pose.head.x - width * 0.5f, pose.head.y - pose.head_radius - 26.0f};
    canvas.fill_rect(Rect{at.x - 7.0f, at.y - 3.0f, width + 14.0f, 22.0f},
                     Paint::solid(Color{0x11, 0x14, 0x1B, 0xB4}), 6.0f);
    context.text->draw(canvas, label, at, 15.0f, Color{0xFF, 0xE2, 0x8A, 0xFF});
  }
}

void GameView::paint_effects(const st::ui::RenderContext& context, Canvas& canvas) const {
  for (const Effect& effect : effects_) {
    const float t = static_cast<float>(effect.age) / static_cast<float>(std::max(1, effect.life));
    switch (effect.kind) {
      case EffectKind::Spark: {
        // 打击火花：四条放射线，随年龄张开并淡出。
        const float radius = 8.0f + 34.0f * t * effect.scale;
        const auto alpha = static_cast<std::uint8_t>(std::clamp(1.0f - t, 0.0f, 1.0f) * 230.0f);
        for (int ray = 0; ray < 6; ++ray) {
          const float angle = 0.5236f * static_cast<float>(ray) + t * 0.4f;
          st::math::Path line;
          line.move_to(Point{effect.at.x + std::cos(angle) * radius * 0.32f,
                             effect.at.y + std::sin(angle) * radius * 0.32f});
          line.line_to(Point{effect.at.x + std::cos(angle) * radius,
                             effect.at.y + std::sin(angle) * radius});
          canvas.stroke_path(line, Paint::solid(Color{0xFF, 0xE9, 0xA8, alpha}), 3.0f,
                             st::raster::StrokeStyle{st::raster::LineCap::Round,
                                                     st::raster::LineJoin::Round});
        }
        canvas.fill_circle(effect.at, radius * 0.5f,
                           Paint::solid(Color{0xFF, 0xFF, 0xFF, static_cast<std::uint8_t>(alpha / 2)}));
        break;
      }
      case EffectKind::Block: {
        const float radius = 26.0f + 16.0f * t;
        const auto alpha = static_cast<std::uint8_t>(std::clamp(1.0f - t, 0.0f, 1.0f) * 200.0f);
        st::math::Path arc;
        arc.add_arc(effect.at, radius, -80.0f, 80.0f);
        canvas.stroke_path(arc, Paint::solid(Color{0x8E, 0xD9, 0xFF, alpha}), 4.0f);
        break;
      }
      case EffectKind::Dust: {
        const float spread = 10.0f + 26.0f * t;
        const auto alpha = static_cast<std::uint8_t>(std::clamp(1.0f - t, 0.0f, 1.0f) * 90.0f);
        canvas.fill_circle(Point{effect.at.x - spread, effect.at.y - 4.0f - t * 8.0f},
                           6.0f * effect.scale, Paint::solid(Color{0xD8, 0xC8, 0xB0, alpha}));
        canvas.fill_circle(Point{effect.at.x + spread * 0.8f, effect.at.y - 2.0f - t * 6.0f},
                           5.0f * effect.scale, Paint::solid(Color{0xD8, 0xC8, 0xB0, alpha}));
        break;
      }
      case EffectKind::Shake: break;   // 震屏由 view_.shake 承担
      case EffectKind::Text: {
        if (context.text == nullptr || effect.label.empty()) break;
        const auto alpha = static_cast<std::uint8_t>(std::clamp(1.0f - t, 0.0f, 1.0f) * 255.0f);
        const float width = context.text->measure_width(effect.label, 20.0f);
        const Point at{effect.at.x - width * 0.5f, effect.at.y - 26.0f * t};
        // 描边（先画深色底）→ 再画亮色字：小字号在大背景上要能读清。
        context.text->draw(canvas, effect.label, Point{at.x + 2.0f, at.y + 2.0f}, 20.0f,
                           Color{0x11, 0x14, 0x1B, alpha});
        context.text->draw(canvas, effect.label, at, 20.0f, Color{0xFF, 0xE2, 0x8A, alpha});
        break;
      }
    }
  }
}

void GameView::paint_hitboxes(const st::ui::RenderContext& context, Canvas& canvas) const {
  (void)context;
  for (int side = 0; side < 2; ++side) {
    const Rect body = match_->hurtbox(side);
    st::math::Path outline;
    outline.add_rounded_rect(body, 4.0f);
    canvas.stroke_path(outline, Paint::solid(Color{0x5A, 0xD1, 0xFF, 0xC0}), 1.5f);
    const Rect attack = match_->active_hitbox(side);
    if (!attack.is_empty()) {
      canvas.fill_rect(attack, Paint::solid(Color{0xFF, 0x53, 0x53, 0x46}), 3.0f);
      st::math::Path edge;
      edge.add_rounded_rect(attack, 3.0f);
      canvas.stroke_path(edge, Paint::solid(Color{0xFF, 0x7A, 0x7A, 0xE0}), 1.5f);
    }
  }
}

void GameView::paint_hud(const st::ui::RenderContext& context, Canvas& canvas) const {
  const Rect area = bounds();
  const float pad = 30.0f;
  const float bar_width = area.width * 0.36f;
  const float bar_height = 22.0f;
  const float top = area.y + 34.0f;

  const auto draw_side = [&](int side, float x) {
    const FighterState& fighter = match_->state(side);
    const FighterProfile& profile = fighter_profile(fighter.profile);
    const bool mirror = side == 1;
    // 血条底
    const Rect track{x, top, bar_width, bar_height};
    canvas.fill_rect(track, Paint::solid(Color{0x00, 0x00, 0x00, 0x66}), bar_height * 0.5f);
    const float ratio = std::clamp(fighter.hp / std::max(1.0f, fighter.max_hp), 0.0f, 1.0f);
    const float filled = bar_width * ratio;
    if (filled > 1.0f) {
      const Rect fill_rect = mirror ? Rect{x + bar_width - filled, top, filled, bar_height}
                                    : Rect{x, top, filled, bar_height};
      const Color low = ratio < 0.3f ? Color{0xFF, 0x6B, 0x5A, 0xFF} : profile.gi;
      const auto gradient = st::raster::Gradient::linear(
          Point{fill_rect.x, top}, Point{fill_rect.x + fill_rect.width, top},
          {{0.0f, low}, {1.0f, low.mix(Color{0xFF, 0xFF, 0xFF, 0xFF}, 0.35f)}});
      canvas.fill_rect(fill_rect, Paint::with_gradient(gradient), bar_height * 0.5f);
    }
    // 血条描边（浅底/深底都要有边界）
    st::math::Path edge;
    edge.add_rounded_rect(track, bar_height * 0.5f);
    canvas.stroke_path(edge, Paint::solid(Color{0xFF, 0xFF, 0xFF, 0x3C}), 1.5f);

    // 必杀槽：血条下方一条更细的条，满时变金色并轻微脉动。
    const Rect meter_track{x, top + bar_height + 6.0f, bar_width * 0.7f, 8.0f};
    const float meter_ratio = std::clamp(fighter.meter / kMeterMax, 0.0f, 1.0f);
    canvas.fill_rect(meter_track, Paint::solid(Color{0x00, 0x00, 0x00, 0x55}), 4.0f);
    const float meter_filled = meter_track.width * meter_ratio;
    if (meter_filled > 1.0f) {
      const Rect meter_rect = mirror
                                  ? Rect{meter_track.right() - meter_filled, meter_track.y,
                                         meter_filled, meter_track.height}
                                  : Rect{meter_track.x, meter_track.y, meter_filled,
                                         meter_track.height};
      const bool ready = meter_ratio >= 0.999f;
      const float pulse = ready ? 0.55f + 0.45f * std::sin(hue_phase_ * 5.0f) : 1.0f;
      const Color meter_color = ready ? Color{0xFF, 0xD5, 0x66, 0xFF}.darken(1.0f - pulse)
                                      : Color{0x7F, 0xD8, 0xFF, 0xFF};
      canvas.fill_rect(meter_rect, Paint::solid(meter_color), 4.0f);
    }

    if (context.text != nullptr) {
      const std::string name = std::format("{} {}", profile.name, profile.title);
      const float name_width = context.text->measure_width(name, 16.0f);
      const float name_x = mirror ? x + bar_width - name_width : x;
      context.text->draw(canvas, name, Point{name_x, top + bar_height + 18.0f}, 16.0f, kPale);
      const std::string hp_text = std::format("{:.0f}", fighter.hp);
      const float hp_width = context.text->measure_width(hp_text, 15.0f);
      const float hp_x = mirror ? x : x + bar_width - hp_width;
      context.text->draw(canvas, hp_text, Point{hp_x, top - 22.0f}, 15.0f,
                         Color{0xFF, 0xE2, 0x8A, 0xFF});
    }
  };

  draw_side(view_.player_side, area.x + pad);
  draw_side(1 - view_.player_side, area.right() - pad - bar_width);

  // 中央：回合状态/计时（用逻辑帧换算成秒，确定性）。
  if (context.text != nullptr) {
    const double seconds = static_cast<double>(view_.logic_frames) / kLogicHz;
    const std::string clock = std::format("{:02d}:{:04.1f}", static_cast<int>(seconds) / 60,
                                          std::fmod(seconds, 60.0));
    const float width = context.text->measure_width(clock, 26.0f);
    canvas.fill_rect(Rect{area.center().x - width * 0.5f - 18.0f, top - 10.0f, width + 36.0f, 46.0f},
                     Paint::solid(Color{0x00, 0x00, 0x00, 0x66}), 10.0f);
    context.text->draw(canvas, clock, Point{area.center().x - width * 0.5f, top + 2.0f}, 26.0f,
                       view_.paused ? Color{0xFF, 0xC8, 0x63, 0xFF} : kPale);

    // 底部提示条：操作说明（三段，居中排）。
    const std::string hint =
        view_.paused ? "已暂停 · P 继续"
                     : "A/D 移动 · J 轻拳 · K 重拳 · L 蹬腿 · U 必杀 · S 防御 · P 暂停 · H 判定盒";
    const float hint_width = context.text->measure_width(hint, 13.0f);
    const float hint_y = area.bottom() - 34.0f;
    canvas.fill_rect(Rect{area.center().x - hint_width * 0.5f - 14.0f, hint_y - 7.0f,
                          hint_width + 28.0f, 26.0f},
                     Paint::solid(Color{0x00, 0x00, 0x00, 0x59}), 8.0f);
    context.text->draw(canvas, hint, Point{area.center().x - hint_width * 0.5f, hint_y}, 13.0f,
                       Color{0xC9, 0xD6, 0xF0, 0xE0});

    // 难度/AI 状态角标（右上角，与控制通道的"谁在打"一致）。
    const std::string mode = dummy_ ? "训练桩"
                                    : std::format("电脑 · {}", difficulty_name(ai_.difficulty()));
    const float mode_width = context.text->measure_width(mode, 12.0f);
    context.text->draw(canvas, mode, Point{area.right() - pad - mode_width, area.bottom() - 62.0f},
                       12.0f, Color{0x9A, 0xA8, 0xC8, 0xE0});
  }
}

void GameView::paint_banner(const st::ui::RenderContext& context, Canvas& canvas) const {
  const Rect area = bounds();
  // 必杀闪光：整屏一层暖白（快速衰减）。
  if (view_.flash > 0.02f) {
    canvas.fill_rect(area, Paint::solid(Color{0xFF, 0xF2, 0xD0,
                                              static_cast<std::uint8_t>(view_.flash * 70.0f)}));
  }
  if (view_.banner.empty() || context.text == nullptr) return;
  // 横幅：给 3 帧入场缩放，然后稳定显示。
  const float t = std::clamp(static_cast<float>(view_.banner_age) / 8.0f, 0.0f, 1.0f);
  const float size = 76.0f * (0.6f + 0.4f * t);
  const float width = context.text->measure_width(view_.banner, size);
  const float x = area.center().x - width * 0.5f;
  const float y = area.y + area.height * 0.33f;
  const auto alpha = static_cast<std::uint8_t>(255.0f * t);
  canvas.fill_rect(Rect{x - 34.0f, y - 14.0f, width + 68.0f, size + 22.0f},
                   Paint::solid(Color{0x00, 0x00, 0x00, static_cast<std::uint8_t>(110.0f * t)}), 16.0f);
  context.text->draw(canvas, view_.banner, Point{x + 3.0f, y + 3.0f}, size,
                     Color{0x11, 0x14, 0x1B, alpha});
  context.text->draw(canvas, view_.banner, Point{x, y}, size, Color{0xFF, 0xE2, 0x8A, alpha});
}

auto GameView::on_event(const st::ui::RenderContext& context, st::ui::Event& event) -> bool {
  (void)context;
  if (event.kind != st::ui::EventKind::KeyDown && event.kind != st::ui::EventKind::KeyUp) {
    return false;
  }
  const bool down = event.kind == st::ui::EventKind::KeyDown;
  const std::string& key = event.key;
  // 键位：A/D 移动、S 防御、J 轻、K 重、L 腿、U 必杀。
  // **按住分离**：KeyDown 置位、KeyUp 清位——不能只靠 KeyDown 的重复事件。
  if (key == "a" || key == "A") {
    key_left_ = down;
    event.handled = true;
  } else if (key == "d" || key == "D") {
    key_right_ = down;
    event.handled = true;
  } else if (key == "s" || key == "S") {
    key_guard_ = down;
    event.handled = true;
  } else if (down && (key == "j" || key == "J")) {
    pending_attack_ = MoveId::Light;
    event.handled = true;
  } else if (down && (key == "k" || key == "K")) {
    pending_attack_ = MoveId::Heavy;
    event.handled = true;
  } else if (down && (key == "l" || key == "L")) {
    pending_attack_ = MoveId::Kick;
    event.handled = true;
  } else if (down && (key == "u" || key == "U")) {
    pending_attack_ = MoveId::Special;
    event.handled = true;
  } else if (down && (key == "p" || key == "P")) {
    set_paused(!view_.paused);
    event.handled = true;
  } else if (down && (key == "h" || key == "H")) {
    set_show_hitbox(!view_.show_hitbox);
    event.handled = true;
  } else if (down && (key == "r" || key == "R")) {
    restart();
    event.handled = true;
  } else {
    return false;
  }
  // 折算成这一帧的意图（移动方向相对**自己朝向**：朝正面为 +1）。
  Intent intent{};
  const bool facing_right = match_->state(view_.player_side).facing_right;
  float horizontal = 0.0f;
  if (key_left_) horizontal -= 1.0f;
  if (key_right_) horizontal += 1.0f;
  intent.move = facing_right ? horizontal : -horizontal;
  intent.guard = key_guard_;
  intent.attack = pending_attack_;
  // 攻击是**边沿触发**：消费掉就清（按住不放不会连打——那是格斗游戏的基本诚实）。
  if (down && pending_attack_.has_value()) pending_attack_.reset();
  player_intent_ = intent;
  mark_dirty();
  return true;
}

void GameView::apply_theme(const st::ui::Theme& theme) {
  // 格斗场景刻意用自己的深色调色板（主题只影响外围控件），
  // 但保留一条：主题是暗色时背景再压暗一档，浅色主题时地板略微提亮。
  const bool dark = theme.mode() == st::ui::ThemeMode::Dark;
  style_.background = dark ? Color{0x07, 0x09, 0x10, 0xFF} : Color{0x0B, 0x0E, 0x16, 0xFF};
}

auto GameView::semantics_text() const -> std::string {
  const FighterState& left = match_->state(0);
  const FighterState& right = match_->state(1);
  return std::format("格斗场：{} {:.0f} vs {} {:.0f}", fighter_profile(left.profile).name, left.hp,
                     fighter_profile(right.profile).name, right.hp);
}

auto GameView::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "state") return snapshot();
  if (name == "policy_probe") return policy_probe_json();
  if (name == "policy_action") return std::format("{}", policy_action_index());
  if (name == "policy_loaded") return policy_.valid() ? "true" : "false";
  if (name == "logic_frames") return std::format("{}", view_.logic_frames);
  if (name == "paused") return view_.paused ? "true" : "false";
  if (name == "show_hitbox") return view_.show_hitbox ? "true" : "false";
  if (name == "dummy") return dummy_ ? "true" : "false";
  if (name == "ai") return view_.ai_enabled ? "true" : "false";
  if (name == "difficulty") return difficulty_name(ai_.difficulty());
  if (name == "player_side") return std::format("{}", view_.player_side);
  if (name == "phase") return match_->phase() == RoundPhase::Fight ? "fight" : "over";
  if (name == "left_move") return std::string(move_name(match_->state(0).move));
  if (name == "right_move") return std::string(move_name(match_->state(1).move));
  if (name == "left_hp") return std::format("{:.1f}", match_->state(0).hp);
  if (name == "right_hp") return std::format("{:.1f}", match_->state(1).hp);
  if (name == "left_meter") return std::format("{:.1f}", match_->state(0).meter);
  if (name == "right_meter") return std::format("{:.1f}", match_->state(1).meter);
  if (name == "left_hitbox") {
    const Rect box = match_->active_hitbox(0);
    return std::format("{:.1f},{:.1f},{:.1f},{:.1f}", box.x, box.y, box.width, box.height);
  }
  if (name == "right_hitbox") {
    const Rect box = match_->active_hitbox(1);
    return std::format("{:.1f},{:.1f},{:.1f},{:.1f}", box.x, box.y, box.width, box.height);
  }
  if (name == "left_x") return std::format("{:.1f}", match_->state(0).x);
  if (name == "right_x") return std::format("{:.1f}", match_->state(1).x);
  if (name == "banner") return view_.banner;
  if (name == "error") return last_error_;
  if (name == "winner") {
    const auto who = match_->winner();
    return who.has_value() ? std::format("{}", *who) : std::string{};
  }
  return std::nullopt;
}

auto GameView::set_property(std::string_view name, std::string_view value) -> bool {
  const std::string text{value};
  if (name == "paused") {
    set_paused(text == "true" || text == "1");
    return true;
  }
  if (name == "show_hitbox") {
    set_show_hitbox(text == "true" || text == "1");
    return true;
  }
  if (name == "dummy") {
    set_dummy(text == "true" || text == "1");
    return true;
  }  if (name == "ai") {
    set_ai_enabled(text == "true" || text == "1");
    reset_ai();
    return true;
  }
  if (name == "difficulty") {
    set_difficulty(difficulty_from_name(text.c_str()));
    reset_ai();
    mark_dirty();
    return true;
  }
  if (name == "player_side") {
    try {
      const int side = std::stoi(text);
      if (side != 0 && side != 1) return false;
      view_.player_side = side;
      mark_dirty();
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

auto GameView::property_names() const -> std::vector<std::string_view> {
  return {"state",     "logic_frames", "paused",      "show_hitbox", "dummy",      "ai",
          "difficulty", "player_side", "phase",      "left_move",   "right_move", "left_hp",
          "right_hp",  "left_meter",   "right_meter", "left_hitbox", "right_hitbox", "left_x",
          "right_x",   "banner",       "winner",      "error",
          // 策略自检（进游戏验证用）：观测/ logits / 动作下标，
          // 用来与评测环境逐值对比——不一致时才能定位到“训练/现场不同构”。
          "policy_loaded", "policy_action", "policy_probe"};
}

auto GameView::invoke_action(std::string_view action, std::string_view argument) -> bool {
  const std::string arg{argument};
  if (action == "restart") {
    restart();
    return true;
  }
  if (action == "pause") {
    set_paused(true);
    return true;
  }
  if (action == "resume") {
    set_paused(false);
    return true;
  }
  if (action == "toggle_pause") {
    set_paused(!view_.paused);
    return true;
  }
  if (action == "show_hitbox") {
    set_show_hitbox(arg.empty() ? true : (arg == "true" || arg == "1"));
    return true;
  }
  if (action == "set_dummy") {
    set_dummy(arg.empty() ? true : (arg == "true" || arg == "1"));
    return true;
  }
  if (action == "step") {
    int frames = 1;
    try {
      if (!arg.empty()) frames = std::stoi(arg);
    } catch (...) {
      return false;
    }
    (void)step_logic(frames);
    return true;
  }
  if (action == "attack") {
    const auto move = move_from_name(arg);
    if (!move.has_value()) {
      last_error_ = std::format("未知招式「{}」（可用：light/heavy/kick/special）", arg);
      return false;
    }
    // 直接让**玩家侧**发起这一招（绕过按键，供剧本化驱动用）。
    // 失败时把原因写进 `last_error_`：`invoke` 只回 `handled=false`，
    // 而“为什么不行”（硬直中？槽不够？）才是调用方真正要知道的。
    if (!match_->force_move(view_.player_side, *move)) {
      const FighterState& me = match_->state(view_.player_side);
      last_error_ = *move == MoveId::Special && me.meter < kMeterCost
                        ? std::format("必杀槽不足（{:.0f}/{:.0f}）", me.meter, kMeterCost)
                        : std::string("当前不可出招（硬直或倒地）");
      return false;
    }
    last_error_.clear();
    player_intent_ = Intent{0.0f, false, std::nullopt};
    mark_dirty();
    return true;
  }
  if (action == "charge") {
    // 充满必杀槽（剧本化演示/截图用；正常打要靠命中与受击攒）。
    int side = view_.player_side;
    if (!arg.empty()) {
      try {
        side = std::stoi(arg);
      } catch (...) {
        return false;
      }
    }
    match_->charge_meter(std::clamp(side, 0, 1));
    mark_dirty();
    return true;
  }
  if (action == "opponent_attack") {
    const auto move = move_from_name(arg);
    if (!move.has_value()) return false;
    const bool ok = match_->force_move(1 - view_.player_side, *move);
    if (ok) mark_dirty();
    return ok;
  }
  if (action == "guard") {
    player_intent_.guard = true;
    mark_dirty();
    return true;
  }
  if (action == "damage") {
    // `damage <side>`：给指定侧扣一大块血（e2e 造"残血/击倒"场景用）。
    const int side = arg == "1" ? 1 : 0;
    return match_->apply_damage(side, match_->state(side).hp);
  }
  if (action == "focus") {
    set_focused(true);
    return true;
  }
  return false;
}

}  // namespace gedou
