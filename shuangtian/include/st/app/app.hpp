#pragma once

/// 应用运行时：装配 后端 + 字体 + UI 树 + 控制通道 + 帧循环。
/// 无头模式与窗口模式共用同一条渲染路径（软件光栅器 → 帧缓冲 → 后端 present）。

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "st/core/async.hpp"
#include "st/control/control.hpp"
#include "st/ext/script.hpp"
#include "st/core/error.hpp"
#include "st/shell/shell.hpp"
#include "st/text/text.hpp"   // font_stack() 的返回类型（只读字体栈）
#include "st/ui/ui_root.hpp"
#include "st/ui/window_control.hpp"

namespace st::app {

/// 解析文字抗锯齿形态（命令行 `--text-lcd` 取值；`auto` 时再看 `ST_TEXT_LCD`）。
///
/// **默认 `true`（亚像素）**——以**用户机器上 VS Code 的实测**为准（2026-10-21）。
///
/// 为什么不是灰度：曾经因为“黑字上的彩边太多”把默认改成灰度，但那**方向错了**。
/// 同口径实测（同文本、14px、暗底、中性像素）：
///
/// | 来源 | 实心% | 过渡% | 平均覆盖 | 彩边(到连线垂距) |
/// |---|---|---|---|---|
/// | **VS Code 实拍** | 40.5 | 59.5 | 0.687 | **33.5** |
/// | DWrite ClearType（natural） | 38.6 | 61.4 | 0.664 | 56.7 |
/// | DWrite 灰度 | 34.6 | 65.4 | 0.741 | **0.5** |
///
/// 实心与覆盖率只在**亚像素侧**对齐，且 VS Code 的彩边 33.5 远高于灰度的 0.5
/// —— **VS Code 用的是 ClearType 亚像素**（它是 Electron/Chromium，跟随系统 ClearType 设置）。
/// 另有直接判据：同一条笔画内相邻像素的 R-B 摆幅中位 **130**（灰度不可能）。
///
/// 所以“彩边多”的真因不是“不该用亚像素”，而是**霜天的亚像素实现比 Chromium 更极端**
/// （通道极差 P99 208 vs 173）。要修的是实现，不是关掉这个模式。
[[nodiscard]] auto resolve_text_lcd(std::string_view mode) -> bool;

/// 解析字形网格拟合模式（命令行 `--text-fit` 取值；`auto` 时再看 `ST_TEXT_FIT`）。
///
/// **默认档位由 `resolve_text_fit("auto")` 给出**（当前是 `Off`，以真窗口浏览器
/// 为基准逐轴标定后的结论）。下面那句“默认 `Normal`”是**单指标、无参照**时的推理，
/// 已被参照推翻；留这个提醒是因为“实现改了、注释不改”正是默认档静默漂移的温床。
///
/// **测试必须走 `resolve_text_fit("auto")` 拿默认档**，不要自己写 `GridFitMode::Normal`：
/// 实测踩过——`text_ink_compensation_evens_out_glyph_weight` 一直钉的是 `Normal`，
/// 而产品默认早已是 `Off`，于是它变成一条**与产品行为无关的恒红**：
/// 既不能证明补偿坏了，也不能证明它好着。
[[nodiscard]] auto resolve_text_fit(std::string_view mode) -> st::text::GridFitMode;

/// **拟合墨量补偿是否应开启**。与 `resolve_text_fit` 成对——补偿解决的是「拟合对每个字
/// 的墨量改变幅度不一致」，所以**只有拟合开着时才需要它**（拟合关掉时逐字墨量恒为 1.000）。
///
/// 抽成函数而不是在 `app.cpp` 里写 `set_ink_compensation(true)`：实测踩过“三处各写一份默认值”
/// 的错位——应用无条件开补尝、而拟合默认档已改为 `Off`；回归用例又自己写死 `Normal`，
/// 于是它测的不是产品行为（详见 `text_ink_compensation_evens_out_glyph_weight`）。
[[nodiscard]] auto resolve_ink_compensation(st::text::GridFitMode fit) -> bool;

/// **按字形类的覆盖率 gamma 档位**（数字 / 拉丁字母 / 汉字）。
///
/// 为什么要放在头文件里当常量：这三个值是"应用默认观感"的一部分，而它们**在
/// `TextRenderer` 之外**（渲染器只提供机制）。曾经漏接过一次——字母档只写到渲染器与探针里、
/// `app.cpp` 当时只接了数字，于是"探针测得好、实际应用没变"，接口层测试抓不到。
/// 抽成常量后，回归用例可以直接断言它，改档位时测试与实现必须一起动。
/// **亮色主题的对比度 S 曲线强度**（暗色用 `coverage_contrast_strength_default()` = 0.6）。
///
/// 为何分主题：这一层管“满黑像素占比”，而亮/暗底下同一强度的效果不同
/// （与 `ClassGammas` / `DarkClassGammas` 同一理由）。
/// 亮色重标后（类 γ 统一到 1.60）配 1.0 让四个字形类的平均覆盖率最接近参照：
///
/// | 分类 | 参照 mean | c=0.6 | **c=1.0** |
/// |---|---|---|---|
/// | han | 0.5958 | 0.5675 | **0.6087** |
/// | latin | 0.6003 | 0.5767 | **0.6071** |
/// | mixed | 0.5885 | 0.5651 | **0.6026** |
inline constexpr float kLightContrastStrength = 1.0f;

/// 亮色主题的字形类 gamma。
struct ClassGammas {
  /// ASCII 数字。**亮色 1.60**（2026-10-22 重标）。
  ///
  /// 三类统一到 1.60 是量尺的结论：以 `--theme light` 的 DWrite natural 为参照，
  /// 按**字形类分带统计平均覆盖率**，三类的最优 γ 都落在 1.60 附近。
  /// 旧值（0.84/0.70/0.62）是错位标定——它们让亮色整体偏黑（han +0.092、latin +0.048）。
  float digit{1.60f};
  /// 拉丁字母。**亮色 1.60**：latin `mean` 0.5999 vs 参照 0.6003（差 0.0004）。
  float letter{1.60f};
  /// 汉字。**亮色 1.60**：han `mean` 0.6087 vs 参照 0.5958（配 `kLightContrastStrength`）。
  ///
  /// 旧 `han` 是 0.62，而它的注释写着“暗色主题 15px”——暗色本就另有一套
  /// `DarkClassGammas`，这个值是从暗色档混进来的。后果：亮色汉字**偏黑 0.092**
  /// （平均覆盖率 0.688 vs 参照 0.596），笔画显得比拉丁重、发腻。
  float han{1.60f};
};

/// **暗色主题**的类 gamma——与亮色**不是同一组值**，必须分开标定。
///
/// 为什么（2026-10-06 实测）：覆盖率 → 码值的映射在亮/暗底下**不对称**。
/// 亮底是"白底黑字"（覆盖率把码值**压暗**），暗底是"黑底白字"（覆盖率把码值**提亮**），
/// 同一个 gamma 指数在两个方向上产生的对比度变化不同。实测（真窗口浏览器为基准，
/// 逐字形墨量比）：沿用亮色档（0.84/0.84/0.88）时暗底**系统性偏轻 5~6%**
/// （字母 0.936 / 数字 0.936 / 汉字 0.934~0.954，而亮底是 0.996）。
/// 按暗底重扫后：**字母 0.62 / 数字 0.64 / 汉字 0.75**（三类落到 0.97~1.01）。
struct DarkClassGammas {
  // **2026-10-21 重标**：旧值（0.64/0.62/0.75）的参照是**亚像素时代**的浏览器测量，
  // 且当时没有对比度 S 曲线层。现在两者都变了，必须重标。
  //
  // ⚠ **标定必须用 VS Code 的真实字号**：实测其 `devicePixelRatio=1.5`、`fontSize=13px CSS`
  // ⇒ **物理 19.5px**。早期用 15px 对比是**尺度错配**，得出的"已对齐"有相当部分是巧合
  // （同参数下 15px 实心 37.7%，19.5px 是 43.2%——字号一变结论就变）。
  //
  // 正确口径（scale=1.3 = 19.5px、subpixel_strength=0.5、coverage_contrast_strength=0.6）：
  //   汉字实心 39.6%（VS Code 38.2%）· 拉丁 52.1%（52.3%）· 数字 52.1%（51.4%）
  //
  // 注意：**数字/字母档从 0.64/0.62 降到 0.58/0.50**（加墨方向），而汉字档**升到 0.85**
  // （减墨方向）——方向相反是因为对比度层把汉字推过头了，必须用更高的 γ 拉回来。
  float digit{0.58f};
  float letter{0.50f};
  float han{0.85f};
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
  /// 启动时是否把窗口**铺开到工作区**（默认 true）。
  ///
  /// `true`：按工作区 92% 打开——适合主窗口类应用（IDEA/VSCode 的形态）。
  /// 实测原因：屏幕 3840×2160 + DPI 1.5 ⇒ 工作区 2560×1392 逻辑，而应用期望
  /// 1440×900，旧实现停在 1440×900（**占屏 56%×65%**），用户描述为“右边下方全是桌面、
  /// 页面混乱不可用”。所以默认铺开。
  ///
  /// `false`：**严格用 `width/height` 建窗**，只夹上限（超屏时才缩，且绝不放大）。
  /// 常驻面板/轻提示这类“就该是小窗口”的应用必须置 false：否则 460×620 的提醒卡
  /// 会被撑成整屏（实测：相期在 2355×1280 的全屏尺寸下启动，用户看到的是
  /// “窗口没有圆角”——实际上窗口有两个屏那么大，圆角在屏外）。
  bool fill_work_area{true};
  /// 显示器工作区的逻辑尺寸覆盖（0 = 问后端）。
  ///
  /// 为何需要它：高 DPI + 小屏上“可用逻辑空间不够”是个**只在特定硬件组合下**
  /// 出现的问题（13寸 1080p + 200% 缩放 ⇒ 960×540 逻辑）。没有这个覆盖值，
  /// 无头模式永远模拟不出那个场景，回归测试只能靠猜。显式给出后，
  /// `fit_window_to_work_area()` 就按它夹尺寸——于是“小屏高 DPI”可复现、可断言。
  math::Size work_area_override{0.0f, 0.0f};
  float scale{0.0f}; ///< DPI 缩放：0 = 自动（ST_SCALE 环境变量 → 后端默认：win32 查窗口 DPI，
                    ///< headless 查系统显示缩放（Windows）/1.0（其它）；显式值不被覆盖）
  std::string title{"霜天应用"};
  bool headless{false};
  std::string backend{};              ///< 空=自动（无显示则 headless）
  /// 渲染器：`auto`（**按实测帧耗时选更快的那条**）/ `gpu` / `software`。
  ///
  /// 渲染器：**空串 = 未指定**（无头后端解析为 `software`；窗口后端解析为 `auto`）/ `auto` / `gpu` / `software`。
  ///
  /// 为何缺省是**空串而不是 `auto`**（2026-10-14 修）：两者在字符串上**不可区分**，
  /// 于是无头后端无法兑现“显式 `auto` 照旧探测”这个承诺——实测后果是
  /// `--renderer auto` 的无头 GPU 基准**静默跑在软件路径上**（1.44 ms → 11.49 ms），
  /// 被当成 8.8 倍性能回归。改成空串后三态可区分：
  /// 空 = 各后端自己的缺省；`auto` = 真的探测；`gpu`/`software` = 按名。
  ///
  /// 为什么不硬编码 GPU 优先：渲染器优劣与机器强相关（GPU 弱、驱动差、或呈现路径
  /// 仍需 CPU 拷贝时，软件反而更快）。`auto` 的职责是**测出来**，而不是猜。
  std::string renderer{};
  /// **窗口先用软件画布秒开，后台备好 GPU 后再升级**（默认 `true`）。
  ///
  /// ## 为何需要（实测，2026-10-27）
  ///
  /// GPU 初始化（建 D3D11 设备 ~330 ms + 着色器管线 ~55 ms）发生在
  /// **窗口可见之前**的同步路径上：
  ///
  /// | 渲染器 | 窗口出现 |
  /// |---|---|
  /// | `software` | **147~163 ms** |
  /// | `gpu` | **498~549 ms** |
  ///
  /// 也就是说 350 ms 的“双击后没反应”是无谓的——软件画布的**稳态帧只要
  /// 1.59 ms**（paint 0.55 / present 1.03），完全能先支撑界面运行。
  ///
  /// ## 它做什么
  ///
  /// 建窗时**先分配软件画布并立即返回**，另一个后台线程只做“建设备 + 预热管线”；
  /// 设备就绪后，主线程在帧边界把画布换成 GPU 并建 swapchain（`CreateSwapChain`
  /// 需要消息泵，所以这一步必须留在主线程）。上层（UI/排版/绘制代码）无感。
  ///
  /// ## 什么情况下**不生效**（保持可复现）
  ///
  /// - `renderer` 被显式指定为 `gpu` / `software`；
  /// - 无头后端（没有窗口，也不存在“秒开”之说）。
  ///
  /// 于是 `--renderer gpu` 仍是“一开始就用 GPU”的严格语义，
  /// 测试/基准拿到的仍然是一条确定的路径。
  bool defer_gpu{true};
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
  /// **默认 `off`**（= `auto` 的解析结果）：以真窗口浏览器为基准逐轴标定后，
  /// 拟合在"覆盖墨量 / 覆盖离散 / 边缘柔度"三个正交轴上**都偏离参照**——
  /// 它在单指标（边缘落网格比例）下看着更"锐"，而参照本身的锐度就在不拟合那一档
  /// （详见 `docs/BACKLOG.md` P1）。
  /// 需要它带来的锐度时显式传 `--text-fit=light|normal`。
  std::string text_fit{"auto"};
  /// 拟合的**适用字号上限**（物理 px；`auto` = 不限）。
  ///
  /// 拟合在 10~15px 对覆盖墨量是正收益、20px 以上转负；要"只在小字号拟合"时用它，
  /// 不必整体关掉拟合。
  std::string text_fit_max_size{"auto"};
  /// 逐字形类的覆盖率 gamma（`auto` = 用内置默认档；数值直接给）。
  ///
  /// 三类的度量特性不同（数字笔画最细、汉字最密），本就是**独立旋钮**——
  /// 因此按类给参数，而不是打包成一个"观感档位"名。
  std::string text_digit_gamma{"auto"};
  std::string text_letter_gamma{"auto"};
  std::string text_han_gamma{"auto"};
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
  /// **内置主题名**（`st/ui/builtin_themes.hpp` 里的那些；空 = 不用）。
  ///
  /// 与 `theme` 的分工：`theme` 只决定**明暗基准**（影响文本 gamma、卡片描边强度
  /// 这类派生量），而 `theme_name` 指定一套完整的调色板。两者同时给时：
  /// `theme_name` 生效，`theme` 被它自己的 `base` 覆盖。
  ///
  /// 为何不用 `theme_file` 传内置主题：内置主题必须**开箱即用**
  /// （用户不该为了用默认观感而先下载一份 JSON）。
  std::string theme_name{};
  /// **自定义主题文件**（JSON，稀疏覆盖，见 `st/ui/theme_io.hpp`）。
  ///
  /// 命令行 `--theme-file PATH` > 环境变量 `ST_THEME_FILE`。空 = 用内置主题。
  /// 加载失败（文件不在 / 语法错 / 未知 token）**不阻止启动**，也不静默——
  /// 退回内置主题并写一条告警日志：主题是外观配置，不是运行前提
  ///（与“配置写错不该让进程崩”的失败安全姿态一致）。
  ///
  /// 优先级：`theme_name` 的调色板 → 本文件覆盖 → 运行期 `theme.set`。
  /// 这样“选一套主题再用文件微调”是自然写法。
  /// **窗口/任务栏图标**（PNG 字节，通常是编译期嵌入的资源）。
  ///
  /// 为何交给应用而不是框架自己找文件：图标是产品资产（各应用各不相同），
  /// 而应用已经是单文件交付——它用 `b::embed<>` 把 PNG 嵌进来，这里只收字节。
  /// 空 = 不设（保持系统默认图标）。后端不支持时只记告警，不阻止启动。
  std::vector<std::uint8_t> window_icon_png{};
  /// 启用系统托盘（常驻类应用的形态）。
  ///
  /// 默认关：只有“想常驻”的应用才要托盘，普通应用多一个孤立图标只是噪音。
  /// 后端不支持时（无头/未实现平台）只告警，不阻止启动。
  bool tray_enabled{false};
  /// 托盘图标 PNG（空 = 用 `window_icon_png`）。
  std::vector<std::uint8_t> tray_icon_png{};
  /// 托盘悬停提示（空 = 用窗口标题）。
  std::string tray_tooltip{};
  /// 关窗→托盘常驻：`true` 时用户点 × 只隐藏窗口（进程与托盘图标保留），
  /// 退出只能走托盘菜单（或显式 `request_quit`）。默认 `false`（关窗即退出）。
  /// 无托盘（未启用/后端不支持）时自动退化回“关窗即退出”。
  bool tray_minimize_on_close{false};
  std::string theme_file{};
  std::string log_level{"info"};
  std::uint32_t max_frames{0};        ///< >0 时跑满即退出（无头冒烟/CI 用）
  bool exit_on_ready{false};          ///< 首帧后立即退出（自检用）
  double frame_budget_ms{16.0};       ///< 帧间隔目标

