#include "st/ui/script_host.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "battery/embed.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/ui/actions.hpp"

#include "st/ext/json.hpp"

namespace st::ui {
namespace {

/// 脚本运行时前置（`src/ui/script_api.js`，编译期嵌入）。
///
/// 用 embed 而不是 C++ 原始字符串：这段 JS 有 200 行，写成原始字符串就不能被编辑器高亮、
/// 且每处转义都要小心；作为真实 `.js` 文件维护，构建时嵌进库。
[[nodiscard]] auto prelude_source() -> std::string_view {
  const auto file = b::embed<"src/ui/script_api.js">();
  return std::string_view(file.data(), file.length());
}

/// UI 事件种类 → 脚本侧事件名（与 DOM 命名习惯对齐，便于 AI 迁移既有知识）。
[[nodiscard]] auto event_name(EventKind kind) -> std::string_view {
  switch (kind) {
    case EventKind::Click: return "click";
    case EventKind::DoubleClick: return "dblclick";
    case EventKind::TripleClick: return "tripleclick";
    case EventKind::MouseDown: return "mousedown";
    case EventKind::MouseUp: return "mouseup";
    case EventKind::MouseMove: return "mousemove";
    case EventKind::Wheel: return "wheel";
    case EventKind::KeyDown: return "keydown";
    case EventKind::KeyUp: return "keyup";
    case EventKind::TextInput: return "input";
    case EventKind::FocusIn: return "focus";
    case EventKind::FocusOut: return "blur";
    case EventKind::HoverIn: return "hover";
    case EventKind::HoverOut: return "unhover";
  }
  return "unknown";
}

/// 高频事件：只在这些事件被**显式监听**时才桥接，避免鼠标移动把脚本打爆。
[[nodiscard]] auto is_high_frequency(std::string_view name) noexcept -> bool {
  return name == "mousemove" || name == "hover" || name == "unhover" || name == "wheel";
}

/// 事件名是不是框架真的会分发的（与 `event_name` 一一对应）。
///
/// 为何要校验：绑一个**不存在**的事件名（`'change'`、拼错的 `'keydown'`）现在会
/// **静默永不触发**——`on` 只是把“选择器 + 事件名”存起来，宿主按事件名匹配。
/// 于是现象是“点了没反应”，而排查得一路查到事件表（实测踩过：
/// 勾选框绑 `'change'`，而框架只有 `click`，表现为计数永远不动）。
[[nodiscard]] auto is_known_event(std::string_view name) noexcept -> bool {
  return name == "click" || name == "dblclick" || name == "tripleclick" ||
         name == "mousedown" || name == "mouseup" || name == "mousemove" ||
         name == "wheel" || name == "keydown" || name == "keyup" || name == "input" ||
         name == "focus" || name == "blur" || name == "hover" || name == "unhover";
}

/// 事件 → JSON（脚本处理器拿到的 `event` 参数）。
[[nodiscard]] auto event_to_json(const Event& event, Element& target) -> st::Json {
  st::Json value = st::Json::object();
  value["type"] = std::string(event_name(event.kind));
  value["id"] = target.derived_id();
  st::Json position = st::Json::object();
  position["x"] = static_cast<double>(event.position.x);
  position["y"] = static_cast<double>(event.position.y);
  value["position"] = std::move(position);
  value["button"] = event.button;
  value["clicks"] = event.click_count;
  value["wheel"] = static_cast<double>(event.wheel_delta);
  if (!event.key.empty()) value["key"] = event.key;
  if (!event.text.empty()) value["text"] = event.text;
  st::Json modifiers = st::Json::object();
  modifiers["ctrl"] = event.ctrl;
  modifiers["shift"] = event.shift;
  modifiers["alt"] = event.alt;
  modifiers["meta"] = event.meta;
  value["modifiers"] = std::move(modifiers);
  return value;
}

/// 当前单调时钟毫秒（脚本的 `now()` 与定时器都基于它）。
[[nodiscard]] auto now_ms() -> double {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now).count()) /
         1000.0;
}

[[nodiscard]] auto to_string_view(const st::Json& value) -> std::string {
  return value.is_string() ? st::json_as_string(value) : st::json_dump(value);
}

/// 脚本侧状态同步到宿主（`state` 对象是脚本的，宿主只读快照）。
/// 读回时机：`set_state` 之后与每次阶段末，保证 `state()` 返回最新值。
}  // namespace

