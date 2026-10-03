#pragma once

/// 声明式 UI：组件（Component）+ 状态（State）+ 重组器（Composer）。
///
/// 对标 Jetpack Compose（remember/mutableStateOf/重组）与鸿蒙 ArkTS（@Component/@State/build）。
/// 形态：
/// ```cpp
/// struct CounterPage : dsl::Component {
///   dsl::State<int> count{0};                     // ≈ @State count = 0
///   void build(dsl::Composer& c) override;        // ≈ build()
/// };
/// dsl::mount(app, std::make_shared<CounterPage>());
/// ```
///
/// 三条铁律（与 docs/declarative.md 的不变式一致）：
/// 1. 真值树唯一——DSL 产出的修改全部经 `ui::apply_properties`/元素树 API 落地；
/// 2. 一套语义——`tree`/`get`/`set`/`invoke` 对 DSL 元素照常可用；
/// 3. 不挂即零开销——不 mount 声明式根时帧循环无任何额外成本。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "st/ext/json.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {
class MenuBar;   // 菜单栏（组件头在实现文件；声明式这里只用引用）
}

namespace st::ui::dsl {

class Composer;
class StateBase;

namespace detail {
/// State 读/写的接线点（dsl.cpp 实现；StateBase 虚函数默认实现调用）。
void on_state_read(StateBase* state);
void on_state_write(StateBase* state);
}

/// 声明式组件基类（≈ @Component struct）。
class Component {
 public:
  virtual ~Component() = default;
  /// 构建界面（≈ build()）。重组时反复调用：只做「声明」，不做副作用重的活。
  virtual void build(Composer& c) = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// 状态：读时登记依赖、写时失效并调度重组
// ──────────────────────────────────────────────────────────────────────────

/// 状态基类（依赖登记与失效传播；持有方为 shared_ptr——闭包捕获安全）。
class StateBase {
 public:
  virtual ~StateBase() = default;
  /// 读：把「当前重组作用域」登记为订阅者（重组期间由 thread-local 提供当前 Composer）。
  virtual void subscribe() { detail::on_state_read(this); }
  /// 写：失效订阅它的所有活跃 Composer（调度下一帧重组）。
  virtual void invalidate() { detail::on_state_write(this); }
};

/// thread-local：当前正在重组的 Composer（State 读时用它登记依赖）。
/// 无重组进行时为 nullptr（如事件回调里读状态——不登记依赖，属正常读）。
[[nodiscard]] Composer* current_composer() noexcept;

/// 响应式状态句柄（≈ mutableStateOf / @State）。State<T> 成员由 Component 持有；
/// 事件回调捕获 `this` 安全（Component 由宿主 shared_ptr 持有）。
template <class T>
class State : public StateBase {
 public:
  explicit State(T initial = {}) : value_(std::move(initial)) {}
  State(const State&) = delete;
  auto operator=(const State&) = delete;

  /// 读值（重组期间调用会登记依赖）。
  [[nodiscard]] const T& value() const {
    invalidate_const();
    return value_;
  }
  /// 写值（相等不触发；变化才失效 + 调度重组）。
  void set(T next) {
    if (next == value_) return;
    value_ = std::move(next);
    invalidate();
  }
  /// 原地修改（读-改-写便捷路径；总是触发失效——无法廉价判等）。
  template <class F>
  void update(F&& fn) {
    fn(value_);
    invalidate();
  }

 private:
  /// const 读路径的依赖登记（subscribe 非 const，这里转发）。
  void invalidate_const() const { const_cast<State*>(this)->subscribe(); }

  T value_{};
};

/// 依赖集（memo/effect 用）：按 StateBase 指针比较。
struct Deps {
  std::vector<StateBase*> items{};
};

// ── 异步资源（`resource`，对应 JS 侧 `useResource`）───────────────────────
//
// 语义（与 docs/declarative.md §3.1 一致）：
// - 返回 `State<AsyncValue<T>>` 引用，状态为 pending/ok/error；
// - `input` 变化 → 重发（代次计数：旧代次结果丢弃）；
// - fetcher 在**工作线程**上跑（不阻塞帧），结果回主线程后才写状态；
// - 依赖 `Composer::pump_async()` 在主循环每帧取结果（不能从工作线程写 State——
//   重组是单线程的，跨线程写会与遍历中的树竞争）。

enum class AsyncStatus : std::uint8_t { Pending, Ok, Error };

/// 异步值（三态 + 负载）。
template <class T>
struct AsyncValue {
  AsyncStatus status{AsyncStatus::Pending};
  T value{};
  std::string error{};