  /// **`defer_gpu` 是否真的生效**：`defer_gpu` 为真、且渲染器**未被显式指定**。
  ///
  /// 抽成函数而不是在两处各写一份：`app.cpp` 用它决定“要不要交给后端”，
  /// 回归测试也用它。本仓有前车之鉴（`resolve_ink_compensation` 的注释）：
  /// 实现与测试各写一份默认值时，它们会静静地跑在不同判据上。
  ///
  /// 为何显式渲染器要排除：`--renderer gpu` / `software` 是“我要一条确定的路径”，
  /// 测试与基准靠它可复现。尤其 `software` 必须真的全程软件，
  /// 不能被延迟升级偷偷换成 GPU（否则基准数据就不可比了）。
  [[nodiscard]] auto defer_gpu_effective() const noexcept -> bool {
    return defer_gpu && renderer.empty();
  }
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
  /// 当前显示器**工作区**的逻辑尺寸（已扣任务栏/系统栏）。
  ///
  /// 应用用它把窗口初始尺寸**夹到屏幕范围内**——写死逻辑尺寸（如 1280×800）
  /// 在小屏高 DPI 上会超屏：13寸 1080p 开 200% 缩放时可用逻辑区只有 960×540。
  /// 无头/后端不支持时退回当前视口尺寸（语义：“窗口就是全部可用空间”）。
  [[nodiscard]] auto work_area_logical() const -> math::Size;
  /// 这台显示器能给的最小逻辑视口（低于它三栏布局会互相挤压）。
  [[nodiscard]] auto min_logical_size() const -> math::Size;
  /// 字体栈（只读）：给需要**独立缓存域**的离屏排版用
  /// （见 `st::app::OffscreenTextPort`——后台排版若复用主线程的 `TextRenderer`，
  /// 会把它 32k 条的宽度 LRU 刷爆，主线程随后每段文字都要重新整形）。
  /// 字体数据本身不可变，多个 `TextRenderer` 共享同一个 `FontStack` 是安全的。
  [[nodiscard]] auto font_stack() const -> const st::text::FontStack&;
  [[nodiscard]] auto metrics() const -> control::Metrics override;
  void set_theme_mode(ui::ThemeMode mode) override;
  /// 按名字切主题（内置主题库：light/dark/nebula/qinhan/…）。
  /// 与 `set_theme_mode` 同一管线（`compose_theme_with` 重合成），
  /// `--theme-file` 与运行期 `theme.set` 的覆盖不丢。未知名字返回失败、不切换。
  [[nodiscard]] auto set_theme_name(std::string_view name) -> Status;
  /// 控制通道 `theme.set`：在当前主题上施加稀疏覆盖，成功即重绘。
  [[nodiscard]] auto apply_theme_overrides(const st::Json& overrides) -> Status override;
  /// 控制通道 `theme.tokens`：当前生效主题的完整快照。
  [[nodiscard]] auto theme_snapshot() const -> st::Json override;
  [[nodiscard]] auto capture_to_file(std::string_view path, math::IntRect region)
      -> Result<control::Host::SavedShot> override;
  [[nodiscard]] auto capture_png(math::IntRect region)
      -> Result<control::Host::PngView> override;
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
  /// 从最小化/后台恢复到前台（常驻类应用的“点托盘把我叫回来”）。
  [[nodiscard]] auto window_restore() -> bool override;

