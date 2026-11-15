/// 霜天桌面（st_desktop）：替代原生桌面的**全屏置底启动器**。
///
/// ## 它是什么
///
/// 一个铺满整块显示器、钉在窗口层最底部的窗口 = 桌面：壁纸（渐变 + 底部光带）
/// + 应用图标网格 + 底部**液态玻璃任务栏（Dock）**。点图标 → 启动对应应用的 exe
/// （真实窗口进程，**不受本桌面管理**），应用窗口自然浮在桌面之上。
///
/// ## 它不是什么（这一轮明确去掉的）
///
/// **不内嵌子窗口**。上一版用 `NativeHost::attach` 把外部进程的主窗口嵌进桌面的
/// 一块矩形里——做法能跑，但它把"桌面"与"窗口管理器"混成一件事：应用窗口的
/// 位置/尺寸/焦点全归桌面管，弹出的对话框也跟着跑进那块矩形，而嵌入内容的像素
/// 又不进 framebuffer（截不到、断言不到）。现在回到最朴素的语义：
/// **桌面是桌面，应用是应用的进程**。
///
/// ## 液态玻璃的验收载体
///
/// Dock 开的是 `Style::backdrop_blur` + `backdrop_refraction` + `backdrop_dispersion`
/// （元素级背板毛玻璃 + 折射 + 色散，GPU 腿生效；软件/无头腿自动降级实色底）。
/// 壁纸是一张**真实纹理照片**（见 `wallpaper_bitmap`）——玻璃要“有东西可透”才成立，
/// 而纹理的细颗粒正好提供不规则层次（不是画出来的硬边色阶）。
/// `--debug-stripes` 换成高对比色块，用于**量**折射位移与色散。
///
/// ## 配置
///
/// 应用清单在 exe 同目录的 `desktop.json`（`--config PATH` 可指定别处）。
/// 缺文件/字段非法时**退回内置默认清单并记日志**，不阻断启动——
/// 桌面起不来比"少配一个应用"严重得多。

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// 桌面守卫要**看另一个进程**与**探 explorer 的任务栏**——没有平台中立封装能表达它。
// 这是本文件唯一直接引系统头的地方，已在 CONVENTIONS.md §8 的 L14 表登记。
#define NOMINMAX 1
#include <windows.h>  // NOLINT(l14)
#endif

#include "app_icon_element.hpp"
#include "battery/embed.hpp"   // b::embed<...>()：各应用图标与壁纸是编译期嵌入的
#include "st/app/app.hpp"
#include "st/app/cli.hpp"
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/button.hpp"
#include "st/ui/components/text.hpp"
#include "st/ui/element.hpp"
#include "st/ui/theme.hpp"
#include "wallpaper.hpp"

