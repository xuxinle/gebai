#pragma once

/// **慢内容异步排版**（2026-10-19 性能轮）：让重活组件的排版不再阻塞主线程帧循环。
///
/// ## 为什么需要它
///
/// 帧循环是单线程同步管线：`tick() → render_frame() → layout() → paint()`。
/// 任何一个组件在 `measure`/`arrange` 里花 100ms，整个窗口（菜单、标签、
/// 滚动、光标）就冻结 100ms——用户报「打开大 Markdown 整窗变卡」的根因
/// 不是绘制（绘制已有二分视口剔除），而是**排版在主线程同步发生**。
///
/// 本组件把「排版」这一步移出主线程：
///
/// ```
/// 声明 <SlowContent> 内容组件 + 排版函数
///   ├─ measure/arrange：先用**估算高度**占位（布局不被阻塞）
///   ├─ 排版投给后台线程（Tasks::run_async，协作取消）
///   ├─ 排版期间 paint：画浅色骨架（行占位条），可选"排版中"角标
///   └─ 完成 → post_ui 回主线程 → adopt 结果 → mark_dirty（坐标连续，无跳动）
/// ```
///
/// ## 线程契约（与 `st::app::async.hpp` 同源）
///
/// - 后台只做**纯数据排版**：输入快照（源文本 + 度量参数 + 宽度），输出行模型；
///   **不碰** `Element`、不写 `State`、不调任何 UI 接口；
/// - 文本度量经 `TextPort`：宽度/整形缓存内部有锁（并发压测见
///   `tools/font_thread_safety_probe.cpp`：3 线程 measure + 主线程 draw，零畸变）；
/// - 结果**只能**经 `post_ui` 回主线程落地（与业务线程同一契约）。
///
/// ## 降级路径（每一级都保证功能不缺）
///
/// - 内容小于阈值（`kSyncBelowBytes`，默认 32KB）：同步排版——异步的调度开销
///   不值得，绝大多数文档走这条快路；
/// - `Tasks` 不可用（组件独立使用、未接 `Application`）：同步排版；
/// - 流式追加（`append_chunk`）期间：退同步（流式片小，排版便宜；并发交叠的
///   正确性成本远大于收益）。
///
/// ## 观感约束
///
/// 占位高度与真实高度的差通过 `height_hint` 校准（宿主可给更准的估算函数）；
/// 切换瞬间布局自然更新（`ScrollView` 按新内容高重算滚动范围），**不做**坐标动画——
/// 骨架与内容同一坐标系，视觉上是"渐进填充"而不是"跳变"。

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <string>
#include <vector>

#include "st/app/async.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class TextPort;  // 排版快照携带（定义见 st/ui/text_port.hpp）

/// 慢内容壳：把「排版慢」的内容组件包起来，排版移出主线程。
///
/// 用法（宿主侧）：
/// ```cpp
/// // 内容组件自己实现 SlowLayout 接口（见下）；壳负责调度与占位。
/// auto* shell = column.add<SlowContent>(std::make_unique<MarkdownView>(), layout_fn);
/// shell->set_source(text);   // 触发异步排版
/// ```
///
/// 组件与壳的分工：
/// - **内容组件**（如 `MarkdownView`）实现 `SlowLayoutTarget`：能快照输入、
///   能在**无 UI 树**的环境下排版、能采纳排版结果；
/// - **壳**（本类）：调度（同步/异步分档）、占位绘制、取消与去抖。
class SlowContent;

/// 排版快照：内容组件自定义解释的**自包含**输入/输出载体。
///
/// 设计取舍：不用模板（`SlowContent` 是运行时组件，泛型会传染 DSL/工厂）；
/// 负载走 `shared_ptr<void>`（内容组件自己知道里面是什么——同源组件配对使用，
/// 类型错配是编程错误，采纳侧 `static_pointer_cast` 前有宽/字号核对兜底）。
struct SlowLayoutSnapshot {
  // —— 输入侧（主线程拍）——
  /// 排版用的文本端口（`Application` 的 `RendererTextPort`，进程级存活；
  /// 度量缓存带锁，跨线程安全——见 tools/font_thread_safety_probe.cpp）。
  /// 为空时后台退 `NullTextPort`（布局退化但不崩，同「无字体亦可布局」的兑底）。
  const TextPort* port{nullptr};
  std::string source{};
  float width{0.0f};
  float base_size{14.0f};
  float code_size{13.0f};
  float table_size{12.0f};
  float max_width{0.0f};
  std::array<float, 6> heading_sizes{};
  // —— 输出侧（后台填）——
  float content_height{0.0f};
  std::size_t row_count{0};
  std::shared_ptr<void> payload{};   ///< 行模型（内容组件私有类型）
};