  auto operator==(const AsyncValue& other) const -> bool = default;
};

/// 取消牌：一次异步执行的「还要不要这结果」。（≈ JS `AbortSignal`）
///
/// 两处用到：
/// - **输入变化**：`resource` 开新代时翻牌——旧代的结果丢弃（不再写状态），
///   旧代的工作线程可在耗时点调 `is_cancelled()` 提前放弃；
/// - **Composer 销毁**：全牌翻掉（未开始的任务丢弃，已开始的在下一个检查点退出）。
///
/// 为何用 `shared_ptr<atomic<bool>>` 而不是 Composer 里的一个标记：取消牌的生命周期
/// 长于 Composer（线程可能还在跑），引用计数保证牌总是有效——销毁时翻牌即可，
/// 不需要等线程结束（避免析构阻塞在长任务上）。
class AsyncCancel {
 public:
  AsyncCancel() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
  /// 是否已被取消（工作线程在耗时点调；fetcher 可接收它以提前退出）。
  [[nodiscard]] auto is_cancelled() const noexcept -> bool {
    return flag_ != nullptr && flag_->load(std::memory_order_relaxed);
  }
  void cancel() noexcept {
    if (flag_ != nullptr) flag_->store(true, std::memory_order_relaxed);
  }

 private:
  std::shared_ptr<std::atomic<bool>> flag_{};
};

/// 订阅登记辅助（Composer 侧调用）。
void detail_subscribe(StateBase& state);

// ──────────────────────────────────────────────────────────────────────────
// 重组器
// ──────────────────────────────────────────────────────────────────────────

/// 重组护栏（双宿主同参数；docs/declarative.md §4.2）。
struct Guardrails {
  double frame_budget_ms{4.0};   ///< 单帧重组预算（超时作用域顺延下一帧）
  int max_depth{64};             ///< 作用域嵌套上限（防递归 build）
  bool freeze_on_error{true};    ///< build 抛异常冻结该作用域（保留上一帧 UI）
};

/// 重组结果（诊断与测试用）。
struct ReconcileStats {
  int scopes_rerun{0};        ///< 本次重组重跑的作用域数
  int elements_created{0};    ///< 新建元素数
  int elements_removed{0};    ///< 移除元素数
  int properties_applied{0};  ///< 经 apply_properties 落地的属性数
  bool budget_exceeded{false};///< 预算耗尽（剩余作用域顺延）
  std::string error;          ///< 首个错误（frozen 时填）
};

/// 重组器：驱动 Component::build 产出/更新真值树。
///
/// 生命周期：`mount()` 后由 `DeclarativeHost` 持有；`reconcile()` 在帧首调用
/// （UiRoot 布局之前），事件回调写状态 → 下一帧 reconcile → mark_dirty → 正常绘制。
class Composer {
 public:
  explicit Composer(UiRoot& root, Guardrails guardrails = {});
  ~Composer();
  Composer(const Composer&) = delete;
  auto operator=(const Composer&) = delete;

  /// 挂载根组件（替换之前的根）。返回是否成功。
  auto mount(std::shared_ptr<Component> root_component) -> bool;
  /// 挂载到**既有元素的子树**（不替换 `UiRoot::content()`）——宿主元素由调用方提供，
  /// 声明式产出的顶层元素成为它的子元素（位置对齐 + 末尾裁剪同普通容器）。
  /// 用途：宿主界面（如 gallery）里的一页用声明式描述，其余部分仍手搭。
  auto mount_into(Element& host, std::shared_ptr<Component> root_component) -> bool;
  /// 重组一帧：失效作用域重跑 build → diff → 落地。返回统计。
  auto reconcile() -> ReconcileStats;
  /// 是否有失效作用域待重组。
  [[nodiscard]] auto dirty() const noexcept -> bool;
  /// 强制全量重建（调试/主题切换后的兜底路径）。
  void rebuild_all();

