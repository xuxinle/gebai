#include "st/ui/components/slow_content.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <future>
#include <utility>

#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

// —— SlowLayoutTarget 默认实现 ——

auto SlowLayoutTarget::estimate_height(const SlowLayoutSnapshot& input) const -> float {
  // 粗经验式：行高 ≈ base_size × 1.6；每行平均字符数 ≈ width / (base_size × 0.55)
  //（中英混排的平均字符宽约为字号的 0.55 倍）。宁可略高不高——占位偏高的
  // 跳动方向是"收紧"，比"撑开再缩回"的观感好。
  if (input.width <= 0.0f || input.base_size <= 0.0f) return 256.0f;
  const float chars_per_line = std::max(input.width / (input.base_size * 0.55f), 8.0f);
  // 源字节数近似字符数（UTF-8 中文 3 字节/字会低估行数 → 高估高度，方向安全）。
  const float lines = std::max(static_cast<float>(input.source.size()) / chars_per_line, 1.0f);
  return lines * input.base_size * 1.6f + 24.0f;  // +首尾留白
}

// —— SlowContent ——

SlowContent::SlowContent(std::unique_ptr<Element> content,
                         std::shared_ptr<SlowLayoutTarget> target, app::Tasks* tasks)
    : content_{std::move(content)}, target_{std::move(target)}, tasks_{tasks} {
  // 内容作为唯一子元素挂入：绘制/事件/命中全部沿用既有树遍历，
  // 本壳只在「排版未就绪」时盖一层骨架。
  if (content_ != nullptr) (void)add_child(std::move(content_));
  style_.background = math::Color{0, 0, 0, 0};
}

SlowContent::~SlowContent() = default;

void SlowContent::setup(std::unique_ptr<Element> content,
                        std::shared_ptr<SlowLayoutTarget> target, app::Tasks* tasks) {
  if (content_ != nullptr || target_ != nullptr) return;  // 已装配：幂等忽略
  content_ = std::move(content);
  target_ = std::move(target);
  tasks_ = tasks;
  if (content_ != nullptr) (void)add_child(std::move(content_));
}

void SlowContent::set_source(std::string source) {
  params_.source = std::move(source);
  schedule_layout();
}

void SlowContent::schedule_layout() {
  if (target_ == nullptr) return;
  ++generation_;
  const std::size_t bytes = params_.source.size();
  if (bytes < sync_threshold_ || tasks_ == nullptr) {
    // 同步分档：小内容不值得异步（调度 + future 轮询的往返 > 排版本身）。
    const auto start = std::chrono::steady_clock::now();
    auto result = target_->layout_offscreen(params_);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    finish_layout(std::move(result), ms);
    return;
  }
  pending_.store(true);
  pending_since_ = std::chrono::steady_clock::now();
  run_offscreen(*tasks_);
  // ⚠ pending 期间必须**请求连续帧**：帧循环只在 dirty/animation 时调 measure/paint
  //（`Application::tick` 的门），而 pump_delivery 挂在 measure/paint 里——不请求的话
  // 标一次脏只换到一帧，任务没跑完就再无帧，交付永远等不到（实测：pending 恒真）。
  // 「等待中的骨架」本身就是动画语义（collect_animation_requests 汇总驱动 followup 帧），
  // 采纳完成（finish_layout 里 mark_dirty）后自然停。
  mark_dirty();
  request_animation();
}

void SlowContent::run_offscreen(app::Tasks& tasks) {
  // 输入快照 + 世代号 + 目标引用全部拷进任务体（自包含：`target_` 是 shared_ptr，
  // 即使壳先析构，任务手里的目标仍活到跑完——不会悬垂）。
  // 结果经 future 交回：主线程帧循环里 `pump_delivery()` 非阻塞探测，就绪才采纳
  //（不走 post_ui：那需要后台线程回调进 Tasks 对象，而 future 轮询天然帧节拍）。
  const SlowLayoutSnapshot snapshot = params_;
  const std::uint64_t generation = generation_;
  const std::shared_ptr<SlowLayoutTarget> target = target_;
  auto [future, handle] = tasks.run_async(
      [snapshot, generation, target](std::stop_token token) -> OffscreenOutcome {
        (void)token;  // 排版不设检查点：单次排版百毫秒级，跑完再交回比抢占中间态便宜
        const auto start = std::chrono::steady_clock::now();
        auto result = target->layout_offscreen(snapshot);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        return OffscreenOutcome{generation, ms, std::move(result)};
      });
  (void)handle;  // 不提供外部取消：等它跑完（世代号保证旧结果不落地）
  pending_deliver_ = std::move(future);
}

void SlowContent::pump_delivery() {
  if (!pending_deliver_.valid()) return;
  // 非阻塞探测：任务没跑完就等下一帧（帧循环里每帧 pump 一次）。
  using namespace std::chrono_literals;
  if (pending_deliver_.wait_for(0s) != std::future_status::ready) return;
  auto outcome = pending_deliver_.get();
  pending_.store(false);
  if (outcome.generation != generation_) return;  // 世代不符：输入已变，丢弃
  finish_layout(std::move(outcome.result), outcome.elapsed_ms);
}

void SlowContent::finish_layout(SlowLayoutSnapshot result, double elapsed_ms) {
  if (target_ == nullptr) return;
  last_layout_ms_ = elapsed_ms;
  const bool adopted = target_->adopt_layout(result);
  if (adopted) {
    // 内容高度已真实化：清占位估值，标脏让外层（ScrollView）重新量。
    estimated_height_ = 0.0f;
    mark_dirty();
    mark_layout_dirty();
  }
}

