#pragma once

/// @file frame.hpp
/// **帧剖析**：阶段细分 + 逐元素成本 + 逐帧序列，报告直接给出"该改哪里"。
///
/// ## 它回答的三个问题（其余都是证据）
///
/// | 问题 | 字段 |
/// |---|---|
/// | 这一帧慢在哪一段？（排版 / 绘制 / 送显 / 收集） | `phases()` |
/// | 哪一类组件、哪一个具体元素最贵？ | `elements()` |
/// | 抖动在哪帧？是偶发还是持续？ | `frames()`（逐帧序列） |
///
/// 第三项是前两项做不到的：3 帧的 p50 好看、第 4 帧 200 ms 的毛刺，
/// 只看分位永远发现不了（分位是"典型值"，而毛刺是"例外值"的极端）。
///
/// ## 关闭时零成本
///
/// 所有埋点都先问 `perf::enabled()`（一次 atomic 读 + 一个分支），关掉时不读时钟、
/// 不碰任何容器。这是硬要求：剖析代码常驻在帧路径上，不能因为"备着"而变慢。
///
/// ## 元素级为什么按"类型 + id"两份
///
/// 只知道"`Card` 类一共 6.1 ms"仍不知改哪一个；只知道"`#user-card-3` 贵"又说不出
/// 普遍性问题。所以两份都给：
/// - **按类型**（`Card` / `Text` / `Icon`）→ 判断"是不是某类组件的实现有系统性问题"；
/// - **按 id**（稳定的元素 id 或业务 key）→ 直接定位到人可以改的那一处数据/结构。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/perf/histogram.hpp"
#include "st/perf/report.hpp"

