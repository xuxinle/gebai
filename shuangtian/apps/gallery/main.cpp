/// 霜天组件画廊（gallery）——框架的"美观"验证载体与无头协同开发的主要靶场。
///
/// 用法：
///   gallery --headless [--frames 3] [--scale 2.0] [--control-port 0] [--control-file path]
///           [--theme dark] [--shots DIR] [--bench N]
/// 无头模式下不创建窗口，控制通道（TCP）承担全部交互：tree / find / get / set / invoke /
/// input.* / capture / visual / wait / metrics —— 智能体可据此开发与验证界面。
///
/// 本文件只负责**装配**（顶部栏 / 侧栏导航 / 滚动容器 / 状态栏 / 导航切页）；
/// 页面内容在 `pages.cpp`（一页一个函数）。切页用 `set_visible`——
/// 框架在 measure/arrange/paint/hit_test/semantics/visual 六处都尊重 `visible`，
/// 所以隐藏页零布局零绘制，且**组件状态不丢**（输入框里的文字、表格选中行切回来还在）。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <ranges>     // std::ranges::sort/unique/binary_search（树自检集合运算）
#include <string>
#include <thread>
#include <vector>

#include "pages.hpp"
#include "../app_icon.hpp"   // 图标两件套：PNG（窗口/任务栏）+ SVG（标题栏品牌）
#include "st/app/app.hpp"
#include "st/app/cli.hpp"   // 通用参数解析（框架统一入口；本应用只声明特有参数）
#include "st/core/entry.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"   // 长驻子进程（演示用浏览器）：StreamHandle::process_id
#include "st/core/string.hpp"   // `parse_f64` / `parse_i64`（参数取值校验）
#include "st/core/time.hpp"
#include "st/core/log.hpp"
#include "st/perf/report.hpp"
#include "st/perf/startup.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/window_frame.hpp"   // 自绘外壳（标题栏 + 内容槽 + 缩放边缘）
#include "st/ui/dsl.hpp"   // 声明式页的宿主（`dsl::mount_into`）
#include "st/ui/icon.hpp"

// 展示集登记真源（tests 与 gallery 共用；树自检的对账基准，不手抄）。
#include "../../tests/support/gallery_exhibits.hpp"

