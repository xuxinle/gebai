#pragma once

/// **JS ↔ C++ 桥**：把 C++ 能力暴露给脚本，并把脚本文件装进来。
///
/// ## 它补的是哪一块（现状与动机）
///
/// 在这之前，脚本能做的只有「读快照 / 写属性 / 绑事件」（`script_api.js` 的
/// `$()`/`on()`/`every()`）和「声明式建树」（`declarative.js` + `__d_*` 窄桥）。
/// **脚本不能调用应用自己的 C++ 函数**——而"用 JS 写界面逻辑、把真实能力
/// （读文件、跑命令、查数据库、算业务）留在 C++"正是这套架构的分工。
/// 没有这一层，纯 JS 应用只能是个演示。
///
/// ## 用法（三步）
///
/// ```cpp
/// st::ui::ScriptBridge bridge(app.script(), app.root());
/// bridge.expose("fs.read", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
///   return st::Json(st::fs::read_text(args.at(0).get<std::string>()).value_or(""));
/// });
/// bridge.load("app.js");     // 脚本里 `const text = app.fs.read("a.txt");`
/// ```
///
/// 脚本侧看到的是**命名空间化的全局对象**：`app.fs.read(...)`（`expose("fs.read", …)`
/// 的点号直接映射成对象层级）。这样不必污染全局、也不会与内建 API 撞名。
///
/// ## 三条约定（都是"不这样就会踩"的）
///
/// 1. **JS 名字不以下划线开头**：`__` 前缀是**内建扩展层**的命名段
///    （`__d_*` 树操作、`__snapshot` 等）。应用桥占用它就会与框架抢名字，
///    而且症状是"某个框架桥突然行为变了"——极难定位。这里直接拒绝。
/// 2. **回调抛错 → JS 异常**：C++ 侧返回 `Result` 的失败要变成脚本能捕获的异常，
///    而不是静默 `undefined`。否则"读文件失败了"在 JS 里表现为"拿到空字符串"。
/// 3. **载入顺序**：`expose` 必须在 `load` 之前——脚本顶层代码就可能在用它们
///    （模块化脚本尤其如此）。
///
/// ## 与 `ScriptHost::register_function` 的关系
///
/// 后者是**引擎级原语**（直接把函数注册进引擎，名字即全局名，`__` 前缀约定靠人守）。
/// 本层是**应用级契约**：命名空间映射、名字校验、文件装载、错误转异常、
/// 以及"哪些函数被暴露了"的可查询面（诊断用）。应用不该直接碰引擎原语。

#include <functional>
#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json_fwd.hpp"

namespace st::ui {

class ScriptHost;
class UiRoot;

/// 宿主函数签名：收参数数组、返回 `Result<Json>`（失败会变成 JS 异常）。
using ScriptBridgeFn = std::function<Result<st::Json>(const std::vector<st::Json>&)>;

class ScriptBridge {
 public:
  /// 绑定到已就绪的脚本宿主。
  ///
  /// `root` 可为空：只有需要**元素相关**的便利函数（`ui.set`/`ui.invoke` 这类）才用它。
  /// 传入时构造会额外暴露一组 `ui.*` 便利函数（读写属性 / 触发动作 / 查元素），
  /// 它们是 `ui::apply_properties` / `ui::invoke_element` 的薄包装——
  /// **不是**另立一套语义（与协议 `set`/`invoke` 同一份实现）。
  ScriptBridge(ScriptHost& script, UiRoot* root = nullptr);
  ~ScriptBridge();
  ScriptBridge(ScriptBridge&&) noexcept;
  auto operator=(ScriptBridge&&) noexcept -> ScriptBridge&;
  ScriptBridge(const ScriptBridge&) = delete;
  auto operator=(const ScriptBridge&) -> ScriptBridge&;

  /// 暴露一个 C++ 函数给脚本。
  ///
  /// `name` 允许点号分层（`"fs.read"` → 脚本里 `app.fs.read`）。
  /// 返回 `false`：名字非法（空/以下划线开头/含连续点）或注册失败——
  /// **原因看 `last_error()`**。
  auto expose(std::string name, ScriptBridgeFn function) -> bool;

  /// 暴露**框架级**运行时原语（名字允许下划线开头）。
  ///
  /// 为何单独开一个入口而不是给 `expose` 加开关：下划线前缀是框架保留段，
  /// 而“允许下划线”的开关迟早会被应用摸到（那就等于没有约定）。
  /// 框架的脚本原语（`__bridgeBuild` 等）走它；**应用代码不该调**。
  auto expose_internal(std::string name, ScriptBridgeFn function) -> bool;

  /// 加载并执行一个脚本文件（`.js`）。
  ///
  /// **不是** `eval` 的别名：读文件失败会返回带路径的错误（`load` 的失败原因
  /// 必须是"哪一步错了"，而不是一句"脚本出错"）。
  auto load(const std::string& path) -> Status;

  /// 装载并执行一段脚本源码（与 `load` 同一份实现，只是不读文件）。
  ///
  /// 热重载用它把"已经读进内存 + 已做过语法检查"的内容重放进来：先检查全部、
  /// 通过后才落地，任何一页写错都不会让旧逻辑半途消失（见 `ui::HotReload`）。
  auto run(std::string_view source, std::string_view filename = "<inline>") -> Status;

  /// 已暴露的名字（诊断/测试面：**能用什么**应当可查）。
  ///
  /// ⚠ 报的是**调用方传入的名字**（`"counter.bump"`），不是脚本侧的完整路径
  ///（`"app.counter.bump"`）：这个清单回答的是"我 expose 过什么"，
  /// 而"脚本里要写 `app.` 前缀"是**统一的约定**（见文件头），
  /// 每条都带前缀只会让清单与调用点对不上、还得在脑子里做一次翻译。
  [[nodiscard]] auto exposed() const -> const std::vector<std::string>&;
  /// 已加载的文件路径（按加载顺序）。
  [[nodiscard]] auto loaded_files() const -> const std::vector<std::string>&;

  /// 最近一次 `expose` / `load` 的失败原因（空 = 没失败过）。
  ///
  /// 为何必要：这两件事失败的原因很多（名字非法 / 名字冲突 / 引擎拒绝 /
  /// 挂载代码抛错 / 文件读不到 / 语法错），而只返回 `bool` 会把它们压成同一个
  /// “失败”——排查时就只能猜。
  [[nodiscard]] auto last_error() const -> std::string;

  /// 调用脚本里的全局函数（C++ → JS 的反向通道）。
  [[nodiscard]] auto call(const std::string& function,
                          const std::vector<st::Json>& arguments) -> Result<st::Json>;

 private:
  struct Impl;
  /// 真正的注册路径（`expose` 与 `expose_internal` 共用，避免两份逻辑分裂）。
  auto register_one(std::string name, ScriptBridgeFn function) -> bool;
  std::unique_ptr<Impl> impl_;
};

}  // namespace st::ui