struct ScriptHost::Impl {
  UiRoot* root{nullptr};
  std::unique_ptr<ext::ScriptEngine> engine{};
  std::vector<ScriptBinding> bindings{};
  std::size_t next_binding_id{1};
  std::size_t commit_count{0};
  std::size_t committed_properties{0};
  bool prelude_ready{false};

  [[nodiscard]] auto find_binding(std::string_view id) const -> const ScriptBinding* {
    for (const auto& binding : bindings) {
      if (binding.id == id) return &binding;
    }
    return nullptr;
  }

  /// 构建界面快照（一次跨边界交给 JS）。
  ///
  /// 只放**有 id 或类型**的节点：无 id 又无类型的匿名容器对脚本没有操作价值，
  /// 放进去只会让快照膨胀（快照大小直接决定跨边界成本）。
  [[nodiscard]] auto snapshot() -> st::Json {
    st::Json nodes = st::Json::object();
    st::Json order = st::Json::array();
    // 走语义树拿"全部可见节点"，再补父链以便子选择器查询
    const auto collect = [&](auto&& self, Element& element) -> void {
      const std::string id = element.derived_id();
      st::Json node = element_snapshot(element);
      node["parent"] = element.parent() != nullptr ? element.parent()->derived_id() : std::string();
      const auto* existing = st::json_find(nodes, id);
      if (existing == nullptr) {
        order.push_back(id);
      }
      nodes[id] = std::move(node);
      for (std::size_t index = 0; index < element.children().size(); ++index) {
        self(self, *element.child_at(index));
      }
    };
    if (root->content() != nullptr) collect(collect, *root->content());
    st::Json result = st::Json::object();
    result["nodes"] = std::move(nodes);
    result["order"] = std::move(order);
    return result;
  }

  /// 取回并应用 JS 排队的变更集（一次跨边界取回全部）。
  ///
  /// ⚠ **不调 `mark_dirty_all`**（2026-10-15 收敛）：逐元素路径已自带精确标脏——
  /// `apply_properties` 对每个真改到的属性调 `element.mark_dirty()`（元素级
  /// damage 上报）与 `apply_layout_property` 尾部的 `mark_layout_dirty()`；
  /// `invoke_element` 的 focus/blur 走 `root.set_focus`（自带标脏与
  /// `note_changed`）。末尾再整帧标脏会把这套收敛摊回「每次 JS 写入都整帧重绘」
  /// ——控制通道 `set`（server.cpp）同语义路径从没有这个尾巴。回归：
  /// `script_write_takes_partial_damage_path`（ui_script_host_test）。
  auto commit_changes() -> void {
    auto changes = engine->call("__take_changes", {});
    if (!changes || !changes->is_array()) return;
    if (changes->empty()) return;
    ++commit_count;
    for (const st::Json& change : *changes) {
      const std::string id = st::json_get_string(change, "id");
      if (id.empty()) continue;
      Element* element = root->find(id);
      if (element == nullptr) continue;  // 元素在这一阶段被移除了：静默跳过
      if (const st::Json* properties = st::json_find(change, "props"); properties != nullptr) {
        const st::Json changed = apply_properties(*root, *element, *properties);
        committed_properties += changed.size();
      }
      const std::string action = st::json_get_string(change, "action");
      if (!action.empty()) {
        (void)invoke_element(*root, *element, action, st::json_get_string(change, "argument"));
      }
    }
  }

  /// 一次脚本入口的标准流程：失效快照 → 执行 → 提交变更。
  auto run_entry(const std::function<Result<st::Json>()>& body) -> Result<st::Json> {
    (void)engine->eval("__invalidate && __invalidate()", "<host>");
    auto outcome = body();
    commit_changes();
    return outcome;
  }
};

