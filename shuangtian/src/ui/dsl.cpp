#include "st/ui/dsl.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/command_palette.hpp"
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
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/table.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/toggle.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/components/window_frame.hpp"

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

auto deps_signature(const Deps& deps) -> std::string {
  // 指纹 = 逐项 `指针:写版本`。为何要版本而不只看指针：依赖「变了没有」才是
  // 重算/重跑的判据，而同一个 State 对象可以反复写（指针不变）。
  //
  // **顺带订阅**（关键）：把依赖列入 `Deps` 就是声明「本作用域依赖它」——
  // 若不在这里登记，`memo` 命中缓存那帧就不会读依赖，作用域于是「忘了」它，
  // 依赖下一次变化时无人订阅 → 界面再也不更新（静默停在旧值，实测踩到）。
  std::string out;
  for (StateBase* item : deps.items) {
    if (item != nullptr) item->subscribe();
    out += std::to_string(reinterpret_cast<std::uintptr_t>(item));  // lint-allow: L6 作指纹，不解引用
    out += ':';
    out += item != nullptr ? std::to_string(item->version()) : std::string("-");
    out += ';';
  }
  return out;
}

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
  /// 子树挂载锚点（`mount_into`）：非空时根层声明落在它的子位（不碰 `UiRoot::content()`）。
  /// 用途：宿主界面（gallery）里的一页用声明式描述，其余部分仍手搭。
  Element* anchor{nullptr};
  /// 声明式叠加层：key → 宿主元素（挂在 root overlay 通道上）。
  /// 生命周期 = 「本次 build 有没有认领它」（见 Composer::overlay_slot/sweep_overlays）。
  std::unordered_map<std::string, Element*> overlays{};
  std::unordered_set<std::string> claimed_overlays{};

  // —─ hooks：跨重组保持的强类型状态（`resource` 等）—─
  std::vector<std::unique_ptr<StateBase>> hook_states{};
  std::size_t hook_cursor{0};

  // —─ 作用域树（多作用域细粒度重组）—─
  //
  // 全局 `scope_dirty` 只能表达“整根重跑”。子作用域（`sub_component`）有自己的依赖集
  // 与脏标记：只订阅了自己的状态变时，**只重跑子作用域**，父不重跑。
  //
  // 嵌套：子作用域的 build 里可以再声明子作用域（孙），形成作用域树——
  // 每层各自独立失效，粒度细到“改哪块只重跑哪块”。
  //
  // 位置与失效的关系（关键设计）：
  // - scope 记「宿主父元素 + 该父元素下的第几个子位」，重跑时先把游标回到那个位置；
  // - **父重跑时刷新子 scope 的位置**（条件分支可能增删前面的兄弟，位置会漂）；
  // - 父重跑会连带把子树 scope 标脏（父的 build 重建了子树，子必须重跑才能对齐）；
  // - `declared` 标记：父重跑后仍未重新声明的子 scope = 被条件分支剪掉 → 递归移除。
  struct Scope {
    std::shared_ptr<Component> component{};
    std::string key{};                              // 兄弟间身份（同 key 复用同一 scope）
    std::unordered_set<StateBase*> deps{};
    bool dirty{true};
    /// 宿主位置：挂到哪个父元素下的第几个子位（宿主为 nullptr = 根层）。
    Element* host_parent{nullptr};
    std::size_t slot{0};
    bool declared{false};                           // 本次父重跑是否又见到它
    Scope* parent{nullptr};
    /// 嵌套深度（根 = 0；子 = 父 + 1）。超 `Guardrails::max_depth` 时拒绝声明——
    /// 递归 build（组件在自己 build 里又声明自己）会让深度无界增长，
    /// 直到爆栈（实测形态：栈溢出 SIGSEGV，现场只剩一大堆 run_scope 帧）。
    int depth{0};
    std::vector<std::unique_ptr<Scope>> children{};

    [[nodiscard]] auto find_child(std::string_view wanted) -> Scope* {
      for (auto& child : children) {
        if (child->key == wanted) return child.get();
      }
      return nullptr;
    }

    /// 整棵子树标脏（父重跑后子必须重跑才能对齐）。
    void mark_subtree_dirty() {
      dirty = true;
      for (auto& child : children) child->mark_subtree_dirty();
    }
  };
  /// 根作用域（持有顶层 Component）。
  std::unique_ptr<Scope> root_scope{};

  /// 当前构建的 scope（状态读登记到它；`sub_component` 据此确定父子关系）。
  Scope* active_scope{nullptr};
  Composer* owner{nullptr};

  /// 声明/复用子作用域，并**刷新其宿主位置**（父重跑时结构可能变了）。
  /// 位置来源：当前构建父元素（`parent_stack.back()`）+ 该父元素当前游标。
  auto declare_child_scope(Scope* parent, std::shared_ptr<Component> child_component,
                           const std::string& key) -> Scope* {
    if (parent == nullptr) parent = root_scope.get();
    if (parent == nullptr) return nullptr;
    // 深度护栏：递归 build 在这里被截住（不是靠栈自己撞上限）。
    // 超限 = 声明被拒（本帧不建这个子作用域），错误写进 stats 供宿主/测试断言。
    if (parent->depth + 1 > guardrails.max_depth) {
      if (last_stats.error.empty()) {
        last_stats.error = std::format("作用域嵌套超过 {} 层（疑似递归 build）",
                                       guardrails.max_depth);
      }
      std::fprintf(stderr, "[dsl] %s\n", last_stats.error.c_str());
      return nullptr;
    }
    Scope* scope = parent->find_child(key);
    if (scope == nullptr) {
      auto fresh = std::make_unique<Scope>();
      fresh->component = std::move(child_component);
      fresh->key = key;
      fresh->dirty = true;   // 首次要跑一次
      fresh->parent = parent;
      fresh->depth = parent->depth + 1;
      scope = fresh.get();
      parent->children.push_back(std::move(fresh));
    } else {
      scope->component = std::move(child_component);   // 组件实例可替换（重建后仍同槽）
    }
    // 宿主位置：栈顶是根槽位哨兵时记录 nullptr（= 根层），否则记录该元素
    Element* top = parent_stack.empty() ? nullptr : parent_stack.back();
    if (top != nullptr && is_root_slot(top)) {
      scope->host_parent = nullptr;
      scope->slot = root_replaced ? 1 : 0;   // 根层：子作用域接在首层元素之后
    } else {
      scope->host_parent = top;
      scope->slot = top != nullptr ? child_cursor[top] : 0;
    }
    scope->declared = true;
    return scope;
  }

  /// 重跑一个作用域（含其内的子作用域声明）：把构建父栈恢复成它的宿主位置，再跑它的 build。
  ///
  /// 关键一：**重置作用域子树内的游标**。父元素的游标记录“下一个子位”，上次跑完停在 N；
  /// 若不重置，本次就会从 N 往后追加（旧元素残留、新元素重复）。根作用域靠
  /// `child_cursor.clear()` 达成同一目的，子作用域只清自己那棵。
  ///
  /// 关键二：**标记本次声明**（`declared=false` 预清，build 中重新声明置 true），
  /// 跑完后未认领的子作用域 = 被条件分支剪掉 → 递归移除（否则残留的 scope 会
  /// 在下次 tick 里对着不存在的宿主位置跑）。
  void run_scope(Scope& scope) {
    scope.deps.clear();
    const Element* host_parent = scope.host_parent;
    const std::size_t saved_cursor = child_cursor[host_parent];
    child_cursor[host_parent] = scope.slot;
    reset_subtree_cursor(scope.host_parent, scope.slot);
    // 清声明标记（build 中重新声明的会被置回 true）
    for (auto& child : scope.children) child->declared = false;

    parent_stack.clear();
    if (scope.host_parent != nullptr) {
      parent_stack.push_back(scope.host_parent);
    } else {
      parent_stack.push_back(root_slot_marker());
    }
    tls_composer = owner;
    Scope* const saved_active = active_scope;
    active_scope = &scope;
    try {
      scope.component->build(*owner);
    } catch (const std::exception& error) {
      log_scope_error(scope, error.what());
    } catch (...) {
      log_scope_error(scope, "未知异常");
    }
    active_scope = saved_active;
    tls_composer = nullptr;
    parent_stack.clear();
    child_cursor[host_parent] = saved_cursor;
    scope.dirty = false;

    // 剪枝：本次未重新声明的子作用域（条件分支去掉了它）
    std::erase_if(scope.children,
                  [](std::unique_ptr<Scope>& child) { return !child->declared; });
  }

  /// 重置某个宿主元素下第 `slot` 个子元素及其全部后代的游标（子作用域的清理）。
  /// `parent == nullptr` 表示根层：扫根内容。
  void reset_subtree_cursor(Element* parent, std::size_t slot) {
    const auto walk = [&](auto&& self, Element& element) -> void {
      child_cursor[&element] = 0;
      for (std::size_t index = 0; index < element.children().size(); ++index) {
        if (Element* child = element.child_at(index); child != nullptr) self(self, *child);
      }
    };
    Element* root_element = parent != nullptr ? parent->child_at(slot) : nullptr;
    if (parent == nullptr) root_element = root_element_of_scope();
    if (root_element != nullptr) walk(walk, *root_element);
  }

  /// 根层子作用域的宿主元素：根内容（子作用域接在它后面）。
  [[nodiscard]] auto root_element_of_scope() const -> Element* {
    return root.content() != nullptr ? root.content()->child_at(0) : nullptr;
  }

  /// 递归跑脏的作用域（自顶向下），带**单帧预算**：
  ///
  /// 超预算时把剩下的脏作用域**留在树上顺延下一帧**（而不是硬跑完）——
  /// 掉帧优于卡死（`docs/declarative.md` §4.2）。判据按作用域计：
  /// 根重跑后剩下的子作用域往往很多，一刀切会退化成“一帧只跑一块”；
  /// 按预算切才真正兼顾「大页面冻住」与「小页面一帧到位」。
  ///
  /// 返回是否因预算而中断（调用方据此把 `budget_exceeded` 置位）。
  auto run_dirty_scopes(Scope& scope, const std::chrono::steady_clock::time_point& start,
                        double budget_ms) -> bool {
    if (scope.dirty) {
      if (static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start).count()) / 1000.0 > budget_ms) {
        return true;   // 本作用域留脏：下一帧再跑（孩子也留着）
      }
      run_scope(scope);
      ++last_stats.scopes_rerun;
    }
    for (auto& child : scope.children) {
      if (run_dirty_scopes(*child, start, budget_ms)) return true;
    }
    return false;
  }

  /// 递归收集作用域内的依赖（`notify_state_written` 用）。
  static auto scope_depends(const Scope& scope, StateBase* state) -> bool {
    if (scope.deps.count(state) > 0) return true;
    for (const auto& child : scope.children) {
      if (scope_depends(*child, state)) return true;
    }
    return false;
  }

  /// 找出直接依赖 `state` 的作用域（递归；返回值可能是任意深度）。
  static auto find_owner_scope(Scope& scope, StateBase* state) -> Scope* {
    if (scope.deps.count(state) > 0) return &scope;
    for (auto& child : scope.children) {
      if (Scope* found = find_owner_scope(*child, state)) return found;
    }
    return nullptr;
  }

  void log_scope_error(const Scope& scope, std::string_view message) {
    std::fprintf(stderr, "[dsl] 作用域 %s 构建失败：%s\n", scope.key.c_str(),
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
  /// 各代次的取消牌（`async_begin` 翻旧代的牌；析构翻全部的牌）。
  std::unordered_map<std::uint64_t, AsyncCancel> async_cancels{};
  /// 主线程任务队列（工作线程投递；`pump_async` 在主循环取走执行）。
  std::mutex inbox_mutex{};
  std::vector<std::function<void()>> inbox{};

  // —─ effect：待执行的副作用（重组末尾跑）—─
  //
  // 为何不在 build 里直接跑：副作用里写状态属于连锁，而**本次重组的脏标记清理**
  // 在 build 之后——内联执行的那次写会被当成"本次已处理"而吞掉（界面停在旧值）。
  // 登记到队列、重组末尾统一跑，写状态自然标脏下一帧（§4.2 连锁写隔帧）。
  struct PendingEffect {
    State<EffectSlot>* slot{nullptr};
    std::function<std::function<void()>()> body{};
  };
  std::vector<PendingEffect> pending_effects{};

  // —─ 工作线程池 —─
  //
  // 为何不用「每任务一线程」：密集场景（列表里几十个 `resource`）会瞬间开几十个线程，
  // 上下文切换与栈开销压倒实际工作（每个任务往往只是一次 IO 或一次计算）。
  // 固定 N 个线程 + 队列：开销与并发度都可控，且**线程数有上限**（不会打满系统）。
  //
  // N 的取值：`std::thread::hardware_concurrency()`（拿不到则 2），下限 1、上限 8——
  // 声明式 UI 的异步任务多数是 IO 等待，超过核数收益递减而调度开销上升。
  std::mutex queue_mutex{};
  std::condition_variable queue_cv{};
  std::deque<std::function<void()>> work_queue{};
  std::vector<std::thread> pool{};
  bool pool_stopping{false};

  /// 启动线程池（首次 `run_on_worker` 时懒建——不跑异步的应用不为它付代价）。
  void ensure_pool() {
    if (!pool.empty()) return;
    unsigned count = std::thread::hardware_concurrency();
    if (count == 0) count = 2;
    count = std::clamp(count, 1U, 8U);
    for (unsigned index = 0; index < count; ++index) {
      pool.emplace_back([this] { pool_loop(); });
    }
  }

  /// 工作线程主循环：取队列 → 执行；队列空则等待（不忙轮询）。
  void pool_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [this] { return pool_stopping || !work_queue.empty(); });
        if (pool_stopping && work_queue.empty()) return;
        job = std::move(work_queue.front());
        work_queue.pop_front();
      }
      if (job) {
        // 任务自身的异常不得杀掉工作线程（否则后续任务全部饿死）——
        // `async_run` 已把 fetcher 调用包在 try 里，这里是二道保险。
        try {
          job();
        } catch (...) {
        }
      }
    }
  }

  /// 析构：先跑 effect 清理，再停池并 join。
  void stop_pool() {
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      pool_stopping = true;
      pending_cancelled += work_queue.size();   // 未开始的任务 = 被取消
      work_queue.clear();
    }
    queue_cv.notify_all();
    // 翻掉全部在飞代次的牌：已开始的任务在下一个检查点退出（不必等它跑完）。
    for (auto& entry : async_cancels) entry.second.cancel();
    async_cancels.clear();
    live_tokens.clear();
    for (auto& thread : pool) {
      if (thread.joinable()) thread.join();
    }
    pool.clear();
  }

  ~Impl() { stop_pool(); }

  /// 已取消的任务计数（诊断/测试用）。
  std::size_t pending_cancelled{0};

  /// 会话级持久状态仓（`persisted_slot` 的落点）：名字 → 状态。
  /// 生命周期 = Composer（即整个会话期）：条件剪掉再声明能拿回旧值。
  std::unordered_map<std::string, std::unique_ptr<StateBase>> persisted_states{};

  /// 正在构建的 keyed item（`for_each`）：没有显式传入 key 的首个声明自动带上它。
  /// 空 = 不在 keyed 区段（或该 item 还没声明元素）。
  std::string keyed_item_key{};
  /// 当前 keyed 区段（`for_each`）用到的全部 key：区分「还没轮到的兄弟」（待复用）
  /// 与「真残留」（本次数据里没有它——可释放）。
  std::unordered_set<std::string> keyed_region{};
  bool keyed_region_active{false};

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