namespace st::perf {

/// 帧剖析器。
///
/// 生命周期：应用持有一个（`AppOptions::profile_frames > 0` 或环境变量开启时创建）。
/// `begin_frame()` / `end_frame()` 由 `Application::render_frame` 调用；
/// 元素埋点在 `Element::paint` 里（先取 `active_frame_profiler()`，为空则零成本）。
class FrameProfiler;

/// 当前生效的帧剖析器（无则 `nullptr`）。
///
/// 为何用"进程级当前剖析器"而不是把指针穿过几十层绘制签名：`Element::paint`
/// 是被 42 个组件重写的虚函数，给它们加参数要改全部覆写与所有调用点（那是
/// 一个跨模块的大改）。改成一个 `atomic` 指针后，埋点只是**一次 atomic 读 + 一个判空**，
/// 而剖析器的所有权仍属于应用（析构时置空，不存在悬垂）。
[[nodiscard]] auto active_frame_profiler() noexcept -> FrameProfiler*;

/// 设置/清空当前帧剖析器（`nullptr` = 关闭）。
///
/// 同一时刻只允许一个（多线程同时剖析同一界面没有意义，且两份数据无法归因）。
/// 重复设置会覆盖——报告里的人数只会有它自己的样本。
void set_active_frame_profiler(FrameProfiler* profiler) noexcept;

/// 一个帧阶段的统计（一帧一次，故用直方图而非累计值）。
struct FramePhase {
  std::string name{};
  Histogram ns{};   ///< 每次该阶段的耗时（纳秒）。**只含稳态帧**（冷启动帧另计）。
  std::uint64_t count{0};
  /// 冷启动帧的累计（单独留一份：“启动第一帧 200ms”是真问题，不能被排除掉）。
  Histogram cold_ns{};
};

/// 冷启动帧数：进程起来后的前两帧算冷启动（第一帧建字体/缓存，第二帧补第一次布局）。
///
/// 为何是常量而不是可配：它是**测量口径**，不是产品行为——不同值的两份报告
/// 不可比（而“不可比的两份数字”正是本报告要消灭的东西）。
inline constexpr std::uint64_t kColdFrameCount = 2;

/// 元素累计表的上限（真实界面通常几十到几百个；异常界面不撑爆内存）。
inline constexpr std::size_t kMaxElementEntries = 4096;

/// 一类元素的成本（**按其出现的那一帧**）；`ns` 是这一帧里该类元素自身耗时之和。
struct ElementClassCost {
  std::string type{};        ///< 组件类型名（`Card` / `Text` / …）
  std::uint64_t elements{0}; ///< 覆盖的元素数（去重后）
  Histogram ns{};            ///< 每次（每帧对该类的累计）耗时
};

/// 一个具体元素的成本。
struct ElementCost {
  std::string id{};       ///< 元素 id（可能带业务 key，如 `tasks/ListItem@task-42`）
  std::string type{};     ///< 类型名
  Histogram ns{};         ///< **自身**每帧耗时（不含子孙）——排序口径
  std::int64_t total_ns{0};  ///< 含子孙的每帧均值（解释“子元素加起来多少”）
};

/// 一帧的采样（逐帧序列的一项）。
struct FrameSample {
  std::uint64_t index{0};
  double total_ms{0.0};      ///< 帧内合计（layout+paint+present，与 metrics 同口径）
  double layout_ms{0.0};
  double paint_ms{0.0};
  double present_ms{0.0};
  double wall_ms{0.0};       ///< 与上一帧的墙钟间隔（**大而帧内小 = 被限帧**，不是慢）
  std::uint64_t elements{0}; ///< 本帧实际绘制元素数（视口剔除后）
  bool partial{false};       ///< 本帧是否只重绘了损坏区
  bool presentable{true};    ///< 本帧是否有可上屏的变化（空帧 = 否）
  /// 是否为**冷启动帧**（进程起来后的头两帧：字体首次光栅化、图元缓存建表、
  /// 着色器/设备初始化都发生在这时，实测 200ms 而稳态 3.8ms）。
  ///
  /// 它们保留在逐帧序列里（“启动慢”是真问题，不能被藏起来），但**不进分位统计**——
  /// 否则一次 200ms 的冷启动会把 p95 抬到 19ms，报告就会说“超出 60FPS 预算”，
  /// 而实际稳态是 3.8ms。实测踩过：这正是“帧内耗时超出预算”的假警报来源。
  bool cold{false};
};

/// 帧剖析器。
///
/// 生命周期：应用持有一个（`AppOptions::profile_frames > 0` 或环境变量开启时创建）。
/// `begin_frame()` / `end_frame()` 由 `Application::render_frame` 调用；
/// 元素埋点在 `UiRoot::paint_subtree` 里（按 `enabled()` 判）。
class FrameProfiler {
 public:
  /// 逐帧序列的容量上限（默认 600 帧 ≈ 释放 10 秒@60fps）。
  ///
  /// 为何有上限：剖析要能长时间跑（"我按着鼠标拖 10 分钟"），而逐帧序列是唯一的
  /// 线性增长项。超出后**环形覆盖**（丢最旧），并在报告里声明"只保留最后 N 帧"。
  explicit FrameProfiler(std::size_t capacity = 600);

  // —— 帧级 ——
  void begin_frame(double now_ms);
  /// `presentable` 为假表示这是空帧（未绘制、未上屏）——仍记一笔，
  /// 因为它正是"空转烧 CPU"的证据（见 `idle_churn` 判据）。
  void end_frame(std::uint64_t elements, bool partial, bool presentable);
  /// 记一段阶段（`layout`/`paint`/`present` 之外的自定义阶段也可）。
  void record_phase(std::string_view name, double ms);

  // —— 元素级（在绘制遍历里调用）——
  /// 记一次元素绘制耗时。`type` 与 `id` 都要给：报告同时按两者聚合。
  ///
  /// `self_ns` 是**不含子孙**的耗时（进 `elements()`/`hot_elements()` 排序的就是它）；
  /// `total_ns` 含子孙（进 `sections.elements` 的 `total`）。
  ///
  /// 为何要分两份：只记含子孙的耗时，排序结果永远是根容器——而根容器是**不能改的**
  /// （它的耗时全是子元素之和）。实测踩过：首版报告把 `app-root` 报成“每帧 55.93ms”，
  /// 读者拿这个数字只能得出“整个界面慢”这个已知事实。
  void record_element(std::string_view type, std::string_view id, std::int64_t self_ns,
                      std::int64_t total_ns);
  /// 本帧元素累计开始/结束（把单元素样本折算成"每类每帧"）。
  void begin_element_batch();
  void end_element_batch();