namespace {

using st::math::Color;
using st::math::Insets;
using st::math::Point;
using st::ui::Button;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Panel;
using st::ui::Text;
using stdd::AppIcon;
using stdd::IconBitmap;
using stdd::ImageBitmap;
using stdd::Wallpaper;

// ─── 布局常量（逻辑像素） ────────────────────────────────────────────────────

// ## Dock 的高度不是常量，而是**问窗口系统要**（见 `DockGeometry`）
//
// 需求是“与系统任务栏等高、避免最大化窗口遮蔽和间隙”，那是一个**几何约束**：
//
//     Dock 顶缘 == rcWork.bottom   （最大化窗口的底边也落在这里）
//     Dock 底缘 == rcMonitor.bottom（屏幕底）
//
// 所以 Dock 高度 = rcMonitor.bottom − rcWork.bottom = 系统任务栏厚度。
// 实测（`tools/desktop_workarea_probe.py`，2560×1440 / 100% 缩放）：该值为 **48**，
// 且**隐藏任务栏后 rcWork 不变**——于是“桌面壳隐藏任务栏”与“和它等高”可以同时成立，
// 不需要在两者之间二选一。
//
// 为何不写死 48：任务栏厚度随 DPI（1.25x → 60）、系统版本、以及「小任务栏按钮」
// 设置而变。写死就会在那些机器上出现缝隙或遮蔽——而那正是本需求要消灭的东西。
constexpr float kDockFallbackHeight = 48.0f;   // 拿不到工作区时的兜底（100% 缩放下的大多数系统）
constexpr float kTileSize = 112.0f;
constexpr float kTileGap = 10.0f;
/// 图标网格的列数上限（超过就换行——桌面有更多应用时不该铺成一条长龙）。
constexpr int kGridColumns = 8;

// ─── 应用清单 ────────────────────────────────────────────────────────────────

/// 清单里的一条应用。
struct DesktopApp {
  std::string id;                 ///< 稳定身份（元素 id、图标查找键）
  std::string label;              ///< 桌面图标下的文字
  std::string icon;               ///< 内置图标名（`builtin_icons()` 的键）
  std::string icon_path;          ///< 外部图标文件（PNG；空 = 只用内置那份）
  std::string exe;                ///< 可执行文件名（带不带 .exe 都行）
  std::vector<std::string> args;  ///< 启动参数
  std::string glyph;              ///< 缺图时的占位字符
};

/// 内置图标：**直接嵌各应用自己那份 `<app>-icon.png`**（就是窗口/任务栏用的那张）。
///
/// ## 为什么不自己画一套
///
/// 那份图已经是各应用对外的一致性标识（标题栏、任务栏、资源管理器都认它），
/// 桌面再画一套会出现"桌面上是 A、任务栏上是 B"。体积代价合计约 20KB。
///
/// ## 为什么用 `std::span` 而不是每项存一个 vector
///
/// 嵌入的字节在进程生命周期内**恒定不变**（编译期常量），`span` 直接指过去；
/// 存 vector 会在启动时白拷一份，且让"可共享"变成"不可共享"。
using IconEmbed = std::pair<std::string, std::span<const char>>;

[[nodiscard]] auto builtin_icons() -> const std::vector<IconEmbed>& {
  // `b::embed<...>()` 返回 `EmbeddedFile`（带 `data()`/`length()`），不是容器；
  // 在静态表里存成 `span<const char>`，解码时再转字节视图。
  static const std::vector<IconEmbed> icons = [] {
    std::vector<IconEmbed> list;
    const auto add = [&list](std::string id, auto asset) {
      list.emplace_back(std::move(id), std::span<const char>(asset.data(), asset.length()));
    };
    // 顺序与 `default_apps()` 一致，便于对照；**八件套一个不少**
    //（`st.pkg` 的 `group "all"` 那八个应用，加上桌面自己）。
    add("gallery", b::embed<"apps/gallery/assets/gallery-icon.png">());
    add("gbcode", b::embed<"apps/gbcode/assets/gbcode-icon.png">());
    add("jianmu", b::embed<"apps/jianmu/assets/jianmu-icon.png">());
    add("caiyun", b::embed<"apps/caiyun/assets/caiyun-icon.png">());
    add("louyue", b::embed<"apps/louyue/assets/louyue-icon.png">());
    add("xiangqi", b::embed<"apps/xiangqi/assets/xiangqi-icon.png">());
    add("cefu", b::embed<"apps/cefu/assets/cefu-icon.png">());
    add("gedou", b::embed<"apps/gedou/assets/gedou-icon.png">());
    // 桌面自己：霜天桌面本来只有 `.ico`（PE 资源）与一份 32×32 的 raw RGBA，
    // 没有可用的 PNG。这份 256×256 是从 `gebai.ico` 里**抽出来的那张 PNG 条目**
    //（`.ico` 允许内嵌 PNG，最大那档就是 256²），所以它与任务栏图标**同一份像素**，
    // 不是另画的一套。
    add("st_desktop", b::embed<"apps/st_desktop/assets/st_desktop-icon.png">());
    return list;
  }();
  return icons;
}

[[nodiscard]] auto make_default(const std::string& id, const std::string& label,
                                const std::string& glyph) -> DesktopApp {
  DesktopApp app;
  app.id = id;
  app.label = label;
  app.icon = id;
  app.icon_path = {};
  app.exe = id + ".exe";
  app.args = {};
  app.glyph = glyph;
  return app;
}

/// 内置默认清单：`desktop.json` 缺失或不可用时的兜底，也是"这桌面能开什么"的说明。
///
/// ## 为什么是这九个（“全套”的定义）
///
/// 前八个 = `st.pkg` 里 `group "all"` 的成员（`st build all` 构建的那批真实应用），
/// 不多不少；第九个是**桌面自己**，让用户能从桌面里重开一份（换配置/多实例调试都要它）。
///
/// 为什么把“构建清单”当权威而不是凭印象列：漏掉一个应用的表现是“桌面上没有那个图标”，
/// 而用户无从判断是没装、没配、还是清单漏了。以 `group "all"` 为准能保证
/// **新增应用时只要加进 all，桌面自动就有它**（这一版补上了此前缺的册府与格斗）。
[[nodiscard]] auto default_apps() -> std::vector<DesktopApp> {
  return {
      make_default("gallery", "画廊", "画"), make_default("gbcode", "码本", "码"),
      make_default("jianmu", "建木", "木"),   make_default("caiyun", "裁云", "云"),
      make_default("louyue", "镂月", "月"),   make_default("xiangqi", "相期", "期"),
      make_default("cefu", "册府", "册"),     make_default("gedou", "格斗", "斗"),
      make_default("st_desktop", "霜天桌面", "桌"),
  };
}

/// 读 `desktop.json`。返回 `nullopt` = 该用默认清单（文件不存在**不算错误**）。
///
/// 解析失败也返回 `nullopt` 并记一条 error：配置写坏时宁可跑默认清单，
/// 也不要让用户对着一个起不来的桌面去排查 JSON。
[[nodiscard]] auto load_apps(const std::string& path) -> std::optional<std::vector<DesktopApp>> {
  if (path.empty() || !st::fs::exists(path)) return std::nullopt;
  const auto text = st::fs::read_text(path);
  if (!text) {
    st::log::err("桌面配置读不到：{}（{}）\n", path, text.error().to_string());
    return std::nullopt;
  }
  const auto parsed = st::json_parse(*text);
  if (!parsed) {
    st::log::err("桌面配置解析失败：{}（{}）→ 用内置清单\n", path, parsed.error().to_string());
    return std::nullopt;
  }
  if (const st::Json* apps_json = st::json_find(*parsed, "apps"); apps_json != nullptr) {
    const st::Json& list = *apps_json;
    if (!list.is_array() || list.empty()) {
      st::log::err("桌面配置的 apps 不是非空数组：{} → 用内置清单\n", path);
      return std::nullopt;
    }
    std::vector<DesktopApp> apps;
    for (const st::Json& node : list) {
      DesktopApp app;
      app.id = st::json_get_string(node, "id");
      app.label = st::json_get_string(node, "label", app.id);
      app.icon = st::json_get_string(node, "icon");
      app.icon_path = st::json_get_string(node, "icon_path");
      app.exe = st::json_get_string(node, "exe");
      app.args = st::json_get_string_array(node, "args");
      app.glyph = st::json_get_string(node, "glyph");
      if (app.id.empty()) {
        // 没有 id 就没有稳定标识（元素 id 与图标查找都靠它）——跳过并点名。
        st::log::err("桌面配置：一条应用缺 id，已跳过（label={}）\n", app.label);
        continue;
      }
      if (app.glyph.empty() && !app.label.empty()) app.glyph = app.label.substr(0, 3);
      apps.push_back(std::move(app));
    }
    if (apps.empty()) {
      st::log::err("桌面配置里没有一条可用应用：{} → 用内置清单\n", path);
      return std::nullopt;
    }
    st::log::out("桌面配置已加载：{} 项（{}）\n", apps.size(), path);
    return apps;
  }
  st::log::err("桌面配置缺 apps 数组：{} → 用内置清单\n", path);
  return std::nullopt;
}

/// 默认配置文件路径：**exe 同目录**的 `desktop.json`。
///
/// 不用当前工作目录：发行形态下双击启动时 cwd 是资源管理器给的目录，与 exe 位置无关。
[[nodiscard]] auto default_config_path() -> std::string {
  const auto self = st::process::executable_path();
  if (!self) return "desktop.json";
  return st::fs::join(st::fs::parent(*self), "desktop.json");
}

// ─── exe 解析 ────────────────────────────────────────────────────────────────

/// 解析 exe 的完整路径：**与桌面 exe 同目录优先**（分发形态），退 PATH。
///
/// 找不到返回空串（图标按不可用置灰但仍显示——"这个应用没装/不在旁边"
/// 是用户该知道的信息，静默隐藏反而让人以为清单漏了）。
[[nodiscard]] auto resolve_exe(const std::string& exe) -> std::string {
  if (exe.empty()) return {};
  std::string name = exe;
#if defined(_WIN32)
  // Windows 上 CreateProcess/which 都要求带扩展名，而配置里用户多半只写 `gallery`。
  if (st::fs::extension(name).empty()) name += ".exe";
#endif
  const auto self = st::process::executable_path();
  if (self) {
    const std::string candidate = st::fs::join(st::fs::parent(*self), name);
    if (st::fs::is_regular_file(candidate)) return candidate;
  }
  const auto found = st::process::which(name);
  return found.has_value() ? *found : std::string{};
}

// ─── 启动一条应用 ────────────────────────────────────────────────────────────

/// 启动一条应用（分离进程：零继承零管道，行为与双击一致）。
///
/// ## 为什么用分离启动
///
/// 桌面**不管**被启动应用的生命期：它起来之后就是独立的应用窗口，关掉它、
/// 最小化它都与桌面无关。`spawn_detached` 恰好是这个语义（不持句柄、不重定向管道）。
///
/// ## 工作目录必须显式设成那个应用 exe 的目录
///
/// 应用按**当前工作目录**找自己的资源（建木的 `scenes/`、码本的配置）。
/// 双击启动时 cwd 就是 exe 目录，而这里由桌面拉起——不设就会"应用起来了、界面空"。
[[nodiscard]] auto launch_app(const DesktopApp& app, std::string& status) -> bool {
  if (app.exe.empty()) {
    status = app.label + "：未配置可执行文件";
    return false;
  }
  const std::string exe_path = resolve_exe(app.exe);
  if (exe_path.empty()) {
    status = app.label + "：找不到 " + app.exe;
    st::log::err("启动失败（找不到 exe）：{}\n", app.exe);
    return false;
  }
  st::process::Options options;
  options.cwd = st::fs::parent(exe_path);
  options.capture_output = false;
  // `spawn_notepady` 不存在：这里用 `spawn_detached` 的语义（不持句柄）。
  // 参数与 cwd 走它自己的入口（见 process.hpp 的重载）。
  const std::uint64_t pid = st::process::spawn_detached(exe_path, app.args, options.cwd);
  if (pid == 0) {
    status = app.label + "：启动失败";
    st::log::err("启动失败（spawn 返回 0）：{}\n", exe_path);
    return false;
  }
  status = app.label + " 已启动";
  st::log::out("已启动 {}（pid={}）\n", exe_path, pid);
  return true;
}

/// 玻璃参数集合：让“透亮度”可**量**、也可在运行时扫档选择。
///
/// ## 为何做成参数而不是直接写死
///
/// “透亮”是观感指标，它的验收标准在用户身上（不是测试红绿）。而调它需要同时兼顾
/// 三个正交量（亮度差 / 纹理透过率 / 文字可读性）——拍一个值然后说“好了”是没根据的。
/// 搞成参数后可以**同一份代码跑几档、同一把尺子量**，再把数字与图一起给人选。
///
/// 命令行 `--glass-blur / --glass-tint / --glass-refraction / --glass-dispersion`
/// 可直接覆盖（调完不用重编）；色也可改（`--glass-base RRGGBB --glass-alpha N`）。
struct GlassParams {
  /// 模糊半径（逻辑 px）。**它是“透不透”的主旋钮**：壁纸的细颗粒（石纹裂痕
  /// 只有 2~3px）会被大于它几倍的半径抹平，剩下的只是一块均匀色。
  ///
  /// **默认 6px**（实测选型，不是拍脑袋）：同一把尺子扫四档，纹理透过率
  /// 42%（22px）→ **54%**（6px）→ 62%（4px），而 4px 那一档玻璃感开始退化
  /// （看着像半透明而不是毛玻璃）。6px 是“透得出细节 + 仍然是玻璃”的交点。
  float blur{6.0f};
  /// 玻璃上那层着色的不透明度。0 = 只模糊不叠色（背后最清楚，但文字容易读不出）。
  float tint{0.30f};
  /// 折射强度（逻辑 px；边缘把背景吸入的位移量）。
  float refraction{14.0f};
  /// 色散强度（无量纲）。
  float dispersion{0.4f};
  /// 降级底的色与 alpha（无 GPU 时它就是最终颜色；有 GPU 时它是 tint 叠的那层色）。
  Color base{0xA8, 0xB4, 0xC8, 0xFF};
  float base_alpha{0.55f};
};

/// Dock 的**几何**：从窗口系统的真实矩形推导，不写死。
///
/// ## 三个数各自回答一个“缝隙/遮蔽”问题
///
/// | 字段 | 来源 | 它防的是什么 |
/// |---|---|---|
/// | `height` | `rcMonitor.bottom − rcWork.bottom` | 比任务栏矮 → 最大化窗口底边**遮蔽** Dock；高 → 上方多占一截 |
/// | `top` | 屏幕底 − height | 与 `rcWork.bottom` 重合，**零缝隙** |
/// | `radius` | 固定 0（直角） | 底部圆角会贴屏幕底露出两个缺角 |
///
/// ## 为何 radius 是 0 而不是保留圆角
///
/// 通栏贴底的条，下面两角直接贴屏幕边缘。框架的 `Style::radius` 是**四角统一**的，
/// 没有“只圆上方”的字段；给它非零值就会在屏幕底部露出两个三角形缺口（看到壁纸）。
/// 而系统任务栏本身是直角矩形——需求既然要“与它等高”，形态上也该对齐。
struct DockGeometry {
  float height{kDockFallbackHeight};
  float top{0.0f};
  float radius{0.0f};
};

/// 从应用的视口与工作区推导 Dock 几何。
///
/// `viewport` 是当前窗口的逻辑尺寸（铺满显示器时 = `rcMonitor` 的逻辑尺寸）；
/// `work` 是工作区逻辑尺寸（已扣任务栏）。两者相减就是那条带子的高度。
///
/// 拿不到工作区（无头后端、或后端报 0）时退回 `kDockFallbackHeight`：
/// 宁可高度略差一档，也不要因为一次探测失败就让 Dock 消失或吃掉半个屏。
[[nodiscard]] auto resolve_dock_geometry(const st::math::Size& viewport,
                                         const st::math::Size& work) -> DockGeometry {
  DockGeometry dock;
  const float strip = viewport.height - work.height;
  // 只在“确实有一条位于底部的带子”时采用探测值：
  // 工作区与视口相等 = 没有任务栏（或后端不知道）→ 退回兜底；
  // strip 异常大（>1/3 屏）= 探测值可疑（工作区被别的程序改过）→ 不信任它。
  const bool plausible = strip > 0.5f && strip < viewport.height * 0.34f;
  dock.height = plausible ? strip : kDockFallbackHeight;
  dock.top = viewport.height - dock.height;
  return dock;
}

// ─── 界面状态 ────────────────────────────────────────────────────────────────

/// 桌面壳的状态（界面构建期与回调期共享）。
///
/// 为什么用一个结构而不是散在 lambda 捕获里：桌面网格、Dock、状态条三处都要
/// 同一份清单与图标位图，且回调里要能改状态文本。
struct ShellState {
  st::app::Application* app{nullptr};
  std::vector<DesktopApp> apps{};
  /// id → 图标位图。**同一份共享给网格与 Dock**（同一张图不解码两次，
  /// 也让两处的像素完全一致）。
  std::map<std::string, std::shared_ptr<const IconBitmap>> icons{};
  std::string status{"就绪"};
bool debug_stripes{false};
/// 玻璃参数（Dock 与状态条共用；可用命令行扫档）。
GlassParams glass{};
/// Dock 几何（启动时按真实显示器/工作区算；见 `resolve_dock_geometry`）。
DockGeometry dock{};
};

[[nodiscard]] auto icon_of(const ShellState& state, const DesktopApp& app)
    -> std::shared_ptr<const IconBitmap> {
  if (!app.icon.empty()) {
    if (const auto found = state.icons.find(app.icon); found != state.icons.end()) {
      return found->second;
    }
  }
  return nullptr;
}

/// 把状态文本推回界面。
///
/// 按 id 查元素（**不能存元素指针**：重组会销毁重建元素，存下来的指针会悬垂——
/// 那是 `PITFALLS` 里"构建期存的元素指针跨帧用"那条坑）。
void refresh_status(ShellState& state) {
  if (state.app == nullptr) return;
  if (Element* element = state.app->root().find("status-text"); element != nullptr) {
    if (auto* text = dynamic_cast<Text*>(element); text != nullptr) {
      text->set_content(state.status);
    }
  }
}

// ─── 壁纸与背景 ──────────────────────────────────────────────────────────────

/// 壁纸位图：**嵌入的细颗粒纹理照片**（`caiyun/assets/media/bg-slate.png`）。
///
/// ## 为什么是位图而不是画出来的图形（2026-11-15 实测教训）
///
/// 第一版用径向渐变 + 底部署条**平涂色阶**当壁纸，意图是给玻璃提供可透的
/// 结构（背板模糊必须有东西可糊）。但几个平涂矩形叠出来的硬边横带**读起来
/// 就是渲染 bug**——用户的反馈是那几条“条纹怎么回事”，而不是“这是背景”。
///
/// 换成一张真实纹理照片后两件事同时成立：① 背景本身就是桌面壁纸，看起来自然；
/// ② 它的**细颗粒**在模糊后留下不规则层次，玻璃依旧可辨（不靠硬边）。
///
/// ## 为何从 `bg-deep` 换成 `bg-slate`（同日第二轮，用户：“不够透亮”）
///
/// 两者纹理形态几乎一样（石面细纹），差别只在**亮度**：
///
/// | 素材 | 平均亮度 | 中位亮度 |
/// |---|---|---|
/// | `bg-deep` | **23.2%** | 23.1% |
/// | `bg-slate` | **36.5%** | 37.0% |
///
/// 玻璃是“透出下面的像素再叠一层薄色”。背景只有 23% 时，透出来的就是一片
/// 暗——无论把玻璃底调得多淡，**透不到亮东西就不可能显得透亮**。实测当时 Dock
/// 内部比壁纸暗 16.2，用户看到的是“一块发闷的板”。换成 36.5% 的那张后，
/// 同样参数下玻璃就有了可透的亮度差。
///
/// 为什么不直接拿更亮的 `bg-dusk`（50.9%）/`accent-glow`（54.3%）：它们是
/// 紫罗兰/粉色氛围图，风格与“石面纹理”不一致，而且亮到那时深色玻璃会显得更暗。
///
/// ## 为什么用 caiyun 的素材而不是另存一份
///
/// 它已经是仓里的产品资源（裁云的背景图库），再拷一份会让“同一个视觉资产”
/// 有两个真相源。代价是构建期多嵌入约 1MB（单文件分发形态可接受）。
[[nodiscard]] auto wallpaper_bitmap() -> std::shared_ptr<const ImageBitmap> {
  // 只解码一次（静态缓存）：壁纸是固定资源，重组/切模式都不需要重新解码。
  //
  // `b::embed<...>()` 返回 `EmbeddedFile`（带 `data()`/`length()`），**不是**容器、
  // 也不能隐式转 `span`——先取出视图再传（与 `builtin_icons()` 同一写法）。
  static const std::shared_ptr<const ImageBitmap> bitmap = [] {
    const auto asset = b::embed<"apps/caiyun/assets/media/bg-slate.png">();
    return ImageBitmap::from_png(std::span<const char>(asset.data(), asset.length()));
  }();
  return bitmap;
}

/// 高频条纹背板（`--debug-stripes`）：量折射位移与色散用。
///
/// ## 条纹周期必须**大于**模糊半径，否则测的不是折射
///
/// 第一版用了 6px 周期（跟旧的弹层实验一致），实测结果是**玻璃内一片均匀色**：
/// 模糊半径 22px 的盒式均值把三个以上周期全抹平了，而折射位移只是 14px——
/// 无论采样点怎么移，邻域均值都是同一个数。于是"折射生效"与"折射没接上"
/// 在这个背景下给出**逐像素完全一样**的图（实测：边缘带与中心都是 78,28,35）。
///
/// 正确口径是**色块周期 ≳ 2×模糊半径**：这样每个采样点落在哪个色块上才决定平均值，
/// 位移才表现为“边缘处的颜色与中心不同”。48px 色块隔 48px、周期 96px，
/// 对半径 22 / 位移 14 这组参数够用。
[[nodiscard]] auto build_debug_stripes() -> std::unique_ptr<Panel> {
  auto stripes = std::make_unique<Panel>(FlexDirection::Column);
  stripes->set_id("debug-stripes");
  stripes->style().absolute = true;
  stripes->style().left = 0.0f;
  stripes->style().right = 0.0f;
  stripes->style().bottom = 0.0f;
  stripes->style().height = 300.0f;
  // **色块必须铺到屏幕底**（含 Dock 背后）：绝对定位列的高度只决定自己的框，
  // 子项不满时会留空——而那些留空的像素恰好是 Dock 要模糊的东西。
  // 第一版写死 4×48=192，下面留了 108px 空区，玻璃背后就是空壁纸，
  // 于是"折射生没生效"在截图上完全看不出来（实测：边缘带与中心逐像素相同）。
  // 用 `grow` 让四行均分整框，覆盖区域随高度自洽。
  for (int i = 0; i < 4; ++i) {
    auto stripe = std::make_unique<Panel>(FlexDirection::Row);
    stripe->style().grow = true;
    stripe->style().set_solid_background((i % 2 == 0) ? Color{0xC8, 0x38, 0x38, 0xFF}
                                                     : Color{0x18, 0x08, 0x10, 0xFF});
    stripes->add_child(std::move(stripe));
  }
  return stripes;
}

// ─── 界面元素 ────────────────────────────────────────────────────────────────

/// 状态条：左上角小字（"就绪 / 已启动 X"）。玻璃底，不抢注意力。
[[nodiscard]] auto make_status_chip(const ShellState& state) -> std::unique_ptr<Element> {
  auto chip = std::make_unique<Panel>(FlexDirection::Row);
  chip->set_id("status-chip");
  chip->style().absolute = true;
  chip->style().left = 26.0f;
  chip->style().top = 24.0f;
  chip->style().height = 32.0f;
  chip->style().padding = Insets::symmetric(15.0f, 0.0f);
  chip->style().radius = 16.0f;
  chip->style().align_items = st::ui::Align::Center;
  // 同 Dock 口径：用同一份玻璃参数（只是半径与折射按比例弱一档）。
  //
  // 为何不另写一套数字：两处玻璃要“看起来是同一种材质”，而分别拍值必然漂。
  // 小尺寸元素上同样的 22px 模糊会把整个底完全抹匀（半径比它自己的高度还大），
  // 所以按比例减——这是**尺寸**相关的差异，不是风格差异。
  Color chip_base = state.glass.base;
  chip_base.a = static_cast<std::uint8_t>(std::lround(
      st::math::clampf(state.glass.base_alpha, 0.0f, 1.0f) * 255.0f));
  chip->style().set_solid_background(chip_base);
  chip->style().backdrop_blur = state.glass.blur * 0.6f;
  chip->style().backdrop_tint = state.glass.tint;
  chip->style().backdrop_refraction = state.glass.refraction * 0.55f;
  chip->style().backdrop_dispersion = state.glass.dispersion;
  chip->style().top_highlight = Color{0xFF, 0xFF, 0xFF, 0x3C};
  chip->style().highlight_width = 1.0f;

  auto label = std::make_unique<Text>(state.status);
  label->set_id("status-text");
  label->set_scale(Text::Scale::Xs);
  label->set_text_color(Color{0xCC, 0xD6, 0xE8, 0xF0});
  chip->add_child(std::move(label));
  return chip;
}

/// 一个桌面图标格子：真实图标 + 文字 + 透明覆盖按钮（整格可点）。
[[nodiscard]] auto make_app_tile(ShellState& state, const DesktopApp& app,
                                 float icon_size) -> std::unique_ptr<Element> {
  const bool usable = !resolve_exe(app.exe).empty();

  auto cell = std::make_unique<Panel>(FlexDirection::Column);
  cell->set_id("tile-" + app.id);
  cell->style().width = kTileSize;
  cell->style().height = kTileSize;
  cell->style().gap = 8.0f;
  cell->style().align_items = st::ui::Align::Center;
  cell->style().padding = Insets::all(6.0f);

  auto icon = std::make_unique<AppIcon>(icon_of(state, app), app.glyph, icon_size);
  icon->set_id("icon-" + app.id);
  icon->set_dimmed(!usable);
  cell->add_child(std::move(icon));

  auto label = std::make_unique<Text>(app.label);
  label->set_scale(Text::Scale::Xs);
  label->set_text_color(usable ? Color{0xE8, 0xEC, 0xF4, 0xFF} : Color{0x76, 0x7C, 0x8A, 0xFF});
  label->set_id("label-" + app.id);
  cell->add_child(std::move(label));

  if (usable) {
    // 整格可点是 macOS 桌面图标语义。
    //
    // **必须绝对定位铺满整格**（2026-11-15 实测踩过）：直接 `add_child` 会把它
    // 当成 Column 的**第三个流内子元素**排在文字下面——实测图标占 y=82..150、
    // 而可点区落在 y=182..216（文字下方），用户看到的现象是“点图标没反应、
    // 点图标下面的空白才启动”。绝对定位 + 四边归零 = 盖住整格（含图标与文字）。
    //
    // 可点区比图标大是有意的：图标只有 68px，而 macOS 的桌面图标整格（含文字）
    // 都可点——小图标更好点中。
    auto hit = std::make_unique<Button>("", st::ui::Variant::Ghost);
    hit->set_id("launch-" + app.id);
    hit->style().absolute = true;
    hit->style().left = 0.0f;
    hit->style().top = 0.0f;
    hit->style().right = 0.0f;
    hit->style().bottom = 0.0f;
    DesktopApp copy = app;
    ShellState* state_ptr = &state;
    hit->on_click = [copy, state_ptr]() {
      (void)launch_app(copy, state_ptr->status);
      refresh_status(*state_ptr);
      if (state_ptr->app != nullptr) state_ptr->app->request_repaint();
    };
    cell->add_child(std::move(hit));
  }
  return cell;
}

/// 图标网格：`kGridColumns` 列，按行换行（超出就往下铺）。
///
/// 为什么是"横向分组的行"而不是一个 `wrap=true` 的 Panel：换行宽度要靠显式宽度
/// 约束才能生效，而桌面要**跟随窗口宽度**（切回窗口模式后网格不该溢出屏幕）。
/// 行分组让每行都是普通 flex，宽度随父容器自适应。
[[nodiscard]] auto build_icon_grid(ShellState& state) -> std::unique_ptr<Element> {
  auto grid = std::make_unique<Panel>(FlexDirection::Column);
  grid->set_id("icon-grid");
  grid->style().absolute = true;
  grid->style().left = 26.0f;
  grid->style().top = 76.0f;
  grid->style().gap = kTileGap;
  grid->style().align_items = st::ui::Align::Start;

  auto row = std::make_unique<Panel>(FlexDirection::Row);
  row->style().gap = kTileGap;
  row->style().align_items = st::ui::Align::Start;
  int column = 0;
  for (const DesktopApp& app : state.apps) {
    row->add_child(make_app_tile(state, app, 68.0f));
    if (++column >= kGridColumns) {
      grid->add_child(std::move(row));
      row = std::make_unique<Panel>(FlexDirection::Row);
      row->style().gap = kTileGap;
      row->style().align_items = st::ui::Align::Start;
      column = 0;
    }
  }
  if (column > 0) grid->add_child(std::move(row));
  return grid;
}

/// Dock 里的一格：小图标（点击启动）。
[[nodiscard]] auto make_dock_item(ShellState& state, const DesktopApp& app)
    -> std::unique_ptr<Element> {
  const bool usable = !resolve_exe(app.exe).empty();

  // 图标尺寸**随任务栏高度自适应**（不写死）。
  //
  // 为什么不能写死：Dock 高度现在等于系统任务栏厚度（100% 缩放是 48，125% 是 60）。
  // 写死 42px 的图标在 48px 的条里上下只剩 1px（贴边），而在 60px 的条里又缩成一小块。
  // 取 62% 是“有留白但不空旷”的比例；下限 20px 防止极小任务栏（用户开了小按钮）把
  // 图标压没。
  const float icon_size = std::max(20.0f, state.dock.height * 0.62f);
  const float slot_size = std::min(state.dock.height - 4.0f, icon_size + 6.0f);

  auto slot = std::make_unique<Panel>(FlexDirection::Column);
  slot->set_id("dock-item-" + app.id);
  slot->style().width = slot_size;
  slot->style().height = slot_size;
  slot->style().align_items = st::ui::Align::Center;
  slot->style().justify = st::ui::Justify::Center;

  auto icon = std::make_unique<AppIcon>(icon_of(state, app), app.glyph, icon_size);
  icon->set_id("dock-icon-" + app.id);
  icon->set_dimmed(!usable);
  slot->add_child(std::move(icon));

  if (usable) {
    // 同桌面格：**绝对定位铺满**，否则它会作为第二个流内子元素被排到图标下面
    //（实测：图标 y=683..725，可点区落在 y=725..759）。
    auto hit = std::make_unique<Button>("", st::ui::Variant::Ghost);
    hit->set_id("dock-launch-" + app.id);
    hit->style().absolute = true;
    hit->style().left = 0.0f;
    hit->style().top = 0.0f;
    hit->style().right = 0.0f;
    hit->style().bottom = 0.0f;
    DesktopApp copy = app;
    ShellState* state_ptr = &state;
    hit->on_click = [copy, state_ptr]() {
      (void)launch_app(copy, state_ptr->status);
      refresh_status(*state_ptr);
      if (state_ptr->app != nullptr) state_ptr->app->request_repaint();
    };
    slot->add_child(std::move(hit));
  }
  return slot;
}

/// Dock：底部**液态玻璃**通栏任务栏。
///
/// 玻璃四段参数的分工（与 `Style` 的字段一一对应）：
/// - `backdrop_blur`：各向同性模糊（毛玻璃本体）；
/// - `backdrop_refraction`：圆角 SDF 法线方向的采样位移——边缘把背景“吸入”，
///   这是“液态”与“磨砂”的区别所在；
/// - `backdrop_dispersion`：R/B 通道沿法线错位，边缘出现细微彩虹纹；
/// - `backdrop_tint`：玻璃上那层半透明着色（只模糊不叠色会读不出文字）。
///
/// ## 三个参数互相说谎：要“透”就要同时动它们（2026-11-15 实测）
///
/// 用户反馈“不够透亮”后测得两个**正交量**（不能只看亮度）：
///
/// | 量 | 含义 | 只看它会错在哪 |
/// |---|---|---|
/// | Dock−壁纸亮度差 | “亮不亮” | 只要把底色改成浅灰就能“变亮”，但板子仍是死的 |
/// | 纹理透过率（内部标准差 / 背景标准差） | “透不透” | 只看它会以为调 tint 有用，而真因是 blur 把 2~3px 的纹理抹平了 |
///
/// 实测：只把深底改浅底（tint 0.55→0.34）+ 换亮壁纸，亮度差从 −16.2 翻到 **+13.4**，
/// 而纹理透过率只从 34% → **36%**——板子依旧是一块均匀的青灰（内部 R 极差 16，
/// 而壁纸同宽极差 39）。真因是 **blur 22px 把石纹（2~3px 裂痕）全抹平了**。
///
/// 所以最终参数是三条一起改：**blur 降一档**（保留可辨纹理）、**tint 降**（少盖色）、
/// **圆角半径进 SDF**（折射靠它，与视觉圆角同值）。
///
/// 降级契约：无 GPU（软件/无头腿）时代 `supports_blur()` 为假 → 自动跳模糊与折射，
/// 用 `set_solid_background` 的实色底，界面仍正确（只是不“玻璃”）。
[[nodiscard]] auto build_dock(ShellState& state, const GlassParams& glass)
    -> std::unique_ptr<Element> {
  // ## Dock 是**通栏**的一条，不是浮在中间的圆角条
  //
  // 需求是“与系统任务栏等高、避免最大化窗口遮蔽和间隙”。两件事都要求它**没有边距**：
  //
  // | 有底边距/圆角 | 后果 |
  // |---|---|
  // | 底边留 12px | 屏幕底与 Dock 之间存在一条缝，露出壁纸 |
  // | 高度比任务栏矮 | 最大化窗口的底边（`rcWork.bottom`）落在 Dock 顶缘之上 → **遮蔽** |
  // | 高度比任务栏高 | Dock 顶缘抬高 → 上方多 24px 被永久占掉（窗口可用区变矮且**有缝**） |
  //
  // 所以这里：底边距 0、高度 = 系统任务栏厚度、左右通栏。
  // 圆角只在**上方两角**有意义（下方贴屏幕底，圆角在那里反而露出两个缺角）。
  auto wrap = std::make_unique<Panel>(FlexDirection::Row);
  wrap->set_id("dock-wrap");
  wrap->style().absolute = true;
  wrap->style().left = 0.0f;
  wrap->style().right = 0.0f;
  wrap->style().bottom = 0.0f;
  wrap->style().height = state.dock.height;
  wrap->style().align_items = st::ui::Align::Center;
  wrap->style().justify = st::ui::Justify::Center;

  auto dock = std::make_unique<Panel>(FlexDirection::Row);
  dock->set_id("dock");
  // 高度与父一致（`grow` 在 Row 里是**主轴**扩展，所以纵向用显式高度）。
  dock->style().height = state.dock.height;
  dock->style().width = st::ui::kAuto;
  dock->style().grow = true;
  // 直角：通栏贴底的条，下方两角直接贴屏幕边缘（见 `DockGeometry` 的说明）。
  // 框架的 `Style::radius` 是四角统一的，给非零值会在屏幕底部露出两个缺角。
  dock->style().radius = state.dock.radius;
  // 降级态（无 GPU）也成立：模糊没发生时它就是一块半透明底。
  //
  // ## 为何是**浅色**底而不是深色底（2026-11-15 实测修正）
  //
  // 第一版用 `0x14,0x1A,0x2C @ 0xB4`（深蓝黑、70% 不透明）。实测 Dock 内部
  // 比它背后的壁纸**暗 16.2**（35.4 vs 51.7）——即玻璃在**压暗**背景，于是
  // “透”无从谈起：模糊与折射其实都在，只是被那层深色底盖住了（纹理标准差
  // 只透出 34%）。用户的反馈“不够透亮”说的就是这个。
  //
  // 底色改成**中性浅灰**（不自带强烈蓝色调）+ tint 降——玻璃后面亮的地方就显亮。
  Color base = glass.base;
  base.a = static_cast<std::uint8_t>(std::lround(
      st::math::clampf(glass.base_alpha, 0.0f, 1.0f) * 255.0f));
  dock->style().set_solid_background(base);
  dock->style().backdrop_blur = glass.blur;
  dock->style().backdrop_tint = glass.tint;
  dock->style().backdrop_refraction = glass.refraction;
  dock->style().backdrop_dispersion = glass.dispersion;
  // 顶部高光：玻璃“亮”的观感很大一部分来自这条内边缘高光（与 macOS/Windows
  // 的亚克力同手法）。之前 0x30 偏保守，抬到 0x4A 让边界先亮起来。
  dock->style().top_highlight = Color{0xFF, 0xFF, 0xFF, 0x4A};
  dock->style().highlight_width = 1.5f;
  dock->style().align_items = st::ui::Align::Center;
  dock->style().justify = st::ui::Justify::Center;
  dock->style().gap = 20.0f;
  dock->style().padding = Insets::symmetric(30.0f, 0.0f);

  for (const DesktopApp& app : state.apps) {
    dock->add_child(make_dock_item(state, app));
  }
  wrap->add_child(std::move(dock));
  return wrap;
}

// ─── 启动 / 收尾 ─────────────────────────────────────────────────────────────

/// 进程退出前必须还原系统任务栏（桌面壳 hid 了它）。
///
/// 为什么单独一个 RAII 而不是散在各处：退出路径不止一条（正常 `app.run()` 返回、
/// 启动失败提前 return、控制通道 `app.quit`）。漏一条就是"用户桌面没有任务栏"。
struct TaskbarGuard {
  st::app::Application* app{nullptr};
  bool active{false};
  ~TaskbarGuard() {
    if (active && app != nullptr) {
      if (auto status = app->set_system_taskbar_visible(true); !status) {
        st::log::err("系统任务栏还原失败：{}（请手动重启 explorer）\n",
                     status.error().message);
      }
    }
  }
};

/// 把选项收敛成"桌面壳"形态：铺满显示器 + 置底 + 隐藏系统任务栏。
void apply_shell_options(st::app::AppOptions& options) {
  options.fill_monitor = true;
  options.fill_work_area = false;   // 与 fill_monitor 互斥（它是 92% 工作区口径）
  options.stay_at_bottom = true;
  options.system_taskbar_visible = false;
  // 桌面不是普通窗口：不能拖边框改尺寸（拖了也只会露出后面的原生桌面），
  // 也不挂 DWM 外阴影（整屏窗口的"影子"没有意义，还会在边缘露出一圈）。
  options.resizable = false;
  options.window_shadow = false;
  // 启动卡在整屏窗口上会闪一下全屏底色（`splash_card_window` 先把窗口缩成卡片再
  // 放大），桌面壳不需要它：图标网格本身就是启动画面。
  options.splash = false;
}

}  // namespace