  /// 运行时改窗口逻辑尺寸（常驻面板的“缩成一角 / 展开主界面”）。
  ///
  /// 为何不用多个窗口：多窗口在无头后端与截图验证里都要额外路径，
  /// 而“同一个窗口换尺寸 + overlay 换内容”就能满足轻提示/主界面两种形态。
  [[nodiscard]] auto set_window_size(int width_logical, int height_logical) -> Status;

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
  /// 应用级异步任务系统（`st/core/async.hpp`）：
  ///
  /// - `tasks().run_async(fn)`：独立线程（长任务/IO；返回 future + 可取消句柄）
  /// - `tasks().submit(fn)`：共享线程池（短任务/`parallel_for`）
  /// - `tasks().post_ui(fn)`：任意线程回主线程（下一帧 `tick()` 内有序执行）
  ///
  /// 生命周期随 `Application`：析构时停池、join 全部线程、清空 UI 队列。
  /// 这是「主线程只渲染」契约的一等支撑（CONVENTIONS §11）。
  [[nodiscard]] auto tasks() -> app::Tasks&;
  /// 是否收到退出请求（控制通道 `app.quit` / 自检终止条件）。
  [[nodiscard]] auto quit_requested() const noexcept -> bool;

  // ── 系统托盘（常驻形态）──
  //
  // 语义：托盘图标在 `start()` 时按 `AppOptions::tray_enabled` 创建；
  // 点击事件由应用在主循环里主动取（不引入事件类型：托盘点击只是一句“把我叫回来”）。

