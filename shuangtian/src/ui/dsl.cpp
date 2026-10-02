#include "st/ui/dsl.hpp"

#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/select.hpp"
#include "st/ui/components/slider.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/components/tree.hpp"

namespace st::ui::dsl {

// ── thread-local 当前 Composer + 全局活跃 Composer 注册表 ────────────────

namespace {
thread_local Composer* tls_composer = nullptr;
/// 全局活跃 Composer 表（State 写时找不到 thread-local 时逐个通知——
/// 事件回调在主循环发起，不在重组线程上，thread-local 为空）。
std::unordered_set<Composer*> active_composers;
}

auto current_composer() noexcept -> Composer* { return tls_composer; }

namespace detail {

void on_state_read(StateBase* state) {
  if (tls_composer != nullptr && state != nullptr) tls_composer->add_dependency(state);
}

void on_state_write(StateBase* state) {
  if (state == nullptr) return;
  // 事件回调线程：thread-local 为空 → 通知全部活跃 Composer（各自判断是否订阅过它）
  if (tls_composer != nullptr) {
    tls_composer->notify_state_written(state);
    return;
  }
  for (Composer* composer : active_composers) composer->notify_state_written(state);
}

}  // namespace detail

// ── Composer::Impl ────────────────────────────────────────────────────────

struct Composer::Impl {
  UiRoot& root;
  Guardrails guardrails{};
  std::shared_ptr<Component> component{};
  bool mounted{false};

  // 依赖图：StateBase* → 订阅作用域集合（v1 单作用域 = 根组件 build，
  // 多作用域（子组件作用域）在 M2 后续版本引入；先把依赖收集管道打通）。
  std::unordered_set<StateBase*> subscribed_states{};
  bool scope_dirty{true};         // 根作用域是否需要重跑
  std::vector<Element*> parent_stack{};   // 构建期父元素栈
  ReconcileStats last_stats{};
  // 已建元素追踪：v1 简化——根 Panel 持久，build 只更新既有元素（位置对齐）。
  Element* root_panel{nullptr};       // 已废弃（单根语义后不再预建）
  bool root_replaced{false};          // 本次 reconcile 是否已落根元素
  /// 声明式叠加层：key → 宿主元素（挂在 root overlay 通道上）。
  /// 生命周期 = 「本次 build 有没有认领它」（见 Composer::overlay_slot/sweep_overlays）。
  std::unordered_map<std::string, Element*> overlays{};
  std::unordered_set<std::string> claimed_overlays{};

  // —─ hooks：跨重组保持的强类型状态（`resource` 等）—─
  std::vector<std::unique_ptr<StateBase>> hook_states{};
  std::size_t hook_cursor{0};

  // —─ 作用域（多作用域细粒度重组）—─
  //
  // 全局 `scope_dirty` 只能表达“整根重跑”。
  // 子作用域（`sub_component`）有自己的依赖集与脏标记：
  // 只订阅了自己的状态变时，**只重跑子作用域**，父不重跑。
  struct Scope {
    std::shared_ptr<Component> component{};
    std::string key{};                              // 兄弟间身份（重复则复用同一 scope）
    std::unordered_set<StateBase*> deps{};
    bool dirty{true};
    /// 宿主位置：挂到哪个父元素下的第几个子位（nullptr 父 = 根槽位）。
    Element* host_parent{nullptr};
    std::size_t slot{0};
    Scope* parent{nullptr};
    std::vector<std::unique_ptr<Scope>> children{};

    [[nodiscard]] auto find_child(std::string_view wanted) -> Scope* {
      for (auto& child : children) {
        if (child->key == wanted) return child.get();
      }
      return nullptr;
    }
  };
  std::unique_ptr<Scope> root_scope{};
  /// 已注册的子作用域（按 key 查找；真值树位置记录在 scope 里）。
  std::vector<std::unique_ptr<Scope>> sub_scopes{};

  [[nodiscard]] auto find_sub_scope(std::string_view key) -> Scope* {
    for (auto& scope : sub_scopes) {
      if (scope->key == key) return scope.get();
    }
    return nullptr;
  }

  /// 重跑一个子作用域：把构建父栈恢复成它的宿主位置，再跑它的 build。
  ///
  /// 关键：**重置子作用域子树内的游标**。父元素的游标记录“下一个子位”，
  /// 上次跑完停在 N；若不重置，本次就会从 N 往后追加（旧元素残留、新元素重复）。
  /// 根作用域靠 `child_cursor.clear()` 达成同一目的，子作用域只清自己那棵。
  void run_sub_scope(Scope& scope) {
    scope.deps.clear();
    const std::size_t saved_cursor = child_cursor[scope.host_parent];
    child_cursor[scope.host_parent] = scope.slot;   // 回到注册时的位置
    reset_subtree_cursor(scope.host_parent, scope.slot);
    parent_stack.clear();
    parent_stack.push_back(scope.host_parent);
    tls_composer = owner;
    active_scope = &scope;
    try {
      scope.component->build(*owner);
    } catch (const std::exception& error) {
      log_sub_scope_error(scope, error.what());
    } catch (...) {
      log_sub_scope_error(scope, "未知异常");
    }
    active_scope = nullptr;
    tls_composer = nullptr;
    parent_stack.clear();
    child_cursor[scope.host_parent] = saved_cursor;
    scope.dirty = false;
  }

