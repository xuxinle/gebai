#include "st/ui/components/slow_content.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "st/raster/paint.hpp"
#include "st/ui/theme.hpp"

#if defined(__linux__)
#include <unistd.h>   // nice(2)：把后台排版线程降到最低优先级
#endif

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
  inflight_ = params_;   // 记下**本次投递的输入**：`measure` 拿它与真值比对，避免重复重投
  const std::size_t bytes = params_.source.size();
  if (bytes < sync_threshold_ || tasks_ == nullptr) {
    // 同步分档：小内容不值得异步（调度 + future 轮询的往返 > 排版本身）。
    const auto start = std::chrono::steady_clock::now();
    static const std::stop_token never_cancel{};   // 同步分档不可取消（本来就在主线程）
    auto result = target_->layout_offscreen(params_, never_cancel);
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
  // —— 单一后台排版线程 + 协作取消（2026-10-19 二次修）——
  //
  // 旧实现每次 schedule 都 `run_async`（= 新起一个 jthread）。重投频繁时
  // （gbcode 切预览会连发 4 次：首拍 + 字号族变化 + port 变化 + 宽度变化）
  // 就有多个排版线程**同时**在跑全文排版——它们反复争抢 `TextRenderer` 的
  // 全局宽度缓存锁，而**主线程绘制也走同一把锁**：
  // 实测（`tools/lock_contention_probe.cpp`）主线程相等工作量放慢 **2.19×**。
  // 这就是「排版已移出主线程、版面仍卡」的根因——线程搬走了，**锁没搬**。
  //
  // 现在两条约束同时上：
  // ① **同一时刻只允许一个排版线程**（旧的就地取消，不等它跑完）；
  // ② 排版线程降到最低优先级（POSIX `nice(19)`）——后台重活不该与主线程抢 CPU。
  (void)tasks;  // 排版不用共享池：池被别的短任务抢占会把延迟做成噪声
  if (worker_.joinable()) {
    worker_.request_stop();   // 协作取消：排版在自己帧边界检查
    worker_.join();           // 等它退（百毫秒级；旧任务此时已能立即退出）
  }
  const SlowLayoutSnapshot snapshot = params_;
  const std::uint64_t generation = generation_;
  const std::shared_ptr<SlowLayoutTarget> target = target_;
  worker_ = std::jthread([this, snapshot, generation, target](std::stop_token token) {
    lower_thread_priority();
    const auto start = std::chrono::steady_clock::now();
    auto result = target->layout_offscreen(snapshot, token);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    // 结果只入队（不加锁就写回成员）：主线程 `pump_delivery` 取。
    {
      const std::scoped_lock lock(delivery_mutex_);
      delivered_ = OffscreenOutcome{generation, ms, std::move(result)};
      has_delivery_ = true;
    }
    pending_.store(false, std::memory_order_release);
    // ⚠ **必须**叫醒主线程：pending 已 false，paint_content 不会再请求下一帧，
    // 没这一下交付就永远取不走（实测卡在骨架）。
    this->on_offscreen_ready();
  });
}

void SlowContent::lower_thread_priority() {
#if defined(__linux__)
  // POSIX：降到最低优先级（nice 19 = "能让就让"）。主线程绘制优先，
  // 后台排版只抢占空余 CPU——这是「后台重活不拖主窗」在调度器层面的保障。
  // 返回值是新的 nice 值（失败为 -1）：这里只表达"尽力降级"，不因失败改变行为。
  if (::nice(19) == -1) {
    // 无权限降级（极少见）：照常排版，不做补偿——优先级只是优化不是正确性前提。
  }
#endif
  // 其它平台不处理：Windows 需 SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)，
  // 留作后续（本机验证在 Linux；跨平台语义已在文档里写明是"尽力而为"）。
}

void SlowContent::pump_delivery() {
  OffscreenOutcome outcome{};
  bool ready = false;
  {
    const std::scoped_lock lock(delivery_mutex_);
    if (!has_delivery_) return;
    has_delivery_ = false;
    outcome = std::move(*delivered_);
    delivered_.reset();
    ready = true;
  }
  if (!ready) return;
  if (outcome.generation != generation_) return;  // 世代不符：输入已变，丢弃
  // 不加锁调 finish_layout：它要 mark_dirty/mark_layout_dirty（主线程的事），
  // 把锁范围局限在「取结果」这一步就够。
  finish_layout(std::move(outcome.result), outcome.elapsed_ms);
}