  /// 托盘是否真启用（后端不支持时为 false——如实反映，不假装）。
  [[nodiscard]] auto tray_ready() const noexcept -> bool;

  /// 取走自上次调用以来的托盘点击次数（取后归零）。
  ///
  /// 典型用法（主循环里一行）：
  /// ```cpp
  /// if (app.take_tray_clicks() > 0) app.window_restore();
  /// ```
  [[nodiscard]] auto take_tray_clicks() noexcept -> std::uint32_t;

  /// 更新托盘提示文字（空串 = 用窗口标题）。
  ///
  /// 为何单独开这个：常驻应用常把“待处理几件”写在提示里，
  /// 而那是一个会变的数字——不必为改一个字符串重建图标。
  [[nodiscard]] auto set_tray_tooltip(std::string_view tooltip) -> Status;

  /// 主循环节拍（自定义主循环在每轮末尾调用；`run_loop` 用同一实现）：
  ///
  /// - **有活干**（待重绘/动画/布局脏）→ 睡到帧预算余量（60fps 节拍）；
  /// - **空闲** → 等**控制通道可读**，超时夹到最近的定时唤醒点（光标闪烁/Toast
  ///   倒计时等低频周期事件登记的到帧时刻，见 `Element::request_wake_at`）；
  ///   无唤醒点时维持 4ms 上限（输入响应保障）。没有可等句柄时回退盲睡。
  ///   盲睡会把命令落地时刻量化到节拍边界（实测延迟 = 帧节拍 − 距上次命令的间隔：
  ///   间隔 0ms → 15.7ms、8ms → 8.5ms、16ms → 3.8ms）——连续操作几乎总是白等一拍。
  ///
  /// `tick_start_ms`：本轮 `tick()` 开始前的时刻（`st::time::now_ms()`）。
  /// 非 const（2026-10-29）：空闲分支要把帧时间轴刷新到当前墙钟（`set_time`）。
  void pace_loop(std::int64_t tick_start_ms);

