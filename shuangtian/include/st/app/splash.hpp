#pragma once

/// 启动卡：首帧画完之前，用户看到的那张「磨砂玻璃 + 品牌 + 进度」的画面。
///
/// ## 它解决的是什么问题
///
/// 实测（`docs/STARTUP_UX_STUDY_20261108.md`）：窗口在 ~60ms 就可见，而内容要到 ~452ms
/// 才出现——中间那 **~395ms 是一块完全静止的主题底色**（采样确认：一种颜色、占比 100%、
/// 期间零像素变化）。那才是"启动慢"的全部体感来源。
///
/// 本模块把那段静止换成一张**会动的卡**：磨砂底 + 品牌标记 + 进度指示。
/// 动效由同一张画布逐帧重绘，**不需要 UI 树、不需要字体栈**——这是硬约束：
/// 启动卡必须在字体栈装配完成之前就能画（否则它自己就成了新的启动成本）。
///
/// ## 三层合成（从后到前）
///
/// | 层 | 画什么 | 为什么这么画 |
/// |---|---|---|
/// | ① 磨砂底 | **什么都不画**（alpha=0） | 交给合成器（Win32: DWM Acrylic）。实测：画布 alpha=0 的像素会原样透出背景的模糊像；自绘模糊只能糊自己画布里的像素，对"透出窗口背后"无能为力 |
/// | ② 品牌卡 | 圆角卡片（半透明 + 细描边 + 柔光晕） | 由 `--splash-opacity` 决定透明度：有磨砂时 0.06~0.18 就很高级，无磨砂时退化为近不透明 |
/// | ③ 动效 | 两个交错脉冲环 + 中心点 + 底部扫光 | 见下「为什么是这几个」 |
///
/// ## 为什么是这几个动效（而不是 spinner / 进度百分比）
///
/// - **无百分比**：启动耗时不可预测（首帧 267~402ms 随 DPI 变），假进度条是"假装知道"；
/// - **脉冲环**：呼吸节律天然表达"在等，但系统活着"，且**越靠近首帧越亮**（`progress`
///   由外部按已耗时/预期耗时推进）——用户看到的是"要来了"，而不是"卡住了"；
/// - **扫光**：单向匀速，给画面一个确定的运动方向（人眼对"有方向的运动"宽容度最高）；
/// - 全部是**解析式渐进**（`ease_out` + 正弦），没有状态机，掉帧也只表现为变慢。
///
/// ## 与「观感类改动纪律」的关系
///
/// 本文件的参数（脉冲周期 / 环半径 / 透明度档 / 扫光时长）都是 `CONVENTIONS.md` §9.2
/// 意义上的**观感令牌**：改默认值属于产品决策，不该在实现里悄悄调。
/// 所以它们集中在 `Options` 里，且默认值有明确取向（见各字段注释）。

#include <cstdint>
#include <span>
#include <string>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/shell/shell.hpp"