namespace {

/// 树自检（39 轮对账单未竟项②：登记与实物一致性由运行时实证，不再靠人工 grep）。
///
/// 遍历启动树收集 type()，与登记集（kGalleryExhibits 真源）双向对账：
/// - **全展示**：每个非懒 exhibit（kGalleryExhibits − kGalleryLazy）至少上树一次
///   ——「登记了却没真卡」当场红；懒创建卡（kGalleryLazy）另有触发钮在场断言。
/// - **登记外放行**：树上出现但不在登记集的 type 必须在放行清单里逐名白名单
///   （注入型/自绘类，见 gallery_exhibits.hpp 头部口径）；新名字冒出来即红
///   ——登记集漂移（漏登/改名）无处藏。
/// - **注入型在场**：建卡期即随父组件出现的注入型（ListItem/ScrollBar/TitleBar）
///   不在树上也要红（防放行清单烂成坟场，与豁免两条铁律同族）；MenuPanel 是菜单卡
///   交互触发（make_panel 在回调里），启动不要求在场，仅登记外放行。
///
/// 集合运算用 vector + std::ranges::sort/unique（避开 set 红黑树首用例重分配的
/// 真实性陷阱——disjoint 教训；两个 std::set 头文件从不引入）。
auto audit_gallery_tree(const st::ui::UiRoot& root) -> void {
  // 树上 type 收集（递归；children() span 直接遍历）。
  std::vector<std::string> seen;
  const auto collect = [&seen](const st::ui::Element& node, auto&& self) -> void {
    seen.emplace_back(node.type());
    for (const auto& child : node.children()) {
      if (child != nullptr) self(*child, self);
    }
  };
  if (const st::ui::Element* content = root.content(); content != nullptr) {
    collect(*content, collect);
  }
  std::ranges::sort(seen);
  seen.erase(std::ranges::unique(seen).begin(), seen.end());

  // 登记集（真源拷入 vector；懒创建单独一份）。拷贝后统一排序——三份都不是
  // 字典序（注册序/触发钮序/分组序），而下方全部 binary_search 都要求有序。
  std::vector<std::string> exhibits{gallery::kGalleryExhibits.begin(),
                                   gallery::kGalleryExhibits.end()};
  std::vector<std::string> lazy{gallery::kGalleryLazy.begin(),
                                gallery::kGalleryLazy.end()};
  std::vector<std::string> allowed{
      // —— 放行清单（与 gallery_exhibits.hpp 头部口径逐条对应）——
      // 注入型（前三个建卡期即在场，见下方 injected 断言；MenuPanel 是菜单卡
      // 点击后 make_panel 才挂层的交互触发型——启动不在场，仅放行不要求在场）：
      "ListItem", "ScrollBar", "MenuPanel", "TitleBar",
      // 非注册的框架组件（gallery 直接 new，无展示义务）：SceneView 未
      // override type()，树上以基类默认「Element」露面（scene_view.hpp 无 type 重写）。
      "Element",
      // gallery 自绘演示类（pages.cpp 本地类）：
      "ProgressRow", "IconCell", "SvgIconCell",
      // 框架懒注入（交互触发，非登记实例）：
      "SelectPanel",
  };
  // 结构性豁免名同样放行：豁免理由是「无展示价值/父组件内建」，不是「不上树」
  // ——例：声明式页的 dsl::spacer() 会把「Spacer」造上树；豁免里的注入型名字
  //（ListItem/ScrollBar/MenuPanel）与上面重复登记也无妨（排序去重后等价）。
  for (const auto& [name, reason] : gallery::kGalleryExempt) {
    (void)reason;
    allowed.emplace_back(name);
  }
  std::ranges::sort(exhibits);
  std::ranges::sort(lazy);
  std::ranges::sort(allowed);
  allowed.erase(std::ranges::unique(allowed).begin(), allowed.end());
  // ⚠ Tooltip 是懒注入（hint-bubble）+ 直接卡同 type：直接卡在场由 exhibits 断言
  // 盯住，这里不另列放行（树上有它不红——它本就在登记集里）。

  std::vector<std::string> missing;   // 登记了却不上树（非懒）
  std::vector<std::string> injected;  // 放行清单里要求在场却缺席的注入型
  for (const std::string& name : exhibits) {
    if (std::ranges::binary_search(lazy, name)) continue;  // 懒创建：启动树不要求在场
    if (!std::ranges::binary_search(seen, name)) missing.push_back(name);
  }
  // 注入型在场：只有「建卡期即随父组件出现」的三类（List 卡的 ListItem、
  // ScrollView 构造内建的 ScrollBar、WindowFrame 恒建的 TitleBar）要求在场；
  // MenuPanel 是菜单卡交互触发（make_panel 在回调里），启动树不要求。
  for (const std::string_view name : {"ListItem", "ScrollBar", "TitleBar"}) {
    if (!std::ranges::binary_search(seen, std::string{name})) injected.emplace_back(name);
  }

  // 登记外：树上出现、但不在（登记 ∪ 懒 ∪ 放行）里的 type。
  std::vector<std::string> unexpected;
  for (const std::string& name : seen) {
    const bool known = std::ranges::binary_search(exhibits, name) ||
                       std::ranges::binary_search(lazy, name) ||
                       std::ranges::binary_search(allowed, name);
    if (!known) unexpected.push_back(name);
  }

  // 对账结论：不达标不阻断启动（展示型自检，失败也照常进入主循环），
  // 但逐条打印——无头 e2e/人工都能从启动日志直接看到。
  const bool ok = missing.empty() && injected.empty() && unexpected.empty();
  if (!ok) {
    st::log::out("[gallery 树自检] 登记与实物不符：\n");
    for (const std::string& name : missing) {
      st::log::out("  - 展示集登记了「{}」但启动树上没有（非懒卡应建卡即上树）\n", name);
    }
    for (const std::string& name : injected) {
      st::log::out("  - 放行清单要求「{}」在场（注入型，父组件应在）但树上没有\n", name);
    }
    for (const std::string& name : unexpected) {
      st::log::out("  - 树上出现登记集外的「{}」（新组件？该登记进 kGalleryExhibits 或放行）\n", name);
    }
  } else {
    st::log::out("[gallery 树自检] 通过：树上 {} 种 type，登记 {}（含懒 {}）/豁免外放行 {}\n",
              seen.size(), exhibits.size(), lazy.size(), allowed.size());
  }
}

using st::math::Insets;
using st::ui::Align;
using st::ui::Button;
using st::ui::Element;
using st::ui::FlexDirection;
using st::ui::Heading;
using st::ui::IconView;
using st::ui::Justify;
using st::ui::Panel;
using st::ui::ScrollView;
using st::ui::Text;
using st::ui::TextAlign;
using st::ui::Tone;
using st::ui::UiRoot;

/// 本应用特有参数（框架通用开关全部在 `st::app::CommonOptions` 里）。
///
/// 为何只留这三项：`--headless`/`--renderer`/`--theme` 等 24 个通用开关，本文件
/// 曾自己再接一遍，两个方向都出过事——框架新增的开关到这里被静默吐掉（实测：
/// 三档渲染出来的图几乎一样，差点被当成"三档观感接近"的结论），而这里的开关到
/// 别的应用又不存在。**参数的归属与"谁在用"一致**：通用的归框架，特有的归这张表。
struct Options {
  /// 框架的通用解析结果（`--frames`/`--ms`/`--perf-*`/`--shots`…）。
  ///
  /// 为何把它整个带在 `Options` 里而不抄成一个个字段：`--frames`/`--ms` 这类
  /// “跑多久后退出”的语义由框架定义（`max_frames` / `max_ms`），拆成同名字段
  /// 就成了第二份口径。应用只读它，不重新解释。
  st::app::CommonOptions common{};
  /// 渲染基准帧数（`--bench N`）：每帧**强制全量重绘**并报告分阶段分位耗时。
  std::uint32_t bench{0};
  /// 请求系统标题栏/边框（默认关：窗框一律自绘，见 `CONVENTIONS.md` §10 第 7 条）。
  bool decorations{false};
  /// 覆盖率 gamma 的**小字号档**：通用表里没有它——只对"正文 + 小字"的界面有意义。
  std::string text_gamma_small{"auto"};
};

/// 本应用特有参数表（框架通用开关**不在这里**，别重复声明）。
[[nodiscard]] auto gallery_options() -> std::vector<st::app::AppOption> {
  return {
      {.name = "--bench",
       .value_count = 1,
       .help = "渲染基准：跑 N 帧强制定量重绘并报告分阶段耗时"},
      {.name = "--decorations",
       .value_count = 0,
       .help = "使用系统标题栏/边框（默认自绘）"},
      {.name = "--dpi",
       .value_count = 1,
       .help = "--scale 的旧名（等价）"},
      {.name = "--text-gamma-small",
       .value_count = 1,
       .help = "小字号档的覆盖率 gamma（auto / off / 数值）"},
  };
}

/// 解析命令行 → `(应用特有选项, 可直接交给 Application 的 AppOptions)`。
///
/// 出错时**不 `exit`**：返回 `Result` 让入口统一处理。上一版这里用的是
/// `int_or_die`，实测 `--scale abc` 直接 `std::exit(2)`——把参数解析写成了
/// "会杀进程"的形状，测试与嵌入场景都没法调用。
///
/// `--dpi` 是 `--scale` 的旧名：两个名字落两个地方就必然有"谁覆盖谁"的歧义，
/// 所以在拼 `AppOptions` 之前先把别名折叠到同一个字段。
[[nodiscard]] auto parse_options(int argc, char** argv)
    -> st::Result<std::pair<Options, st::app::AppOptions>> {
  st::app::CommonOptions common;
  st::app::AppOptionValues values;
  auto parsed = st::app::parse_options(argc, argv, gallery_options(), common, values);
  if (!parsed) return st::forward_error(parsed.error());
  if (common.show_help) {
    st::log::out("{}", st::app::options_usage("gallery", gallery_options()));
    std::exit(0);
  }

  // 框架已按白名单/数值规则校验过 `--scale`；这里只需处理别名。
  if (const std::string dpi = values.get("--dpi"); !dpi.empty()) {
    auto number = st::parse_f64(dpi);
    if (!number.has_value() || *number <= 0.0) {
      return st::unexpected(st::ErrorCode::Invalid,
                            std::format("--dpi 的取值不是正数: {}", dpi));
    }
    common.app.scale = static_cast<float>(*number);
  }

  Options options;
  options.common = common;
  options.decorations = values.has("--decorations");
  options.text_gamma_small = values.get("--text-gamma-small", "auto");
  if (const std::string bench = values.get("--bench"); !bench.empty()) {
    auto number = st::parse_i64(bench);
    if (!number.has_value() || *number < 0) {
      return st::unexpected(st::ErrorCode::Invalid,
                            std::format("--bench 的取值不是非负整数: {}", bench));
    }
    options.bench = static_cast<std::uint32_t>(*number);
  }

  // 框架的解析结果**原样**成为启动参数：不再往下一层抄一遍。
  st::app::AppOptions app = common.app;
  app.title = "霜天 · 组件画廊";
  app.text_gamma_small = options.text_gamma_small;
  app.backend = !common.app.backend.empty()
                    ? common.app.backend
                    : (common.app.headless ? std::string("headless") : std::string{});
  // `--theme` 既接受明暗两档，也接受**内置主题名**（`cyberpunk`/`ink`/`matrix`…）。
  // 未知名字不静默回退——那会让"我选的主题没生效"变成谜——交由
  // `compose_theme_with` 报错（`theme_name` 随参数一起传下去）。
  app.theme = common.app.theme_name == "dark"    ? st::ui::ThemeMode::Dark
              : common.app.theme_name == "light" ? st::ui::ThemeMode::Light
                                                 : st::ui::ThemeMode::Dark;
  return std::make_pair(std::move(options), std::move(app));
}

/// 浏览器位置：从几个常见安装位置里找第一个存在的。
///
/// 为何不读注册表：注册表项名随浏览器版本/发行版（Stable/Beta/Chrome SxS）变化，
/// 而且每一家都不同；常见安装目录是**跨机器最稳**的那一条。找不到就如实报找不到。
[[nodiscard]] auto find_browser_executable() -> std::optional<std::string> {
#if defined(_WIN32)
  const auto env = [](std::string_view name) {
    const auto value = st::fs::read_env(name);
    return value.has_value() && !value->empty() ? std::optional<std::string>(*value)
                                                : std::nullopt;
  };
  std::vector<std::string> candidates;
  // 先看显式指定（自动化/别的机器上用得上）
  for (std::string_view name : {"GEBAI_DEMO_BROWSER", "ST_DEMO_BROWSER"}) {
    if (auto explicit_path = env(name); explicit_path.has_value()) {
      candidates.push_back(*explicit_path);
    }
  }
  for (std::string_view root : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
    const auto base = env(root);
    if (!base.has_value()) continue;
    for (std::string_view relative : {
             "\\Microsoft\\Edge\\Application\\msedge.exe",
             "\\Google\\Chrome\\Application\\chrome.exe",
         }) {
      candidates.push_back(*base + std::string(relative));
    }
  }
  for (const auto& candidate : candidates) {
    if (st::fs::is_regular_file(candidate)) return candidate;
  }
  return std::nullopt;
#else
  return std::nullopt;
#endif
}

/// gallery 的“原生嵌入”演示窗口来源。
///
/// ## 它做什么
///
/// 起一个**长驻**浏览器子进程（独立的用户数据目录，不碰用户自己的浏览器配置），
/// 再按 pid 找它的主窗口句柄交给页面嵌入。
///
/// ## 为何用独立用户数据目录
///
/// 浏览器已在运行时，新起的进程只会**往已有实例里开一个标签页然后自己退出**——
/// 那样拿到的 pid 立刻石。独立目录 + `--no-first-run` 才能拿到一个真正常驻的进程。
///
/// ## 进程活着但窗口没了怎么办
///
/// `acquire()` 按需一次：子进程已退出 → 重启；窗口句柄失效 → 重新按 pid 找。
/// 它**不拥有**窗口的生命周期（那是组件/调用方的事）。
class GalleryNativeHostSource final : public gallery::NativeHostSource {
 public:
  explicit GalleryNativeHostSource(st::app::Application& app) : app_(app) {}

