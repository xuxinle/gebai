#pragma once

/// 应用运行时：装配 后端 + 字体 + UI 树 + 控制通道 + 帧循环。
/// 无头模式与窗口模式共用同一条渲染路径（软件光栅器 → 帧缓冲 → 后端 present）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/control/control.hpp"
#include "st/ext/script.hpp"
#include "st/core/error.hpp"
#include "st/shell/shell.hpp"
#include "st/ui/ui_root.hpp"
#include "st/ui/window_control.hpp"

namespace st::app {

/// 解析文字抗锯齿形态（命令行 `--text-lcd` 取值；`auto` 时再看 `ST_TEXT_LCD`）。
///
/// 默认 `true`（亚像素）：**内置通道（无头）与桌面窗口同一默认**——两种运行形态
/// 的画字方式必须同源，截图与实际运行才可比。灰度基准用 `--text-lcd=off` 显式取得。
[[nodiscard]] auto resolve_text_lcd(std::string_view mode) -> bool;

/// 解析字形网格拟合模式（命令行 `--text-fit` 取值；`auto` 时再看 `ST_TEXT_FIT`）。
/// 默认 `Normal`，与 `text_lcd` 同一理由（内置通道与桌面同源）。
[[nodiscard]] auto resolve_text_fit(std::string_view mode) -> st::text::GridFitMode;

/// **按字形类的覆盖率 gamma 档位**（数字 / 拉丁字母 / 汉字）。
///
/// 为什么要放在头文件里当常量：这三个值是"应用默认观感"的一部分，而它们**在
/// `TextRenderer` 之外**（渲染器只提供机制）。曾经漏接过一次——字母档只写到渲染器与探针里、
/// `app.cpp` 当时只接了数字，于是"探针测得好、实际应用没变"，接口层测试抓不到。
/// 抽成常量后，回归用例可以直接断言它，改档位时测试与实现必须一起动。
struct ClassGammas {
  float digit{0.84f};   ///< ASCII 数字（按真窗口浏览器三轴标定，见 `docs/BACKLOG.md` P1）
  float letter{0.84f};  ///< 拉丁字母（同上）
  float han{0.88f};     ///< 汉字（同上）
};

/// 把三档**真的**应用到渲染器上——`app.cpp` 与回归用例共用这一个函数。
///
/// 为什么不是"在 app.cpp 里各写一行 `set_class_gamma`"：那样测试只能核对本文件里的
/// 常量，核对不到"应用是否真的接上了"（实测漏接过一次：字母档只写进渲染器与探针，
/// `app.cpp` 当时只接了数字，"探针测得好、实际应用没变"）。
/// 抽成函数后，测试调用它、再读渲染器的 `class_gamma()`——**同一个入口**，
/// 接线漏了就必然红。
inline void apply_class_gammas(st::text::TextRenderer& renderer, const ClassGammas& gammas) {
  renderer.set_class_gamma(st::text::GlyphClass::Digit, gammas.digit);
  renderer.set_class_gamma(st::text::GlyphClass::Letter, gammas.letter);
  renderer.set_class_gamma(st::text::GlyphClass::Han, gammas.han);
}

/// 解析覆盖率 gamma 预校正指数（命令行 `--text-gamma` 取值）。
///
/// 取值：`auto`（→ `ST_TEXT_GAMMA` → 默认 `0.6`）/ `off`（= 1.0）/ 数值字面量（`[0.3, 4]`）。
/// **γ < 1 加墨（压黑），γ > 1 提亮/减墨**。
///
/// 默认 **0.6（加墨方向的部分校正）**：由**真机非无头浏览器**对照页 + 朴素像素口径定出——
/// 四条真实行带全部显示参照比霜天更黑更实（实心像素 75.6/67.5/78.2/79.4% vs 67.0/58.3/50.8/65.6%）。
/// ⚠ 该默认值曾被改成 2.2（提亮方向）并被用户实测驳回；改动前请读
/// `st::text::TextRenderer::set_coverage_gamma` 的事故记录与验收口径。
[[nodiscard]] auto resolve_text_gamma(std::string_view mode, float fallback) -> float;

/// 解析界面字号缩放（命令行 `--ui-font-scale` 取值；`auto` 时再看 `ST_UI_FONT_SCALE`）。
///
/// 取值：`auto`（→ `ST_UI_FONT_SCALE` → 1.0）/ 数值字面量（限定在 `[0.5, 3]`——
/// 超出范围会让界面无法使用，属于拼写错误而非意图）。
[[nodiscard]] auto resolve_ui_font_scale(std::string_view mode) -> float;

struct AppOptions {
  int width{1280};   ///< 逻辑宽（UI 坐标）
  int height{720};   ///< 逻辑高
  float scale{0.0f}; ///< DPI 缩放：0 = 自动（ST_SCALE 环境变量 → 后端默认：win32 查窗口 DPI，
                    ///< headless 查系统显示缩放（Windows）/1.0（其它）；显式值不被覆盖）
  std::string title{"霜天应用"};
  bool headless{false};
  std::string backend{};              ///< 空=自动（无显示则 headless）
  /// 渲染器：`auto`（**按实测帧耗时选更快的那条**）/ `gpu` / `software`。
  ///
  /// 为什么不硬编码 GPU 优先：渲染器优劣与机器强相关（GPU 弱、驱动差、或呈现路径
  /// 仍需 CPU 拷贝时，软件反而更快）。`auto` 的职责是**测出来**，而不是猜。
  std::string renderer{"auto"};
  /// 是否让窗口系统画标题栏/边框（默认 `false` = 自绘，见 `CONVENTIONS.md` §10 第 7 条）。
  bool decorations{false};
  /// 是否可缩放（拖边改尺寸）。
  bool resizable{true};
  /// 文字抗锯齿形态：`auto` / `on`（亚像素）/ `off`（灰度）。
  ///
  /// 默认**亚像素（LCD）**，且**内置通道（无头）与桌面窗口同一默认**——
  /// 截图里的文字与实际运行看到的是同一种合成方式，开发闭环的视觉判断才成立。
  /// 需要可逐像素断言的灰度基准（跨版本回归对比等）时显式传 `--text-lcd=off`。
  std::string text_lcd{"auto"};
  /// 字形网格拟合（hinting）：`auto` / `off` / `light` / `normal`。
  ///
  /// 默认 `normal`，与 `text_lcd` 同一理由：内置通道与桌面同源（拟合**刻意**
  /// 改变字形边沿，两侧不同源时截图与实机就是两种字）。实测收益见
  /// `tools/stem_phase_probe.cpp`：中文「每边一个过渡像素」的糊笔画 90.9% → 21.8%。
  /// 需要不改变字形边沿的基准时显式传 `--text-fit=off`。
  std::string text_fit{"auto"};
  /// 字形观感档位（`--text-preset` / `ST_TEXT_PRESET`）：默认 `B`（三轴对齐真窗口浏览器）。
  /// 三档的定义与实测依据见 `src/app/app.cpp` 里那段表格；改默认值前先读 `docs/BACKLOG.md` P1。
  std::string text_preset{"auto"};
  /// 覆盖率 gamma 预校正：`auto` / `off` / 数值字面量（见 `resolve_text_gamma`）。
  ///
  /// **默认 0.6（加墨）**：γ < 1 压黑加墨、γ > 1 提亮减墨。
  /// 该默认值由**真机非无头浏览器**参照 + 朴素像素口径定出；曾被设成 2.2（提亮）
  /// 并引发回归——依据与教训见 `st::text::TextRenderer::set_coverage_gamma`。
  std::string text_gamma{"auto"};
  /// **界面字号缩放**：`auto` / 数值字面量（见 `resolve_ui_font_scale`）。
  ///
  /// 作用在整条字号阶梯上（xs/sm/base/lg/xl/2xl/3xl 同乘），间距与控件高度不动——
  /// 理由见 `Metrics::scale_fonts`。默认 1.0（不改基准），可用命令行或 `ST_UI_FONT_SCALE`
  /// 在不重编的前提下调档。
  std::string ui_font_scale{"auto"};
  /// **小字号覆盖率 gamma 分档**：`--text-gamma-small` / `ST_TEXT_GAMMA_SMALL`。
  ///
  /// 为什么需要分档（实测）：单档 gamma 是把整条曲线同比例平移，**消不掉“小字比正文
  /// 偏重”**——γ 从 0.6 到 1.0，小字/正文字重落差只从 12.5% 收到 7.5%。
  /// 所以小字号单独压一档、正文维持原档。空 = 不分档（行为与以前完全一致）。
  std::string text_gamma_small{};
  /// 分档适用的**物理**字号上限（`--text-gamma-small-max`）。默认 21 ≈ 14 逻辑 px × 1.5。
  float text_gamma_small_max{21.0f};
  std::string control_bind{"127.0.0.1"};
  std::uint16_t control_port{0};      ///< 0=自动分配
  std::string control_file{};         ///< 写入 {port,pid,...} 供客户端发现
  /// 开放控制通道的 `script` 方法与脚本宿主能力（**默认关闭**）。
  ///
  /// 脚本 = 在应用进程内执行代码；控制通道的设计前提本是"没有任意代码执行入口"，
  /// 开启它是一次显式姿态变更，故必须由宿主应用主动决定，不随监听端口联动。
  bool enable_script{false};
  /// 脚本配额（仅 `enable_script` 时生效）：内存/栈/单次执行时限/转换深度。
  ext::ScriptLimits script_limits{};
  std::string screenshot_dir{};       ///< 控制通道 `encode=file` 的落盘目录（空=会话临时目录）
  std::string font_latin{};           ///< 显式指定拉丁字体文件（空=自动探测）
  std::string font_cjk{};             ///< 显式指定 CJK 字体文件（空=自动探测）
  ui::ThemeMode theme{ui::ThemeMode::Light};
  std::string log_level{"info"};
  std::uint32_t max_frames{0};        ///< >0 时跑满即退出（无头冒烟/CI 用）
  bool exit_on_ready{false};          ///< 首帧后立即退出（自检用）
  double frame_budget_ms{16.0};       ///< 帧间隔目标
};

/// 应用：实现控制通道所需的宿主能力（`control::Host`）+ 自绘窗框所需的窗口控制
/// （`ui::WindowControl`）。
///
/// 为什么两个接口都在这一层收口：`control` 与 `ui` 都不该认识 `shell::Backend`
/// （依赖方向），而平台差异只能住 `platform_*`（`CONVENTIONS.md` §10 第 1 条）——
/// `Application` 本来就同时认识后端与 UI 树，把窗口动作在这里"折"一次，
/// 应用代码与组件就都不需要平台分支。
class Application final : public control::Host, public ui::WindowControl {
 public:
  Application(std::string name, std::string version, AppOptions options = {});
  ~Application() override;
  Application(const Application&) = delete;
  auto operator=(const Application&) -> Application& = delete;