Composer::~Composer() {
  // 先跑 effect 清理（≈ 组件卸载）：清理里可能读/写状态，故在摘掉活跃注册表之前。
  run_all_effect_cleanups();
  active_composers.erase(this);
}

void Composer::register_sub_scope(std::shared_ptr<Component> component, const std::string& key) {
  // 兄弟间身份：未给 key 时用当前父下的出现序号（位置稳定即可）
  const std::string name = key.empty() ? std::to_string(impl_->child_cursor[current_parent()]) : key;
  // 父 = 当前构建的 scope（嵌套的关键：孙的父是子，不是根）
  impl_->declare_child_scope(impl_->active_scope, std::move(component), name);
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
  impl_->root_scope = std::make_unique<Impl::Scope>();
  impl_->root_scope->component = impl_->component;
  impl_->root_scope->key = "@root";
  impl_->root_scope->dirty = true;
  impl_->mounted = true;
  impl_->scope_dirty = true;
  (void)reconcile();
  return true;
}

auto Composer::mount_into(Element& host, std::shared_ptr<Component> root_component) -> bool {
  impl_->anchor = &host;   // 根层声明改落到 host 的子位（单根语义换成多子）
  return mount(std::move(root_component));
}

auto Composer::dirty() const noexcept -> bool {
  if (impl_->scope_dirty) return true;
  if (impl_->root_scope == nullptr) return false;
  const auto any_dirty = [](const auto& self, const Impl::Scope& scope) -> bool {
    if (scope.dirty) return true;
    for (const auto& child : scope.children) {
      if (self(self, *child)) return true;
    }
    return false;
  };
  return any_dirty(any_dirty, *impl_->root_scope);
}