// —— 语义 / 控制通道 ——

auto SlowContent::semantics_value() const -> std::string {
  return std::format("pending={} last_layout={:.1f}ms bytes={}", pending_.load() ? 1 : 0,
                     last_layout_ms_, params_.source.size());
}

auto SlowContent::get_property(std::string_view name) const -> std::optional<std::string> {
  if (name == "layout_pending") return pending_.load() ? "true" : "false";
  if (name == "layout_ms") return std::format("{:.2f}", last_layout_ms_);
  if (name == "source_bytes") return std::to_string(params_.source.size());
  if (name == "generation") return std::to_string(generation_);
  return std::nullopt;
}

auto SlowContent::property_names() const -> std::vector<std::string_view> {
  return {"layout_pending", "layout_ms", "source_bytes", "generation"};
}

void SlowContent::measure(const RenderContext& context, const Constraints& constraints) {
  // 参数刷新（主题/端口落定后真值回填）：与在途快照不一致 = 重投（旧结果按世代丢弃）。
  if (target_ != nullptr && !params_.source.empty()) {
    SlowLayoutSnapshot fresh = params_;
    target_->refresh_params(fresh);
    const bool params_changed = fresh.port != params_.port ||
                                fresh.base_size != params_.base_size ||
                                fresh.code_size != params_.code_size ||
                                fresh.table_size != params_.table_size ||
                                fresh.heading_sizes != params_.heading_sizes ||
                                fresh.max_width != params_.max_width;
    if (params_changed) {
      const std::string source = std::move(params_.source);
      params_ = std::move(fresh);
      params_.source = std::move(source);
      schedule_layout();   // 世代 +1：在途旧结果回来会被丢弃
    }
  }
  // 占位语义：排版未就绪时，量高用估算值——布局不等慢路径。
  // 内容元素照常 measure（它自己的 rows 还是空的时也会给出占位行高）。
  pump_delivery();
  for (const auto& child : children_) child->measure(context, constraints);
  if (pending_.load() && estimated_height_ <= 0.0f && target_ != nullptr) {
    estimated_height_ = target_->estimate_height(params_);
  }
  const math::Size content_size =
      children_.empty() ? math::Size{} : children_.front()->measured_size();
  float height =
      pending_.load() ? std::max(estimated_height_, content_size.height) : content_size.height;
  float width = constraints.max_width;
  if (width >= kUnbounded) width = constraints.available_width;
  if (width >= kUnbounded) width = 640.0f;
  if (style_.has_explicit_width()) width = style_.width;
  if (style_.has_explicit_height()) height = style_.height;
  measured_ = math::Size{width, std::max(height, 0.0f)};
  // 排版宽度入快照（内容坐标）：
  if (params_.width <= 0.0f && width > 0.0f) {
    params_.width = std::max(width - style_.padding.horizontal(), 48.0f);
  }
}

void SlowContent::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  // 真实宽度落进快照：宽度变化 = 重新排版（世代 +1 防旧结果落地）。
  const float content_width = std::max(rect.width - style_.padding.horizontal(), 48.0f);
  if (content_width != params_.width && params_.width > 0.0f) {
    params_.width = content_width;
    schedule_layout();  // 宽变了，重排（去抖靠世代号丢弃旧结果）
  }
  for (const auto& child : children_) child->arrange(context, rect);
}

void SlowContent::paint_content(const RenderContext& context, raster::Surface& canvas) const {
  // 帧驱动交付探测：绘制路径每帧必经，在这里看一眼在途任务是否就绪。
  //（`paint_content` 是 const——`pump_delivery` 只动 future/标量，与
  // `MarkdownView::last_visible_*` 同一手法。）
  const_cast<SlowContent*>(this)->pump_delivery();
  // 骨架盖层：只在排版未就绪时画（浅色行条，从上往下渐淡）。
  if (!pending_.load() || !skeleton_) return;
  // pending 期间持续请求下一帧（pump 挂在本函数里，没帧就没交付）。
  // `request_animation` 的语义是「还在动才请求」：等待中的骨架正是这种情况；
  // `collect_animation_requests` 每帧消费（clear），所以这里每帧重新置位。
  request_animation();
  const Palette& colors = context.theme.colors();
  const float alpha = 36.0f;
  float y = bounds_.y + 8.0f;
  const float bar_height = 12.0f;
  const float gap = 10.0f;
  const float min_width = bounds_.width * 0.3f;
  int bar = 0;
  while (y + bar_height <= bounds_.bottom() - 8.0f && bar < 40) {
    // 宽度伪随机变化（行长度错落）：固定序列，避免每帧闪跳。
    const float factor =
        0.35f + 0.6f * static_cast<float>((bar * 37U) % 100U) / 100.0f;
    const float bar_width = std::max(min_width, bounds_.width * factor - 32.0f);
    const math::Rect rect{bounds_.x + 16.0f, y, bar_width, bar_height};
    const std::uint8_t a =
        static_cast<std::uint8_t>(alpha * (1.0f - static_cast<float>(bar) / 48.0f));
    canvas.fill_rect(rect, raster::Paint::solid(colors.text_muted.with_alpha(a)), 4.0f);
    y += bar_height + gap;
    ++bar;
  }
}

}  // namespace st::ui
