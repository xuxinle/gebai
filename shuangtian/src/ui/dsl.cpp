#include "st/ui/dsl.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/markdown_view.hpp"
#include "st/ui/components/terminal.hpp"
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

#include "st/ext/json.hpp"

namespace st::ui::dsl {

// ── thread-local 当前 Composer + 全局活跃 Composer 注册表 ────────────────

namespace {
thread_local Composer* tls_composer = nullptr;

/// 活跃 Composer 注册表（进程级）。
///
/// 为什么需要它：`State` 的写**发生在重组之外**时（事件回调；以及工作线程），
/// `thread_local` 为空——此时只能靠「全进程有哪些 Composer」来定位订阅者。
///
/// ## 并发契约（本处曾是无锁遍历，属真实缺陷）
///
/// - **容器有锁**（`active_composers_mutex`）：注册/注销在 `Composer` 构造/析构里发生，
///   而写通知可能来自另一个线程——无锁时「迭代中 erase」会让迭代器失效（UB）。
/// - **一律先快照再通知**：通知会跑到用户代码（effect 清理、组件 build），期间再有人
///   注册/注销也不会踩坏正在遍历的容器。
/// - **真值树只在 UI 线程动**：`Impl` 的作用域树与元素树由 `reconcile` 独占。非 UI 线程
///   写状态时不在这里直接标脏，而是**投递回 UI 线程**（见 `Composer::notify_state_written`），
///   由 `pump_async()` 在 `reconcile()` 之前落地——语义仍是「下一帧可见」。
///
/// 生命期约定（宿主需遵守）：`Composer`（及其宿主 `DeclarativeHost`）必须在 UI 线程上
/// 销毁，且不得与仍在持帧的事件回调并发析构。
// lint-allow: L8 依赖登记需跨 Composer 实例可见（State 写在重组之外时靠它定位订阅者），故必须进程级；已加锁并快照遍历
std::unordered_set<Composer*> active_composers;
std::mutex active_composers_mutex;

/// 快照当前活跃 Composer（持锁拷贝，返回后即可无锁遍历）。
[[nodiscard]] auto snapshot_active_composers() -> std::vector<Composer*> {
  std::lock_guard<std::mutex> guard(active_composers_mutex);
  return {active_composers.begin(), active_composers.end()};
}
}  // namespace

namespace {

/// 声明式 overlay 宿主：给浮层内容一个坐标系，并决定**面板外的输入怎么走**。
///
/// 两种语义（由 `set_outside_barrier` 选，默认穿透）：
///
/// - **穿透**（默认）：`hit_test` 只认子元素。用于**非模态浮层**（查找条）——
///   面板开着时用户仍要能点编辑器、能选中文字，浮层不该把整屏占住。
/// - **屏障**：`hit_test` 认整个视口，并在面板外按下+点击时发 `on_outside_click`。
///   用于**瞬态浮层**（下拉菜单）——与 Flutter modal barrier / Web backdrop 同款：
///   点外面就相当于“我要它消失”，且不应该顺手把下层那个东西也点了
///   （“想关菜单却触发了背后的按钮”是真实的误操作）。
///
/// 为何不统一：这两个需求正好相反，由一个开关表达比在每个组件里各写一套判定便宜——
/// 而且“面板外”的几何只有宿主自己知道。
class OverlayHost : public Panel {
 public:
  /// 面板外点击回调（仅在屏障形态下触发）。
  std::function<void()> on_outside_click{};

  /// 屏障形态：面板外也命中本宿主（输入不再穿透下层）。
  void set_outside_barrier(bool value) noexcept { barrier_ = value; }

  [[nodiscard]] auto hit_test(math::Point point) const noexcept -> bool override {
    return barrier_ ? Element::hit_test(point) : hit_test_children(point);
  }

  // **屏障只吃“点”，不吃“移动”**：宿主（菜单栏/工具栏）需要移动做悬停反馈。
  // 实测：不分事件类型地拿整块矩形吃命中，会让面板开着时菜单栏收不到 hover，
  // “移到别的标题切菜单”失效（用户报的两条即此）。
  [[nodiscard]] auto hit_test_pointer_move(math::Point point) const noexcept -> bool override {
    return hit_test_children(point);
  }