  // —— 读取 ——
  [[nodiscard]] auto frames() const -> const std::vector<FrameSample>& { return frames_; }
  [[nodiscard]] auto phases() const -> std::vector<FramePhase>;
  [[nodiscard]] auto elements() const -> std::vector<ElementClassCost>;
  [[nodiscard]] auto hot_elements(std::size_t limit = 20) const -> std::vector<ElementCost>;
  /// 已完成采样的帧数（含空帧）。
  [[nodiscard]] auto frame_count() const -> std::uint64_t { return completed_; }
  /// 当前帧序号（元素埋点用它判断“这是我这一帧的数吗”）。无剖析时恒为 0。
  [[nodiscard]] auto frame_serial() const noexcept -> std::uint64_t { return completed_; }
  /// 逐帧序列**是否被截断**（超出容量后丢过帧）。
  [[nodiscard]] auto truncated() const -> bool { return truncated_; }
  [[nodiscard]] auto capacity() const -> std::size_t { return capacity_; }

  void reset();

  /// 生成本轮采样的报告（含 `findings` 与 `caveats`）。
  ///
  /// `context` 由调用方提供（它知道后端/档位/尺寸）；报告会据此判断
  /// "这些数据能不能用来下结论"（例如 debug 档必须标出来）。
  [[nodiscard]] auto report(const MeasurementContext& context) const -> Report;

  /// 逐帧序列的 CSV（`frame,layout_ms,paint_ms,present_ms,total_ms,wall_ms,elements,partial,presentable`）。
  ///
  /// 为何给 CSV：逐帧曲线最适合直接被表格/脚本消费，也是"人工看一眼抖动"最快的形态。
  [[nodiscard]] auto frames_csv() const -> std::string;

 private:
  struct ElementKeyHash;
  struct Accum {
    std::string type{};
    std::string id{};
    std::int64_t self_ns{0};
    std::int64_t total_ns{0};
    std::uint64_t frames{0};
  };

  std::size_t capacity_{600};
  /// 逐帧样本环（**容量固定、预填零**）：`write_` 指向下一个写入槽。
  ///
  /// 为何预填：实时剖析在第一帧就要给出“当前这一帧”的读数；
  /// 若用 `push_back` 增长，`frames()` 返回的容器大小与“已完成帧数”不一致，
  /// 读端就会把未写入的槽位当成样本（实测：报告里 200 帧全是 0）。
  std::vector<FrameSample> frames_{};
  std::size_t write_{0};
  bool truncated_{false};
  std::uint64_t completed_{0};
  /// **稳态且真正绘制过东西**的帧数（元素级成本的分母）。
  ///
  /// 为何不用 `completed_`：空帧（无像素变化、一跳而过）与冷启动帧里根元素照样
  /// 记一笔，用全部帧做分母会把每帧成本系统性压低（实测：3.89ms 的 p50 被摊成 0.57ms）。
  std::uint64_t painted_frames_{0};
  /// 已写满一轮（决定读取顺序：未满时从 0 开始，满了之后从 `write_` 开始）。
  bool wrapped_{false};

  /// 阶段累计（跟帧的直方图）。
  std::vector<FramePhase> phases_{};
  /// 元素累计：按 `(type, id)` 一份表（报告侧再折成"按类型"与"按 id"两个视角）。
  std::vector<Accum> element_accum_{};

  bool in_frame_{false};
  /// 墙钟帧间隔（相邻两次 `begin_frame` 之差，与 `Metrics::wall_p50_ms` 同口径）。
  double last_begin_ms_{0.0};
  double frame_wall_ms_{0.0};  double frame_layout_ms_{0.0};
  double frame_paint_ms_{0.0};
  double frame_present_ms_{0.0};
  bool batch_open_{false};
  std::int64_t batch_ns_{0};
};

}  // namespace st::perf