  /// 重置某个父元素下第 `slot` 个子元素及其全部后代的游标（子作用域的清理）。
  void reset_subtree_cursor(Element* parent, std::size_t slot) {
    if (parent == nullptr || slot >= parent->child_count()) return;
    const auto walk = [&](auto&& self, Element& element) -> void {
      child_cursor[&element] = 0;
      for (std::size_t index = 0; index < element.children().size(); ++index) {
        if (Element* child = element.child_at(index); child != nullptr) self(self, *child);
      }
    };
    if (Element* root_element = parent->child_at(slot); root_element != nullptr) {
      walk(walk, *root_element);
    }
  }

  Composer* owner{nullptr};
  /// 当前构建的 scope（状态读登记到它）。nullptr = 根作用域。
  Scope* active_scope{nullptr};

  void log_sub_scope_error(const Scope& scope, std::string_view message) {
    std::fprintf(stderr, "[dsl] 子作用域 %s 构建失败：%s\n", scope.key.c_str(),
                 std::string(message).c_str());
  }

  // —─ 异步（`resource`）—─
  /// 槽位 → （输入指纹, 当前 token）。输入变化才开新代。
  struct AsyncSlot {
    std::string fingerprint{};
    std::uint64_t token{0};
  };
  std::vector<AsyncSlot> async_slots{};
  std::uint64_t next_token{1};
  /// 当前有效 token（代次校验：旧 token 的结果丢弃）。
  std::unordered_set<std::uint64_t> live_tokens{};
  /// 主线程任务队列（工作线程投递；`pump_async` 在主循环取走执行）。
  std::mutex inbox_mutex{};
  std::vector<std::function<void()>> inbox{};
  /// 在飞工作线程；析构时 join（保证线程不再访问本结构）。
  std::vector<std::thread> workers{};

  /// 析构：join 全部在飞线程。线程可能仍在 `post_to_main`（投到 inbox）——inbox 与
  /// workers 同属 Impl，join 保证线程结束后才释放本结构。
  ~Impl() {
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }
  }

  std::unordered_map<const Element*, std::size_t> child_cursor{};  // 每父元素的子游标

  /// 根槽位哨兵：parent_stack 的栈底。current_parent() 返回它时，create_element
  /// 落在 UiRoot::content() 槽位（首层元素直接成为根，不再多套一层容器）。
  ///lint-allow: L6 哨兵指针：非解引用值（值 1），单点封装在本结构内
  static auto root_slot_marker() -> Element* {
    return reinterpret_cast<Element*>(static_cast<std::uintptr_t>(1));  // lint-allow: L6 哨兵指针（值 1 非解引用，单点封装）
  }
  static auto is_root_slot(const Element* element) -> bool {
    return element == root_slot_marker();
  }
};

Composer::Composer(UiRoot& root, Guardrails guardrails)
    : impl_(std::make_unique<Impl>(root, guardrails)) {
  impl_->owner = this;
  active_composers.insert(this);
}

Composer::~Composer() { active_composers.erase(this); }

void Composer::register_sub_scope(std::shared_ptr<Component> component, const std::string& key) {
  const std::string name = key.empty() ? std::to_string(impl_->sub_scopes.size()) : key;
  // 首次：登记（记录宿主位置——当前父元素 + 当前游标）
  if (Impl::Scope* existing = impl_->find_sub_scope(name); existing != nullptr) {
    existing->component = std::move(component);   // 组件实例可被替换（重建后仍同槽）
    return;
  }
  auto scope = std::make_unique<Impl::Scope>();
  scope->component = std::move(component);
  scope->key = name;
  scope->dirty = true;   // 首次要跑一次
  Element* host = current_parent();
  scope->host_parent = host;
  // 宿主槽位：子作用域在自己宿主下占一个槽（根重跑时子组件声明的**那个位置**）。
  // 记录当前游标（本子组件是该父元素下的第几个子），重跑时从它开始建。
  scope->slot = host != nullptr ? impl_->child_cursor[host] : impl_->root_replaced ? 1 : 0;
  impl_->sub_scopes.push_back(std::move(scope));
}

void sub_component(Composer& c, std::shared_ptr<Component> component, std::string_view key) {
  if (component == nullptr) return;
  // 声明一个子组件的位置（本函数在**根重跑时**被调用）。
  // 子作用域的重跑由 `Composer::reconcile` 统一驱动（它有自己的宿主位置记录，
  // 所以父不重跑时子作用域也能独立重跑）。
  c.register_sub_scope(std::move(component), std::string(key));
}

auto Composer::guardrails() const noexcept -> const Guardrails& { return impl_->guardrails; }

auto Composer::mount(std::shared_ptr<Component> root_component) -> bool {
  if (root_component == nullptr) return false;
  impl_->component = std::move(root_component);
  impl_->mounted = true;
  impl_->scope_dirty = true;
  (void)reconcile();
  return true;
}

auto Composer::dirty() const noexcept -> bool {
  if (impl_->scope_dirty) return true;
  for (const auto& scope : impl_->sub_scopes) {
    if (scope->dirty) return true;
  }
  return false;
}