  auto on_event(const RenderContext& context, Event& event) -> bool override {
    if (!barrier_) return Panel::on_event(context, event);
    switch (event.kind) {
      case EventKind::MouseDown:
        outside_press_ = on_outside_click != nullptr && !hit_test_children(event.position);
        return true;   // 屏障：消费，不让下层拿到这次按下
      case EventKind::Click:
        if (outside_press_ && on_outside_click != nullptr) on_outside_click();
        outside_press_ = false;
        return true;
      case EventKind::MouseUp:
        return true;   // 上抬与 Click 同属“点”这条链，不给下层
      case EventKind::MouseMove:
        // **鼠标移动必须放行**——屏障只拦“点”，不拦“移动”。
        //
        // 曾经把 MouseMove 也吞掉（注释写的理由是“避免 hover 闪烁”），代价是
        // **宿主收不到 hover**：菜单栏开着下拉面板时，移到另一个标题不再切换
        //（用户报「点击其他按钮直接切换列表」「面板不跟随按钮的位置」两条由此而来，
        // 实测面板开着时 `menubar.hovered` 恒为 false）。
        //
        // “面板外不动下层”的诉求只该作用在**点击**上（那才是误操作来源）；
        // 移动是无害的持续性输入，宿主（菜单栏/工具栏）本就靠它做悬停反馈。
        return Panel::on_event(context, event);
      default:
        return Panel::on_event(context, event);
    }
  }

