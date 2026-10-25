#pragma once

/// **改文件即生效**（`HotReload`）：让「改界面 / 改逻辑」不再需要重编、也不再需要重启进程。
///
/// ## 它解决什么
///
/// `docs/xml-ui.md` 那条通路（XML 写界面 + JS 写逻辑）本来就把两份文件放在**运行期读**，
/// 但改完仍要重开进程：XML 只在启动时建一次树，JS 只在启动时 `bridge.load` 一次。
/// 于是"调一个间距"的一轮是：改文件 → 切终端 → 起进程 → 看窗口，十几秒起。
///
/// 本类把这一轮压成**保存即见效**（进程不重启、不重编）：
///
/// | 改了 | 生效方式 | 代价（必须知道） |
/// |---|---|---|
/// | `app.js` | 清掉旧绑定/定时器/脚本状态后**重新装载** | 脚本侧的临时状态回到初始（与重启进程等价） |
/// | `main.xml` | 重建整棵界面树并**换根** | 旧树销毁 → 输入框内容/滚动位置等运行期状态丢失 |
///
/// ## 两条硬约束：原子与如实
///
/// 1. **原子替换**：先建树（或先语法检查全部脚本页），**全部成功才换上去**。
///    写错一个标签、JS 写成语法错时，界面/逻辑保持原样，错误带**文件名与行号**
///    进 stderr——不做"先清空再失败"的破坏性更新（那会让一次手滑把应用清空）。
/// 2. **降级如实**：界面重建后旧树已销毁，原脚本里绑在旧元素上的处理器自然失效。
///    本类**不会**假装它们还在：重载后重新走一遍应用自己的渲染（脚本重放即重建绑定），
///    而"新 XML 里已删除的 id"由 `ScriptHost::prune_dead_bindings` 回收。
///
/// ## 用法（三步）
///
/// ```cpp
/// // ① 壳层照旧建（窗框/标题栏），但**先不挂 XML 树**：树由 HotReload 管
/// app.set_content(std::move(frame));
/// app.start();
/// // ② 桥与热重载（桥负责 expose，热重载负责 XML/JS 的装载与换根）
/// st::ui::ScriptBridge bridge(*app.script(), &app.root());
/// store.install(bridge);
/// st::ui::HotReload::Options options;
/// options.ui_dir = ui_dir;          // 含 main.xml / app.js 的目录
/// options.watch = true;             // 默认关：按需打开（每帧 stat 两个文件）
/// st::ui::HotReload hot(app.root(), *app.script(), bridge, options);
/// // ③ 开局装载一次（等价于"启动时的 build_from_xml_file + bridge.load"）
/// if (auto status = hot.refresh_all(); !status) { 报错并退出 }
/// // ④ 主循环里推一下（没开 watch 时可省）
/// hot.poll();
/// ```
///
/// ## 与既有组件的关系
///
/// - 建树仍走 `st::ui::build_from_xml_file`（同一份实现、同一份报告），本类只负责
///   "什么时候建、建出来挂哪、旧的怎么退"；
/// - 装载仍走 `ScriptBridge::load`（同一份错误文案），本类只加"先清空再重放"；
/// - 界面可见性开关（开发期按 F12 让界面树让出画面看背景）改的是 XML 根的 `visible`，
///   **不改**壳层（窗框/标题栏仍在）——所以"界面藏起来了"和"应用没了"一眼可分。
///
/// ## 不做什么
///
/// - 不热重载 C++（那是重编的范围，编译器与构建系统的事）；
/// - 不做依赖图（只认 `ui_dir` 下的 `main.xml` / `app.js` 与显式列出的额外脚本页）；
/// - 不保存界面运行期状态（不是"状态迁移"，是"重新建"）——需要保留的东西属于脚本层，
///   由脚本自己在重载前写出去（`app.*` 能力），本层不猜。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "st/core/error.hpp"
// `Json` 只出现在签名里 → 用**前向头**（不拉入 nlohmann 的 25,526 行）。
#include "st/ext/json_fwd.hpp"
#include "st/ui/xml_ui.hpp"