/// 慢内容组件的排版接口（内容组件实现）。
///
/// 设计为**纯数据进、纯数据出**：`layout_offscreen` 不持有也不触碰任何元素
/// ——后台线程调用它，线程安全由实现保证（文本度量走 `TextPort` 的带锁缓存，
/// 其余状态全部是快照副本）。
class SlowLayoutTarget {
 public:
  virtual ~SlowLayoutTarget() = default;

  /// 后台排版（纯函数语义：对同一输入产出同一结果；不修改组件状态）。
  /// 输入快照的**输入侧**字段由壳填好；返回时**输出侧**字段（content_height/
  /// row_count/payload）已填。在后台线程调用。
  ///
  /// `token` 是**协作取消信号**：输入已变（用户又改了文件/拖了窗）时，壳会
  /// `request_stop`——实现应在耗时循环里周期性查 `token.stop_requested()` 并早退，
  /// 返回的 `row_count == 0` 即表示「已取消」。不查也不会错（世代号会丢弃结果），
  /// 但那样旧排版会白烧一大段 CPU，正是要避免的。
  [[nodiscard]] virtual auto layout_offscreen(const SlowLayoutSnapshot& input,
                                              const std::stop_token& token) const
      -> SlowLayoutSnapshot = 0;

  /// 采纳排版结果（主线程调用）：把行模型换入组件、标脏。
  /// 返回 false = 结果已过期（宽度/字号又变了），调用方应丢弃并重排。
  virtual auto adopt_layout(const SlowLayoutSnapshot& input) -> bool = 0;

  /// 高度估算（占位用）：源长度 → 预计内容高。越准占位跳动越小；
  /// 默认按「字节 × 每字节行高 / 排版宽」的粗经验式。
  [[nodiscard]] virtual auto estimate_height(const SlowLayoutSnapshot& input) const -> float;

  /// 参数刷新（主线程，每帧 measure 时被壳调用）：内容组件把**当前真值**
  ///（主题落定后的字号/端口）写回 `params`。壳发现与在途快照不一致时
  /// 自动重投——否则首帧「构造默认字号」拍出的快照会在主题 apply 后被
  /// adopt 核对**永久拒绝**（实测：ui-font-scale 下 base 14→15，排版白做）。
  virtual void refresh_params(SlowLayoutSnapshot& params) const { (void)params; }

  /// 内容组件报回的**待排宽**（内容宽；<=0 = 无待排）。
  ///
  /// 为何要单独一个口：宽的真值来源是**控件实得矩形**（`arrange` 后的真宽），
  /// 而快照里的宽是**上次投递时用的**。两者不等 = 「控件宽变了」——壳据此重投，
  /// 而不是让组件在主线程同步重排一次（那次重排实测 **548ms**，是本轮卡顿的直接原因）。
  [[nodiscard]] virtual auto wanted_width() const -> float { return 0.0f; }
};

/// 慢内容壳组件（容器角色：占位绘制 + 调度；内容组件作为**唯一子元素**挂入）。
class SlowContent final : public Element {
 public:
  /// `layout` 是排版调度器：默认用注入的 `Tasks`（可为 null = 恒同步）。
  explicit SlowContent(std::unique_ptr<Element> content,
                       std::shared_ptr<SlowLayoutTarget> target,
                       app::Tasks* tasks = nullptr);
  /// DSL 注册表要求的无参构造：内容与目标后置 `setup`（未 setup 前是空壳，不排版）。
  SlowContent() = default;
  ~SlowContent() override;
  /// 无参构造后的装配（只允许一次；重复调用是程序错误，忽略）。
  void setup(std::unique_ptr<Element> content, std::shared_ptr<SlowLayoutTarget> target,
             app::Tasks* tasks);

  [[nodiscard]] auto type() const noexcept -> std::string_view override {
    return "SlowContent";
  }

  /// 触发一次内容排版（去抖：同帧多次调用合并为最后一次的输入）。
  /// 字号等排版参数由宿主先写进 `layout_params()`（快照的输入侧字段）。
  void set_source(std::string source);

  /// 最近一次 `set_source` 的源文本（宿主做「内容真变了才写」的比对）。
  [[nodiscard]] auto last_source() const -> const std::string& { return last_source_; }
  /// 配对 `set_source` 的记忆（壳不主动比——同一文本重排是浪费，但比对语义归宿主）。
  void remember_source(std::string source) { last_source_ = std::move(source); }