ScriptHost::ScriptHost(UiRoot& root, ext::ScriptLimits limits)
    : impl_(std::make_unique<Impl>()) {
  impl_->root = &root;
  impl_->engine = std::make_unique<ext::ScriptEngine>(limits);
  if (!impl_->engine->valid()) return;

  // **自接事件观察者**：宿主自己接上，而不是要求调用方记得接。
  // 理由：忘记接线不会报错，只会表现为"脚本绑了事件却永远不响"——最难查的一类问题
  // （写测试时就踩到了：Fixture 建了 ScriptHost 却忘了接观察者，事件全部静默丢失）。
  root.set_event_observer([this](const Event& event, Element& target) {
    (void)dispatch_event(event, target);
  });

  // 宿主桥：JS 侧通过这些函数与 C++ 交互（全部是"批量"或"记账"型，不是逐属性往返）
  auto status = impl_->engine->register_function(
      "__snapshot", [this](const std::vector<st::Json>&) -> Result<st::Json> {
        return impl_->snapshot();
      });
  if (!status) return;
  status = impl_->engine->register_function(
      "__register", [this](const std::vector<st::Json>& args) -> Result<st::Json> {
        if (args.size() < 3) return st::unexpected(st::ErrorCode::Invalid, "__register 需要三个参数");
        const std::string selector = to_string_view(args[0]);
        const std::string event = to_string_view(args[1]);
        const std::string handler = to_string_view(args[2]);
        // 未知事件名**直接报错**（而不是默默绑上）：否则现象是“点了没反应”，
        // 而根因（事件名不存在）要一路查到事件表才能确认。
        if (!is_known_event(event)) {
          return st::unexpected(
              st::ErrorCode::Invalid,
              std::format("未知事件名 '{}'（可用：click/dblclick/mousedown/mouseup/mousemove/"
                          "wheel/keydown/keyup/input/focus/blur/hover/unhover）",
                          event));
        }
        ScriptBinding binding;
        binding.id = std::format("b{}", impl_->next_binding_id++);
        binding.selector = selector;
        binding.event = event;
        binding.handler = handler.size() > 120 ? handler.substr(0, 120) + "…" : handler;
        // 选择器**在绑定时就解析一次**并缓存（dispatch 每帧要用，不能每次都 parse）。
        // 解析失败不阻止绑定（保留原行为：非法选择器只是永不命中），
        // 但缓存呈 nullptr 以免每次 dispatch 重试。
        if (auto parsed = Selector::parse(binding.selector); parsed) {
          binding.parsed = std::make_shared<const Selector>(std::move(*parsed));
        }
        impl_->bindings.push_back(std::move(binding));
        // 注册时回收死绑定：重建式界面“先清元素再重建 + 重绑”，
        // 在**增长点**清理比等事件触发更及时（事件可能很久才来一次）。
        //
        // 为何不做阈值门控：实测重建场景下总能项就十几到几十条，
        // 任何阈值都可能“刚好不触发”；而重建时每条注册本就意味着一次结构变化，
        // 同量级的全表扫（只查 `find`）代价可以忽略。
        //
        // 这是“对象生命周期”问题：元素被销毁后绑定必须跟着走，
        // 不能依赖调用方记得 `off()`（那正是实测崩溃的来源）。
        if (impl_->root != nullptr) {
          UiRoot& tree = *impl_->root;   // 避开外层形参名 `root`（-Wshadow 会报错）
          std::erase_if(impl_->bindings, [&tree](const ScriptBinding& b) {
            if (b.parsed == nullptr) return true;
            const std::string id = b.parsed->primary_id();
            if (id.empty()) return false;   // 非 #id 选择器：不能当作失效
            return tree.find(id) == nullptr;
          });
        }
        return st::Json(impl_->bindings.back().id);
      });
  if (!status) return;
  status = impl_->engine->register_function(
      "__unregister", [this](const std::vector<st::Json>& args) -> Result<st::Json> {
        if (args.empty()) return st::Json(false);
        const std::string id = to_string_view(args[0]);
        const auto before = impl_->bindings.size();
        std::erase_if(impl_->bindings, [&id](const ScriptBinding& b) { return b.id == id; });
        return st::Json(impl_->bindings.size() != before);
      });
  if (!status) return;
  status = impl_->engine->register_function(
      "__log", [](const std::vector<st::Json>& args) -> Result<st::Json> {
        std::string line = "[脚本]";
        for (const auto& argument : args) {
          line.push_back(' ');
          line.append(argument.is_string() ? st::json_as_string(argument) : st::json_dump(argument));
        }
        ST_LOG_INFO("{}", line);
        return st::Json();
      });
  if (!status) return;
  status = impl_->engine->register_function("__now_ms", [](const std::vector<st::Json>&) -> Result<st::Json> {
    return st::Json(now_ms());
  });
  if (!status) return;

  // 注入前置
  auto evaluated = impl_->engine->eval(prelude_source(), "<script_api>");
  if (!evaluated) {
    ST_LOG_ERROR("脚本运行时前置加载失败: {}", evaluated.error().message);
    return;
  }
  impl_->prelude_ready = true;
}

ScriptHost::~ScriptHost() {
  // 先摘掉观察者：它捕获 `this`，晚于本对象失效就会形成悬垂回调
  if (impl_ != nullptr && impl_->root != nullptr) impl_->root->set_event_observer(nullptr);
}

