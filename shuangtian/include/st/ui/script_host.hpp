#pragma once

/// 脚本宿主：让 JS 读写与控制组件，并把 UI 事件/定时器桥接给脚本。
///
/// ## 为什么这样设计（性能模型）
///
/// 跨语言边界是唯一成本，因此**绝不逐属性跨边界**：
///
/// | 反模式 | 本设计 |
/// |---|---|
/// | 每次读属性都跨一次边界 | **快照批量**：JS 请求时把整棵（或命中集合的）元素快照一次送进 JS，之后 JS 内自由读 |
/// | 每次改属性立刻回写 | **变更集批量提交**：JS 侧把 `set/click/focus` 记进队列，一次取回并应用 |
/// | 所有事件都桥接 | 只桥接**被显式监听**的（选择器 × 事件类型）；高频事件按帧合并 |
///
/// 一次脚本入口（`eval`/事件/定时器）的跨界次数是**常数级**（取变更集 1 次 + 事件派发 1 次），
/// 与"改了多少属性"无关。
///
/// ## 三条路径共用一套语义
///
/// 读取走 `ui::element_snapshot`、写入走 `ui::apply_properties`、动作走 `ui::invoke_element`，
/// 与 C++ 应用代码、控制通道的 `get`/`set`/`invoke` **完全同一份实现**，不会出现"同一元素两个样"。
///
/// ## JS 侧 API（前置在 `script_api.js`）
///
/// ```js
/// $('#save').text                    // 读属性
/// $('#status').set({ text: '已保存' })  // 排队变更（阶段末统一提交）
/// $('#save').click()                 // 触发动作
/// on('#save', 'click', () => log('clicked'))  // 事件绑定
/// every(1000, () => $('#clock').set({ text: now() }))
/// state.count = (state.count ?? 0) + 1        // 脚本侧状态（跨执行保留）
/// ```

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"
#include "st/ext/script.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {

/// 一条事件绑定（供诊断/列举/注销）。
struct ScriptBinding {
  std::string id{};        ///< 绑定 id（脚本侧与协议侧共用）
  std::string selector{};  ///< 监听的选择器
  std::string event{};     ///< 事件类型（`click`/`change`/`submit`/…）
  std::string handler{};   ///< 处理器源码片段（截断显示用）
};

/// 脚本宿主（一个 `UiRoot` 配一个；非线程安全，随应用主循环驱动）。
class ScriptHost {
 public:
  ScriptHost(UiRoot& root, ext::ScriptLimits limits = {});
  ~ScriptHost();
  ScriptHost(const ScriptHost&) = delete;
  auto operator=(const ScriptHost&) -> ScriptHost& = delete;

  [[nodiscard]] auto valid() const noexcept -> bool;

  /// 执行脚本片段；返回**最后一条表达式**的值。
  ///
  /// 入口语义：执行前重建快照、执行后提交变更集（见类注释的性能模型）。
  [[nodiscard]] auto eval(std::string_view code, std::string_view filename = "<script>")
      -> Result<st::Json>;

  /// 调用脚本中定义的函数（参数与返回值走 JSON）。
  [[nodiscard]] auto call(std::string_view function, const std::vector<st::Json>& arguments)
      -> Result<st::Json>;

  /// 注册事件绑定。`handler` 是 JS 箭头函数/函数表达式源码，如 `() => log('hi')`。
  /// 返回绑定 id（可交给 `unbind`）。错误：`Parse`（处理器语法错）、`Invalid`（事件名非法）。
  [[nodiscard]] auto bind(std::string_view selector, std::string_view event,
                          std::string_view handler) -> Result<std::string>;

  /// 注销绑定（返回是否存在并已移除）。
  auto unbind(std::string_view id) -> bool;

  /// 清空全部绑定（连同脚本侧处理器表）。
  void clear_bindings();

  [[nodiscard]] auto bindings() const -> std::vector<ScriptBinding>;

  /// 读回脚本侧状态（`state` 对象）——AI 用它检查脚本内部逻辑走到哪一步。
  [[nodiscard]] auto state() -> Result<st::Json>;

  /// 写入脚本侧状态（合并语义，便于测试与预置）。
  auto set_state(const st::Json& patch) -> Status;

  /// 派发一个 UI 事件给脚本：命中绑定的选择器时调用对应处理器。
  ///
  /// 由应用主循环调用（`Event` 已经过 `UiRoot` 的分发）。返回是否有脚本处理器被调用。
  auto dispatch_event(const Event& event, Element& target) -> bool;

  /// 推进定时器：`now_seconds` 为单调时钟秒数（与 `RenderContext::time_seconds` 同源）。
  /// 返回本次触发的定时器数量。
  auto tick(double now_seconds) -> std::size_t;

  /// 申请重绘（脚本改了界面后调用）。
  void request_repaint() noexcept { repaint_requested_ = true; }
  [[nodiscard]] auto take_repaint_request() noexcept -> bool {
    const bool value = repaint_requested_;
    repaint_requested_ = false;
    return value;
  }

  [[nodiscard]] auto last_stats() const noexcept -> const ext::ScriptStats&;
  [[nodiscard]] auto limits() const noexcept -> const ext::ScriptLimits&;
  /// 引擎版本（诊断用）。
  [[nodiscard]] auto engine_version() const -> std::string;

  /// 已提交的变更次数与条目数（性能观测：验证"批量"确实生效）。
  [[nodiscard]] auto commit_count() const noexcept -> std::size_t;
  [[nodiscard]] auto committed_properties() const noexcept -> std::size_t;

 private:
  /// 派发一组绑定（事件 → JS 处理器）。合并逻辑见 `dispatch_event`/`tick`。
  auto dispatch_handlers(const std::vector<std::string>& binding_ids, const Event& event,
                         Element& target) -> bool;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool repaint_requested_{false};
  /// 高频事件合并槽（本帧最后一次 鼠标移动 的 绑定/目标），在 `tick()` 里补发。
  std::optional<std::pair<std::string, std::string>> pending_high_frequency_{};
};

}  // namespace st::ui