  // —— build() 内可用的声明 API（由组件包装函数调用；也可直接用）——
  /// 登记一个状态订阅（State::value() 读时自动调到这里）。
  void add_dependency(StateBase* state);
  /// 内部：State 写通知（订阅了它才标脏）。
  void notify_state_written(StateBase* state);

  // —─ hook 槽位（`resource` 等需要跨重组保持的状态；与 JS 侧游标同理）—─
  /// 本次 build 的 hook 序号（每次重组从 0 起，逐个递增）。
  [[nodiscard]] auto hook_index() -> std::size_t;
  [[nodiscard]] auto hook_state_count() const -> std::size_t;
  [[nodiscard]] auto hook_state_ptr(std::size_t index) -> StateBase*;
  void hook_state_replace(std::size_t index, std::unique_ptr<StateBase> state);

  /// 按序号取/建强类型状态槽（`resource`/自定义 hook 用）。
  template <class T>
  [[nodiscard]] auto state_slot(std::size_t index, T initial) -> State<T>& {
    StateBase* existing = index < hook_state_count() ? hook_state_ptr(index) : nullptr;
    if (auto* typed = dynamic_cast<State<T>*>(existing); typed != nullptr) return *typed;
    auto fresh = std::make_unique<State<T>>(std::move(initial));
    State<T>* raw = fresh.get();
    if (index < hook_state_count()) {
      hook_state_replace(index, std::move(fresh));
    } else {
      hook_state_push(std::move(fresh));
    }
    return *raw;
  }

  /// 子作用域登记（`sub_component` 调；首次记录宿主位置，之后复用）。
  void register_sub_scope(std::shared_ptr<Component> component, const std::string& key);

  // —─ 异步（`resource` 的底座）—─
  /// 输入指纹变化时开新一代：**旧代翻牌取消**，返回新 token；未变化返回 0（不重发）。
  [[nodiscard]] auto async_begin(std::size_t slot, std::string fingerprint) -> std::uint64_t;
  /// 代次是否仍为当前（结果写回前的校验——旧代丢弃）。
  [[nodiscard]] auto async_token_current(std::uint64_t token) -> bool;
  /// 取某代次的取消牌（`resource` 交给 fetcher，供其在耗时点提前退出）。
  [[nodiscard]] auto async_cancel_for(std::uint64_t token) -> AsyncCancel;

  /// 开一次异步任务：`work` 在**工作线程**执行，结果回主线程校验代次后写入 `cell`。
  /// `cell` 必须是本 Composer 的 `state_slot` 持有的状态（生命周期与 Composer 对齐）。
  template <class T>
  void async_run(std::function<AsyncValue<T>()> work, std::uint64_t token,
                 State<AsyncValue<T>>* cell) {
    // ① 工序（工作线程）：结果经 `post_to_main` 回主线程
    run_on_worker([this, work = std::move(work), token, cell]() mutable {
      AsyncValue<T> result = work();
      // ② 回主线程：代次校验 + 写状态
      post_to_main([this, result = std::move(result), token, cell]() {
        if (!async_token_current(token)) return;   // 旧代次（已被取消）：丢弃
        cell->set(std::move(result));
      });
    });
  }

  /// 在主线程执行的任务入队（`pump_async` 取走）；线程安全。
  void post_to_main(std::function<void()> task);
  /// 在**工作线程**执行任务（起短命线程；完成后经 `post_to_main` 回主线程）。
  void run_on_worker(std::function<void()> task);
  /// 主线程每帧：执行已投递任务，返回执行数。
  auto pump_async() -> std::size_t;

  /// 清理未认领的 overlay（`reconcile` 末尾调）。
  void sweep_overlays();
  /// 叠加层槽位登记（Overlays 的内部数据在实现文件）。
  struct Overlays;
  /// 当前正在构建的父元素（组件包装的落点）。
  [[nodiscard]] Element* current_parent() const noexcept;
  /// 压入/弹出构建父元素（组件包装函数用；RAII 由 BuildScope 保证）。
  void push_parent(Element* parent);
  void pop_parent();
  /// 组内创建元素：type → 工厂构造；props 经 `ui::apply_properties` 落地。
  /// 返回裸指针（所有权在真值树上）。
  [[nodiscard]] auto create_element(std::string_view type, const st::Json& props,
                                    std::string_view key = {}) -> Element*;
  /// 事件回调直连（不经属性面）：Button.on_click 等一等接口。
  void connect(Element& element, std::string_view event, std::function<void()> handler);