void Composer::rebuild_all() {
  impl_->subscribed_states.clear();
  impl_->scope_dirty = true;
  (void)reconcile();
}

auto Composer::reconcile() -> ReconcileStats {
  impl_->last_stats = ReconcileStats{};
  if (!impl_->mounted) return impl_->last_stats;
  // 脏判定：根脏 _或_ 任一子作用域脏（后者只重跑子作用域——细粒度）
  bool any_dirty = impl_->scope_dirty;
  for (const auto& scope : impl_->sub_scopes) {
    if (scope->dirty) any_dirty = true;
  }
  if (!any_dirty) return impl_->last_stats;
  const auto start = std::chrono::steady_clock::now();

  // —— ① 根作用域（仅在根脏时跑）——
  if (impl_->scope_dirty) {
    tls_composer = this;
    impl_->active_scope = nullptr;   // 根作用域的读归 subscribed_states
    // 重跑前清空订阅（build 期间重新收集）；overlay 认领集也重置
    impl_->subscribed_states.clear();
    impl_->claimed_overlays.clear();
    impl_->hook_cursor = 0;   // hooks 游标：每次重组从 0 起
    impl_->parent_stack.clear();
    impl_->child_cursor.clear();
    impl_->root_replaced = false;
    impl_->parent_stack.push_back(Impl::root_slot_marker());
    try {
      impl_->component->build(*this);
      impl_->last_stats.scopes_rerun = 1;
    } catch (const std::exception& error) {
      impl_->last_stats.error = error.what();
      if (impl_->guardrails.freeze_on_error) impl_->scope_dirty = false;
    } catch (...) {
      impl_->last_stats.error = "未知异常";
      if (impl_->guardrails.freeze_on_error) impl_->scope_dirty = false;
    }
    tls_composer = nullptr;
    impl_->active_scope = nullptr;
    impl_->parent_stack.clear();
    // 清理未认领的 overlay：本次 build 没声明它 = 它应该消失
    sweep_overlays();
    impl_->scope_dirty = false;
  }

  // —— ② 子作用域（只跑脏的；父不重跑）——
  for (auto& scope : impl_->sub_scopes) {
    if (!scope->dirty) continue;
    impl_->run_sub_scope(*scope);
    ++impl_->last_stats.scopes_rerun;
  }

  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (static_cast<double>(
          std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) / 1000.0 >
      impl_->guardrails.frame_budget_ms) {
    impl_->last_stats.budget_exceeded = true;
  }
  impl_->scope_dirty = false;
  return impl_->last_stats;
}

void Composer::add_dependency(StateBase* state) {
  if (state == nullptr) return;
  // 依赖登记到**当前 scope**（子作用域内的读不脏父）——这是细粒度重组的关键。
  if (impl_->active_scope != nullptr) {
    impl_->active_scope->deps.insert(state);
    return;
  }
  impl_->subscribed_states.insert(state);
}

// —─ hooks（`resource` 等）—─

auto Composer::hook_index() -> std::size_t { return impl_->hook_cursor++; }

auto Composer::hook_state_count() const -> std::size_t { return impl_->hook_states.size(); }

auto Composer::hook_state_ptr(std::size_t index) -> StateBase* {
  return index < impl_->hook_states.size() ? impl_->hook_states[index].get() : nullptr;
}

void Composer::hook_state_replace(std::size_t index, std::unique_ptr<StateBase> state) {
  if (index >= impl_->hook_states.size()) {
    impl_->hook_states.push_back(std::move(state));
    return;
  }
  impl_->hook_states[index] = std::move(state);
}

void Composer::hook_state_push(std::unique_ptr<StateBase> state) {
  impl_->hook_states.push_back(std::move(state));
}

// —─ 异步—─

auto Composer::async_begin(std::size_t slot, std::string fingerprint) -> std::uint64_t {
  if (slot >= impl_->async_slots.size()) impl_->async_slots.resize(slot + 1);
  auto& entry = impl_->async_slots[slot];
  if (entry.fingerprint == fingerprint && entry.token != 0) {
    return 0;   // 输入未变：不重发（已发出的继续在飞）
  }
  entry.fingerprint = std::move(fingerprint);
  entry.token = impl_->next_token++;
  impl_->live_tokens.insert(entry.token);
  return entry.token;
}

auto Composer::async_token_current(std::uint64_t token) -> bool {
  return impl_->live_tokens.count(token) > 0;
}

void Composer::post_to_main(std::function<void()> task) {
  std::lock_guard<std::mutex> guard(impl_->inbox_mutex);
  impl_->inbox.push_back(std::move(task));
}

void Composer::run_on_worker(std::function<void()> task) {
  // 起短命线程（示例规模：任务数少、一趟往返）。线程入表，析构时 join。
  std::thread worker([task = std::move(task)]() {
    if (task) task();
  });
  std::lock_guard<std::mutex> guard(impl_->inbox_mutex);
  // 回收已结束的（joinable 且已 return 的无法直接探测；这里只控制总数量上限）
  if (impl_->workers.size() > 64) {
    for (auto& existing : impl_->workers) {
      if (existing.joinable()) existing.join();
    }
    impl_->workers.clear();
  }
  impl_->workers.push_back(std::move(worker));
}