auto ScriptHost::valid() const noexcept -> bool {
  return impl_ != nullptr && impl_->engine != nullptr && impl_->engine->valid() && impl_->prelude_ready;
}

auto ScriptHost::eval(std::string_view code, std::string_view filename) -> Result<st::Json> {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  return impl_->run_entry([this, code, filename]() {
    return impl_->engine->eval(code, filename);
  });
}

auto ScriptHost::call(std::string_view function, const std::vector<st::Json>& arguments)
    -> Result<st::Json> {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  return impl_->run_entry([this, function, &arguments]() {
    return impl_->engine->call(function, arguments);
  });
}

auto ScriptHost::check_syntax(std::string_view source) -> Status {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  return impl_->engine->check_syntax(source);
}

auto ScriptHost::bind(std::string_view selector, std::string_view event, std::string_view handler)
    -> Result<std::string> {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  if (selector.empty()) return st::unexpected(st::ErrorCode::Invalid, "选择器不能为空");
  if (event.empty()) return st::unexpected(st::ErrorCode::Invalid, "事件名不能为空");
  if (handler.empty()) return st::unexpected(st::ErrorCode::Invalid, "处理器不能为空");

  // 用 `on()` 注册：处理器函数留在 JS 侧（宿主只记"选择器 + 事件 + id"）
  const std::string code =
      std::format("on({}, {}, {})", st::json_dump(st::Json(std::string(selector))),
                  st::json_dump(st::Json(std::string(event))),
                  std::string(handler));
  auto outcome = eval(code, "<bind>");
  if (!outcome) return st::unexpected(outcome.error().code, outcome.error().message);
  if (!outcome->is_string()) {
    return st::unexpected(st::ErrorCode::Parse, "事件绑定未返回 id（处理器可能不是函数）");
  }
  return st::json_as_string(*outcome);
}

auto ScriptHost::unbind(std::string_view id) -> bool {
  if (!valid() || id.empty()) return false;
  const std::string code =
      std::format("off({})", st::json_dump(st::Json(std::string(id))));
  const auto outcome = eval(code, "<unbind>");
  return outcome.has_value() && st::json_as_bool(*outcome);
}

void ScriptHost::clear_bindings() {
  if (!valid()) return;
  for (const auto& binding : impl_->bindings) {
    (void)unbind(binding.id);
  }
  impl_->bindings.clear();
}

auto ScriptHost::bindings() const -> std::vector<ScriptBinding> { return impl_->bindings; }

auto ScriptHost::state() -> Result<st::Json> {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  return impl_->engine->eval("state");
}

auto ScriptHost::set_state(const st::Json& patch) -> Status {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  if (!patch.is_object()) return st::unexpected(st::ErrorCode::Invalid, "state 补丁必须是对象");
  for (const auto& [key, value] : patch.items()) {
    const std::string code = std::format("state[{}] = {}", st::json_dump(st::Json(key)),
                                         st::json_dump(value));
    auto outcome = impl_->engine->eval(code, "<state>");
    if (!outcome) return st::unexpected(outcome.error().code, outcome.error().message);
  }
  return st::ok();
}

auto ScriptHost::dispatch_event(const Event& event, Element& target) -> bool {
  if (!valid() || impl_->bindings.empty()) return false;
  const std::string name(event_name(event.kind));
  // 只处理"有绑定的元素 × 事件类型"：先从绑定的选择器反查，避免为无关事件白跑一次脚本
  const std::string target_id = target.derived_id();
  std::vector<std::string> matching;
  for (const auto& binding : impl_->bindings) {
    if (binding.event != name) continue;
    // 用绑定时缓存的选择器（未缓存 = 当初解析失败 → 这条绑定永远不会命中）
    if (binding.parsed == nullptr) continue;
    if (binding.parsed->matches(target)) matching.push_back(binding.id);
  }

  // 惰性回收死绑定（两个时机，缺一不可）：
  //
  // ① 本次**未命中任何绑定** → 很可能在点一个刚重建出来的新元素，
  //    顺手把旧元素遗留的绑定清掉；
  // ② 绑定表**超过阈值** → 即便每次都命中，也要不时清理。
  //    只做①不够：重建后新元素总是命中自己那条，于是永远走不到①，
  //    而死绑定会一直累积（实测：20 轮重建后 80 条，进程最终 `std::bad_alloc`）。
  //
  // 阈值取 32：实测 20 轮重建（每轮 4 条）总量在 16~80 之间波动，
  // 阈值取 64 会让中小规模重建**根本不触发**清理（诊断实测：死绑定 12 条 / 总数 16 条）。
  // 32 是“正常界面（手绑几十条）几乎不触发、重建式界面很快触发”的折中。
  constexpr std::size_t kPruneThreshold = 32;
  if (matching.empty() || impl_->bindings.size() > kPruneThreshold) {
    (void)prune_dead_bindings();
  }
  if (matching.empty()) return false;
  // 高频事件合并：同一帧内多次触发只跑最后一次（脚本对鼠标移动通常只关心终态）
  if (is_high_frequency(name) && event.kind == EventKind::MouseMove) {
    pending_high_frequency_ = std::make_pair(matching.front(), target_id);
    return true;
  }
  return dispatch_handlers(matching, event, target);
}