  // —— 声明式叠加层（overlay）生命周期 ——
  //
  // 难点：overlay 挂在 root 上而不是声明树的当前位置，「这一帧还要不要它」不能靠位置
  // 对齐推出来。解法：**按 key 认领**——重组开头把所有声明式 overlay 标为“未认领”，
  // 本次 build 里重新声明的 key 会被认领（复用同一元素），末尾仍未认领的被移除。
  //
  // ```cpp
  // if (palette_open_.value()) {
  //   overlay(c, "palette", {}, [&] { ... });   // 关掉时本次不声明 → 自动消失
  // }
  // ```

  /// 声明一个叠加层（`key` 是身份：同 key 复用元素，帧间状态不丢）。
  [[nodiscard]] auto overlay_slot(std::string_view key) -> Element*;
  /// 把元素挂进某个 overlay 槽（槽不存在则新建并挂到 root）。
  void attach_overlay(Element& host, std::unique_ptr<Element> content);

  /// 注册全局快捷键（转发到 `UiRoot::register_shortcut`，供声明式组件用）。
  void register_shortcut(std::string key, UiRoot::Shortcut mods, std::function<bool()> handler);

  /// 压入强类型状态槽（`state_slot` 内部用；`StateBase` 已 type-erased）。
  void hook_state_push(std::unique_ptr<StateBase> state);

  /// 护栏（const 访问给测试）。
  [[nodiscard]] auto guardrails() const noexcept -> const Guardrails&;
  /// 真值树根（叠加层等需要 root 的声明 API 用）。
  [[nodiscard]] auto root() noexcept -> UiRoot&;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// build 作用域 RAII（包装函数用：进入时 push_parent + 捕获新建元素，退出时 pop）。
class BuildScope {
 public:
  BuildScope(Composer& c, Element* parent) : composer_(c) { c.push_parent(parent); }
  ~BuildScope() { composer_.pop_parent(); }
  BuildScope(const BuildScope&) = delete;
  auto operator=(const BuildScope&) = delete;