  /// 设置根组件并运行（返回进程退出码）。
  auto run(std::unique_ptr<ui::Element> content) -> Result<int>;
  /// 直接运行**已设置**的根组件（返回进程退出码）。
  ///
  /// 与 `run(content)` 的区别很实在：后者会先 `set_content(...)`，
  /// 因此 `run(nullptr)` 会把先前 `set_content` 的界面**清掉**（实测踩过：
  /// 先建好界面再 `run(nullptr)`，结果界面是空的）。
  /// 「先 set_content 建界面 → 用 on_ready 装依赖启动态的东西 → run()」是推荐写法。
  auto run() -> Result<int>;

  /// 启动完成后的回调（在 `start()` 成功之后、主循环之前调用一次）。
  ///
  /// 用途：那些"需要应用已经起来才能做"的初始化——典型是装载脚本逻辑层
  /// （脚本宿主在 `start()` 里创建，之前拿不到 `script()`）。
  void on_ready(std::function<void()> callback);
  /// 设置根组件（`run()` 内部同样调用；用 `start()`+`tick()` 自驱主循环时先调用本方法）。
  void set_content(std::unique_ptr<ui::Element> content);
  /// 请求退出（控制通道 `app.quit`、快捷键、信号均走此处）。
  void request_quit() override;
  void request_repaint() override;