// ─── 任务栏守卫（见 apply_shell_options 与 build_dock 之后的说明） ──────────────
//
// ## 为什么需要它（2026-11-15，实测代价）
//
// 桌面壳会 `ShowWindow(SW_HIDE)` **explorer 的系统任务栏**——那是进程外的全局
// 状态。正常退出有 RAII，但这三条路径**一行 C++ 都不会执行**：
//   任务管理器“结束任务”/`TerminateProcess`／进程崩溃／调试器强杀。
// 实测（`tools/desktop_taskbar_kill_probe.py`）：强杀之后用户的桌面**永久没有任务栏**，
// 而用户完全不知道该怎么同复。
//
// 所以隐蒕任务栏时同时拉起**本 exe 的同名子进程**（`--taskbar-guard <父pid>`）：
// 它不建窗口、不读配置、不画任何东西，只在循环里探父进程是否还活着，
// 一旦父走了（**包括被强杀**）→ 把任务栏显回来 → 退出。
//
// ## 为什么是“轮询父 pid”而不是继承父句柄
//
// 继承真实进程句柄要 `STARTUPINFOEX` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`
// （`SetHandleInformation` 只对标准句柄有效），而这里要的只是“父还在不在”：
// 500ms 粒度对一个“人不会盯着看”的收尾动作完全够，且不引入句柄泄露风险。
//
// ## 守卫自己被杀怎么办
//
// 那就回到原本的问题（用户需手动还原）。这是可接受的降级：多一层守卫已经堵住了
// “强杀桌面”这个最常见的缺口；两层都杀是双重故障。
namespace {

#if defined(_WIN32)
constexpr wchar_t kTaskbarWindowClass[] = L"Shell_TrayWnd";

/// 父进程是否还在（`OpenProcess` 只读信号，不控制生命期）。
[[nodiscard]] auto process_alive(std::uint32_t pid) -> bool {
  // 本文件是应用层，不是 `platform_*`——所以对 L14 的预期是「不要碰系统头」。
  // 但桌面守卫的本质就是“看另一个进程”，没有平台中立封装能表达它。
  // 代价是这条豁免必须登记（见 CONVENTIONS.md §8 的 L14 表）。
  HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return false;
  DWORD exit_code = 0;
  const BOOL ok = ::GetExitCodeProcess(process, &exit_code);
  ::CloseHandle(process);
  return ok != 0 && exit_code == STILL_ACTIVE;
}

/// 守卫主循环：父进程活着就等着，父进程没了就还原任务栏并退出。
[[nodiscard]] auto run_taskbar_guard(std::uint32_t parent_pid) -> int {
  while (process_alive(parent_pid)) {
    ::Sleep(500);
  }
  if (HWND taskbar = ::FindWindowW(kTaskbarWindowClass, nullptr); taskbar != nullptr) {
    ::ShowWindow(taskbar, SW_SHOW);
  }
  st::log::out("任务栏守卫：父进程 {} 已退出，系统任务栏已还原\n", parent_pid);
  return 0;
}
#endif

}  // namespace