 private:
  Composer& composer_;
};

// ──────────────────────────────────────────────────────────────────────────
// 组件包装（32 内置组件的声明式入口；build() 里用）
// ──────────────────────────────────────────────────────────────────────────

/// 通用属性包（布局/外观，映射 `Style`；字符串键省着先走属性面+style 直写）。
///
/// 注：指定初始化器（`{.gap = 8.0f}`）**必须按声明顺序**——声明顺序即推荐书写顺序。
struct BoxProps {
  // 布局
  float gap{-1.0f};                       ///< -1 = 不设置
  float padding{-1.0f};
  float margin{-1.0f};
  float width{kAuto};
  float height{kAuto};
  bool grow{false};
  // 外观
  float radius{-1.0f};
  std::string id{};                       ///< 显式 id（空 = 自动）
  std::string key{};                      ///< 业务身份（自动 id 用 Type@key）
};

/// 应用 BoxProps 到元素（padding/gap 等映射 Style 成员；id/key 先行截获）。
void apply_box(Element& element, const BoxProps& props);

/// 行容器（子元素声明在 children 回调里）。
auto row(Composer& c, const BoxProps& props, std::function<void()> children) -> Element&;
/// 列容器。
auto column(Composer& c, const BoxProps& props, std::function<void()> children) -> Element&;
/// 文本（content 可为惰性闭包——重组时重新求值）。
auto text(Composer& c, std::function<std::string()> content, const BoxProps& props = {}) -> Element&;
/// 按钮（label + 点击回调）。
auto button(Composer& c, std::string label, std::function<void()> on_click,
            const BoxProps& props = {}) -> Element&;
/// 复选框。
auto checkbox(Composer& c, std::string label, bool checked,
              std::function<void(bool)> on_change = {}, const BoxProps& props = {}) -> Element&;
/// 开关。
auto switch_(Composer& c, bool checked, std::function<void(bool)> on_change = {},
             const BoxProps& props = {}) -> Element&;
/// 滑杆。
auto slider(Composer& c, float value, std::function<void(float)> on_change = {},
            const BoxProps& props = {}) -> Element&;
/// 输入框。
auto input(Composer& c, std::string value, std::function<void(std::string)> on_input = {},
           const BoxProps& props = {}) -> Element&;
/// 进度条。
auto progress(Composer& c, float value, const BoxProps& props = {}) -> Element&;
/// 徽标。
auto badge(Composer& c, std::string text, const BoxProps& props = {}) -> Element&;
/// 标题。
auto heading(Composer& c, std::string content, std::uint32_t level = 1,
             const BoxProps& props = {}) -> Element&;
/// 分隔线。
[[nodiscard]] auto divider(Composer& c, bool vertical = false) -> Element&;
/// 卡片容器。
[[nodiscard]] auto card(Composer& c, const BoxProps& props, std::function<void()> children)
    -> Element&;
/// 弹性/固定占位。
///
/// - `size > 0`：固定尺寸的空块。
/// - `size <= 0`（**含默认参数**）：**弹性空隙**（`grow = true`），把同行/同列的
///   其余内容推到两端。
///
/// 为什么默认值必须是弹性而不是“0 宽固定块”：`spacer()` 是右对齐的惯用写法，
/// “固定 0 宽”与“什么也不做”在观感上完全一样——而调用方要的是**推开**。
/// 早先默认 `size = 0` 且直接把它当尺寸写入，于是 `spacer()` 静默失效：
/// 标题栏的窗口控制按钮紧跟在标题文字后面，而不是贴右缘（实测 x=209 而非 1268）。
[[nodiscard]] auto spacer(Composer& c, float size = 0.0f) -> Element&;
/// 图标（自绘矢量集；`name` 是 IconName）。
[[nodiscard]] auto icon(Composer& c, std::string name, float size = 18.0f,
                        const BoxProps& props = {}) -> Element&;

// —— 逃生舱：任意组件 + 一等接口访问 ——
///
/// 声明式元素类型名（与 `make_element` 的注册名一致）。
///
/// 为什么要显式取 type 名而不是 `T{}.type()`：组件构造函数参数各异（`Button(label)`、
/// `Table(columns)`），`T{}` 不成立。取 type 名的正道是**问工厂**（`make_element` 的
/// 注册表就是权威），但那里需要 type 字符串——所以提供这个特化表（特化定义在
/// `dsl.cpp`，那里能看到全部组件类型）。新增组件时在源文件补一行。
template <class T>
[[nodiscard]] auto type_name() -> std::string;

/// 逃生舱：声明式创建任意组件 + 容器语义，构造后经配置回调接一等接口。
///
/// 为什么需要它：32 个内置组件各有**构造参数与一等接口**（`Tabs::sync_tabs`、
/// `MenuBar::on_action`、`CodeEditor::on_change`…）——为每个都写专用包装是无穷尽的追尾
/// （新组件就要新包装），而属性面（`apply_properties`）只覆盖可序列化值。
/// `custom<T>` 给出**一个**通用入口：声明式创建 + 容器语义 + 强类型指针。
///
/// ```cpp
/// auto& ed = custom<CodeEditor>(c, [](CodeEditor& e) { e.set_language("cpp"); });
/// ed.on_change = ...;   // 一等接口直用
/// ```
template <class T>
[[nodiscard]] auto custom(Composer& c, std::function<void(T&)> configure = {},
                          const BoxProps& props = {}, std::string_view key = {}) -> T& {
  Element* element = c.create_element(type_name<T>(), st::Json::object(), key);
  // 未知类型：**必须是显式错误**——静默返回假元素会让界面缺块而不报，最难查。
  // 这里不做异常（禁令 L5：业务错误走 Result/致命断言）：编译期类型 + 注册表
  // 应当一致，不一致就是程序缺陷——直接终止并打印出缺的类型名。
  if (element == nullptr) {
    std::fprintf(stderr, "[dsl] custom<T> 未注册的组件类型: %s（补 type_name 特化）\n",
                 type_name<T>().c_str());
    std::abort();
  }
  apply_box(*element, props);
  auto* typed = dynamic_cast<T*>(element);
  if (typed != nullptr && configure) configure(*typed);
  return *static_cast<T*>(element);
}

/// 子作用域：容器型组件的 `custom` + 子节点声明（如 ScrollView 内嵌 List）。
template <class T, class Fn>
auto custom_container(Composer& c, Fn&& children, std::function<void(T&)> configure = {},
                      const BoxProps& props = {}) -> T& {
  T& element = custom<T>(c, std::move(configure), props);
  BuildScope scope(c, &element);
  children();
  return element;
}

/// 叠加层（overlay）：模态/浮层内容挂到 `UiRoot` 的叠加层（在内容之上）。
///
/// `key` 是身份：同 key 复用同一个 overlay 宿主元素（帧间状态保持）；
/// **本次 build 不声明 = 本帧消失**（重组末尾清理未认领的 overlay）。
/// 典型用法：`if (open_.value()) overlay(c, "palette", {}, [&] { ...面板内容... });`
[[nodiscard]] auto overlay(Composer& c, std::string_view key, const BoxProps& props,
                          std::function<void()> children) -> Element&;

/// 菜单栏（数据驱动；`on_action(menu_id, item_id)` 是唯一动作出口）。
struct MenuItemData {
  std::string id{};
  std::string label{};
  bool separator{false};
};
struct MenuData {
  std::string id{};
  std::string label{};
  std::vector<MenuItemData> items{};
};
/// 声明菜单栏；返回 MenuBar 引用（调用方留存，供 `menu_panel_overlay` 挂下拉面板）。
/// `on_open_menu(index)` 由调用方接：把「打开哪个菜单」写进自己状态，下一帧由
/// `menu_panel_overlay` 声明面板。
[[nodiscard]] auto menu_bar(Composer& c, const std::vector<MenuData>& menus,
                            std::function<void(const std::string& menu,
                                               const std::string& item)> on_action,
                            std::function<void(std::size_t)> on_open_menu,
                            const BoxProps& props = {}) -> MenuBar*;
/// 把 `bar` 的第 `index` 个菜单的下拉面板挂成 overlay（**只在菜单打开时调用**）。
/// 槽位 key 取 `bar->menu_id(index)`；不调用 → 下一帧 sweep 自动移除。
void menu_panel_overlay(Composer& c, MenuBar& bar, std::size_t index);

// —— 列表类（数据型子项走 sync 接口，v1 覆盖 List；Table/Tabs 见 v1.1）——
/// 列表项数据。
struct ListItemData {
  std::string key{};
  std::string label{};
  std::string subtitle{};
};
/// 列表（数据驱动；按 key 复用，语义同 List::sync_items）。
auto list(Composer& c, const std::vector<ListItemData>& items,
          std::function<void(std::size_t index)> on_click = {}, const BoxProps& props = {})
    -> Element&;
/// Tab 页数据（`Tabs::sync_tabs` 语义：key 复用，活动态跟 key）。
struct TabData {
  std::string key{};
  std::string label{};
  bool modified{false};
  bool closable{true};
};
/// 标签栏（数据驱动：按 key 复用标签元素）。
auto tabs(Composer& c, const std::vector<TabData>& items, std::size_t active,
          std::function<void(std::size_t)> on_change = {},
          std::function<void(const std::string& key)> on_close = {}, const BoxProps& props = {})
    -> Element&;

// —— 构造期属性组件（选项/列/节点在构造时给定；属性面表达不了）——

/// 下拉选项（`Select::set_options` 的声明式入口）。
struct SelectOptionData {
  std::string value{};
  std::string label{};
};
/// 下拉选择（选项 + 选中索引 + 变更回调，均数据驱动）。
auto select(Composer& c, const std::vector<SelectOptionData>& options,
            std::optional<std::size_t> selected = std::nullopt,
            std::function<void(std::size_t)> on_change = {}, const BoxProps& props = {})
    -> Element&;

/// 表格列定义。
struct TableColumnData {
  std::string label{};
  float width{kAuto};
};
/// 表格（列 + 行数据驱动；行点击回调）。
auto table(Composer& c, const std::vector<TableColumnData>& columns,
           const std::vector<std::vector<std::string>>& rows,
           std::function<void(std::size_t)> on_row_click = {}, const BoxProps& props = {})
    -> Element&;

/// 树节点（扁平化数组 + depth；数据表达状态——与 `Tree::sync_nodes` 同语义）。
struct TreeNodeData {
  std::string key{};
  std::string label{};
  bool expanded{false};
  bool is_dir{false};
  int depth{0};
};
/// 树（节点数组数据驱动；展开/选中回调）。
auto tree(Composer& c, const std::vector<TreeNodeData>& nodes,
          std::function<void(const std::string& key, bool expanded)> on_toggle = {},
          std::function<void(const std::string& key)> on_select = {}, const BoxProps& props = {})
    -> Element&;
/// For 控制流：数据数组按 key 对齐复用子元素（每个 item 声明一个子作用域）。
/// key_fn 取业务身份；item_fn 声明该项内容。
template <class T>
void for_each(Composer& c, const std::vector<T>& items,
              std::function<std::string(const T&)> key_fn,
              std::function<void(const T&)> item_fn) {
  // v1 实现：每次重组重建 items 区段（key 对齐复用在 M2 重组器成熟后接入；
  // 数据型组件优先走 list() 的 sync_items 路径）
  for (const auto& item : items) item_fn(item);
}

// ──────────────────────────────────────────────────────────────────────────
// 宿主：挂在应用上驱动重组
// ──────────────────────────────────────────────────────────────────────────

/// 声明式宿主：持有 Composer，接进 `st::app::Application` 的帧循环。
/// （M2 与 Application 集成；此处先提供独立可测的形态。）
class DeclarativeHost {
 public:
  explicit DeclarativeHost(UiRoot& root, Guardrails guardrails = {});
  auto mount(std::shared_ptr<Component> root_component) -> bool;
  /// 子树挂载：声明式树挂到 `host` 的子位（宿主元素已在真值树上）。
  auto mount_into(Element& host, std::shared_ptr<Component> root_component) -> bool;
  /// 帧首推进（reconcile 有失效才做事；UiRoot 布局前调用）。
  /// 内部会先执行已投递的异步结果（`pump_async`）——异步任务的结果同帧可见。
  auto tick() -> ReconcileStats;
  /// 只推进异步结果（不起重组）；需要更细粒度控制时用。返回执行的任务数。
  auto pump_async() -> std::size_t;
  [[nodiscard]] auto dirty() const noexcept -> bool;
  [[nodiscard]] auto stats() const noexcept -> const ReconcileStats&;