void SlowContent::on_offscreen_ready() {
  // 后台线程**完成时**发起的唤醒（2026-10-19 二次修）。
  //
  // 为何必须有它：`pending_` 在后台线程里置 false，而「请求下一帧」写在
  // `paint_content` 的 pending 分支里——于是最后一帧画完就再没帧，
  // `pump_delivery` 等不到下一次时机。实测日志：`worker done gen=4 rows=3471`
  // 之后十几次采样里**一次 pump 也没有**，界面永远停在骨架（row_count=0）。
  //
  // 做法：让主线程“欠一帧”——`tasks_` 在时走 `post_ui`（框架约定的
  // 跨线程回主线程通道，下一帧 tick 内执行）；否则退而标脏 + 请求动画。
  auto wake = [this]() {
    mark_dirty();
    mark_layout_dirty();
    request_animation();   // 保证本帧之后还有下一帧去跑 pump_delivery
  };
  if (tasks_ != nullptr) {
    tasks_->post_ui(wake);
    return;
  }
  wake();
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
  if (name == "measure_ms") return std::format("{:.2f}", last_measure_ms_);
  if (name == "source_bytes") return std::to_string(params_.source.size());
  if (name == "generation") return std::to_string(generation_);
  return std::nullopt;
}

auto SlowContent::property_names() const -> std::vector<std::string_view> {
  return {"layout_pending", "layout_ms", "measure_ms", "source_bytes", "generation"};
}

void SlowContent::measure(const RenderContext& context, const Constraints& constraints) {
  const auto measure_start = std::chrono::steady_clock::now();
  // 参数刷新（主题/端口落定后真值回填）：与快照不一致 = 重投（旧结果按世代丢弃）。
  //
  // 为何不能每帧无条件重投（实测坑）：首帧拍快照时主题还没 apply（字号是构造默认
  // 14），apply_theme 后真值是 15——于是**每一次**排版结果都会在下一帧被
  // “字号变了”打新（世代号 +1 → 旧结果丢弃）。形成
  // 「排完→丢弃→重投→排完→丢弃」的死循环：`layout_pending` 永远 false、
  // `layout_ms` 永远 0、主线程看起来“不卡”但**内容永远不出来**（row_count=0）。
  //
  // 修法：**只在参数真的需要时重投**——判据是「与在途排版的输入不一致」，
  // 而不是「与内存里的 params 副本不一致」。在途输入记在 `inflight_`（投递时快照），
  // 重投后它跟着更新；同一份输入不会被重复打新。
  if (target_ != nullptr && !params_.source.empty()) {
    SlowLayoutSnapshot fresh = params_;
    target_->refresh_params(fresh);
    const bool differs_from_inflight = fresh.port != inflight_.port ||
                                       fresh.base_size != inflight_.base_size ||
                                       fresh.code_size != inflight_.code_size ||
                                       fresh.table_size != inflight_.table_size ||
                                       fresh.heading_sizes != inflight_.heading_sizes ||
                                       fresh.max_width != inflight_.max_width;
    if (differs_from_inflight) {
      const std::string source = std::move(params_.source);
      params_ = std::move(fresh);
      params_.source = std::move(source);
      schedule_layout();   // 世代 +1：在途旧结果回来会被丢弃
    } else {
      // 与在途一致：把本帧刷新到的真值写回（供诊/后续比对），**不**打新。
      params_.port = fresh.port;
      params_.base_size = fresh.base_size;
      params_.code_size = fresh.code_size;
      params_.table_size = fresh.table_size;
      params_.heading_sizes = fresh.heading_sizes;
      params_.max_width = fresh.max_width;
    }
  }
  // 占位语义：排版未就绪时，量高用估算值——布局不等慢路径。
  // 内容元素照常 measure（它自己的 rows 还是空的时也会给出占位行高）。
  pump_delivery();
  for (const auto& child : children_) child->measure(context, constraints);
  // 宽度变化的**唯一判定点在 arrange**（那里拿到的是控件实得宽）。
  //
  // 为何不在 measure 里看 `wanted_width`：measure 拿到的是布局阶段给的**约束**
  // （列容器在 shrink-to-fit 语义下会传 kUnbounded，MarkdownView 兑底到 640−padding），
  // 与 arrange 的真宽不同——两个口径都拿去比对就会**来回重投成死循环**
  //（实测：待排宽恒在 608↔956 之间跳，排版永远不落定、row_count 一直是 0）。
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
  // 排版宽度入快照（内容坐标）：首次没拍到时用本帧约束推。
  if (params_.width <= 0.0f && width > 0.0f) {
    params_.width = std::max(width - style_.padding.horizontal(), 48.0f);
  }
  last_measure_ms_ = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - measure_start).count();
}

void SlowContent::arrange(const RenderContext& context, math::Rect rect) {
  Element::arrange(context, rect);
  // 真实宽度落进快照：**宽度变化的唯一重投点**（arrange 拿到的是控件实得宽，
  // 这是布局阶段唯一可信的口径；measure 那边只有约束，不能当判据）。
  // 加一个宽容阈值（0.5px）：亚像素抖动不该触发重投（否则拖窗时每帧重排）。
  const float content_width = std::max(rect.width - style_.padding.horizontal(), 48.0f);
  if (params_.width <= 0.0f) {
    params_.width = content_width;   // 首次定宽：直接采用，不必重投（排版还没发过）
  } else if (std::abs(content_width - params_.width) > 0.5f) {
    params_.width = content_width;
    schedule_layout();  // 宽真的变了（拖窗/分栏/侧栏）：重投后台
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