void Composer::rebuild_all() {
  impl_->subscribed_states.clear();
  impl_->scope_dirty = true;
  (void)reconcile();
}

auto Composer::last_reconcile_stats() const noexcept -> const ReconcileStats& {
  return impl_->last_stats;
}

auto Composer::reconcile() -> ReconcileStats {
  impl_->last_stats = ReconcileStats{};
  if (!impl_->mounted) return impl_->last_stats;
  // 脏判定：根脏 _或_ 作用域树里任一作用域脏（后者只重跑脏的那些——细粒度）
  bool any_dirty = impl_->scope_dirty;
  if (!any_dirty && impl_->root_scope != nullptr) {
    const auto any_scope_dirty = [](const auto& self, const Impl::Scope& scope) -> bool {
      if (scope.dirty) return true;
      for (const auto& child : scope.children) {
        if (self(self, *child)) return true;
      }
      return false;
    };
    any_dirty = any_scope_dirty(any_scope_dirty, *impl_->root_scope);
  }
  if (!any_dirty) return impl_->last_stats;
  const auto start = std::chrono::steady_clock::now();

  // —— ① 根作用域（仅在根脏时跑）——
  if (impl_->scope_dirty) {
    tls_composer = this;
    impl_->active_scope = impl_->root_scope.get();   // 根声明也登记到根 scope
    // 重跑前清空订阅（build 期间重新收集）；overlay 认领集也重置
    impl_->subscribed_states.clear();
    impl_->claimed_overlays.clear();
    impl_->hook_cursor = 0;   // hooks 游标：每次重组从 0 起
    impl_->parent_stack.clear();
    impl_->child_cursor.clear();
    impl_->root_replaced = false;
    // 根重跑会重建整棵子树 → 所有子作用域必须重跑才能对齐
    if (impl_->root_scope != nullptr) {
      for (auto& child : impl_->root_scope->children) child->mark_subtree_dirty();
      for (auto& child : impl_->root_scope->children) child->declared = false;
      impl_->root_scope->deps.clear();
    }
    impl_->parent_stack.push_back(impl_->anchor != nullptr ? impl_->anchor
                                                           : Impl::root_slot_marker());
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
    // 锚点形态：末尾裁剪（本次未声明的残留子元素移除）——根槽位形态由单根语义代替。
    if (impl_->anchor != nullptr) {
      const std::size_t declared = impl_->child_cursor.count(impl_->anchor) > 0
                                       ? impl_->child_cursor[impl_->anchor]
                                       : 0;
      while (impl_->anchor->content_child_count() > declared) {
        auto removed = impl_->anchor->remove_child(impl_->anchor->child_at(
            impl_->anchor->content_child_count() - 1));
        (void)removed;
        ++impl_->last_stats.elements_removed;
      }
    }
    tls_composer = nullptr;
    impl_->active_scope = nullptr;
    impl_->parent_stack.clear();
    // 清理未认领的 overlay：本次 build 没声明它 = 它应该消失
    sweep_overlays();
    // 剪枝：根重跑后未重新声明的子作用域（条件分支去掉了它）
    if (impl_->root_scope != nullptr) {
      std::erase_if(impl_->root_scope->children,
                    [](std::unique_ptr<Impl::Scope>& child) { return !child->declared; });
    }
    // 根作用域本次已跑过：清脏标记。
    //
    // 不清的后果（实测踩到，且被“构建幂等”掩盖了很久）：②的 `run_dirty_scopes`
    // 会**再跑一次根作用域**，而那条路径用的是根槽位语义（`set_content`）——
    // 于是根内容元素被**销毁重建**：`UiRoot::content()` 的指针身份变了，
    // 从此前 build 里存下的任何元素指针（如子树挂载的锚点）全部悬垂。
    // 正常单根形态下两次构建结果一样，看不出问题（只白跑一遍 + 指针身份漂流）；
    // 子树挂载（`mount_into`）一旦踩到就是非法访问。
    if (impl_->root_scope != nullptr) impl_->root_scope->dirty = false;
    impl_->scope_dirty = false;
  }

  // —— ② 作用域树（递归跑脏的；父不重跑则子独立跑）——
  //
  // 遍历顺序：自顶向下。父未跑则子各自判断；父跑了（它已把子树标脏）则子必然跑。
  // 这样保证「父重建了子树 → 子随后对齐」，且不重复跑（父跑时子被标脏一次、跑一次）。
  // 预算：超了就停手，剩下的留在树上顺延下一帧（它们的 dirty 仍为真）。
  if (impl_->root_scope != nullptr) {
    const bool cut = impl_->run_dirty_scopes(*impl_->root_scope, start,
                                             impl_->guardrails.frame_budget_ms);
    if (cut) impl_->last_stats.budget_exceeded = true;
  }

  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (static_cast<double>(
          std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) / 1000.0 >
      impl_->guardrails.frame_budget_ms) {
    impl_->last_stats.budget_exceeded = true;
  }
  // 注意：`scope_dirty = false` 只清**根**。被预算顺延的子作用域各自还带脏标记，
  // 所以下一帧 `dirty()` 仍为真——顺延是真顺延，不是丢弃。
  impl_->scope_dirty = false;
  // —— ③ 副作用（effect）——
  //
  // 必须在**脏标记清理之后**跑：effect 里写状态是连锁写（隔帧生效）——
  // 写在清理之前的话，那次 `invalidate` 会被下一行的 `scope_dirty = false` 吞掉，
  // 界面永远停在旧值（这类假死最难查：代码看着对、日志也没错）。
  // 副作用体在 build 之外执行 ⇒ 它的 `State::value()` 读不会误登记依赖。
  impl_->last_stats.effects_run = static_cast<int>(run_pending_effects());
  return impl_->last_stats;
}