 private:
  std::unique_ptr<Composer> composer_;
  ReconcileStats last_{};
};

/// 子组件作用域：让一个子 `Component` 拥有**独立的重组作用域**——
/// 它订阅的状态变化只重跑它自己，不重跑父组件（更细粒度的失效传播）。
///
/// ```cpp
/// struct Page : Component {
///   State<int> title_seq{0};
///   void build(Composer& c) override {
///     column(c, {}, [&] {
///       text(c, [&] { return "标题 #" + std::to_string(title_seq.value()); });
///       sub_component(c, subs_[0]);   // 子组件内的状态变化不触发本函数重跑
///     });
///   }
///   std::vector<std::shared_ptr<Component>> subs_{std::make_shared<Sidebar>()};
/// };
/// ```
///
/// 实现要点：重跑前把当前组件/父游标压栈，子作用域的 `build()` 在**同一个 Composer**
/// 上运行（真值树位置由游标对齐）；失效时只把子作用域标脏，不脏父。
void sub_component(Composer& c, std::shared_ptr<Component> component,
                   std::string_view key = {});

/// 便捷挂载：root + component → 挂载并立即重组一次（无头测试/简单场景）。
auto mount(UiRoot& root, std::shared_ptr<Component> root_component, Guardrails guardrails = {})
    -> std::unique_ptr<DeclarativeHost>;

/// 便捷挂载（子树形态）：声明式树挂到既有元素 `host` 下并立即重组一次。
/// 宿主界面里「一页用声明式描述」的场景用它（整根语义见 `mount`）。
auto mount_into(UiRoot& root, Element& host, std::shared_ptr<Component> root_component,
                Guardrails guardrails = {}) -> std::unique_ptr<DeclarativeHost>;

/// 异步资源 hook（≈ JS 侧 `useResource`）：
///
/// ```cpp
/// const auto data = resource<std::vector<std::string>>(c, [](const std::string& query) {
///   return st::fs::list(query);          // 工作线程上跑（不阻塞帧）
/// }, query.value());
/// if (data.value().status == AsyncStatus::Ok) text(c, [&]{ return data.value().value.size(); });
/// ```
///
/// - `input` 变化 → 重发（**旧代翻牌取消**：迟到结果丢弃）；相等则不重发；
/// - fetcher 在**工作线程**执行，结果经 `Composer::pump_async()` 回主线程写状态
///   （跨线程写 State 会与遍历中的树竞争——硬约束）；
/// - `T` 是资源值的类型（fetcher 的返回类型）。
///
/// **取消**：需要提前退出长任务时，让 fetcher 收 `AsyncCancel` 参数——
/// 在耗时点检查 `cancel.is_cancelled()` 并返回（这是 C++ 里唯一安全的取消方式，
/// 强杀线程会留下锁/堆损坏）：
///
/// ```cpp
/// resource<Data>(c, [](const std::string& q, AsyncCancel cancel) {
///   for (auto& chunk : chunks(q)) {
///     if (cancel.is_cancelled()) return Data{};   // 输入已变，不必算完
///     accumulate(chunk);
///   }
///   return result;
/// }, query.value());
/// ```
template <class T, class Fetcher, class Input>
[[nodiscard]] auto resource(Composer& c, Fetcher fetcher, const Input& input)
    -> State<AsyncValue<T>>& {
  const std::size_t slot = c.hook_index();
  State<AsyncValue<T>>& cell = c.state_slot<AsyncValue<T>>(slot, AsyncValue<T>{});
  // 输入指纹（相等的输入不重发）；支持 string / 算术 / 字符串可转换类型
  std::string fingerprint;
  if constexpr (std::is_convertible_v<Input, std::string>) {
    fingerprint = std::string(input);
  } else if constexpr (std::is_arithmetic_v<Input>) {
    fingerprint = std::to_string(input);
  } else {
    // 不可序列化的类型（自定义结构体）：用**对象地址**作指纹。
    // 语义：同一对象（地址不变）视为同一输入，不重发；换了对象则重发。
    // 这是保守而正确的选择（宁可多发不少发）——真要精确比较就传可比较的输入。
    fingerprint = std::to_string(reinterpret_cast<std::uintptr_t>(&input));  // lint-allow: L6 地址作指纹（不可序列化类型的保守回退，非哨兵解引用）
  }
  const std::uint64_t token = c.async_begin(slot, std::move(fingerprint));
  if (token != 0) {
    AsyncValue<T> pending{};
    pending.status = AsyncStatus::Pending;
    cell.set(std::move(pending));
    const AsyncCancel cancel = c.async_cancel_for(token);   // 交给 fetcher：耗时点可提前退出
    Input input_copy = input;
    c.async_run<T>(
        [fetcher, input_copy, cancel]() -> AsyncValue<T> {
          AsyncValue<T> result;
          try {
            if constexpr (std::is_invocable_v<Fetcher, Input, AsyncCancel>) {
              result.value = fetcher(input_copy, cancel);   // fetcher 想收取消牌
            } else {
              result.value = fetcher(input_copy);            // 常规 fetcher
            }
            result.status = AsyncStatus::Ok;
          } catch (const std::exception& error) {
            result.status = AsyncStatus::Error;
            result.error = error.what();
          } catch (...) {
            result.status = AsyncStatus::Error;
            result.error = "未知异常";
          }
          return result;
        },
        token, &cell);
  }
  return cell;
}

/// 元素工厂：类型名 → 构造（dsl 内部与后续协议/脚本建元素共用；未知类型返回 nullptr）。
[[nodiscard]] auto make_element(std::string type) -> std::unique_ptr<Element>;

}  // namespace st::ui::dsl
