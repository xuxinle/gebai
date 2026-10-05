#pragma once

/// 声明式 UI 的 JS 宿主：把 `declarative.js`（VDOM/重组器）接进 `ScriptHost`。
///
/// 与 C++ 宿主（`st::ui::dsl`）同一语义规范（docs/declarative.md）：
/// - 声明 API：`compose(name, buildFn)` + Row/Column/Text/Button/… 构造函数 + `useState`；
/// - 树操作走窄桥（`__d_create/__d_set_root/__d_mount/__d_unmount/__d_apply_batch`），
///   属性写入在重组末尾一次跨界批量落地；
/// - 事件复用 `on()/off()` 绑定管线（VNode 持绑定 id，卸载时反注册）；
/// - 帧驱动：宿主每帧调 `tick()` → JS `__d_reconcile()`（dirty 才重跑）。
///
/// 用法（应用主循环）：
/// ```cpp
/// st::app::AppOptions options; options.enable_script = true;
/// // ... Application app(...); ScriptHost 在 start() 时创建
/// auto decl = ui::DeclarativeHost::attach(*app.script(), app.root());
/// decl->run("compose('Main', () => column({gap: 8}, [text('hi')]))");
/// // 主循环每帧：decl->tick();
/// ```

#include <memory>
#include <string>
#include <string_view>

// `Json` 只出现在签名里 → 用**前向头**（不拉入 nlohmann 的 25,526 行）。
// 需要构造/访问 Json 的 .cpp 自行包含 `st/ext/json.hpp`。见 `st/ext/json_fwd.hpp`。
#include "st/ext/json_fwd.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {

class ScriptHost;

/// JS 声明式宿主（非线程安全；依附 ScriptHost 生命周期，先于其销毁）。
class DeclarativeHost {
 public:
  /// 接到一个已就绪的 ScriptHost 上（注册窄桥 + 注入 declarative.js 前置）。
  /// 返回 nullptr：宿主未就绪 / 桥注册失败。
  ///
  /// `host_element` 非空时，声明式树的根挂到**该元素的子位**（子树形态——gallery 的一页
  /// 就是一个 Panel）；为空时挂到 `UiRoot::content()`（整页形态）。
  [[nodiscard]] static auto attach(ScriptHost& script, UiRoot& root,
                                   Element* host_element = nullptr)
      -> std::unique_ptr<DeclarativeHost>;

  /// 析构/移动在实现文件定义（`Impl` 不完整——头文件里 `= default` 会让调用方
  /// 看到 `sizeof(Impl)`，编译不过）。
  ~DeclarativeHost();
  DeclarativeHost(DeclarativeHost&&) noexcept;
  auto operator=(DeclarativeHost&&) noexcept -> DeclarativeHost&;
  DeclarativeHost(const DeclarativeHost&) = delete;
  auto operator=(const DeclarativeHost&) -> DeclarativeHost& = delete;

  /// 执行声明式代码（`compose(...)` 注册 + 首次重组一并完成）。
  [[nodiscard]] auto run(std::string_view code, std::string_view filename = "<declarative>")
      -> Status;

  /// 帧驱动：JS 侧 dirty 才重跑 build → diff → 变更集提交。
  /// 返回是否发生了重组（发生则调用方应 request_repaint——变更集提交已标脏）。
  auto tick() -> bool;

  /// 整棵卸载（场景切换 / 调用方主动收尾）：事件反注册 + effect 清理 + 真值树摘除。
  /// 返回是否有东西被卸。之后可再 `run()` 装新的一页（不复用旧槽位）。
  /// 仅析构不够：析构只释放桥与引擎，**不会回 JS 跑 effect 清理**——
  /// 「组件卸载时关连接/停定时器」这类清理会静默漏掉。
  auto unmount_declarative() -> bool;

  /// JS 侧状态（诊断）：{ scopes, mounted, dirty }
  [[nodiscard]] auto stats() const -> st::Json;

  /// 泵 Promise 微任务（异步回调不会自己跑）。返回执行的 job 数。
  /// 宿主主循环每帧调用（可在 `tick()` 前后）——`useResource` 等异步逻辑靠它推进。
  auto pump_jobs() -> std::size_t;

 private:
  /// 私有构造：实例只能经 `attach()` 产生；定义在实现文件（`Impl` 不完整类型）。
  DeclarativeHost();
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

}  // namespace st::ui