auto Composer::pump_async() -> std::size_t {
  std::vector<std::function<void()>> ready;
  {
    std::lock_guard<std::mutex> guard(impl_->inbox_mutex);
    ready.swap(impl_->inbox);
  }
  for (auto& task : ready) {
    if (task) task();
  }
  return ready.size();
}

void Composer::notify_state_written(StateBase* state) {
  if (!impl_->mounted) return;
  // ① 根作用域订阅了它：整根重跑
  if (impl_->subscribed_states.count(state) > 0) {
    impl_->scope_dirty = true;
    return;
  }
  // ② 子作用域订阅了它：只脏那个子作用域（**细粒度**：父不重跑）
  for (auto& scope : impl_->sub_scopes) {
    if (scope->deps.count(state) > 0) scope->dirty = true;
  }
}

Element* Composer::current_parent() const noexcept {
  return impl_->parent_stack.empty() ? nullptr : impl_->parent_stack.back();
}

void Composer::push_parent(Element* parent) {
  if (parent != nullptr) impl_->parent_stack.push_back(parent);
}

void Composer::pop_parent() {
  if (impl_->parent_stack.size() > 1) {
    // 裁剪：本次 build 声明的子元素到此为止——游标之后的残留是上一帧多声明的，移除。
    // （不裁剪的症状：条件分支少了一个元素后，它还留在树上——「界面删不掉」）
    if (Element* parent = impl_->parent_stack.back(); parent != nullptr &&
        !Impl::is_root_slot(parent)) {
      const std::size_t declared = impl_->child_cursor[parent];
      while (parent->child_count() > declared) {
        auto removed = parent->remove_child(parent->child_at(parent->child_count() - 1));
        (void)removed;
        ++impl_->last_stats.elements_removed;
      }
    }
    impl_->parent_stack.pop_back();
  }
}

auto Composer::create_element(std::string_view type, const st::Json& props,
                              std::string_view key) -> Element* {
  Element* parent = current_parent();
  if (parent == nullptr) return nullptr;

  // 根槽位：首个顶层声明直接落在 UiRoot::content()（单根语义，与 Compose/ArkTS 一致）。
  if (Impl::is_root_slot(parent)) {
    Element* existing = impl_->root.content();
    const bool type_matches = existing != nullptr && existing->type() == type;
    Element* element = nullptr;
    if (type_matches && !impl_->root_replaced) {
      element = existing;  // 复用根元素：只更新
      ++impl_->child_cursor[parent];
      if (!key.empty()) element->set_key(std::string(key));
      if (props.is_object() && !props.empty()) {
        auto applied = ui::apply_properties(impl_->root, *element, props);
        if (applied.is_array()) {
          impl_->last_stats.properties_applied += static_cast<int>(applied.size());
        }
      }
      return element;
    }
    auto created = make_element(std::string(type));
    if (created == nullptr) return nullptr;
    element = created.get();
    impl_->root.set_content(std::move(created));
    impl_->root_replaced = true;
    ++impl_->last_stats.elements_created;
    ++impl_->child_cursor[parent];
    if (!key.empty()) element->set_key(std::string(key));
    if (props.is_object() && !props.empty()) {
      auto applied = ui::apply_properties(impl_->root, *element, props);
      if (applied.is_array()) {
        impl_->last_stats.properties_applied += static_cast<int>(applied.size());
      }
    }
    return element;
  }

  // 位置对齐：按父元素内游标取「该位置既有子元素」
  auto& cursor = impl_->child_cursor[parent];
  Element* existing = cursor < parent->child_count() ? parent->child_at(cursor) : nullptr;
  const bool type_matches = existing != nullptr && existing->type() == type;
  Element* element = nullptr;
  if (type_matches) {
    element = existing;  // 复用：只更新 props
  } else {
    // 新建（或类型变了）：先移除旧位置元素再插入新元素
    auto created = make_element(std::string(type));
    if (created == nullptr) return nullptr;
    element = created.get();
    if (cursor < parent->child_count()) {
      auto old = parent->remove_child(parent->child_at(cursor));
      (void)old;  // 释放旧元素
      ++impl_->last_stats.elements_removed;
    }
    parent->insert_child(cursor, std::move(created));
    ++impl_->last_stats.elements_created;
  }
  ++cursor;
  // key/id 先行截获（id 的本职是被外部引用——保持选择器安全）
  if (!key.empty()) element->set_key(std::string(key));
  if (props.is_object() && !props.empty()) {
    auto applied = ui::apply_properties(impl_->root, *element, props);
    if (applied.is_array()) {
      impl_->last_stats.properties_applied += static_cast<int>(applied.size());
    }
  }
  return element;
}

void Composer::connect(Element& element, std::string_view event,
                       std::function<void()> handler) {
  // 事件直连：一等接口优先（Button::on_click 等）；属性面兜底（on_click 属性）。
  if (event == "click") {
    if (auto* btn = dynamic_cast<Button*>(&element); btn != nullptr) {
      btn->on_click = std::move(handler);
      return;
    }
  }
  // 其余事件：经属性面动作（v1 只接线 click；更多事件类型按示例需求补）
  (void)handler;
  (void)event;
}