namespace st::ui {

class Element;
class ScriptBridge;
class ScriptHost;
class UiRoot;

/// 热重载器（一个应用一个；非线程安全，随主循环驱动）。
class HotReload {
 public:
  /// 建树钩子：默认 `build_from_xml_file(ui_dir/main.xml)`；给了它就用它
  /// （应用想在 XML 外套一层自己的容器、或改用 `build_from_xml(source)` 时用）。
  using Factory = std::function<std::unique_ptr<Element>()>;

  struct Options {
    /// 资源目录（`main.xml` / `app.js` 所在）。空 = 两个文件都不管（只用 `poll()` 也会空转）。
    std::string ui_dir{};
    std::string xml_name{"main.xml"};
    std::string script_name{"app.js"};
    /// 额外热重载的脚本页（按序在 `script_name` **之后**装载；同一份清单、同一套失败语义）。
    std::vector<std::string> pages{};
    /// 每帧检查文件改动（默认**关**：生产应用不该为一个调试便利付出每帧 IO）。
    bool watch{false};
    /// 两次检查之间的最小间隔（毫秒）。默认 250：人手保存的节奏远比这慢，
    /// 而编辑器"先截断再写入"会连续触发两次改动——节流让第二次看到的是完整文件。
    std::uint64_t poll_ms{250};
    /// XML 建树严格模式（认不出的标签/属性让整棵建树失败）。CI 用；
    /// 交互改文件应保持默认容错（否则一个笔误就让界面"消失"，比报警更糟）。
    bool strict_xml{false};
    /// 开局 XML 树是否可见。开发期"先藏界面看背景"由 `set_ui_visible(false)` 完成；
    /// 这里只给"从第一帧就藏"的场景（少一次可见→隐藏的闪动）。
    bool start_visible{true};
  };

  HotReload(UiRoot& root, ScriptHost& script, ScriptBridge& bridge, Options options);
  ~HotReload();
  HotReload(const HotReload&) = delete;
  auto operator=(const HotReload&) -> HotReload& = delete;

  /// 换建树钩子（必须在第一次 `refresh_ui` 之前设；之后设只影响下一次重建）。
  void set_factory(Factory factory);

  /// 重建界面树（原子：失败时界面保持原样）。错误：`NotFound`（文件读不到）/
  /// `Parse`（XML 语法或严格模式下有不认识的标签/属性）/`Unsupported`（未配置 `ui_dir`）。
  auto refresh_ui() -> Status;

  /// 重装脚本页（原子：先对全部页做语法检查，任一失败则旧逻辑原封不动）。
  auto refresh_script() -> Status;

  /// 先界面后逻辑。界面失败时**不再动逻辑**（逻辑挂在刚建出来的树上才有意义）。
  auto refresh_all() -> Status;

  /// 有改动则刷新（按 `poll_ms` 节流）。返回本次真的刷新了几样（0~2）。
  auto poll() -> std::size_t;

  /// 通知"别处已经重建过界面"（如应用自己按 F12 调了 `refresh_ui` 之外的重建）：
  /// 基线重新取，避免 `poll()` 立刻做第二次重复重建。
  void note_external_refresh() noexcept;

  /// 界面树可见性（开发期调试开关；壳层不受影响）。
  void set_ui_visible(bool visible);
  [[nodiscard]] auto ui_visible() -> bool;

  /// 诊断快照（控制通道 `hot.status` / 脚本 `app.hot.status()` 的数据源）。
  [[nodiscard]] auto status() -> st::Json;

  /// 最近一次成功刷新的摘要（给人看的一行，进日志与 `status`）。
  [[nodiscard]] auto last_summary() const -> const std::string&;

  /// 当前挂着的 XML 根（未装载或装载失败时为 `nullptr`）。
  [[nodiscard]] auto current_ui() const noexcept -> Element*;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace st::ui