  // —— control::Host ——
  [[nodiscard]] auto root() -> ui::UiRoot& override { return root_; }
  [[nodiscard]] auto app_name() const -> std::string override { return name_; }
  [[nodiscard]] auto app_version() const -> std::string override { return version_; }
  [[nodiscard]] auto backend_name() const -> std::string_view override;
  [[nodiscard]] auto headless() const -> bool override;
  [[nodiscard]] auto viewport() const -> math::Size override { return root_.viewport(); }
  [[nodiscard]] auto device_scale() const -> float override;
  auto set_device_scale(float scale) -> Status override;
  [[nodiscard]] auto metrics() const -> control::Metrics override;
  void set_theme_mode(ui::ThemeMode mode) override;
  [[nodiscard]] auto capture_to_file(std::string_view path, math::IntRect region)
      -> Result<std::string> override;
  [[nodiscard]] auto capture_png(math::IntRect region)
      -> Result<std::vector<std::uint8_t>> override;
  /// 截取像素（RGBA8、物理分辨率）——视觉断言（像素哈希/基线比对）的取数口。
  [[nodiscard]] auto capture_pixels(math::IntRect region)
      -> Result<control::PixelView> override;
  [[nodiscard]] auto log_lines(std::size_t limit) const -> std::vector<std::string> override;
  /// 脚本宿主（`control::Host` 接口）：未启用脚本能力时为 `nullptr`。
  [[nodiscard]] auto script() -> ui::ScriptHost* override;