void Composer::attach_overlay(Element& host, std::unique_ptr<Element> content) {
  host.add_child(std::move(content));
}

auto Composer::overlay_slot(std::string_view key) -> Element* {
  const std::string name(key);
  auto found = impl_->overlays.find(name);
  if (found != impl_->overlays.end()) {
    // 复用：标为“已认领”（本帧被重新声明，不清理）
    impl_->claimed_overlays.insert(name);
    return found->second;
  }
  // 新建：FillViewport 形态（浮层自行定位卡片——模态/命令面板标准形态）
  auto host = std::make_unique<Panel>(FlexDirection::Column);
  host->set_id("overlay-" + name);
  Element* raw = host.get();
  impl_->root.add_overlay(std::move(host), UiRoot::OverlayLayout::FillViewport);
  impl_->overlays.emplace(name, raw);
  impl_->claimed_overlays.insert(name);
  return raw;
}

void Composer::register_shortcut(std::string key, UiRoot::Shortcut mods,
                                 std::function<bool()> handler) {
  // 转调 UiRoot：快捷键表是 root 级资源（先于焦点链派发，文本组件吞键也拦得住）。
  (void)impl_->root.register_shortcut(std::move(key), std::move(mods), std::move(handler));
}

void Composer::sweep_overlays() {
  // 重组末尾：本次 build 未认领的 overlay → 移除（“关掉”的表达就是“不声明”）。
  for (auto it = impl_->overlays.begin(); it != impl_->overlays.end();) {
    if (impl_->claimed_overlays.count(it->first) > 0) {
      ++it;
      continue;
    }
    impl_->root.remove_overlay(it->second);
    it = impl_->overlays.erase(it);
  }
}

auto Composer::root() noexcept -> UiRoot& { return impl_->root; }
// ── 元素工厂（type → 构造；dsl 内部 + 后续协议/脚本共用）─────────────────

// —— `custom<T>` 的类型名特化（组件定义在本文件可见处）——
// 新增可声明组件时在这里补一行（比写 32 个包装函数便宜得多）。
//lint-allow: L3 类型名特化表（36 行机械重复，一个宏压缩；无模板替代形式）
#define ST_DSL_TYPE(TYPE, NAME) /* lint-allow: L3 同上 */      \
  template <>                                                   \
  [[nodiscard]] auto type_name<TYPE>() -> std::string { return NAME; }

ST_DSL_TYPE(Panel, "Panel")
ST_DSL_TYPE(Text, "Text")
ST_DSL_TYPE(Heading, "Heading")
ST_DSL_TYPE(IconView, "Icon")
ST_DSL_TYPE(Button, "Button")
ST_DSL_TYPE(Card, "Card")
ST_DSL_TYPE(Divider, "Divider")
ST_DSL_TYPE(KeyValueRow, "KeyValueRow")
ST_DSL_TYPE(Spacer, "Spacer")
ST_DSL_TYPE(Input, "Input")
ST_DSL_TYPE(TextArea, "TextArea")
ST_DSL_TYPE(Checkbox, "Checkbox")
ST_DSL_TYPE(Radio, "Radio")
ST_DSL_TYPE(Switch, "Switch")
ST_DSL_TYPE(Slider, "Slider")
ST_DSL_TYPE(Select, "Select")
ST_DSL_TYPE(List, "List")
ST_DSL_TYPE(ListItem, "ListItem")
ST_DSL_TYPE(Table, "Table")
ST_DSL_TYPE(Tree, "Tree")
ST_DSL_TYPE(ScrollView, "ScrollView")
ST_DSL_TYPE(ScrollBar, "ScrollBar")
ST_DSL_TYPE(Tabs, "Tabs")
ST_DSL_TYPE(ProgressBar, "ProgressBar")
ST_DSL_TYPE(Spinner, "Spinner")
ST_DSL_TYPE(Badge, "Badge")
ST_DSL_TYPE(Chip, "Chip")
ST_DSL_TYPE(Avatar, "Avatar")
ST_DSL_TYPE(Tooltip, "Tooltip")
ST_DSL_TYPE(Dialog, "Dialog")
ST_DSL_TYPE(Toast, "Toast")
ST_DSL_TYPE(MenuBar, "MenuBar")
ST_DSL_TYPE(FileDialog, "FileDialog")
ST_DSL_TYPE(CodeEditor, "CodeEditor")
ST_DSL_TYPE(MarkdownView, "MarkdownView")