  /// 控制通道有新帧到达：若正处于空闲等待，立即结束它（下一轮 `tick()` 就处理）。
  /// 由 `Server` 在读到数据时回调；非空闲时是空操作。
  void wake_control();

 private:
  /// 主循环（`run()` 的公共部分）。
  auto run_loop() -> Result<int>;
  /// 装载 `--theme-file` / `ST_THEME_FILE` 指定的自定义主题（构造函数末尾调用）。
  void apply_theme_file();
  /// 重新构造生效主题（内置基准 + 文件覆盖 + 运行期覆盖）。
  [[nodiscard]] auto compose_theme(ui::ThemeMode mode) const -> Result<ui::Theme>;
  /// `compose_theme` 的显式变体（用指定的运行期覆盖；供"先校验后落地"用）。
  [[nodiscard]] auto compose_theme_with(ui::ThemeMode mode, const st::Json& runtime) const
      -> Result<ui::Theme>;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string name_{};
  std::string version_{};
  AppOptions options_{};
  ui::UiRoot root_{};
  /// 生效主题（= 内置基准 + 下面两层覆盖的合成结果）。
  ///
  /// 为什么**留住一份**而不是每次 `Theme::by_mode`：自定义主题是稀疏覆盖的产物，
  /// 切主题模式时若直接重建，用户改过的 token 会静默丢失（"切一下主题，我的配色又没了"）。
  ui::Theme custom_theme_{};
  bool started_{false};
};

}  // namespace st::app