  // —— ui::WindowControl（自绘窗框的动作出口；实现见 app.cpp，全是对后端的转发）——
  [[nodiscard]] auto window_control_available() const -> bool override;
  [[nodiscard]] auto window_minimize() -> bool override;
  [[nodiscard]] auto window_toggle_maximize() -> bool override;
  [[nodiscard]] auto window_request_close() -> bool override;
  [[nodiscard]] auto window_begin_move() -> bool override;
  [[nodiscard]] auto window_begin_resize(ui::WindowEdge edge) -> bool override;
  [[nodiscard]] auto window_maximized() const -> bool override;

  /// 单帧推进（自检与外部驱动用）：布局 → 绘制 → present。
  void render_frame();
  /// 查询控制通道端口（0=未启动）。
  [[nodiscard]] auto control_port() const noexcept -> std::uint16_t;
  /// 最后一帧的绘制分解（**仅当 `ST_PAINT_PROFILE=1` 时非空**）：
  /// 按原语（填充/路径/描边/阴影/文字/裁剪…）给出调用次数、覆盖像素与累计耗时。
  /// 存在的理由：“一帧 30ms”本身不指向任何行动——是阴影、文字还是渐变，只能靠分解看。
  [[nodiscard]] auto paint_profile() const -> const raster::PaintProfiler*;
  /// 受控提前启动（`run` 内部会调用；自检可先启动再注入事件）。
  auto start() -> Status;
  [[nodiscard]] auto started() const noexcept -> bool { return started_; }
  /// 主循环单步（自检用）。
  void tick();
  /// 是否收到退出请求（控制通道 `app.quit` / 自检终止条件）。
  [[nodiscard]] auto quit_requested() const noexcept -> bool;

  /// 主循环节拍（自定义主循环在每轮末尾调用；`run_loop` 用同一实现）：
  ///
  /// - **有活干**（待重绘/动画/布局脏）→ 睡到帧预算余量（60fps 节拍）；
  /// - **空闲** → 等**控制通道可读**（或 4ms 超时）；没有可等的句柄时才回退盲睡。
  ///   盲睡会把命令落地时刻量化到节拍边界（实测延迟 = 帧节拍 − 距上次命令的间隔：
  ///   间隔 0ms → 15.7ms、8ms → 8.5ms、16ms → 3.8ms）——连续操作几乎总是白等一拍。
  ///
  /// `tick_start_ms`：本轮 `tick()` 开始前的时刻（`st::time::now_ms()`）。
  void pace_loop(std::int64_t tick_start_ms) const;

  /// 控制通道有新帧到达：若正处于空闲等待，立即结束它（下一轮 `tick()` 就处理）。
  /// 由 `Server` 在读到数据时回调；非空闲时是空操作。
  void wake_control();

 private:
  /// 主循环（`run()` 的公共部分）。
  auto run_loop() -> Result<int>;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string name_{};
  std::string version_{};
  AppOptions options_{};
  ui::UiRoot root_{};
  bool started_{false};
};

}  // namespace st::app