auto make_element(std::string type) -> std::unique_ptr<Element> {
  // 基础
  if (type == "Text") return std::make_unique<Text>();
  if (type == "Heading") return std::make_unique<Heading>();
  if (type == "Icon") return std::make_unique<IconView>();
  if (type == "Button") return std::make_unique<Button>("");
  if (type == "Card") return std::make_unique<Card>();
  if (type == "Divider") return std::make_unique<Divider>();
  if (type == "KeyValueRow") return std::make_unique<KeyValueRow>("", "");
  if (type == "Panel") return std::make_unique<Panel>();
  if (type == "Spacer") return std::make_unique<Spacer>();
  // 输入类
  if (type == "Input") return std::make_unique<Input>();
  if (type == "TextArea") return std::make_unique<TextArea>();
  if (type == "Checkbox") return std::make_unique<Checkbox>();
  if (type == "Radio") return std::make_unique<Radio>();
  if (type == "Switch") return std::make_unique<Switch>();
  if (type == "Slider") return std::make_unique<Slider>();
  if (type == "Select") return std::make_unique<Select>();
  // 列表类
  if (type == "ListItem") return std::make_unique<ListItem>("", "");
  if (type == "List") return std::make_unique<List>();
  if (type == "Table") return std::make_unique<Table>();
  if (type == "Tree") return std::make_unique<Tree>();
  // 容器/滚动/标签
  if (type == "ScrollView") return std::make_unique<ScrollView>();
  if (type == "ScrollBar") return std::make_unique<ScrollBar>();
  if (type == "Tabs") return std::make_unique<Tabs>();
  // 反馈类
  if (type == "ProgressBar") return std::make_unique<ProgressBar>();
  if (type == "Spinner") return std::make_unique<Spinner>();
  if (type == "Badge") return std::make_unique<Badge>();
  if (type == "Chip") return std::make_unique<Chip>();
  if (type == "Avatar") return std::make_unique<Avatar>();
  if (type == "Tooltip") return std::make_unique<Tooltip>();
  // 浮层/菜单/对话框
  if (type == "Dialog") return std::make_unique<Dialog>();
  if (type == "Toast") return std::make_unique<Toast>();
  if (type == "MenuBar") return std::make_unique<MenuBar>();
  if (type == "MenuPanel") return std::make_unique<MenuPanel>(std::vector<MenuItem>{});
  if (type == "FileDialog") return std::make_unique<FileDialog>();
  // 文本/多媒体
  if (type == "CodeEditor") return std::make_unique<CodeEditor>();
  if (type == "MarkdownView") return std::make_unique<MarkdownView>();
  return nullptr;
}

// ── BoxProps 应用 ─────────────────────────────────────────────────────────

void apply_box(Element& element, const BoxProps& props) {
  Style& style = element.style();
  if (props.gap >= 0.0f) style.gap = props.gap;
  if (props.padding >= 0.0f) style.padding = math::Insets::all(props.padding);
  if (props.margin >= 0.0f) style.margin = math::Insets::all(props.margin);
  if (props.width != kAuto) style.width = props.width;
  if (props.height != kAuto) style.height = props.height;
  style.grow = props.grow;
  if (props.radius >= 0.0f) style.radius = props.radius;
  if (!props.key.empty()) element.set_key(props.key);
  if (!props.id.empty()) element.set_id(props.id);
}

// ── 组件包装 ──────────────────────────────────────────────────────────────

auto row(Composer& c, const BoxProps& props, std::function<void()> children) -> Element& {
  st::Json props_json = st::Json::object();
  Element* element = c.create_element("Panel", props_json, props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;  // 不应发生（make_element 覆盖 Panel）
  }
  apply_box(*element, props);
  element->style().direction = FlexDirection::Row;
  BuildScope scope(c, element);
  if (children) children();
  return *element;
}

auto column(Composer& c, const BoxProps& props, std::function<void()> children) -> Element& {
  st::Json props_json = st::Json::object();
  Element* element = c.create_element("Panel", props_json, props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  element->style().direction = FlexDirection::Column;
  BuildScope scope(c, element);
  if (children) children();
  return *element;
}

auto text(Composer& c, std::function<std::string()> content, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Text", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* text_element = dynamic_cast<Text*>(element); text_element != nullptr) {
    text_element->set_content(content ? content() : std::string());
  }
  // 注：闭包被丢弃——v1 每次重组整个 build 重跑，content 在 build 期间求值即可。
  // （惰性求值 + 依赖收集的细粒度路径在 M2 后续接入；语义见 docs/declarative.md §4.1）
  return *element;
}

auto button(Composer& c, std::string label, std::function<void()> on_click,
            const BoxProps& props) -> Element& {
  Element* element = c.create_element("Button", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* button_element = dynamic_cast<Button*>(element); button_element != nullptr) {
    button_element->set_label(std::move(label));
  }
  if (on_click) c.connect(*element, "click", std::move(on_click));
  return *element;
}

auto checkbox(Composer& c, std::string label, bool checked,
              std::function<void(bool)> on_change, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Checkbox", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* box = dynamic_cast<Checkbox*>(element); box != nullptr) {
    box->set_label(std::move(label));
    box->set_checked(checked);
    if (on_change) box->on_change = std::move(on_change);
  }
  return *element;
}

auto switch_(Composer& c, bool checked, std::function<void(bool)> on_change,
             const BoxProps& props) -> Element& {
  Element* element = c.create_element("Switch", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* sw = dynamic_cast<Switch*>(element); sw != nullptr) {
    sw->set_checked(checked);
    if (on_change) sw->on_change = std::move(on_change);
  }
  return *element;
}

auto slider(Composer& c, float value, std::function<void(float)> on_change,
            const BoxProps& props) -> Element& {
  Element* element = c.create_element("Slider", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* slider_element = dynamic_cast<Slider*>(element); slider_element != nullptr) {
    slider_element->set_value(value);
    if (on_change) slider_element->on_change = std::move(on_change);
  }
  return *element;
}