namespace st::app {

/// 启动卡的样式参数（**逻辑像素**；内部按画布 DPI 换算）。
struct SplashStyle {
  /// 卡片宽度（逻辑像素）。高度按 `card_aspect` 算，并会被窗口高夹取。
  float card_width{320.0f};
  float card_aspect{1.06f};        ///< 宽/高
  float card_radius{26.0f};
  /// 卡片不透明度（0~1）。**这是"有没有磨砂"的直接体现**：
  /// 有磨砂时只是很淡的一层**白**（透出背景的模糊质感，像一块真玻璃）；
  /// 无磨砂时高得多（否则透出的是桌面，会脏）。
  ///
  /// ⚠ 0.17 是实测调出来的：再低（0.10）卡在暗色磨砂背景上几乎看不见；
  /// 再高（0.30）会盖掉磨砂纹理（变成一块灰板）。
  float card_opacity{0.17f};
  /// 卡片外的柔光晕半径（逻辑像素；0 = 不画）。它让卡片"浮起来"而不是"贴上去"。
  float glow_radius{68.0f};
  float glow_opacity{0.10f};
  /// 品牌标记（菱形）半径与环半径（逻辑像素）。
  float mark_radius{34.0f};
  float ring_radius{56.0f};
  /// 一个脉冲周期的时长（秒）。**0.9~1.4s** 是"活着但不急"的区间；
  /// 低于 0.6s 会显得焦虑，高于 2.5s 会被读成卡死。
  float pulse_seconds{1.15f};
  /// 扫光一次往返的时长（秒）。
  float sweep_seconds{2.2f};
  /// 最大动画帧率。启动卡本身很轻（几块渐变），但没必要烧 CPU：
  /// 它只是"让画面不像死掉"，不是 60fps 的动画电影。
  float max_fps{45.0f};
};

/// 启动卡的运行状态（纯数据，可被测试直接构造）。
struct SplashState {
  /// 已进行的时间（秒），由调用方按单调钟推进。
  double elapsed_seconds{0.0};
  /// 进度提示（0~1，**只影响亮度/环半径，不是进度条**）。可由外部按
  /// "已耗时 / 预期耗时" 推，也可恒为 0（此时纯呼吸，同样成立）。
  float progress{0.0f};
  /// 底色（主题的 `bg`，无磨砂时也用它当卡外用色）。
  math::Color background{0xF7, 0xF7, 0xFA, 0xFF};
  /// 强调色（主题 `accent`），动效与扫光都用它。
  math::Color accent{0x38, 0x6B, 0xFF, 0xFF};
  /// 是否有合成器磨砂（决定卡片透明度用 `style.card_opacity` 还是退化档）。
  bool backdrop_blur{false};
  /// **应用自己的品牌 logo**（PNG 字节）。非空就画它，空着才画霜天雪花。
  ///
  /// 为何是 PNG 字节而不是路径：与 `Backend::set_window_icon` 同一形式——
  /// 应用是单文件交付的，旁边不一定有资源目录，而"编译期嵌入"后的形态就是一段内存字节。
  /// 实际上调用方直接传 `AppOptions::window_icon_png`：那是**任务栏上那个图标**，
  /// 用同一个文件才不会出现"启动卡一个标、任务栏另一个标"。
  std::span<const std::uint8_t> logo{};
};

/// 把一张启动卡画进 `canvas`（**整块覆盖**：先清成全透明/底色，再画卡片）。
///
/// `canvas` 必须是**与窗口等大**的画布（物理像素与客户区一致），因为结果会被整块
/// 推到窗口上。逻辑坐标由 `canvas.device_scale()` 换算。
///
/// 关于「有磨砂 vs 无磨砂」的两种画法（都实测过）：
/// - 有磨砂：卡外**全透明**（alpha=0），让合成器透出背景；卡内半透明。
/// - 无磨砂：卡外填 `state.background`（与主题底色连续），卡内更实。
///   否则会透出桌面壁纸——那不是"高级"，那是"漏"。
void paint_splash_card(raster::Canvas& canvas, const SplashState& state, const SplashStyle& style = {});

/// 无磨砂时建议的卡片不透明度（把"退化为近不透明"这条口径收在一处，测试与实现同源）。
[[nodiscard]] auto splash_opacity_without_backdrop() noexcept -> float;

/// 启动动画的**帧泵**。
///
/// ## 为何需要它（用户要求“动画没了”）
///
/// 上一版把启动卡退化成**一帧静态图**，理由是“动画会净增启动耗时”——
/// 而那个结论混淆了两件事：
///
/// | 写法 | 实际发生 | 总启动 |
/// |---|---|---|
/// | 跑完动画再画首帧 | 动画 341ms **串行在前** | +341ms |
/// | **首帧计算期间穿插动画帧**（本类） | 动画帧只占本来静止的“首帧计算期” | **≈0** |
///
/// 关键事实：首帧的 ~330ms 是**一次性建表**（字体首次光栅化 + 图元缓存建表），
/// 它不要求屏幕静止；用户在它期间看到的本就是一成不变的一帧。
/// 所以正确的做法不是“先跑动画还是先算首帧”的取舍，而是**让两者交错**：
/// 算一会儿 → 泵一帧动画 → 再算一会儿。
///
/// ## 用法
///
/// ```cpp
/// SplashPump pump(*backend, state, style);
/// pump.tick();        // 首帧之前：先把卡画出来
/// /* … 首帧里各个耗时段之间 … */ pump.tick();
////* … 首帧画完、窗口显示之前 … */ pump.tick();
/// ```
///
/// 帧间隔由 `style.max_fps` 定，来不及的 tick 直接返回（不会拖慢调用方）。
class SplashPump {
 public:
  /// 构造参数全是“启动卡自足的输入”：不收 `AppOptions`
  /// （`splash.hpp` 不依赖应用层头文件，否则应用层与绘制层会绑死）。
  SplashPump(shell::Backend& backend, SplashState base, SplashStyle style);
  /// 泵一帧（未到帧间隔则立即返回）。
  void tick();

  /// 泵一帧，**忽略帧间隔**。
  ///
  /// 用于“只有几次机会”的场合（首帧内部的泵点）：那里用 `tick()` 会被
  /// `max_fps` 门限筛掉——实测首帧期间画面 diff 恒为 0（动画定住）。
  void tick_forced();

  /// 真正画过的帧数（日志与测试用：0 说明一帧都没画）。
  [[nodiscard]] auto frames() const noexcept -> int { return frames_; }

 private:
  /// 真正画一帧（两个 `tick*` 的共同出口）。
  void paint_now(double since_ms);

  shell::Backend* backend_{nullptr};
  SplashState state_{};
  SplashStyle style_{};
  std::int64_t started_ns_{0};
  double last_frame_ms_{-1.0};
  int frames_{0};
};

}  // namespace st::app