auto ScriptHost::prune_dead_bindings() -> std::size_t {
  if (!valid() || impl_->root == nullptr) return 0;
  UiRoot& root = *impl_->root;   // 捕获引用：lambda 里不能直接用 impl_->root
  const auto before = impl_->bindings.size();
  std::erase_if(impl_->bindings, [&root](const ScriptBinding& binding) {
    // 当初就解析失败的绑定：永远不可能命中，直接回收
    if (binding.parsed == nullptr) return true;
    const std::string id = binding.parsed->primary_id();
    // 非「单 #id」选择器：不动（重建后可能又有同类元素）
    if (id.empty()) return false;
    // 元素已不在树上 → 该绑定永不命中
    return root.find(id) == nullptr;
  });
  return before - impl_->bindings.size();
}

auto ScriptHost::dispatch_handlers(const std::vector<std::string>& binding_ids, const Event& event,
                                   Element& target) -> bool {  st::Json payload = event_to_json(event, target);
  bool invoked = false;
  for (const auto& id : binding_ids) {
    auto outcome = impl_->run_entry([this, &id, &payload]() {
      return impl_->engine->call("__dispatch", {st::Json(id), payload});
    });
    if (!outcome) {
      ST_LOG_WARN("脚本事件处理器失败（绑定 {}）: {}", id, outcome.error().message);
      continue;
    }
    invoked = invoked || st::json_as_bool(*outcome);
  }
  return invoked;
}

auto ScriptHost::tick(double /*now_seconds*/) -> std::size_t {
  if (!valid()) return 0;
  // 高频事件合并的补发：本帧若发生过鼠标移动，这里一次性派发
  if (pending_high_frequency_.has_value()) {
    const auto [binding_id, target_id] = *pending_high_frequency_;
    pending_high_frequency_.reset();
    if (Element* target = impl_->root->find(target_id); target != nullptr) {
      Event merged;
      merged.kind = EventKind::MouseMove;
      (void)dispatch_handlers({binding_id}, merged, *target);
    }
  }
  const double now = now_ms();
  auto fired = impl_->run_entry([this, now]() {
    return impl_->engine->call("__run_timers", {st::Json(now)});
  });
  if (!fired) return 0;
  return static_cast<std::size_t>(st::json_as_i64(*fired, 0));
}

auto ScriptHost::last_stats() const noexcept -> const ext::ScriptStats& {
  static const ext::ScriptStats empty{};
  return impl_ != nullptr && impl_->engine != nullptr ? impl_->engine->last_stats() : empty;
}

auto ScriptHost::limits() const noexcept -> const ext::ScriptLimits& {
  static const ext::ScriptLimits empty{};
  return impl_ != nullptr && impl_->engine != nullptr ? impl_->engine->limits() : empty;
}

auto ScriptHost::engine_version() const -> std::string {
  return impl_ != nullptr && impl_->engine != nullptr ? std::string(impl_->engine->version())
                                                     : std::string();
}

auto ScriptHost::register_function(std::string name, ext::ScriptHostFunction function)
    -> Status {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  return impl_->engine->register_function(std::move(name), std::move(function));
}

auto ScriptHost::pump_jobs() -> std::size_t {
  if (!valid()) return 0;
  return impl_->engine->pump_jobs();
}

auto ScriptHost::eval_prelude(std::string_view source, std::string_view filename)
    -> Result<st::Json> {
  if (!valid()) return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
  // 前置级：绕过 run_entry（不重建快照、不提交变更集）——基础设施不是页面逻辑
  return impl_->engine->eval(source, filename);
}

auto ScriptHost::commit_count() const noexcept -> std::size_t {
  return impl_ != nullptr ? impl_->commit_count : 0;
}

auto ScriptHost::committed_properties() const noexcept -> std::size_t {
  return impl_ != nullptr ? impl_->committed_properties : 0;
}

}  // namespace st::ui