auto input(Composer& c, std::string value, std::function<void(std::string)> on_input,
           const BoxProps& props) -> Element& {
  Element* element = c.create_element("Input", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* input_element = dynamic_cast<Input*>(element); input_element != nullptr) {
    input_element->set_text(std::move(value));
    if (on_input) {
      input_element->on_change = [on_input](std::string_view v) { on_input(std::string(v)); };
    }
  }
  return *element;
}

auto progress(Composer& c, float value, const BoxProps& props) -> Element& {
  Element* element = c.create_element("ProgressBar", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* bar = dynamic_cast<ProgressBar*>(element); bar != nullptr) {
    bar->set_value(value);
  }
  return *element;
}

auto badge(Composer& c, std::string text_value, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Badge", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* badge_element = dynamic_cast<Badge*>(element); badge_element != nullptr) {
    badge_element->set_text(std::move(text_value));
  }
  return *element;
}

auto heading(Composer& c, std::string content, std::uint32_t level, const BoxProps& props)
    -> Element& {
  Element* element = c.create_element("Heading", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* head = dynamic_cast<Heading*>(element); head != nullptr) {
    head->set_level(level);
    head->set_content(std::move(content));
  }
  return *element;
}

auto divider(Composer& c, bool vertical) -> Element& {
  Element* element = c.create_element("Divider", st::Json::object());
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  if (vertical) element->style().width = 1.0f;  // 竖线近似：v1 水平线为主
  return *element;
}

auto card(Composer& c, const BoxProps& props, std::function<void()> children) -> Element& {
  Element* element = c.create_element("Card", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  BuildScope scope(c, element);
  if (children) children();
  return *element;
}

auto spacer(Composer& c, float size) -> Element& {
  Element* element = c.create_element("Spacer", st::Json::object());
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  element->style().width = size;
  element->style().height = size;
  return *element;
}

auto icon(Composer& c, std::string name, float size, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Icon", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* view = dynamic_cast<IconView*>(element); view != nullptr) {
    view->set_icon(std::move(name));
    view->set_size(size);
  }
  return *element;
}

auto overlay(Composer& c, std::string_view key, const BoxProps& props,
             std::function<void()> children) -> Element& {
  // 槽位（同 key 复用宿主）；`key` 即身份，本次不声明 → sweep 移除
  Element* host = c.overlay_slot(key);
  apply_box(*host, props);
  BuildScope scope(c, host);
  if (children) children();
  return *host;
}

auto menu_bar(Composer& c, const std::vector<MenuData>& menus,
              std::function<void(const std::string&, const std::string&)> on_action,
              std::function<void(std::size_t)> on_open_menu, const BoxProps& props) -> MenuBar* {
  Element* element = c.create_element("MenuBar", st::Json::object(), props.key);
  if (element == nullptr) return nullptr;
  apply_box(*element, props);
  auto* bar = dynamic_cast<MenuBar*>(element);
  if (bar == nullptr) return nullptr;
  std::vector<Menu> list;
  list.reserve(menus.size());
  for (const auto& menu : menus) {
    Menu entry{};
    entry.id = menu.id;
    entry.label = menu.label;
    for (const auto& item : menu.items) {
      MenuItem item_entry{};
      item_entry.id = item.id;
      item_entry.label = item.label;
      item_entry.separator = item.separator;
      entry.items.push_back(std::move(item_entry));
    }
    list.push_back(std::move(entry));
  }
  bar->set_menus(list);
  if (on_action) {
    bar->on_action = [on_action](const std::string& menu, const std::string& item) {
      on_action(menu, item);
    };
  }
  if (on_open_menu) bar->on_open_menu = std::move(on_open_menu);
  return bar;
}

void menu_panel_overlay(Composer& c, MenuBar& bar, std::size_t index) {
  if (index >= bar.menu_count()) return;
  const std::string key = "menu-panel-" + std::string(bar.menu_id(index));
  Element* host = c.overlay_slot(key);
  // 只在首次认领时构造面板（同 key 复用——面板内状态保持）
  if (host->child_count() == 0) {
    host->add_child(bar.make_panel(index));
  }
  bar.set_open_index(index);
}

auto list(Composer& c, const std::vector<ListItemData>& items,
          std::function<void(std::size_t)> on_click, const BoxProps& props) -> Element& {
  Element* element = c.create_element("List", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* list_element = dynamic_cast<List*>(element); list_element != nullptr) {
    std::vector<List::Entry> entries;
    entries.reserve(items.size());
    for (const auto& item : items) {
      List::Entry entry{};
      entry.key = item.key;
      entry.label = item.label;
      entries.push_back(std::move(entry));
    }
    list_element->sync_items(entries);
    if (on_click) {
      // List 的统一回调口是 on_select（点击/键盘/协议 invoke 都汇聚到 select→on_select；
      // Entry::on_activate 是「项级回调」，会覆盖 bind_item 的选中链路，不用它）
      list_element->set_on_select([on_click](std::size_t index) { on_click(index); });
    }
  }
  return *element;
}