void Composer::add_dependency(StateBase* state) {
  if (state == nullptr) return;
  // 依赖登记到**当前作用域**：根作用域 → `subscribed_states`（跑整根）；
  // 子作用域 → 它自己的 `deps`（跑它自己，父不跑）——细粒度重组的根基。
  if (impl_->active_scope != nullptr && impl_->active_scope != impl_->root_scope.get()) {
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

// —─ effect ──

void Composer::effect_impl(std::function<std::function<void()>()> body, const Deps& deps) {
  const std::string signature = deps_signature(deps);
  State<EffectSlot>& slot =
      state_slot<EffectSlot>(hook_index(), EffectSlot{});
  EffectSlot& current = slot.peek_mut();
  if (current.valid && current.signature == signature) return;   // 依赖未变：不重跑
  current.valid = true;
  current.signature = signature;
  impl_->pending_effects.push_back(Impl::PendingEffect{&slot, std::move(body)});
}

auto Composer::run_pending_effects() -> std::size_t {
  if (impl_->pending_effects.empty()) return 0;
  std::vector<Impl::PendingEffect> jobs;
  jobs.swap(impl_->pending_effects);
  std::size_t ran = 0;
  for (auto& job : jobs) {
    if (job.slot == nullptr) continue;
    // 先跑旧清理（≈ 上一次依赖的订阅关闭），再跑新体并记下本次清理。
    EffectSlot& slot = job.slot->peek_mut();
    if (slot.cleanup) {
      std::function<void()> previous = std::move(slot.cleanup);
      slot.cleanup = {};
      try {
        previous();
      } catch (...) {
        std::fprintf(stderr, "[dsl] effect 清理异常\n");
      }
    }
    try {
      slot.cleanup = job.body();
      ++ran;
    } catch (const std::exception&
                  error) {   // lint-allow: L5 捕获合法（副作用体不得拖垮重组）
      std::fprintf(stderr, "[dsl] effect 异常：%s\n", error.what());
    } catch (...) {
      std::fprintf(stderr, "[dsl] effect 未知异常\n");
    }
  }
  return ran;
}

auto Composer::pending_effect_count() const noexcept -> std::size_t {
  return impl_->pending_effects.size();
}

auto Composer::persisted_lookup(const std::string& key) -> StateBase* {
  const auto found = impl_->persisted_states.find(key);
  return found != impl_->persisted_states.end() ? found->second.get() : nullptr;
}

void Composer::persisted_store(const std::string& key, std::unique_ptr<StateBase> state) {
  impl_->persisted_states[key] = std::move(state);
}

auto Composer::persisted_keys() const -> std::vector<std::string> {
  std::vector<std::string> keys;
  keys.reserve(impl_->persisted_states.size());
  for (const auto& entry : impl_->persisted_states) keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  return keys;
}

void Composer::run_all_effect_cleanups() {
  impl_->pending_effects.clear();
  for (auto& state : impl_->hook_states) {
    auto* slot = dynamic_cast<State<EffectSlot>*>(state.get());
    if (slot == nullptr) continue;
    EffectSlot& value = slot->peek_mut();
    if (!value.cleanup) continue;
    std::function<void()> cleanup = std::move(value.cleanup);
    value.cleanup = {};
    try {
      cleanup();
    } catch (...) {
      std::fprintf(stderr, "[dsl] effect 清理异常\n");
    }
  }
}
// —─ 异步—─

auto Composer::async_begin(std::size_t slot, std::string fingerprint) -> std::uint64_t {
  if (slot >= impl_->async_slots.size()) impl_->async_slots.resize(slot + 1);
  auto& entry = impl_->async_slots[slot];
  if (entry.fingerprint == fingerprint && entry.token != 0) {
    return 0;   // 输入未变：不重发（已发出的继续在飞）
  }
  // **取消旧代**（关键修正）：只把旧 token 从 live_tokens 里摸掉是不够的——
  // 旧代的结果回到主线程时才能发现“自己已过期”。这里额外**翻牌**，
  // 让仍在跑的旧代能在下一个检查点提前退出（不等它自然结束）。
  if (entry.token != 0) {
    auto cancel = impl_->async_cancels.find(entry.token);
    if (cancel != impl_->async_cancels.end()) {
      cancel->second.cancel();
      impl_->async_cancels.erase(cancel);
    }
    impl_->live_tokens.erase(entry.token);
  }
  entry.fingerprint = std::move(fingerprint);
  entry.token = impl_->next_token++;
  impl_->live_tokens.insert(entry.token);
  impl_->async_cancels.emplace(entry.token, AsyncCancel{});
  return entry.token;
}

auto Composer::async_token_current(std::uint64_t token) -> bool {
  return impl_->live_tokens.count(token) > 0;
}

auto Composer::async_cancel_for(std::uint64_t token) -> AsyncCancel {
  const auto found = impl_->async_cancels.find(token);
  return found != impl_->async_cancels.end() ? found->second : AsyncCancel{};
}

void Composer::post_to_main(std::function<void()> task) {
  std::lock_guard<std::mutex> guard(impl_->inbox_mutex);
  impl_->inbox.push_back(std::move(task));
}

void Composer::run_on_worker(std::function<void()> task) {
  impl_->ensure_pool();
  {
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    if (impl_->pool_stopping) return;   // 已停：不再接受新任务
    impl_->work_queue.push_back(std::move(task));
  }
  impl_->queue_cv.notify_one();
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
  // ② 某个子作用域（任意深度）订阅了它：只脏那个作用域（**细粒度**：父不重跑）
  if (impl_->root_scope != nullptr) {
    if (Impl::Scope* owner = Impl::find_owner_scope(*impl_->root_scope, state); owner != nullptr) {
      owner->mark_subtree_dirty();
    }
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
      // 只裁到「内容子元素」边界：载体内部件（ScrollView 的滚动条等）永远保留
      while (parent->content_child_count() > declared) {
        auto removed = parent->remove_child(parent->child_at(parent->content_child_count() - 1));
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

  // 位置对齐：按父元素内游标取「该位置既有子元素」。
  //
  // 只对齐**内容子元素**（`content_child_count`）：少数组件把自己的内部件
  // （如 `ScrollView` 的滚动条）也放在 `children_` 末尾，它们不归调用方管——
  // 当成「上一帧多声明的残留」移除会让组件持有的裸指针悬垂
  // （实测：声明式里 ScrollView 与 List 分支互切 → `bar_->arrange` 段错误）。
  //
  // keyed 项（`begin_keyed_item` 背书）**复用依据是 key 而不是位置**：同一 key 的
  // 项在数据重排/中间插入后仍命中同一个元素（id/事件/元素级状态都跟着 key 走）。
  // 两套机制共用同一份对齐代码：命中 key ⇒ 把元素挪到当前游标位；**优先挪**——
  // 先挪开，被释放的那格恰好就是新元素要落的位置（于是不必做中间缓冲）。
  auto& cursor = impl_->child_cursor[parent];
  const std::size_t content_count = parent->content_child_count();
  const std::string wanted_key = !key.empty() ? std::string(key) : impl_->keyed_item_key;
  // ① 按 key 找既有元素（**只取第一个**：同 key 重复时后一个退回位置对齐，不歧义）
  Element* existing = nullptr;
  if (!wanted_key.empty()) {
    for (std::size_t index = 0; index < content_count; ++index) {
      Element* candidate = parent->child_at(index);
      if (candidate != nullptr && candidate->key() == wanted_key) {
        existing = candidate;
        break;
      }
    }
  }
  // ② 命中且不在游标位：挪到游标位（其余元素相对顺序不变；挪出的那格留给本项）
  if (existing != nullptr && existing != parent->child_at(cursor)) {
    auto moved = parent->remove_child(existing);
    if (moved != nullptr) {
      parent->insert_child(cursor, std::move(moved));
      ++impl_->last_stats.elements_moved;
    }
  }
  // ③ 未命中：退回位置对齐（无 key 的声明走的就是这条；keyed 新增落到游标位）
  if (existing == nullptr) {
    existing = cursor < parent->content_child_count() ? parent->child_at(cursor) : nullptr;
  }
  const bool type_matches = existing != nullptr && existing->type() == type;
  const bool key_matches = wanted_key.empty() || existing == nullptr ||
                           existing->key() == wanted_key;
  Element* element = nullptr;
  if (type_matches && key_matches) {
    element = existing;   // 复用：只更新 props
  } else {
    // 新建（或类型/key 对不上）：腾出游标位，把新元素插进那一格。
    //
    // **腾位只释放真正的残留**（`keyed_region` 里没有它的 key）：占着这一格的兄弟
    // 很可能只是「还没轮到」（它的 key 在本区段里，待会儿自己会被复用）——
    // 把它摘掉就毁掉了 key 复用。实测踩到：头插一项后其余项全部重建、id 漂移。
    auto created = make_element(std::string(type));
    if (created == nullptr) return nullptr;
    element = created.get();
    Element* occupant = cursor < parent->content_child_count() ? parent->child_at(cursor) : nullptr;
    if (occupant != nullptr && impl_->keyed_region.count(occupant->key()) == 0) {
      auto removed = parent->remove_child(occupant);
      (void)removed;   // 释放旧元素（它的 key 不在本次列表里 = 已被数据移除）
      ++impl_->last_stats.elements_removed;
    }
    parent->insert_child(std::min(cursor, parent->content_child_count()), std::move(created));
    ++impl_->last_stats.elements_created;
  }
  ++cursor;
  // key/id 先行截获（id 的本职是被外部引用——保持选择器安全）
  if (!wanted_key.empty() && element->key() != wanted_key) element->set_key(wanted_key);
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
ST_DSL_TYPE(SplitView, "SplitView")
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
ST_DSL_TYPE(CommandPalette, "CommandPalette")
ST_DSL_TYPE(TitleBar, "TitleBar")
ST_DSL_TYPE(WindowFrame, "WindowFrame")
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
  if (type == "SplitView") return std::make_unique<SplitView>();
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
  if (type == "CommandPalette") return std::make_unique<CommandPalette>();
  if (type == "MarkdownView") return std::make_unique<MarkdownView>();
  // 窗框（自绘标题栏 + 内容槽 + 缩放边缘；窗口控制由 ui::WindowControl 端口注入）
  if (type == "TitleBar") return std::make_unique<TitleBar>();
  if (type == "WindowFrame") return std::make_unique<WindowFrame>();
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

auto window_frame(Composer& c, std::string title, std::function<void(WindowFrame&)> configure,
                  const BoxProps& props, std::function<void()> children) -> Element& {
  auto& frame = custom<WindowFrame>(c, [title = std::move(title)](WindowFrame& it) {
    // 标题在构造后设（声明式要求**无参构造**，而标题是业务数据）
    if (it.title_bar() != nullptr) it.title_bar()->set_title(title);
  }, props);
  if (configure) configure(frame);
  // 子节点声明进**内容槽**（`WindowFrame::add_child` 已按此语义转发）：
  // 于是声明式里写 `window_frame(c, "标题", …, [&]{ ...内容... })` 就得到完整外壳。
  BuildScope scope(c, frame.content());
  if (children) children();
  return frame;
}

auto title_bar(Composer& c, std::string title, std::function<void(TitleBar&)> configure,
               const BoxProps& props) -> Element& {
  return custom<TitleBar>(c, [title = std::move(title), configure = std::move(configure)](
                                   TitleBar& it) {
    it.set_title(title);
    if (configure) configure(it);
  }, props);
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
    // 幂等写入：内容相同就不写——`Input::set_text` 会把光标推到末尾，
    // 而声明式下 build 随时可能重跑（任一状态变更）；无条件写会让
    // 「打字中光标乱跳」（输入框回归到上一次 set 时的末尾）——实测踩到过。
    if (input_element->value() != value) input_element->set_text(value);
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
  // `size <= 0` = **弹性空隙**（不是“0 宽固定块”）：
  // `spacer()` 是右对齐的惯用写法，而固定宽度为 0 的块在布局里等同于“不存在”
  // ——调用方要的是「把其余内容推开」，写成固定 0 宽只会静默失效
  // （实测：标题栏的 — □ × 因此挤在标题文字后面）。
  if (size > 0.0f) {
    element->style().width = size;
    element->style().height = size;
  } else {
    element->style().grow = true;
  }
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

DeclarativeHost::DeclarativeHost(UiRoot& root, Guardrails guardrails)
    : composer_(std::make_unique<Composer>(root, guardrails)) {}

// keyed 区段（`for_each`）：置上当前项的 key——随后该 item 声明的首个元素
// 自动按这个 key 跨位置复用（也可以显式传 `.key`，显式优先）。
void Composer::begin_keyed_item(const std::string& key, std::size_t index) {
  (void)index;   // 身份是 key；数据位置只在诊断里用
  impl_->keyed_item_key = key;
}

void Composer::end_keyed_item() { impl_->keyed_item_key.clear(); }

void Composer::begin_keyed_region(const std::vector<std::string>& keys) {
  // 本区段用到的全部 key：新建项顶到游标位时「那个位置上的兄弟是否还有归宿」靠它判断。
  impl_->keyed_region.clear();
  for (const std::string& key : keys) {
    if (!key.empty()) impl_->keyed_region.insert(key);
  }
  // 本区段游标从当前子位数起（区段前的兄弟已经声明过了）——区段结束前不动它。
  impl_->keyed_region_active = true;
}

void Composer::end_keyed_region() {
  impl_->keyed_region.clear();
  impl_->keyed_region_active = false;
  impl_->keyed_item_key.clear();
}

auto DeclarativeHost::mount(std::shared_ptr<Component> root_component) -> bool {
  const bool mounted = composer_->mount(std::move(root_component));
  // 记下挂载帧的统计：`mount` 本身就跑了一次重组（元素新建/嵌套护栏错误都在里面）
  // ——不记的话 `stats()` 在首次 `tick()` 之前永远是空默认值（调用方看不到刚发生的事）。
  last_ = composer_->last_reconcile_stats();
  return mounted;
}

auto DeclarativeHost::mount_into(Element& host, std::shared_ptr<Component> root_component)
    -> bool {
  const bool mounted = composer_->mount_into(host, std::move(root_component));
  last_ = composer_->last_reconcile_stats();
  return mounted;
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

auto mount_into(UiRoot& root, Element& host_element, std::shared_ptr<Component> root_component,
                Guardrails guardrails) -> std::unique_ptr<DeclarativeHost> {
  auto host = std::make_unique<DeclarativeHost>(root, guardrails);
  if (!host->mount_into(host_element, std::move(root_component))) return nullptr;
  return host;
}

}  // namespace st::ui::dsl