 private:
  bool barrier_{false};
  bool outside_press_{false};
};

}  // namespace

auto current_composer() noexcept -> Composer* { return tls_composer; }

/// 声明式包装函数的不变量失败：`create_element(type)` 返回 nullptr。
///
/// 为什么不再沿用旧的 `static Element* none = nullptr; return *none;`：
/// 那是**解引用空指针**（未定义行为）——优化器可以把它变成任意行为，而作者的本意是
/// “这里不可能发生”。真实缺陷就该**立刻、带类型名、可定位**地失败，而不是悄悄 UB。
/// （`make_element` 已覆盖全部内置组件；走到这里只可能是新组件忘了登记。）
[[noreturn]] void fail_missing_element_factory(std::string_view type) {
  st::eprint("[dsl] 内部错误：create_element(\"{}\") 返回空——make_element 未覆盖该类型", type);
  std::abort();
}

namespace detail {

void on_state_read(StateBase* state) {
  if (tls_composer != nullptr && state != nullptr) tls_composer->add_dependency(state);
}

void on_state_write(StateBase* state) {
  if (state == nullptr) return;
  // ① 正在重组（thread-local 有值）：直接通知它，别的 Composer 与本状态无关。
  if (tls_composer != nullptr) {
    tls_composer->notify_state_written(state);
    return;
  }
  // ② 不在重组中（事件回调 / 工作线程）：通知全部活跃 Composer，各自判断是否订阅过它。
  //    先快照——通知过程可能注销别的 Composer（`~Composer`）。
  for (Composer* composer : snapshot_active_composers()) {
    composer->notify_state_written(state);
  }
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
  /// 构造它的线程 = UI 线程。用于区分「主线程事件回调」与「工作线程写状态」：
  /// 后者必须投递回本线程才能标脏（作用域树由 `reconcile` 独占，见 §notify_state_written）。
  std::thread::id ui_thread{std::this_thread::get_id()};

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
  /// 本区段用到的全部 key：区分「还没轮到的兄弟」（待复用）与「真残留」（本次数据里没有它——可释放）。
  std::unordered_set<std::string> keyed_region{};
  /// 本区段里**重复出现**过的 key（对齐时必须旁路它们，见 `begin_keyed_region`）。
  std::unordered_set<std::string> keyed_duplicates{};
  /// 本区段里**已被认领**的元素：同一 key 只允许被一个 item 认领（防“后来者偷走前一个的元素”）。
  std::unordered_set<const Element*> claimed_keyed{};
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
  {
    std::lock_guard<std::mutex> guard(active_composers_mutex);
    active_composers.insert(this);
  }
}

Composer::~Composer() {
  // 先跑 effect 清理（≈ 组件卸载）：清理里可能读/写状态，故在摘掉活跃注册表之前。
  run_all_effect_cleanups();
  {
    std::lock_guard<std::mutex> guard(active_composers_mutex);
    active_composers.erase(this);
  }
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
  // 线程分流（本函数曾有无锁/跨线程写树的缺陷）：
  //
  // A. 重组线程（正在跑 build）：订阅表在本线程内刚收集，直接判。
  // B. UI 主线程（事件回调/帧循环，非重组）：直接标脏——**行为与改造前逐位相同**，
  //    仍由下一帧 `reconcile` 落地。
  // C. 其他线程（`resource` 的工作线程等）：**不能触碰作用域树**（`reconcile` 独占），
  //    投递回 UI 线程执行；`tick()` 里 `pump_async()` 排在 `reconcile()` 之前，
  //    所以仍是「下一帧可见」，与 A/B 同语义。
  const bool on_reconcile_thread = tls_composer == this;
  const bool on_ui_thread = impl_->ui_thread == std::this_thread::get_id();
  if (on_reconcile_thread || on_ui_thread) {
    mark_state_dirty(state);
    return;
  }
  post_to_main([this, state] { mark_state_dirty(state); });
}

void Composer::mark_state_dirty(StateBase* state) {
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
    // **单根契约的破坏必须报错**（2026-10-06 补，来自实测段错误）。
    //
    // 根层只允许**一个**顶层声明：第二次声明会 `set_content()` 顶掉第一个
    // （连同把它析构），于是任何保存了第一个元素指针的地方（组件字段、测试、
    // `custom<T>` 的返回值）当场悬垂——实测表现为下一次访问跳到地址 0 的
    // **SIGSEGV**，且现场只有调用方那一行，看不出是这里干的。
    //
    // 旧行为是静默替换（因为“替换根”本身是合法能力：换页时根元素类型变了就要重建）。
    // 区分两者只看一件事：**本帧是否已经落过根元素**——`root_replaced` 就是它。
    // 已落过还想再落 = 一次 build 里声明了两个顶层元素，那是契约错误，不是换页。
    if (impl_->root_replaced) {
      if (impl_->last_stats.error.empty()) {
        impl_->last_stats.error = std::format(
            "一个 build() 里声明了多个顶层元素（根层第二个 {}）——DSL 是**单根**契约："
            "把两个顶层元素包进 column/row（或用一个容器）再声明",
            type);
      }
      std::fprintf(stderr, "[dsl] %s\n", impl_->last_stats.error.c_str());
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
  std::size_t content_count = parent->content_child_count();
  const std::string wanted_key = !key.empty() ? std::string(key) : impl_->keyed_item_key;
  // ① 按 key 找既有元素（跳过的元素 = 本区段里已被认领的同区兄弟/本 item 自己的）
  //
  // ⚠ **重名 key 必须旁路**（2026-10-05 修，实测事故）：key 重复时若照旧“取第一个”，
  // 第二项会把第一项的**元素偷走**（挪到自己的游标位），于是两项落到同一个元素上、
  // 列表直接少一行——而文案与行数在画面上看起来都没错（少的那行正是被偷的），
  // 排查方向很容易跑偏。改成：重名的 key **退回按位置对齐**（两项各自守住自己的位置），
  // 重名清单记进 `ReconcileStats::key_collisions`。
  const bool key_is_unique =
      !wanted_key.empty() && impl_->keyed_duplicates.count(wanted_key) == 0;
  Element* existing = nullptr;
  if (key_is_unique) {
    // **从游标开始找，而不是从 0**（2026-10-05 修，实测性能事故）。
    //
    // 对齐是**顺序**做的：调用方按 item 顺序声明，游标从区段头单调推进到尾部。
    // 游标之前的位置已经对齐完了，那里的元素要么就是本项要的那个（→ 命中且就在
    // 游标位，下面会直接复用），要么属于已经处理过的前序 item（→ 本项要找的
    // 必然在更后面）。换句话说：**要复用的元素不可能待在游标之前**——那种情形只会是
    // “已被认领”，而认领集本来就会跳过它。
    //
    // 从 0 扫的代价是 O(N²)：每个 item 都把前面所有位置重新扫一遍。实测 5000 项
    // （每项 1 个元素、只改 1 条数据、零结构变更）**43.6 ms**，而同一份列表
    // 把 key 去掉（退回纯位置对齐）只要 **0.67 ms**——65 倍差全在这一行。
    // 改从游标起步后回到 0.6 ms 量级。
    //
    // 为何“重名旁路”与“认领集”两个机制不受影响：重名时根本不进本分支；
    // 认领集只在同 key 被多个 item 认领时生效，而那只可能发生在游标处（本项自己）
    // 或游标之后（尚未处理的后继 item）。
    for (std::size_t position = cursor; position < content_count; ++position) {
      ++impl_->last_stats.alignment_probes;   // 复杂度证据（见 `ReconcileStats::alignment_probes`）
      Element* candidate = parent->child_at(position);
      if (candidate == nullptr || candidate->key() != wanted_key) continue;
      if (impl_->claimed_keyed.count(candidate) != 0) continue;   // 已被认领（同区兄弟）
      existing = candidate;
      break;
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
    // ⚠ **无 key 的元素不得当残留释放**（2026-10-06 修，实测段错误）。
    //
    // “无 key”不等于“无主”：`key` 是**列表内的身份**（仅由 `for_each` 背书或显式 `.key`
    // 给出），而大量元素只有 `.id`（id 是**给外部引用的稳定名字**，两回事）。
    // 旧判定只问“key 在不在本区段里”——无 key 的元素（`""`）同样不命中，于是被当
    // “上帧残留”摘掉并析构，而调用方（组件字段、测试、`custom<T>` 的返回值）还指着它。
    //
    // 实测后果：两个 `row(.id=...)` 顺序声明（第二个类型对不上 → 走本分支），
    // 第一个被当场销毁，调用方持有的指针 vptr 清零，下一行
    // `bar->content_child_count()` 跳到地址 0 → **SIGSEGV**，且每次必崩。
    //
    // 口径：**只释放“有 key、但 key 不在本轮列表里”的元素**（那才是被数据移除的旧项）。
    // 无 key 的元素归位置对齐管：类型不符就在它前面插入新元素，
    // 多余的那个由本轮结束时的尾部截断处理（那里才能确定谁真的没被声明）。
    //
    // ⚠ 注意不能反过来写成“释放 keyed 但未认领的”——那恰好毁掉 key 复用
    // （占着游标位的只是“还没轮到”的兄弟）。实测：这么改 `dsl_for_each_reuses_by_key`
    // 当场变红（身份丢失、child_at 全对不上）。
    const bool occupant_is_stale =
        occupant != nullptr && impl_->keyed_region.count(occupant->key()) == 0;
    if (occupant_is_stale) {
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
  // 本项声明过的元素都算「已认领」（同区兄弟 / 同一 item 的多个顶层元素）
  impl_->claimed_keyed.insert(element);
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
  auto host = std::make_unique<OverlayHost>();
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

/// 声明式 overlay 宿主：铺满视口，但**只在自己的子元素上命中**。
///
/// 为什么不能直接用 `Panel`：`Element::hit_test` 是「bounds 含点」——铺满视口的宿主
/// 会把整屏点击都吃下，于是浮层盖上来之后编辑器、侧栏、状态栏**全部点不动**
/// （实测：菜单面板打开后，连“点面板外把它关掉”都做不到）。
///
/// 宿主的职责是「给浮层内容一个坐标系」，不是「占住屏幕」：命中收窄到子元素，
/// 浮层外的点击自然落到下层内容——与 `ContextMenu` 的 dismiss barrier 相比这里
/// 选择**穿透**而非拦截，因为声明式浮层（查找条/菜单面板）都不需要屏障语义。
/// 定义在文件头部（`overlay_slot` 之前）。

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

// ── 组件注册表：**类型身份的唯一真源**（结构评审 A1） ─────────────────────
//
// 背景：同一份「组件类型」原先散在**两处**，谁也不认识谁——
//   ① `type_name<T>()` 特化表（38 行，给 `custom<T>` 用）；
//   ② `make_element` 的 `if (type == "...")` 链（42 行，给协议/脚本/属性填装用）。
// 新增组件漏改一边就是**静默失效**（`custom<T>` 报「未注册」，或 `make_element` 返回空
// → 声明式节点凭空消失），而编译器不会提醒。
//
// 现在**一份清单、两处派生**：清单是唯一的类型身份表，`ST_COMPONENT_LIST` 之外没有第二份。
//
//   - `ST_DSL_REGISTRY`：清单 → `kComponents` 表（`make_element` 按注册名查表）；
//   - `ST_DSL_TYPENAME`：清单 → `type_name<T>()` 特化（`custom<T>` 按类型取名字）。
//
// 于是「新增一个可声明组件」= 在清单里加**一行**。
//
// 注册名是**对外契约**（属性面 / 控制协议 / 脚本都按它寻址），改名属破坏性变更；
// 特别注意 `IconView` 的注册名是 `"Icon"`、`TextArea`/`CommandPalette` 与类名同名——
// 这些都不是笔误，而是既有对外契约。名字唯一性与两处派生的一致性由单测钉住
// （`tests/ui_dsl_test.cpp` 的 `dsl_registry_*`）。
//
// 为什么用 X-macro 而不是模板：组件的构造函数签名各异（`Button(label)`、`Table(columns)`、
// `MenuPanel(items)`），没有统一的 `T{}` 形态，因此无法只靠模板从类型推出实例化方式。
// X-macro 是「一份清单、多次展开」在 C++ 里的标准解法——这里是 `CONVENTIONS` §8 认可的
// 机械重复压缩用途（与 `ST_DSL_TYPE` 原用法同类）。

// 组件清单：`(C++ 类型, 注册名, 构造表达式)`。**新增组件只改这里一行。**
#define ST_COMPONENT_LIST(X) /* lint-allow: L3 一份清单多处展开（X-macro） */ \
  X(Panel, "Panel", std::make_unique<Panel>())                            \
  X(Text, "Text", std::make_unique<Text>())                               \
  X(Heading, "Heading", std::make_unique<Heading>())                      \
  X(IconView, "Icon", std::make_unique<IconView>())                       \
  X(Button, "Button", std::make_unique<Button>(""))                       \
  X(Card, "Card", std::make_unique<Card>())                               \
  X(Divider, "Divider", std::make_unique<Divider>())                      \
  X(KeyValueRow, "KeyValueRow", std::make_unique<KeyValueRow>("", ""))     \
  X(Spacer, "Spacer", std::make_unique<Spacer>())                         \
  X(Input, "Input", std::make_unique<Input>())                            \
  X(TextArea, "TextArea", std::make_unique<TextArea>())                   \
  X(Checkbox, "Checkbox", std::make_unique<Checkbox>(""))                 \
  X(Radio, "Radio", std::make_unique<Radio>(""))                          \
  X(Switch, "Switch", std::make_unique<Switch>(""))                       \
  X(Slider, "Slider", std::make_unique<Slider>())                         \
  X(Select, "Select", std::make_unique<Select>())                         \
  X(List, "List", std::make_unique<List>())                               \
  X(ListItem, "ListItem", std::make_unique<ListItem>("", ""))             \
  X(Table, "Table", std::make_unique<Table>())                            \
  X(Tree, "Tree", std::make_unique<Tree>())                               \
  X(ScrollView, "ScrollView", std::make_unique<ScrollView>())             \
  X(ScrollBar, "ScrollBar", std::make_unique<ScrollBar>())                \
  X(SplitView, "SplitView", std::make_unique<SplitView>())                \
  X(Tabs, "Tabs", std::make_unique<Tabs>())                               \
  X(ProgressBar, "ProgressBar", std::make_unique<ProgressBar>())          \
  X(Spinner, "Spinner", std::make_unique<Spinner>())                      \
  X(Badge, "Badge", std::make_unique<Badge>())                            \
  X(Chip, "Chip", std::make_unique<Chip>())                               \
  X(Avatar, "Avatar", std::make_unique<Avatar>())                         \
  X(Tooltip, "Tooltip", std::make_unique<Tooltip>())                      \
  X(Dialog, "Dialog", std::make_unique<Dialog>())                         \
  X(Toast, "Toast", std::make_unique<Toast>())                            \
    X(MenuBar, "MenuBar", std::make_unique<MenuBar>())                                \
  X(MenuPanel, "MenuPanel", std::make_unique<MenuPanel>(std::vector<MenuItem>{})) \
  X(ContextMenu, "ContextMenu", std::make_unique<ContextMenu>(math::Point{}, std::vector<MenuItem>{})) \
  X(FileDialog, "FileDialog", std::make_unique<FileDialog>())             \
  X(Terminal, "Terminal", std::make_unique<Terminal>())                   \
  X(CodeEditor, "CodeEditor", std::make_unique<CodeEditor>())             \
  X(CommandPalette, "CommandPalette", std::make_unique<CommandPalette>()) \
  X(MarkdownView, "MarkdownView", std::make_unique<MarkdownView>())       \
  X(TitleBar, "TitleBar", std::make_unique<TitleBar>())                   \
  X(WindowFrame, "WindowFrame", std::make_unique<WindowFrame>())

//lint-allow: L3 组件清单的 X-macro 展开（一份清单多处派生；无模板替代形式）
#define ST_DSL_REGISTRY(TYPE, NAME, CONSTRUCT) /* lint-allow: L3 同上 */ \
  { NAME, []() -> std::unique_ptr<Element> { return CONSTRUCT; } },

/// 注册表条目：注册名 → 构造器。
struct ComponentEntry {
  std::string_view name;
  std::unique_ptr<Element> (*create)();
};

/// 全部可声明组件（由清单展开，**不手工维护**）。
const ComponentEntry kComponents[]{ST_COMPONENT_LIST(ST_DSL_REGISTRY)};

//lint-allow: L3 同上（清单的第二处展开）
#define ST_DSL_TYPENAME(TYPE, NAME, CONSTRUCT) /* lint-allow: L3 同上 */                \
  template <>                                                                      \
  [[nodiscard]] auto type_name<TYPE>() -> std::string { return NAME; }              \
  static_assert(true, "type_name 特化定义需要分号终止");

ST_COMPONENT_LIST(ST_DSL_TYPENAME)

#undef ST_DSL_REGISTRY
#undef ST_DSL_TYPENAME

auto make_element(std::string type) -> std::unique_ptr<Element> {
  for (const ComponentEntry& entry : kComponents) {
    if (entry.name == type) return entry.create();
  }
  return nullptr;
}

/// 全部注册名（诊断 / 测试 / 协议 `ui.create` 的合法类型枚举）。
auto registered_element_types() -> std::vector<std::string> {
  std::vector<std::string> names;
  names.reserve(std::size(kComponents));
  for (const ComponentEntry& entry : kComponents) names.emplace_back(entry.name);
  return names;
}

// ── BoxProps 应用 ─────────────────────────────────────────────────────────

void apply_box(Element& element, const BoxProps& props) {
  Style& style = element.style();
  if (props.gap >= 0.0f) style.gap = props.gap;
  if (props.padding >= 0.0f) style.padding = math::Insets::all(props.padding);
  if (props.margin >= 0.0f) style.margin = math::Insets::all(props.margin);
  // 轴向外边距在整体内边距**之后**应用：于是「padding=8, padding_x=12」
  // 表达的是「上下 8、左右 12」，与协议属性面的先后语义一致。
  if (props.padding_x >= 0.0f) {
    style.padding.left = props.padding_x;
    style.padding.right = props.padding_x;
  }
  if (props.padding_y >= 0.0f) {
    style.padding.top = props.padding_y;
    style.padding.bottom = props.padding_y;
  }
  if (props.width != kAuto) style.width = props.width;
  if (props.height != kAuto) style.height = props.height;
  style.grow = props.grow;
  if (props.radius >= 0.0f) style.radius = props.radius;
  if (!props.key.empty()) element.set_key(props.key);
  if (!props.id.empty()) element.set_id(props.id);
  // 排版三件套：**记成显式覆盖**（`apply_theme` 正常会从主题重算颜色/字号/字重，
  // 不记就每帧被主题盖回默认值——症状是“设了颜色，首帧对、下一帧就没了”）。
  //
  // 色值两种写法：字面 `hex_color` 优先于语义 `color`（两者同给时以更具体的为准）。
  if (!props.hex_color.empty()) {
    if (const auto parsed = svg::parse_color(props.hex_color); parsed.has_value()) {
      element.set_text_color(*parsed);
    }
    // 解析失败即当成没设（不把色值写坏、也不静默改成黑）：`parse_color` 认
    // `#rgb`/`#rgba`/`#rrggbb`/`#rrggbbaa` 与 17 个 CSS 基本色名。
  } else if (props.color.has_value()) {
    element.set_text_tone(*props.color);
  }
  if (props.size >= 0.0f) element.set_text_size(props.size);
  if (props.weight.has_value()) element.set_text_weight(*props.weight);
  // 背景表面档位（与上面的文字色是两条互不干涉的通道）：
  // 它不做“显式覆盖”，而是**声明语义**——`apply_theme` 按主题取色，
  // 所以主题切换后颜色自动跟随（这正是它与 `hex_color` 的分工）。
  if (props.surface != Element::Surface::None) element.set_surface(props.surface);
}

// ── 组件包装 ──────────────────────────────────────────────────────────────

auto row(Composer& c, const BoxProps& props, std::function<void()> children) -> Element& {
  st::Json props_json = st::Json::object();
  Element* element = c.create_element("Panel", props_json, props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Panel");
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
    fail_missing_element_factory("Panel");
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
    fail_missing_element_factory("Text");
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
    fail_missing_element_factory("Button");
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
    fail_missing_element_factory("Checkbox");
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
    fail_missing_element_factory("Switch");
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
    fail_missing_element_factory("Slider");
  }
  apply_box(*element, props);
  if (auto* slider_element = dynamic_cast<Slider*>(element); slider_element != nullptr) {
    slider_element->set_value(value);
    if (on_change) slider_element->on_change = std::move(on_change);
  }
  return *element;
}

auto input(Composer& c, std::string value, std::function<void(std::string)> on_input,
           const BoxProps& props, bool password) -> Element& {
  Element* element = c.create_element("Input", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Input");
  }
  apply_box(*element, props);
  if (auto* input_element = dynamic_cast<Input*>(element); input_element != nullptr) {
    // 幂等写入：内容相同就不写——`Input::set_text` 会把光标推到末尾，
    // 而声明式下 build 随时可能重跑（任一状态变更）；无条件写会让
    // 「打字中光标乱跳」（输入框回归到上一次 set 时的末尾）——实测踩到过。
    if (input_element->value() != value) input_element->set_text(value);
    // 密码态也幂等写（`set_password` 自身有相等早退，这里写直白些与 set_text 对齐）
    if (input_element->password() != password) input_element->set_password(password);
    if (on_input) {
      input_element->on_change = [on_input](std::string_view v) { on_input(std::string(v)); };
    }
  }
  return *element;
}

auto progress(Composer& c, float value, const BoxProps& props) -> Element& {
  Element* element = c.create_element("ProgressBar", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("ProgressBar");
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
    fail_missing_element_factory("Badge");
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
    fail_missing_element_factory("Heading");
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
    fail_missing_element_factory("Divider");
  }
  if (vertical) element->style().width = 1.0f;  // 竖线近似：v1 水平线为主
  return *element;
}

auto card(Composer& c, const BoxProps& props, std::function<void()> children) -> Element& {
  Element* element = c.create_element("Card", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("Card");
  }
  apply_box(*element, props);
  BuildScope scope(c, element);
  if (children) children();
  return *element;
}

auto spacer(Composer& c, float size) -> Element& {
  Element* element = c.create_element("Spacer", st::Json::object());
  if (element == nullptr) {
    fail_missing_element_factory("Spacer");
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
    fail_missing_element_factory("Icon");
  }
  apply_box(*element, props);
  if (auto* view = dynamic_cast<IconView*>(element); view != nullptr) {
    view->set_icon(std::move(name));
    view->set_size(size);
  }
  return *element;
}

auto markdown(Composer& c, std::function<std::string()> source, const BoxProps& props)
    -> Element& {
  Element* element = c.create_element("MarkdownView", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("MarkdownView");
  }
  apply_box(*element, props);
  if (auto* view = dynamic_cast<MarkdownView*>(element); view != nullptr) {
    // `source` 是惰性闭包（与 `text` 同口径）：重组时重新求值，闭包内读 State 即自动订阅。
    // `set_markdown` 自带相等早退，所以这里不必像 `input` 那样手写幂等判定。
    view->set_markdown(source ? source() : std::string());
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
  if (on_open_menu) bar->on_open_menu = on_open_menu;
  // 下拉面板的关闭请求（Esc / 点面板外 / 激活条目）必须回落给调用方——
  // 面板的 `on_close` 已在 `MenuBar::make_panel` 里发，但**谁来摘这个 overlay**
  // 只有调用方知道。不接它的话：“Esc 关了”的只是 `MenuBar::open_index_`，
  // 面板本身永远留在屏上（实测：菜单打开后关不掉）。
  //
  // 这里接的是**同一个 `on_open_menu` 回调的逆语义**：`kNoIndex` 表示“不打开任何菜单”，
  // 即声明层面不认领那个 overlay，下一帧框架 sweep 自动摘除。
  bar->on_menu_close = [bar, on_open_menu]() {
    bar->set_open_index(MenuBar::kNoIndex);
    if (on_open_menu) on_open_menu(MenuBar::kNoIndex);
  };
  return bar;
}

auto context_menu(Composer& c, math::Point anchor, std::vector<MenuItem> items,
                  const BoxProps& props) -> ContextMenu& {
  Element* element = c.create_element("ContextMenu", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("ContextMenu");
  }
  apply_box(*element, props);
  // 锚点与条目是构造后写入的一等接口（与 `ContextMenu::make` 同一套：
  // 工厂里也走 `anchor_` + `MenuPanel` 两段，语义一致）。
  auto* menu = dynamic_cast<ContextMenu*>(element);
  if (menu != nullptr) {
    menu->anchor_ = anchor;
    if (menu->panel() != nullptr) {
      menu->panel()->set_items(std::move(items));
    }
  }
  return *menu;
}

void menu_panel_overlay(Composer& c, MenuBar& bar, std::size_t index) {
  if (index >= bar.menu_count()) return;
  // **整个菜单栏只有一个下拉面板**，切换菜单 = 换内容 + 换锚点。
  //
  // key 曾经是 `"menu-panel-" + menu_id(index)`（每个菜单一个 overlay），于是
  // 「切换菜单」被建模成“卸掉旧 overlay + 装一个新 overlay”，而 overlay 是
  // **声明式重建**：旧 key 不再被声明 ⇒ 下一帧 sweep 把它摘掉。
  //
  // 用户报的两件事都出在这里（实测）：
  // ① 「点击其他按钮不能直接切换列表」——点“编辑”时旧面板先被摘掉，
  //    新面板下一帧才认领，中间有一帧“什么都没有”，观感就是面板闪没/没切过去；
  // ② 面板不跟随按钮——`make_panel` 里的 `set_anchor(title_rect(index))` 只在
  //    **首次构造**时执行（同 key 复用分支跳过了它），而 key 变了就换了新对象……
  //    但同一 key 复用时光标移开又回来并不会重设 anchor，锚点会停在**首次打开**那一项。
  //
  // 用**固定 key** 后：同一宿主、同一面板对象，切换只更新
  // “条目 + 锚点”，不再有摘装过程，面板位置也就必然跟着按钮走。
  const std::string key = "menu-panel";
  Element* host = c.overlay_slot(key);
  // 首次认领时构造面板；之后**每次**都把内容与锚点刷成当前菜单
  //（`set_items` 带 0 值早退、`set_anchor` 是纯赋值，逐帧调用无代价）。
  if (host->child_count() == 0) {
    host->add_child(bar.make_panel(index));
  } else if (auto* panel = dynamic_cast<MenuPanel*>(host->children()[0].get())) {
    // 条目 / 回调 / 锚点三样一起换——收在 `bind_panel` 里，避免切换路径漏掉某一样
    //（这三种漏法各自都对应过一个缺陷，见那里的说明）。
    bar.bind_panel(*panel, index);
  }
  // 点面板外 = 关闭请求：下拉菜单的通行手势（菜单栏自己没有全屏命中，
  // 只有宿主知道“点在面板外”）。事件仍**穿透**给下层，
  // 于是“点空白处”既关了菜单、又落到该落的地方。
  // 菜单是**瞬态浮层**：面板外点击=“我要它消失”，且不应顺手点到下层
  //（“想关菜单却触发了背后的按钮”是真实误操作）——切到屏障形态。
  if (auto* overlay_host = dynamic_cast<OverlayHost*>(host); overlay_host != nullptr) {
    overlay_host->set_outside_barrier(true);
    overlay_host->on_outside_click = [&bar]() { bar.close_panel(); };
  }
  bar.set_open_index(index);
}

auto list(Composer& c, const std::vector<ListItemData>& items,
          std::function<void(std::size_t)> on_click, const BoxProps& props) -> Element& {
  Element* element = c.create_element("List", st::Json::object(), props.key);
  if (element == nullptr) {
    fail_missing_element_factory("List");
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
    fail_missing_element_factory("Select");
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
    fail_missing_element_factory("Table");
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
    fail_missing_element_factory("Tree");
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
    fail_missing_element_factory("Tabs");
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
    : composer_(std::make_unique<Composer>(root, guardrails)) {
  // **自登记**：本树每帧由 `UiRoot::tick_declarative_hosts()` 统一推进。
  // 为何不让调用方自己记得：一个页面可以占多处树位（`mount_into` 挂进标题栏的
  // 附属槽），进程里于是有多棵树；漏推第二棵时“点击命中了、状态也变了、
  // 面板就是不出现”（gbcode 菜单与标题栏合并时实测）。
  //
  // 以**弱引用**持有宿主回调（`weak_from_this` 风格）：`DeclarativeHost` 常由
  // `unique_ptr` 持有，析构时不保证还有机会清表——所以回调里先确认
  // “本对象还在”。用 `shared_ptr<atomic<bool>>` 做存活旗标最省事且无需侵入所有权。
  auto alive = std::make_shared<std::atomic<bool>>(true);
  alive_ = alive;
  root.register_declarative_host([this, alive] {
    if (!alive->load()) return false;
    // **先泵异步再判脏**（顺序不能反）：异步结果到达时状态还没写（scope 不脏），
    // 只在 dirty 时才推进的话异步结果永远落不了地（gallery 实测：“任务永远停在计算中”）。
    (void)composer_->pump_async();
    if (!dirty()) return false;
    last_ = tick();
    return true;
  });
}

DeclarativeHost::~DeclarativeHost() {
  // 先落存活旗标（表里的回调从此直接返回），再析构——回调捕获了 `this`，
  // 反过来（先析构再落旗标）会让下一帧的推进入栈一个已死对象。
  if (alive_ != nullptr) alive_->store(false);
}

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
  impl_->keyed_duplicates.clear();
  impl_->claimed_keyed.clear();
  impl_->last_stats.key_collisions.clear();
  std::unordered_set<std::string> seen;
  for (const std::string& key : keys) {
    if (key.empty()) continue;
    impl_->keyed_region.insert(key);
    // 重名 → 列入旁路集（并只在诊断里报一次）
    if (!seen.insert(key).second) {
      if (impl_->keyed_duplicates.insert(key).second) {
        impl_->last_stats.key_collisions.push_back(key);
      }
    }
  }
  // 本区段游标从当前子位数起（区段前的兄弟已经声明过了）——区段结束前不动它。
  impl_->keyed_region_active = true;
}

void Composer::end_keyed_region() {
  impl_->keyed_region.clear();
  impl_->keyed_duplicates.clear();
  impl_->claimed_keyed.clear();
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