auto select(Composer& c, const std::vector<SelectOptionData>& options,
            std::optional<std::size_t> selected,
            std::function<void(std::size_t)> on_change, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Select", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* widget = dynamic_cast<Select*>(element); widget != nullptr) {
    std::vector<SelectOption> list;
    list.reserve(options.size());
    for (const auto& option : options) {
      list.push_back(SelectOption{option.value, option.label});
    }
    widget->set_options(std::move(list));
    widget->set_selected_index(selected);
    if (on_change) {
      // `Select::on_change` 传的是**选中项的 value 文本**（组件不知道调用方的索引语义）。
      // 声明式入口要的是「第几项」——按 value 反查（options 是本次声明的快照）。
      widget->on_change = [on_change, options](std::string_view value) {
        for (std::size_t index = 0; index < options.size(); ++index) {
          if (options[index].value == value) {
            on_change(index);
            return;
          }
        }
      };
    }
  }
  return *element;
}

auto table(Composer& c, const std::vector<TableColumnData>& columns,
           const std::vector<std::vector<std::string>>& rows,
           std::function<void(std::size_t)> on_row_click, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Table", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* widget = dynamic_cast<Table*>(element); widget != nullptr) {
    std::vector<TableColumn> list;
    list.reserve(columns.size());
    for (const auto& column : columns) {
      TableColumn entry{};
      entry.name = column.label;   // 组件字段叫 `name`（表头文本），声明式条目叫 `label`
      entry.width = column.width;
      list.push_back(std::move(entry));
    }
    widget->set_columns(std::move(list));
    widget->clear_rows();
    for (const auto& row : rows) widget->add_row(row);
    if (on_row_click) widget->set_on_row_click(std::move(on_row_click));
  }
  return *element;
}

auto tree(Composer& c, const std::vector<TreeNodeData>& nodes,
          std::function<void(const std::string&, bool)> on_toggle,
          std::function<void(const std::string&)> on_select, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Tree", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* widget = dynamic_cast<Tree*>(element); widget != nullptr) {
    std::vector<TreeNode> list;
    list.reserve(nodes.size());
    for (const auto& node : nodes) {
      TreeNode entry{};
      entry.key = node.key;
      entry.label = node.label;
      entry.expanded = node.expanded;
      entry.is_dir = node.is_dir;
      entry.depth = node.depth;
      list.push_back(std::move(entry));
    }
    widget->sync_nodes(list);   // 按 key 对齐（同 `Tree::sync_nodes` 语义）
    if (on_toggle) {
      widget->on_toggle = [on_toggle](std::string_view key, bool expanded) {
        on_toggle(std::string(key), expanded);
      };
    }
    if (on_select) {
      widget->on_select = [on_select](std::string_view key) { on_select(std::string(key)); };
    }
  }
  return *element;
}

auto tabs(Composer& c, const std::vector<TabData>& items, std::size_t active,
         std::function<void(std::size_t)> on_change,
         std::function<void(const std::string&)> on_close, const BoxProps& props) -> Element& {
  Element* element = c.create_element("Tabs", st::Json::object(), props.key);
  if (element == nullptr) {
    static Element* none = nullptr;
    return *none;
  }
  apply_box(*element, props);
  if (auto* tabs_element = dynamic_cast<Tabs*>(element); tabs_element != nullptr) {
    std::vector<Tabs::Tab> list;
    list.reserve(items.size());
    for (const auto& item : items) {
      list.push_back(Tabs::Tab{item.key, item.label, item.modified, item.closable});
    }
    tabs_element->sync_tabs(list);   // 按 key 复用（id/活动态保持）
    tabs_element->set_active(active);
    if (on_change) {
      tabs_element->on_change = [on_change](std::size_t index) { on_change(index); };
    }
    if (on_close) {
      tabs_element->on_close = [tabs_element, on_close](std::size_t index) {
        // 回调给「key」而不是索引：索引会随标签增删漂移，key 才是业务身份
        if (index < tabs_element->tab_count()) {
          on_close(std::string(tabs_element->tab_label(index)));
        }
      };
    }
  }
  return *element;
}

// ── DeclarativeHost ───────────────────────────────────────────────────────

DeclarativeHost::DeclarativeHost(UiRoot& root, Guardrails guardrails)
    : composer_(std::make_unique<Composer>(root, guardrails)) {}

auto DeclarativeHost::mount(std::shared_ptr<Component> root_component) -> bool {
  return composer_->mount(std::move(root_component));
}

auto DeclarativeHost::tick() -> ReconcileStats {
  // 先执行已投递的异步结果（它们写 State 会标脏）——同帧可见
  (void)composer_->pump_async();
  last_ = composer_->reconcile();
  return last_;
}

auto DeclarativeHost::pump_async() -> std::size_t { return composer_->pump_async(); }

auto DeclarativeHost::dirty() const noexcept -> bool { return composer_->dirty(); }

auto DeclarativeHost::stats() const noexcept -> const ReconcileStats& { return last_; }

auto mount(UiRoot& root, std::shared_ptr<Component> root_component, Guardrails guardrails)
    -> std::unique_ptr<DeclarativeHost> {
  auto host = std::make_unique<DeclarativeHost>(root, guardrails);
  if (!host->mount(std::move(root_component))) return nullptr;
  return host;
}

}  // namespace st::ui::dsl