// ─── 入口 ───────────────────────────────────────────────────────────────────

auto run_app(int argc, char** argv) -> int {
  // **守卫模式必须最先拦**（在任何 Application/字体/控制通道之前）：
  // 守卫不建窗、不读配置、不画界面，越早分叉越不容易被启动期其他初始化干扰。
  // 它每 500ms 探一次父进程，父进程没了就还原系统任务栏然后退出。
#if defined(_WIN32)
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view(argv[index]) == "--taskbar-guard") {
      try {
        return run_taskbar_guard(
            static_cast<std::uint32_t>(std::stoul(argv[index + 1])));
      } catch (const std::exception&) {
        st::log::err("任务栏守卫：父 pid 参数非法（{}）\n", argv[index + 1]);
        return 2;
      }
    }
  }
#endif

  st::app::CommonOptions common;
  st::app::AppOptionValues values;
  const std::vector<st::app::AppOption> extra{
      st::app::flag("--debug-stripes", "壁纸换成高频条纹（量折射位移与色散）"),
      st::app::takes_value("--config", "应用清单 JSON 路径（默认 exe 同目录 desktop.json）"),
      st::app::flag("--windowed", "以普通窗口模式启动（不铺满、不置底、不动系统任务栏）"),
      // 玻璃调参：`透亮`是观感指标，需要能**不重编就扫档**（同一把尺子量完再定默认）。
      st::app::takes_value("--glass-blur", "玻璃模糊半径（逻辑 px，默认 10）"),
      st::app::takes_value("--glass-tint", "玻璃着色不透明度 0..1（默认 0.30）"),
      st::app::takes_value("--glass-refraction", "折射强度（逻辑 px，默认 14）"),
      st::app::takes_value("--glass-dispersion", "色散强度（默认 0.4）"),
      st::app::takes_value("--glass-alpha", "降级底不透明度 0..1（默认 0.55）"),
  };
  const auto parsed = st::app::parse_options(argc, argv, extra, common, values);
  if (!parsed) {
    st::log::err("st_desktop: {}\n", parsed.error().to_string());
    return 2;
  }
  if (common.show_help) {
    st::log::out("{}", st::app::options_usage("st_desktop", extra));
    return 0;
  }

  st::app::AppOptions options = common.app;
  options.title = "霜天桌面";
  const bool shell_mode = !values.has("--windowed");
  if (shell_mode) apply_shell_options(options);

  const std::string config_path =
      values.get("--config", default_config_path());
  std::vector<DesktopApp> apps;
  if (auto loaded = load_apps(config_path); loaded.has_value()) {
    apps = std::move(*loaded);
  } else {
    apps = default_apps();
    st::log::out("使用内置应用清单（{} 项）；可写 {} 自定义\n", apps.size(), config_path);
  }

  // 图标解码：内置嵌入的那份（`icon`）+ 可选的 `icon_path`（外部文件优先）。
  //
  // 为什么外部优先：`desktop.json` 里显式写 `icon_path` 就是要覆盖内置那份
  // （用户想换图标时不必重编 exe）。
  // 玻璃参数：默认值在 `GlassParams` 里，命令行可覆盖（扫档/调参用）。
  GlassParams glass;
  const auto read_float = [&values](std::string_view name, float fallback) {
    const std::string text = values.get(name);
    if (text.empty()) return fallback;
    try {
      return std::stof(text);
    } catch (const std::exception&) {
      // 与框架同一个姿态：解析失败不静默吞掉，但也不阻止启动。
      st::log::err("参数 {} 取值非法：{}（用默认 {}）\n", name, text, fallback);
      return fallback;
    }
  };
  glass.blur = std::max(0.0f, read_float("--glass-blur", glass.blur));
  glass.tint = st::math::clampf(read_float("--glass-tint", glass.tint), 0.0f, 1.0f);
  glass.refraction = std::max(0.0f, read_float("--glass-refraction", glass.refraction));
  glass.dispersion = std::max(0.0f, read_float("--glass-dispersion", glass.dispersion));
  glass.base_alpha = st::math::clampf(read_float("--glass-alpha", glass.base_alpha), 0.0f, 1.0f);
  st::log::out("玻璃参数：blur={} tint={} refraction={} dispersion={} base_alpha={}\n",
               glass.blur, glass.tint, glass.refraction, glass.dispersion, glass.base_alpha);

  ShellState state;
  state.glass = glass;
  state.apps = std::move(apps);
  state.debug_stripes = values.has("--debug-stripes");
  const auto& embedded = builtin_icons();
  for (const IconEmbed& entry : embedded) {
    state.icons[entry.first] = IconBitmap::from_png(entry.second);
  }
  for (const DesktopApp& app : state.apps) {
    if (app.id.empty()) continue;
    if (!app.icon_path.empty()) {
      state.icons[app.id] = IconBitmap::from_png_file(app.icon_path);
    }
    // `icon` 指向内置名时，把那份位图也挂到本应用 id 下（`icon_of` 只按 id 查）。
    if (!app.icon.empty() && state.icons.find(app.id) == state.icons.end()) {
      if (const auto found = state.icons.find(app.icon); found != state.icons.end()) {
        state.icons[app.id] = found->second;
      }
    }
  }

  st::app::Application app("st_desktop", "1.0.0", options);
  // Dock 几何先按**当前已知**的值算一份（构造后、start 前：视口还是 --width/--height，
  // 工作区已能从后端问到）。真正的重算在 `start()` 之后（见下）——那时视口才是
  // 真实窗口尺寸（`fit_window_to_work_area` / 启动小窗恢复都会改它）。
  state.dock = resolve_dock_geometry(app.viewport(), app.work_area_logical());

  state.app = &app;

  // —— 桌面根 ——
  //
  // 层次（自下而上）：壁纸（**真实纹理位图**）→ （可选）调试条纹 → 图标网格 →
  // 状态条 → Dock。壁纸是整窗底图，不再是“渐变 + 色阶”：
  // 硬边色阶看起来像渲染 bug（见 `wallpaper_bitmap` 的说明）。
  auto root = std::make_unique<Panel>(FlexDirection::Column);
  root->set_id("desktop-root");
  // 纯色底只是壁纸缺图/未画满时的兑底（与壁纸主色接近，不会跳变）。
  root->style().set_solid_background(Color{0x0E, 0x18, 0x1E, 0xFF});

  auto wallpaper = std::make_unique<Wallpaper>(wallpaper_bitmap());
  wallpaper->set_id("wallpaper");
  wallpaper->style().absolute = true;
  wallpaper->style().left = 0.0f;
  wallpaper->style().top = 0.0f;
  wallpaper->style().right = 0.0f;
  wallpaper->style().bottom = 0.0f;
  root->add_child(std::move(wallpaper));

  // `--debug-stripes`：只用于量折射/色散，**不是默认壁纸**。
  // 它盖在壁纸之上、图标之下（量的是 Dock 背后那一层）。
  if (state.debug_stripes) root->add_child(build_debug_stripes());
  root->add_child(build_icon_grid(state));
  root->add_child(make_status_chip(state));
  root->add_child(build_dock(state, state.glass));
  app.set_content(std::move(root));

  TaskbarGuard guard{&app, shell_mode && options.system_taskbar_visible == false};
  if (auto started = app.start(); !started) {
    st::log::err("st_desktop: {}\n", started.error().to_string());
    return 1;
  }

  // ## Dock 几何在这里**重算并重建**（为什么不能在建界面前算）
  //
  // `start()` 之前 `app.viewport()` 报的是 `--width/--height` 的初值，而不是真实
  // 窗口尺寸——`fit_window_to_work_area`（工作区 92%）与启动小窗恢复都在 `start()`
  // 里改它，直到 `start()` 末尾才把视口同步成后端真实尺寸。
  //
  // 实测代价（没重算的版本）：日志报“视口高 720 / 系统任务栏占 0”，于是走了兜底
  // 48px——数字碰巧对，但那是**猜的**，在 1.25x 缩放的机器上就会错（应为 60）。
  // 而且“视口高 720”本身就说明拿错了源：真实是 754。
  const st::math::Size viewport = app.viewport();
  const st::math::Size work = app.work_area_logical();
  const DockGeometry dock = resolve_dock_geometry(viewport, work);
  st::log::out("Dock 几何：高 {} px（视口高 {} / 工作区高 {}，系统任务栏占 {}），顶缘 {}\n",
               dock.height, viewport.height, work.height,
               viewport.height - work.height, dock.top);
  if (std::abs(dock.height - state.dock.height) > 0.5f) {
    // 只有真的不同才重建（避免无谓的一帧重排）。
    state.dock = dock;
    // 取出根内容 → 换掉 Dock → 放回。用 `take_content` 而不是 `set_content(nullptr)`：
    // 后者会把整棵树销毁，图标位图等状态全丢。
    auto content = app.root().take_content();
    if (content != nullptr) {
      // Dock 是最后一个子元素（见上面的层次说明）。
      const std::size_t count = content->child_count();
      if (count > 0) {
        if (Element* last = content->child_at(count - 1); last != nullptr) {
          // `remove_child` 返回的 `unique_ptr` 在这里就地析构（该子树不再被任何人持有）。
          (void)content->remove_child(last);
        }
        content->add_child(build_dock(state, state.glass));
      }
    }
    app.set_content(std::move(content));
    app.request_repaint();
  }

  // 启动即接管系统任务栏的还原责任：从这里往后任何退出路径都要还原。
  return app.run().value_or(0);
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