  ~GalleryNativeHostSource() override {
    // 顺序：**先把窗口摘回桌面，再杀进程**。
    //
    // 为何不能反过来：本对象声明得**比 `app` 晚**（C++ 逆序析构 ⇒ 它先死），
    // 而界面树里还持着那个句柄。先杀进程 → 子窗口被连带销毁 → 之后界面树
    // 析构时的 `dismiss_child_view` 拿到的是已失效的 HWND（不可预期）。
    if (window_ != nullptr) {
      app_.dismiss_child_view(window_);
      window_ = nullptr;
    }
    // 结束演示浏览器进程：它是本次演示的**副产物**，不退会在后台常驻。
    if (process_.valid() && process_.still_running()) {
      process_.terminate();
      (void)process_.finish();
    }
  }

  [[nodiscard]] auto supported() const -> bool override {
    // 两个条件都满足才叫“能做”：后端有宿主能力 + 本机找得到浏览器。
    return app_.child_view_supported() && find_browser_executable().has_value();
  }

  [[nodiscard]] auto acquire() -> void* override {
    if (!app_.child_view_supported()) {
      status_ = "当前后端不支持嵌入（无头/未实现平台）";
      return nullptr;
    }
    // 已有窗口且进程还活着：直接给（重复点“嵌入”应当幂等）。
    if (window_ != nullptr && process_.valid() && process_.still_running()) {
      status_ = "已就绪";
      return window_;
    }
    // 进程在但窗口没了（关掉了）：重新按 pid 找一次。
    if (process_.valid() && process_.still_running()) {
      window_ = app_.find_window_by_process(process_.process_id());
      if (window_ != nullptr) {
        status_ = "已就绪（重新定位窗口）";
        return window_;
      }
    }
    if (!launch()) return nullptr;
    status_ = "浏览器已启动，等待窗口…";
    // 窗口不是 CreateProcess 返回就能用的：浏览器还要建窗口、加载页面。
    //
    // ⚠ 这里是**主线程**（按钮回调中）——探测循环的上限直接就是“界面冻结多久”。
    // 所以上限给得很小（0.8s）：换一个“稍后再点一次”的诚实提示，而不是把界面
    // 冻上几秒假装在忙。实测浏览器冷启动窗口通常 1~2s 才出来，第二次点击
    //（`acquire` 会再被调）就命中缓存。
    for (int attempt = 0; attempt < 8; ++attempt) {
      if (!process_.still_running()) break;
      window_ = app_.find_window_by_process(process_.process_id());
      if (window_ != nullptr) {
        status_ = "已就绪";
        return window_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    status_ = process_.still_running() ? "没等到窗口（稍后再点一次）" : "浏览器进程已退出";
    return nullptr;
  }

  [[nodiscard]] auto status_text() const -> std::string override { return status_; }

 private:
  /// 演示页（`data:` URL，不依赖网络也不依赖本地文件）。
  [[nodiscard]] static auto demo_page_url() -> std::string {
    // 只用 ASCII：命令行参数过 `CreateProcessW` 的窄转宽先前，非 ASCII 内容
    // 在旧控制台上容易被代码页搞坏。
    static constexpr std::string_view kHtml =
        "<!doctype html><meta charset=utf-8>"
        "<style>body{margin:0;font:15px/1.6 system-ui,sans-serif;color:#1d2330;"
        "background:linear-gradient(135deg,#eef2ff,#f8fafc 45%,#eefcf6)}"
        ".wrap{padding:44px 48px}h1{margin:0 0 6px;font-size:30px;letter-spacing:-.5px}"
        "p.sub{margin:0 0 28px;color:#5b6478}"
        ".cards{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;max-width:760px}"
        ".card{background:#fff;border:1px solid #e3e8f2;border-radius:12px;padding:18px;"
        "box-shadow:0 2px 10px rgba(30,50,90,.06)}"
        ".card b{display:block;font-size:13px;color:#3b5bdb;margin-bottom:6px}"
        "code{background:#f1f3f9;border-radius:5px;padding:2px 6px;font-size:12px}"
        "</style><div class=wrap><h1>这是真网页</h1>"
        "<p class=sub>由系统浏览器渲染，经平台合成器直接上屏——霜天的 framebuffer 里没有它。</p>"
        "<div class=cards>"
        "<div class=card><b>不进 framebuffer</b>capture 在这块区域拍到的是占位层，不是本页</div>"
        "<div class=card><b>看属性面</b><code>get native-host-demo</code> 读 attached / content_y</div>"
        "<div class=card><b>真实交互</b>网页里的滚动、选择、输入都是浏览器自己的能力</div>"
        "</div></div>";
    std::string url = "data:text/html,";
    for (const char ch : kHtml) {
      // `data:` URL 里空格与特殊字符要转义；这里只保留非保留字符，其余百分号编码。
      const auto byte = static_cast<unsigned char>(ch);
      const bool safe = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                        byte == '.' || byte == '~';
      if (safe) {
        url.push_back(ch);
      } else {
        url += std::format("%{:02X}", byte);
      }
    }
    return url;
  }
  /// 起一个演示浏览器。返回 false = 没起来（`status_` 已写明原因）。
  [[nodiscard]] auto launch() -> bool {
    const auto browser = find_browser_executable();
    if (!browser.has_value()) {
      status_ = "没找到浏览器（msedge/chrome）";
      return false;
    }
    // 独立用户数据目录：不碰用户自己的浏览器配置，也能让新进程**真的常驻**
    //（否则浏览器会把请求转给已有实例然后自己退出）。
    const std::string profile = st::fs::join(st::fs::temp_dir(), "shuangtian-nh-demo-profile");
    (void)st::fs::create_directories(profile);
    const std::vector<std::string> arguments{
        "--no-first-run", "--no-default-browser-check", "--disable-features=Translate",
        // 演示专用 profile 每次都是“新装的”，而上一轮常是被 terminate 结束的——
        // 不加这个就会先弹一条“Chrome 未正确关闭”的恢复提示，看着像出错。
        "--disable-session-crashed-bubble", "--noerrdialogs",
        "--user-data-dir=" + profile, "--window-size=1000,700",
        // 展示一个有内容的页面而不是 about:blank：演示要能看出“真的是网页”
        //（空页面与白色占位块在截图上无法区分）。
        demo_page_url(),
    };
    window_ = nullptr;
    process_.open(*browser, arguments, "");
    if (!process_.valid()) {
      status_ = "启动浏览器失败：" + std::string(process_.error());
      return false;
    }
    return true;
  }

  st::app::Application& app_;
  st::process::StreamHandle process_{};
  void* window_{nullptr};
  std::string status_{"未启动"};
};

/// 导航项：图标 + 文字，左对齐（选中态由切页逻辑切 variant）。
[[nodiscard]] auto make_nav_item(std::string_view icon, const std::string& label)
    -> std::unique_ptr<Button> {
  auto button =
      std::make_unique<Button>(label, Button::Variant::Ghost, Button::Size::Medium);
  button->set_icon(std::string(icon));
  button->style().justify = Justify::Start;
  button->style().text_align = TextAlign::Start;
  return button;
}

// —— DPI 档位（**唯一真源**：标签、切档序列、状态栏都从这里取）——
//
// 先前三处各写一份换算：标签把 1.5/2.0 映射成 “2x/3x”（与状态栏的 “1.5x/2.0x”
// 自相矛盾），档位序列又是另一套阈值。显式比例才是**可核对的事实**——
// 标签就该是后端实际生效的那一个数。

/// 按钮/状态栏用的 DPI 文案：直接报**实际比例**（1.0 → “1x”，1.5 → “1.5x”）。
[[nodiscard]] auto dpi_label(float scale) -> std::string {
  return std::format("DPI {}x", scale == std::floor(scale)
                                     ? std::format("{:.0f}", static_cast<double>(scale))
                                     : std::format("{:.1f}", static_cast<double>(scale)));
}

/// 下一档缩放。非整比例（如系统默认的 1.25x）先收报到 1.0，再走 1.5 / 2.0 三档循环。
[[nodiscard]] auto next_scale(float current) -> float {
  if (!(current > 0.0f)) return 1.0f;
  if (current < 1.25f) return 1.5f;
  if (current < 1.75f) return 2.0f;
  return 1.0f;
}

}  // namespace

/// 渲染基准（性能优化的可重复标尺）：连续 `frames` 帧**强制全量重绘**，
/// 报告总帧耗时与分阶段（排版 / 绘制 / 送显）的 p50、p95。
///
/// 为什么放在示例而不是框架：基准要跑的"内容"是应用自己的界面；
/// 框架只需提供可组合的原语（`root().mark_dirty_all()` + `render_frame()` + `metrics()`）。
/// 每帧都先标脏整棵树——这是**最坏情况**，也正因为最坏才可复现：
/// 否则"第二帧开始不重绘"会把排版成本静默地排除在测量之外（实测被坑过）。

/// 把帧剖析报告写到 `--perf-report` 指定的位置（空则打 stdout）。
///
/// 只在给了 `--perf-frames` 时有意义（没开剖析时 `perf_report()` 返回空）。
/// 单独成一个函数：`run_app` 有多条退路（bench / quit / 帧数用完），
/// 每条都要调它——写两遍必然漂移。
void emit_perf_report(st::app::Application& app, const Options& options) {
  // 启动分解与帧剖析**独立**：前者不需要 `--perf-frames`（它答的是“首帧那 200ms 花在哪”），
  // 而 gallery 用自己的主循环（不走 `Application::run` 的收尾），所以在这里主动打一次。
  st::perf::log_startup_summary();
  if (options.common.app.profile_frames == 0) return;
  const auto report = app.perf_report();
  if (!report.has_value()) {
    st::eprint("未生成性能报告：剖析未开启（需要 --perf-frames N）\n");
    return;
  }
  const std::string json = st::perf::to_json(*report);
  if (options.common.perf_report.empty()) {
    st::print("{}\n", json);
    return;
  }
  if (const auto parent = st::fs::parent(options.common.perf_report); !parent.empty()) {
    (void)st::fs::create_directories(parent);
  }
  if (auto written = st::fs::write_text(options.common.perf_report, json); !written) {
    st::eprint("性能报告写入失败（{}）: {}\n", options.common.perf_report, written.error().to_string());
    return;
  }
  st::print("性能报告: {}\n", options.common.perf_report);
}

[[nodiscard]] auto run_render_bench(st::app::Application& app, std::uint32_t frames) -> int {
  std::vector<double> total;
  std::vector<double> layout;
  std::vector<double> paint;
  std::vector<double> present;
  total.reserve(frames);
  layout.reserve(frames);
  paint.reserve(frames);
  present.reserve(frames);
  for (std::uint32_t index = 0; index < frames; ++index) {
    // 剖析时**强制画满**：否则第二帧起界面不脏、`paint_frame` 走空帧早退，
    // 元素样本会是空（实测：--perf-frames 400 只拿到 5 帧样本，因为界面静止）。
    app.root().mark_dirty_all();
    app.render_frame();
    const st::control::Metrics metrics = app.metrics();
    total.push_back(metrics.last_frame_ms);
    layout.push_back(metrics.layout_ms);
    paint.push_back(metrics.paint_ms);
    present.push_back(metrics.present_ms);
  }
  const auto summarize = [](const char* name, std::vector<double>& samples) {
    std::ranges::sort(samples);
    const auto at = [&samples](double ratio) -> double {
      const auto index = static_cast<std::size_t>(ratio * static_cast<double>(samples.size() - 1));
      return samples[index];
    };
    double sum = 0.0;
    for (const double value : samples) sum += value;
    st::log::out("  {:<8} p50 {:6.2f} ms · p95 {:6.2f} ms · 均 {:6.2f} ms · 最大 {:6.2f} ms\n", name,
              at(0.5), at(0.95), sum / static_cast<double>(samples.size()), samples.back());
  };
  st::log::out("渲染基准：{} 帧（每帧强制全量重绘）\n", frames);
  summarize("总帧", total);
  summarize("排版", layout);
  summarize("绘制", paint);
  summarize("送显", present);
  // 绘制分解（需 `ST_PAINT_PROFILE=1`）：只看"绘制 30ms"不知道该改哪里
  if (const st::raster::PaintProfiler* profile = app.paint_profile(); profile != nullptr) {
    st::log::out("绘制分解（最后一帧，按累计耗时排序）：\n");
    std::vector<std::pair<double, std::string>> rows;
    for (std::size_t index = 0; index < static_cast<std::size_t>(st::raster::PaintOp::Count);
         ++index) {
      const auto op = static_cast<st::raster::PaintOp>(index);
      const st::raster::PaintOpStat& stat = profile->op(op);
      if (stat.calls == 0) continue;
      rows.emplace_back(
          stat.ms, std::format("{:<10} 调用 {:>5} · {:>8.2f} ms · {:>9} 像素 · {:>7.3} ms/次",
                               st::raster::paint_op_name(op), stat.calls, stat.ms, stat.pixels,
                               stat.ms / static_cast<double>(stat.calls)));
    }
    std::ranges::sort(rows, [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    for (const auto& [ms, text] : rows) st::log::out("  {}\n", text);
    // **同帧自校验**：各原语累计之和。
    //
    // ⚠ 不要拿它去减“本批绘制的最小值”或中位数——`PaintProfiler` 记的是
    // **最后一帧**，而摘要里的分位是**全部帧**的。两者不是同一帧，相减得到的
    // “未归因”会掺进帧间抖动（实测拿 min 去减，得出 0.16~0.42 ms 的假差额，
    // 而连续 6 次实测真值是 **0.00 ms**）。
    // 要对比就与**同帧的 `paint_ms`** 比（见 `metrics.paint_ms` 与输出顺序）。
    const double total_ms = profile->total_ms();
    st::log::out("  原语合计 {:.2f} ms（最后一帧；各原语之和 == 本帧 `paint` 耗时）\n", total_ms);
    // **最贵的调用点**：“shadow 0.6ms”不指向任何行动，而“shadow 0.6ms，其中 0.48ms
    // 在 card.cpp:812 那一处”直接告诉人该改哪一行。
    st::log::out("调用点热点（每类原语 top3，按累计耗时）：\n");
    for (std::size_t index = 0; index < static_cast<std::size_t>(st::raster::PaintOp::Count);
         ++index) {
      const auto op = static_cast<st::raster::PaintOp>(index);
      const st::raster::PaintOpStat& stat = profile->op(op);
      if (stat.sites.empty()) continue;
      std::vector<st::raster::PaintSiteStat> sites = stat.sites;
      std::ranges::sort(sites, [](const auto& lhs, const auto& rhs) { return lhs.ms > rhs.ms; });
      for (std::size_t rank = 0; rank < std::min<std::size_t>(3, sites.size()); ++rank) {
        st::log::out("  {:<10} {:>8.3} ms × {:>4}  {}\n", st::raster::paint_op_name(op),
                     sites[rank].ms, sites[rank].calls, sites[rank].site);
      }
    }
    // **样本量自述**：`--bench 150` 的 150 帧是可信的；`--bench 3` 的 3 帧只能看趋势。
    // 数字后面不跟样本量就是“裸数字”（本仓反复踩的坑）。
    if (frames < 30) {
      st::log::out("  ⚠ 只有 {} 帧样本（分位需 ≥30 帧才有意义）——请用 --bench 150 重跑\n",
                   frames);
    }
  }
  return 0;
}

auto run_app(int argc, char** argv) -> int {
  auto parsed = parse_options(argc, argv);
  if (!parsed) {
    st::log::err("gallery: {}\n", parsed.error().to_string());
    return 2;
  }
  const Options options = parsed->first;
  // 框架的解析结果**直接**就是启动参数——这里不再逐个字段抄一遍。
  st::app::AppOptions app_options = std::move(parsed->second);

  // 窗口/任务栏图标 + 标题栏品牌标记（两份素材，用途不同——见 app_icon.hpp）。
  st::app::icon::attach_png(app_options.window_icon_png,
                            b::embed<"apps/gallery/assets/gallery-icon.png">());
  st::app::icon::register_svg("gallery-logo",
                              b::embed<"apps/gallery/assets/gallery-logo.svg">());
  // 图标全集页里的 SVG 图标（`svg:<id>`）也要注册才能画：
  // sprite 是 `assets/icons.svg`（17 个 `<symbol>`，Lucide 子集）。
  //
  // ⚠ 之前**从未装载过**——于是那一整页 `svg:` 图标静默空白，
  // 而“图标与其名字并排”这个设计意图正好就是拿来发现问题图标的。
  st::app::icon::load_sprite(b::embed<"apps/gallery/assets/icons.svg">());

  // 启动自检：把注册表里“本应用要用的两个 id”报出来。
  //
  // 为何值得一行日志：SVG 图标注册失败/名字拼错时，`Icon::draw` 只会**静默不画**
  // （没有图标、没有空白块、没有报错）。而“标题栏左侧空了一格”既可能是没注册、
  // 也可能是注册了但名字对不上——日志一下就能分开（实测为了这个加过一轮）。
  {
    const auto ids = st::ui::svg_registry().ids();
    const auto present = [&ids](std::string_view id) {
      return std::ranges::find(ids, id) != ids.end();
    };
    st::log::out("图标注册：gallery-logo={} icons.sprite={} 项\n",
                 present("gallery-logo") ? "ok" : "缺失",
                 static_cast<int>(ids.size()) - (present("gallery-logo") ? 1 : 0));
  }

  st::app::Application app("gallery", "0.1.0", app_options);

  auto* app_ptr = &app;
  auto* root_ptr = &app.root();

  // —— 根布局 ——
  //
  // 外壳用 `WindowFrame`（**组件化的窗口**：标题栏 + 内容槽 + 八向缩放边缘）。
  // 为什么不再手搭一条"顶部栏"：
  //   ① 窗框是**框架**该提供的东西（`CONVENTIONS.md` §10 第 7 条：装饰一律自绘）；
  //   ② 无边框窗口**只能**靠窗框提供拖动/缩放区域——手搭 shell 时窗口拖不动；
  //   ③ 主题/DPI/截图这些控件本来就长在标题栏那一行（VSCode / 浏览器同构），
  //      挂进标题栏的尾部槽即可，不必再叠一条 60px 的横条。
  auto frame = std::make_unique<st::ui::WindowFrame>("概览");
  frame->set_id("app-root");                       // 兼容钩子：原来的根 id 保持在根元素上
  auto root_panel = std::make_unique<Panel>(FlexDirection::Column);   // 内容槽内的主体
  root_panel->set_id("content-root");
  // 内容主体**铺满内容槽**：不声明 grow 时它按自身测量高排（导航 + 页 + 状态栏），
  // 而窗框的内容区是"扣掉标题栏之后的那一块"，主体本就该占满它。
  root_panel->style().grow = true;
  st::ui::WindowFrame* frame_ptr = frame.get();
  if (frame_ptr->title_bar() != nullptr) {
    frame_ptr->title_bar()->set_id("titlebar");
    // 品牌标记（本应用自己的 SVG 记号，不是通用 `sparkles`）。
    st::app::icon::apply_brand(frame_ptr->title_bar(), "gallery-logo");
    frame_ptr->title_bar()->set_window_control(&app);
  }
  // 品牌：用 `TitleBar::set_brand`（**真正的品牌区**，不是占 `leading` 槽）——
  //   `[图标][霜天 · 组件画廊][……][主题 DPI 截图 | — □ ×]`
  //
  // 为何不再挂 `leading` 槽：槽是**宿主控件位**（菜单栏/面包屑/搜索框），
  // 把品牌名塞进去会让两者在窄窗口下争同一段宽度，而且无法表达“品牌与宿主控件
  // 之间有间隔”这类意图（见 `set_brand` 的说明）。
  //
  // 兼容：控制通道的 `#app-title` 钩子仍在（有一个隐藏的品牌文本元素承担它）——
  // 自动化脚本按 id 寻址，不能因为内部换成品牌区就失联。
  if (frame_ptr->title_bar() != nullptr) {
    frame_ptr->title_bar()->set_brand("霜天 · 组件画廊");
    auto badge = std::make_unique<Text>("霜天 · 组件画廊");
    badge->set_id("app-title");
    badge->set_visible(false);   // 只作寻址钩子：不给它占布局
    frame_ptr->title_bar()->add_trailing(std::move(badge));
  }

  // 主题入口**只在标题栏**（`TitleBar` 内嵌的 `ThemePicker`：点画笔图标弹色卡墙，
  // 内置主题一次选完）。这里不再放“亮色/暗色”两档按钮——
  // 它与色卡墙是同一个能力的两个入口，而两档按钮只是内置主题库的子集，
  // 两个入口并存会让人以为“只有两档可切”。
  //
  // 按钮标签与**实际缩放**必须同一份事实：`dpi_label()` 是唯一入口，
  // 初值也由它给出（写死 "DPI 1x" 与后端解析出的 scale 无关，启动即错）。
  auto scale_button =
      std::make_unique<Button>(dpi_label(app.device_scale()), Button::Variant::Neutral,
                               Button::Size::Small);
  scale_button->set_id("dpi-toggle");
  scale_button->set_icon("cpu");
  auto* scale_button_ptr = scale_button.get();
  auto shot_button = std::make_unique<Button>("截图", Button::Variant::Primary, Button::Size::Small);
  shot_button->set_id("btn-capture");
  shot_button->set_icon("image");
  auto* shot_button_ptr = shot_button.get();

  // 两个按钮进标题栏尾部槽（顺序：DPI · 截图，与控制按钮同排不重叠）。
  if (frame_ptr->title_bar() != nullptr) {
    frame_ptr->title_bar()->add_trailing(std::move(scale_button));
    frame_ptr->title_bar()->add_trailing(std::move(shot_button));
  }

  // —— 内容槽：主体（导航 + 页 + 状态栏）——
  // 子节点声明进**内容区**：`WindowFrame::add_child` 已按此语义转发，
  // 所以这里照旧写 `root_panel->add_child(...)`，但最后把 root_panel 挂到窗框内容槽。

  // 主体
  auto body = std::make_unique<Panel>(FlexDirection::Row);
  body->set_id("body");
  body->style().grow = true;
  auto sidebar = std::make_unique<Panel>(FlexDirection::Column);
  sidebar->set_id("sidebar");
  sidebar->style().width = 224.0f;
  sidebar->style().padding = Insets{12.0f, 16.0f, 12.0f, 16.0f};
  sidebar->style().gap = 4.0f;
  // 侧栏是**内容容器**，声明中层面板（`surface`）。
  //
  // 为何必须声明而不是“看着还行就算了”：不声明时它是透明的，直接露出页面底（`bg`）。
  // 而主题的存在意义就是「这几个层级必须分得开」——`ui_theme_test` 锁着
  // `surface` 对 `bg` ≥ 1.08，但那个护栏验的是**令牌之间**的关系；
  // 应用如果不把令牌用上，实机上三者仍是同一个色。
  // 实测（声明前）：侧栏↔内容底 **1.008:1**、状态栏↔内容底 **1.008:1**（暗色），
  // 相当于整屏只有一种底色，所有“层次”都靠阴影硬撑。
  sidebar->set_surface(Element::Surface::Base);
  auto nav_label = std::make_unique<Text>("导航");
  nav_label->set_tone(Tone::Faint);
  nav_label->set_scale(Text::Scale::Xs2);
  sidebar->add_child(std::move(nav_label));

  // 导航项与页面一一对应（清单来自 `page_specs()`，不在两处各写一遍）
  const auto& specs = gallery::page_specs();
  std::array<Button*, gallery::kPageCount> nav_ptrs{};
  for (std::size_t index = 0; index < gallery::kPageCount; ++index) {
    auto item = make_nav_item(specs[index].icon, std::string(specs[index].label));
    item->set_id(std::format("nav-{}", specs[index].id));
    nav_ptrs[index] = item.get();
    sidebar->add_child(std::move(item));
  }
  auto sidebar_spacer = std::make_unique<Panel>(FlexDirection::Column);
  sidebar_spacer->style().grow = true;
  sidebar->add_child(std::move(sidebar_spacer));
  auto version_text = std::make_unique<Text>("霜天 v0.1.0 · C++20");
  version_text->set_tone(Tone::Faint);
  version_text->set_scale(Text::Scale::Xs2);
  sidebar->add_child(std::move(version_text));
  body->add_child(std::move(sidebar));

  auto scroll = std::make_unique<ScrollView>();
  scroll->set_id("content-scroll");
  scroll->style().grow = true;
  auto* scroll_ptr = scroll.get();

  // 页面容器：5 个页面都在里面，用 `visible` 切换（隐藏页零布局零绘制）
  auto pages_host = std::make_unique<Panel>(FlexDirection::Column);
  pages_host->set_id("pages");
  std::array<Element*, gallery::kPageCount> page_ptrs{};

  // 运行时字段：页面注册"这里要实时值"，应用在刷新时填（两边互不知道细节）
  std::vector<std::pair<std::string, std::function<void(std::string)>>> runtime_fields;

  gallery::PageHooks hooks;
  // ⚠ 状态栏回调必须**先接上再建页**：页面按值捕获 PageHooks（拷贝），
  // 后接的 set_status 对已建页面不可见——旧序（建页后才接）会让所有页面的
  // set_status 全部落到初始空操作上，状态栏永远不变（实测踩到）。
  auto status_text_early = std::make_unique<Text>("就绪");
  status_text_early->set_id("status-text");
  status_text_early->set_tone(Tone::Muted);
  status_text_early->set_scale(Text::Scale::Xs);
  auto* status_ptr = status_text_early.get();
  hooks.set_status = [status_ptr](std::string text) { status_ptr->set_content(std::move(text)); };
  hooks.add_overlay = [root_ptr](std::unique_ptr<Element> overlay) {
    // FillViewport：模态遮罩/宽通知的标准形态（组件在 arrange 里自定位卡片）
    root_ptr->add_overlay(std::move(overlay), UiRoot::OverlayLayout::FillViewport);
  };
  hooks.remove_overlay = [root_ptr](Element* overlay) { root_ptr->remove_overlay(overlay); };
  // 窗框的窗口动作出口（页面把它注入 `ui::TitleBar`）。
  // ⚠ 同样必须在**建页之前**接上：页面按值捕获 `PageHooks`（与 `set_status` 同一个坑）。
  hooks.window_control = &app;
  hooks.register_runtime_field =
      [&runtime_fields](std::string_view field, std::function<void(std::string)> setter) {
        runtime_fields.emplace_back(std::string(field), std::move(setter));
      };
  // 原生嵌入页的外部窗口来源（起浏览器 + 按 pid 找窗口）。
  // 同理必须在**建页之前**接上（页面按值捕获 `PageHooks`）。
  // 生命周期：比页面长——页面回调里会用到它，所以它得活到 run_app 结束。
  GalleryNativeHostSource native_host_source(app);
  hooks.native_host_source = &native_host_source;

  for (std::size_t index = 0; index < gallery::kPageCount; ++index) {
    auto page = gallery::build_page(index, hooks);
    page->set_visible(index == 0);  // 默认第一页
    page_ptrs[index] = page.get();
    pages_host->add_child(std::move(page));
  }
  scroll->add_child(std::move(pages_host));
  body->add_child(std::move(scroll));
  root_panel->add_child(std::move(body));

  // 状态栏
  auto status_bar = std::make_unique<Panel>(FlexDirection::Row);
  status_bar->set_id("status-bar");
  // 状态栏是**外壳条带**（与标题栏同色一圈）：声明 `Alt` 而不是让它透明。
  // 不声明时它直接露页面底（`bg`），与内容区对比实测 **1.008:1**——
  // 肉眼就是“没有状态栏”，只剩文字浮在页面底上。
  status_bar->set_surface(Element::Surface::Alt);
  status_bar->style().height = 30.0f;
  status_bar->style().padding = Insets{24.0f, 0.0f, 24.0f, 0.0f};
  status_bar->style().align_items = Align::Center;
  status_bar->style().gap = 8.0f;
  auto status_dot = std::make_unique<IconView>("dot", 8.0f);
  status_dot->set_tone(Tone::Success);
  status_bar->add_child(std::move(status_dot));
  // status_text 在建页前已创建（hooks.set_status 需要先拿到指针，见上方注释）
  status_bar->add_child(std::move(status_text_early));
  auto status_spacer = std::make_unique<Panel>(FlexDirection::Row);
  status_spacer->style().grow = true;
  status_bar->add_child(std::move(status_spacer));
  auto refresh_button =
      std::make_unique<Button>("刷新指标", Button::Variant::Ghost, Button::Size::Small);
  refresh_button->set_id("btn-refresh");
  // 图标名取**内置表**里的：写成表外的名字会不画图标（内容被空位推偏 12px）。
  // 内置表用 `Icon::names()` 拿（当前 81 个，见 `icon.cpp`）；`activity` 并不在其中。
  // ⚠ 别在这里写死个数：它随图标表变动，是陈旧注释的常见来源（已改成动态取）。
  refresh_button->set_icon("refresh");
  auto* refresh_ptr = refresh_button.get();
  status_bar->add_child(std::move(refresh_button));
  auto status_right = std::make_unique<Text>("等待控制通道…");
  status_right->set_id("status-right");
  status_right->set_tone(Tone::Faint);
  status_right->set_scale(Text::Scale::Xs);
  auto* status_right_ptr = status_right.get();
  status_bar->add_child(std::move(status_right));
  root_panel->add_child(std::move(status_bar));

  // —— 交互 ——（set_status 已在建页前接上；此处不再重复赋值）

  // 运行时字段刷新：从 `metrics` 与 `Application` 回读——界面自己展示自己的可观测性
  const auto refresh_runtime = [app_ptr, status_right_ptr, &runtime_fields]() {
    const st::control::Metrics metrics = app_ptr->metrics();
    for (auto& [field, setter] : runtime_fields) {
      if (field == "backend") setter(std::string(app_ptr->backend_name()));
      else if (field == "renderer") setter(metrics.renderer);
      else if (field == "stat_renderer") {
        // 卡片位置短：写"GPU (D3D11)"这类短名，长描述留在 renderer_note（控制通道可读）
        setter(metrics.renderer == "gpu" ? "GPU (D3D11)" : "软件光栅器");
      } else if (field == "stat_dpi") {
        setter(std::format("{:.1f}x", static_cast<double>(app_ptr->device_scale())));
      }
      else if (field == "dpi") setter(std::format("{:.1f}x", static_cast<double>(app_ptr->device_scale())));
      else if (field == "frames") setter(std::format("{}", metrics.frames));
      else if (field == "port") setter(std::format("{}", app_ptr->control_port()));
      else if (field == "stat_nodes") {
        // 语义树节点数（与控制通道 tree 同源）：切页/主题/DPI 变化后都会不同
        setter(std::format("{}", metrics.nodes));
      } else if (field == "stat_port") {
        setter(std::format("127.0.0.1:{}", app_ptr->control_port()));
      }
    }
    status_right_ptr->set_content(std::format("{} · DPI {:.1f} · {}x{} · 第 {} 帧",
                                              app_ptr->backend_name(),
                                              static_cast<double>(app_ptr->device_scale()),
                                              metrics.physical_width, metrics.physical_height,
                                              metrics.frames));
  };
  refresh_ptr->on_click = [app_ptr, refresh_runtime]() {
    refresh_runtime();
    app_ptr->root().mark_dirty_all();
  };

  // 切页：visible + 导航高亮 + 滚动复位 + 清焦点
  const auto show_page = [&](std::size_t index) {
    for (std::size_t each = 0; each < gallery::kPageCount; ++each) {
      page_ptrs[each]->set_visible(each == index);
      nav_ptrs[each]->set_variant(each == index ? Button::Variant::Soft : Button::Variant::Ghost);
    }
    // 窗框标题跟随当前页：与编辑器把当前文件名写进标题栏同构
    // （标题栏不只是装饰——它回答"我现在在哪"）。
    if (frame_ptr != nullptr && frame_ptr->title_bar() != nullptr) {
      frame_ptr->title_bar()->set_title(std::string(specs[index].label));
    }
    // 滚动复位：否则切到更短的页面会停在"上一页的中段"，看起来像内容缺失
    scroll_ptr->scroll_to(0.0f);
    // 清焦点：焦点元素若随页面被隐藏，键盘事件仍会送到它（"看不见的输入框在收字"）
    root_ptr->set_focus(nullptr);
    status_ptr->set_content(std::format("已切到「{}」", specs[index].label));
    root_ptr->mark_dirty_all();
  };
  for (std::size_t index = 0; index < gallery::kPageCount; ++index) {
    nav_ptrs[index]->on_click = [show_page, index]() { show_page(index); };
  }
  // 初始页的导航项也要**高亮**。
  //
  // `show_page` 只在点击时调用，而初始页是由 `page->set_visible(index == 0)` 定的——
  // 于是启动时侧栏 5 项全是 Ghost，**完全看不出当前在哪一页**（实测现象）。
  // 这里只设高亮、不改状态栏文案：启动时"就绪"比"已切到「概览」"更合适。
  nav_ptrs[0]->set_variant(Button::Variant::Soft);

  scale_button_ptr->on_click = [app_ptr, scale_button_ptr, status_ptr, status_right_ptr,
                                refresh_runtime, root_ptr]() {
    const float current = app_ptr->device_scale();
    const float next = next_scale(current);
    if (auto status = app_ptr->set_device_scale(next); status.has_value()) {
      // 标签取**切换后回读**的实际值，而不是把 `next` 再格式化一遍：
      // 两者一旦分叉（改档位却忘了改标签），界面就会自相矛盾。
      scale_button_ptr->set_label(dpi_label(app_ptr->device_scale()));
      status_ptr->set_content(std::format("DPI {:.1f}x · 字形已按物理分辨率重栅格化",
                                          static_cast<double>(app_ptr->device_scale())));
      // 概览页的「DPI 缩放」统计卡与底部右侧字段都是**运行时事实**，
      // 切档后必须刷新——否则界面显示 1.5x 而实际已是 2.0x（实测复发过）。
      refresh_runtime();
      status_right_ptr->set_content(std::format("DPI {:.1f}x · 字形已按物理分辨率重栅格化",
                                                static_cast<double>(app_ptr->device_scale())));
    } else {
      status_ptr->set_content("DPI 切换失败: " + status.error().message);
    }
    root_ptr->mark_dirty_all();
  };
  shot_button_ptr->on_click = [app_ptr, status_ptr, root_ptr]() {
    auto saved = app_ptr->capture_to_file("", {});
    if (saved.has_value()) {
      status_ptr->set_content("已截图: " + st::fs::file_name(saved->path));
    } else {
      status_ptr->set_content("截图失败: " + saved.error().message);
    }
    root_ptr->mark_dirty_all();
  };

  // —— 启动 ——
  //
  // 窗框接管根内容（**单根**）：`WindowFrame` 自带标题栏与内容槽，
  // 主体（`root_panel`）进内容槽。窗口动作端口已在上面注入（`set_window_control(&app)`），
  // 三个控制按钮与八向边缘条因此都接了真实后端。
  frame->content()->add_child(std::move(root_panel));
  app.set_content(std::move(frame));
  if (auto status = app.start(); !status) {
    std::fprintf(stderr, "启动失败: %s\n", status.error().to_string().c_str());
    return 1;
  }

  // —— 声明式页：把声明式子树挂到该页的宿主容器下 ——
  //
  // `dsl::mount()` 是**单根语义**（会替掉 `UiRoot::content()`），gallery 是手搭壳，
  // 所以用 `mount_into()`：声明式树只占 `#declarative-host` 那一块。
  // 帧首推进**不需要在调用方写**：`DeclarativeHost` 构造时已向 `UiRoot` 自登记，
  // 主循环调一次 `root().tick_declarative_hosts()` 就会把它（以及任何其它树）推起来。
  // 时机在 `set_content` 之后：锚点得先上树，才拿得到稳定地址。
  std::unique_ptr<st::ui::dsl::DeclarativeHost> decl_host{};
  if (Element* decl_anchor = app.root().find("declarative-host"); decl_anchor != nullptr) {
    decl_host = st::ui::dsl::mount_into(app.root(), *decl_anchor,
                                        gallery::make_declarative_component());
    if (decl_host == nullptr) st::log::out("声明式页挂载失败（该页将只显示壳）\n");
  }
  // 启动树自检（mount_into 之后：声明式子树也是被审计对象；39 轮未竟项②）。
  // 不达标不阻断启动（展示型自检），结论打印在启动日志；无头 e2e 可 grep 断言。
  audit_gallery_tree(app.root());
  status_right_ptr->set_content(std::format("{} · headless={} · DPI {:.1f} · 控制通道 127.0.0.1:{}",
                                            app.backend_name(), app.headless(),
                                            static_cast<double>(app.device_scale()),
                                            app.control_port()));
  // **启动后回读实际 DPI 再写标签**：`scale` 在构造时还是 0（“自动”由后端解析，
  // 无头模式下按系统缩放得到 1.0/1.25/1.5），拿它算出来的标签会与画面不符。
  if (scale_button_ptr != nullptr) scale_button_ptr->set_label(dpi_label(app.device_scale()));
  refresh_runtime();
  app.root().mark_dirty_all();
  app.render_frame();

  if (options.bench > 0) {
    const int bench_code = run_render_bench(app, options.bench);
    // **跑完不退出**：`st perf` 要靠控制通道来取报告。bench 只是一段“强制定量采样”
    // 的前置动作，取完报告由外部发 `shutdown`（与 `--frames` 那条退路不同）。
    // 若没人来取，进程会一直空转——所以只在开了剖析时才留着
    // （`--bench` 单独用是命令行看数字的老用法，那时应当跑完就退）。
    if (options.common.app.profile_frames > 0) {
      while (!app.quit_requested()) {
        app.tick();
        app.pace_loop(st::time::now_ms());
      }
    }
    emit_perf_report(app, options);
    return bench_code;
  }

  const std::int64_t started_ms = st::time::now_ms();
  std::uint32_t frames = 1;
  while (!app.quit_requested()) {
    const std::int64_t frame_start_ms = st::time::now_ms();
    // 声明式页：帧首推进（**全体已登记的树**，不只本页那棵）。
    //
    // `DeclarativeHost` 登记时的回调里已经先 `pump_async()` 再判脏——**顺序不能反**：
    // 异步结果到达时状态还没写（scope 不脏），只在 dirty 时才调 tick 的话
    // `pump_async` 永远不会被调到，任务结果就永远停在“计算中…”（实测踩到）。
    // 框架回调已把这个顺序内建，调用方不必再写一遍。
    const std::size_t rebuilt = app.root().tick_declarative_hosts();
    if (rebuilt > 0) app.request_repaint();
    app.tick();
    ++frames;
    if (options.common.max_frames > 0 && frames >= options.common.max_frames) break;
    if (options.common.max_ms > 0 &&
        st::time::now_ms() - started_ms >= options.common.max_ms) break;
    // 节拍交给框架：有活 → 帧预算；空闲 → 4ms（控制通道响应节拍）。
    // 旧实现固定 `sleep_for(16ms)`：命令延迟被拉到 16~31ms（实测 ping p50=31ms）。
    app.pace_loop(frame_start_ms);
  }
  st::log::out("gallery 退出：{} 帧，DPI {:.1f}，后端 {}\n", frames,
            static_cast<double>(app.device_scale()), std::string(app.backend_name()));
  // 剖析报告：给了 `--perf-frames` 就把它落盘（不写路径则打到 stdout）。
  //
  // 为何在退出处统一收口：`run` 里有多条退路（bench/控制通道 quit/帧数用完），
  // 在这里写才能保证**每一条**退路都留下证据（否则“跑完了但没有报告”会变成谜）。
  emit_perf_report(app, options);
  return 0;
}

// 跨平台入口：正规化 argv 编码（Windows 的 argv 是 ANSI）并设好控制台代码页
ST_MAIN(run_app)