  /// 排版参数面（宿主在 set_source 前填）：直接暴露输入快照的可变引用，
  /// 避免「每加一个排版参数就加一个 setter」的接口臂 situated（内容组件自定义解释）。
  [[nodiscard]] auto layout_params() -> SlowLayoutSnapshot& { return params_; }

  /// 占位骨架样式开关（默认开）。
  void set_skeleton(bool enabled) { skeleton_ = enabled; }
  /// 同步阈值（字节）：源小于它就直接在主线程排版。默认 32KB。
  void set_sync_threshold(std::size_t bytes) { sync_threshold_ = bytes; }

  // —— 状态观测（测试/控制通道）——
  [[nodiscard]] auto layout_pending() const noexcept -> bool { return pending_.load(); }
  [[nodiscard]] auto last_layout_ms() const noexcept -> double { return last_layout_ms_; }
  [[nodiscard]] auto content_element() const noexcept -> Element* { return content_.get(); }

  // —— 语义 / 控制通道 ——
  [[nodiscard]] auto semantics_value() const -> std::string override;
  [[nodiscard]] auto get_property(std::string_view name) const
      -> std::optional<std::string> override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;

  // —— 元素接口 ——
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;

 private:
  /// 后台任务的结果包装（generation 随行：主线程核对世代后才采纳）。
  struct OffscreenOutcome {
    std::uint64_t generation{};
    double elapsed_ms{0.0};
    SlowLayoutSnapshot result{};
  };

  /// 发起排版（按分档决定同步/异步）。
  void schedule_layout();
  /// 后台线程体：跑 `layout_offscreen`，结果经 `delivered_` 交回。
  /// **单一后台线程**（新任务就地取消旧的）——多线程并发排版会与主线程
  /// 争 `TextPort` 的全局缓存锁，实测把主线程拖慢 2.19×（`tools/lock_contention_probe.cpp`）。
  void run_offscreen(app::Tasks& tasks);
  /// 把本线程降到最低优先级（后台排版不与主线程抢 CPU）。
  static void lower_thread_priority();
  /// 主线程帧循环里取交付结果（空则立即返回）；就绪则采纳（世代号校验）。
  void pump_delivery();
  /// 后台排版完成后的主线程唤醒（详见实现处的说明）。
  /// 可能**在后台线程被调用**：内部只调线程安全的 `post_ui` 或标脏接口。
  void on_offscreen_ready();

  /// 主线程：采纳或丢弃结果（同步分档与异步交付共用）。
  void finish_layout(SlowLayoutSnapshot result, double elapsed_ms);

  std::unique_ptr<Element> content_{};
  std::shared_ptr<SlowLayoutTarget> target_{};
  app::Tasks* tasks_{nullptr};
  /// 唯一的后台排版线程（新任务就地取消旧的：保证同时最多一个）。
  std::jthread worker_{};
  /// 后台→主线程的结果交接（小锁、瞬时：后台只在跑完时写一次，主线程每帧读一次）。
  /// 比 future 轮询好在：不靠线程创建/销毁，也不会因重投而堆积。
  mutable std::mutex delivery_mutex_{};
  std::optional<OffscreenOutcome> delivered_{};
  mutable bool has_delivery_{false};

  /// 当前/在途排版的输入快照（主线程独占写；宿主经 layout_params() 预填参数）。
  /// 输入世代（每次 `set_source`/宽度变化 +1）：在途结果回来时对不上就丢弃。
  std::uint64_t generation_{0};
  std::atomic<bool> pending_{false};
  SlowLayoutSnapshot params_{};
  /// 最近一次**已投递**的排版输入（`measure` 拿它与当前真值比对）。
  ///
  /// 为何要它与 `params_` 分开：参数刷新（主题 apply 后的字号/端口）会每帧
  /// 从内容组件读回真值，而「真值 vs 内存副本」的比较会让每次排版结果都被打新
  ///（实测：字号 14→15 一次变化，却让排版永远落不了地）。判据换成
  /// 「真值 vs **在途输入**」后，同一份输入只投一次。
  SlowLayoutSnapshot inflight_{};
  /// 占位期间的估算高度（measure 用它，不等排版）。
  float estimated_height_{0.0f};
  /// 上一次 `measure` 自身耗时（诊断：区分「壳的 measure 慢」与「别的元素慢」）。
  double last_measure_ms_{0.0};
  /// 上一次排版耗时（诊断）。
  double last_layout_ms_{0.0};

  std::string last_source_{};

  bool skeleton_{true};
  std::size_t sync_threshold_{32U * 1024U};
  /// 排版中提示的起始时刻（画"排版中"角标的淡入计时）。
  std::chrono::steady_clock::time_point pending_since_{};
};

}  // namespace st::ui
