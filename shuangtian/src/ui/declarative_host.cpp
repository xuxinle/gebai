#include "st/ui/declarative_host.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "battery/embed.hpp"
#include "st/core/log.hpp"
#include "st/ext/json.hpp"   // 内联脚本的宿主函数要读写 Json（完整类型）
#include "st/ui/actions.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/script_host.hpp"

namespace st::ui {

namespace {

/// 声明式运行时前置（`src/ui/declarative.js`，编译期嵌入——与 script_api.js 同机制）。
[[nodiscard]] auto declarative_source() -> std::string_view {
  const auto file = b::embed<"src/ui/declarative.js">();
  return std::string_view(file.data(), file.length());
}

}  // namespace

struct DeclarativeHost::Impl {
  ScriptHost* script{nullptr};
  UiRoot* root{nullptr};
  /// 声明式根元素的宿主（子树形态）：非空时根挂到它的子位；空则挂 `UiRoot::content()`。
  Element* host_element{nullptr};
  bool ready{false};
  /// 待挂载元素（`__d_create` 创建后、`__d_set_root`/`__d_mount` 接管前的临时所有权）。
  /// JS 侧两步之间还会设置属性（pending 变更集按 id 引用），因此不能创建即挂树。
  std::vector<std::unique_ptr<Element>> pending{};
  /// 临时 id 序号（`__d_create` 到挂树之间）——单调递增，无指针参与。
  std::uint64_t next_temp_id{1};

  [[nodiscard]] auto take(const std::string& id) -> std::unique_ptr<Element> {
    for (auto& owned : pending) {
      if (owned != nullptr && owned->derived_id() == id) {
        auto result = std::move(owned);
        return result;
      }
    }
    return nullptr;
  }
};

DeclarativeHost::DeclarativeHost() = default;
DeclarativeHost::~DeclarativeHost() = default;
DeclarativeHost::DeclarativeHost(DeclarativeHost&&) noexcept = default;
auto DeclarativeHost::operator=(DeclarativeHost&&) noexcept -> DeclarativeHost& = default;

auto DeclarativeHost::attach(ScriptHost& script, UiRoot& root, Element* host_element)
    -> std::unique_ptr<DeclarativeHost> {
  if (!script.valid()) {
    log::error("DeclarativeHost：脚本宿主未就绪（enable_script 打开了吗？）");
    return nullptr;
  }
  // 私有构造 + make_unique：需要显式把构造开放给本函数（成员函数内可见，故直接 make_unique）
  auto host = std::unique_ptr<DeclarativeHost>(new DeclarativeHost());  // lint-allow: L1 私有构造无法用 make_unique（access）
  host->impl_ = std::make_unique<Impl>();
  host->impl_->script = &script;
  host->impl_->root = &root;
  host->impl_->host_element = host_element;

  auto* impl = host->impl_.get();
  UiRoot* root_ptr = &root;

  // —— 窄桥：树操作（唯一跨边界的树语义；属性写入走既有 pending 管线）——
  // 桥函数名以 `__d_` 开头（内建扩展层命名段，不与脚本 API 冲突）。

  // __d_create(type)：元素工厂 → 返回新元素临时 id（未挂树；属性可先经 pending 队列
  // 排队——按 id 引用；随后 __d_set_root/__d_mount 接管所有权并返回正式 id）。
  if (auto status = script.register_function(
          "__d_create",
          [impl](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.empty() || !args[0].is_string()) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_create 需要类型名");
            }
            const std::string type = st::json_as_string(args[0], "");
            auto created = dsl::make_element(type);
            if (created == nullptr) {
              return st::unexpected(st::ErrorCode::Unsupported, "未知组件类型: " + type);
            }
            Element* element = created.get();
            // 临时 id：挂树前可被 pending 变更集引用（$('#__d0x...').set({...})）
            // 临时 id：单调序号（不用指针值——不要为了造 id 去 reinterpret 指针）
            const std::string temp_id = std::format("__d{}", impl->next_temp_id++);
            element->set_id(temp_id);
            impl->pending.push_back(std::move(created));
            return st::Json(temp_id);
          });
      !status) {
    log::error("DeclarativeHost：__d_create 注册失败: {}", status.error().message);
    return nullptr;
  }

  // __d_set_root(id)：设为根内容（接管所有权；返回正式 id——挂树后按路径生成 "root"）。
  if (auto status = script.register_function(
          "__d_set_root", [impl, root_ptr](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.empty() || !args[0].is_string()) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_set_root 需要 id");
            }
            auto owned = impl->take(st::json_as_string(args[0], ""));
            if (owned == nullptr) {
              return st::unexpected(st::ErrorCode::NotFound, "__d_set_root：元素不在待挂载列表");
            }
            Element* element = owned.get();
            if (impl->host_element != nullptr) {
              // 子树形态：根挂到宿主元素的**子位**（首次挂载；后续帧由 JS 侧复用 diff，
              // 不再走 set_root——与 C++ dsl 的单根语义一致）。
              Element* mounted = impl->host_element->insert_child(0, std::move(owned));
              return st::Json(mounted->derived_id());
            }
            root_ptr->set_content(std::move(owned));
            return st::Json(element->derived_id());
          });
      !status) {
    return nullptr;
  }

  // __d_mount(parent_id, index, id)：插入子元素（接管所有权；返回正式路径 id）。
  if (auto status = script.register_function(
          "__d_mount", [impl, root_ptr](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.size() < 3) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_mount 需要 (parent, index, id)");
            }
            const std::string parent_id = st::json_as_string(args[0], "");
            std::ptrdiff_t index = -1;
            if (args[1].is_number()) index = static_cast<std::ptrdiff_t>(st::json_as_i64(args[1]));
            if (parent_id.empty()) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_mount：父 id 为空");
            }
            Element* parent = root_ptr->find(parent_id);
            if (parent == nullptr) {
              return st::unexpected(st::ErrorCode::NotFound, "__d_mount：父元素不存在 " + parent_id);
            }
            auto owned = impl->take(st::json_as_string(args[2], ""));
            if (owned == nullptr) {
              return st::unexpected(st::ErrorCode::NotFound, "__d_mount：元素不在待挂载列表");
            }
            const std::size_t at = index < 0
                                       ? parent->child_count()
                                       : std::min(static_cast<std::size_t>(index),
                                                  parent->child_count());
            Element* mounted = parent->insert_child(at, std::move(owned));
            return st::Json(mounted->derived_id());
          });
      !status) {
    return nullptr;
  }

  // __d_unmount(id)：从树上摘除并释放（根元素则清根）。
  if (auto status = script.register_function(
          "__d_unmount", [root_ptr](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.empty() || !args[0].is_string()) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_unmount 需要 id");
            }
            Element* element = root_ptr->find(st::json_as_string(args[0], ""));
            if (element == nullptr) return st::Json(false);
            if (element->parent() == nullptr) {
              root_ptr->set_content(nullptr);
            } else {
              auto removed = element->parent()->remove_child(element);
              (void)removed;
            }
            root_ptr->mark_dirty_all();
            return st::Json(true);
          });
      !status) {
    return nullptr;
  }

  // __d_set_direction(id, 'row'|'column')：Row/Column 布局语义落到 Panel。
  if (auto status = script.register_function(
          "__d_set_direction",
          [root_ptr](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.size() < 2) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_set_direction 需要 (id, dir)");
            }
            Element* element = root_ptr->find(st::json_as_string(args[0], ""));
            if (element == nullptr) {
              return st::unexpected(st::ErrorCode::NotFound, "元素不存在");
            }
            element->style().direction =
                st::json_as_string(args[1], "") == "row" ? FlexDirection::Row : FlexDirection::Column;
            element->mark_layout_dirty();
            return st::Json(true);
          });
      !status) {
    return nullptr;
  }

  // __d_apply_batch([{id, props}...])：重组末尾的**属性批量落地**（一次跨界）。
  // 走 ui::apply_properties（与协议 set/脚本 set 同一份语义）；未命中元素跳过
  // （可能本帧刚被裁剪——下一帧重建）并计入返回值供诊断。
  if (auto status = script.register_function(
          "__d_apply_batch", [root_ptr](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.empty() || !args[0].is_array()) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_apply_batch 需要数组");
            }
            std::size_t applied_count = 0;
            for (const auto& batch : args[0]) {
              const std::string id = st::json_get_string(batch, "id");
              if (id.empty()) continue;
              Element* element = root_ptr->find(id);
              if (element == nullptr) continue;
              if (const st::Json* properties = st::json_find(batch, "props");
                properties != nullptr && properties->is_object()) {
                const auto applied = apply_properties(*root_ptr, *element, *properties);
                applied_count += applied.size();
              }
            }
            root_ptr->mark_dirty_all();
            return st::Json(applied_count);
          });
      !status) {
    return nullptr;
  }

  // __d_slot_root()：子树形态下，宿主槽位已有根元素的 id（无则空串）——
  // JS 侧据此采纳已有根进 VDOM，不重复创建。
  if (auto status = script.register_function(
          "__d_slot_root", [impl](const std::vector<st::Json>&) -> Result<st::Json> {
            if (impl->host_element == nullptr || impl->host_element->child_count() == 0) {
              return st::Json(std::string());
            }
            return st::Json(impl->host_element->child_at(0)->derived_id());
          });
      !status) {
    return nullptr;
  }

  // __d_clear_slot()：清掉子树形态的宿主槽位（`__d_unmount` 在根元素非本层创建时用——
  // 单根形态的卸载走 `__d_unmount(root_id)`，子树形态得知道“槽”是谁）。
  if (auto status = script.register_function(
          "__d_clear_slot", [impl](const std::vector<st::Json>&) -> Result<st::Json> {
            if (impl->host_element == nullptr || impl->host_element->child_count() == 0) {
              return st::Json(false);
            }
            auto removed = impl->host_element->remove_child(impl->host_element->child_at(0));
            (void)removed;
            return st::Json(true);
          });
      !status) {
    return nullptr;
  }

  // __d_move(id, parent_id, index)：重排子元素（key 对齐复用的配套——复用元素
  // 保持旧树位置，key 重排后需按新序落位）。已在位时无操作（幂等）。
  if (auto status = script.register_function(
          "__d_move",
          [root_ptr](const std::vector<st::Json>& args) -> Result<st::Json> {
            if (args.size() < 3) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_move 需要 (id, parent, index)");
            }
            const std::string id = st::json_as_string(args[0], "");
            const std::string parent_id = st::json_as_string(args[1], "");
            const auto index = static_cast<std::ptrdiff_t>(st::json_as_i64(args[2]));
            if (id.empty() || parent_id.empty()) {
              return st::unexpected(st::ErrorCode::Invalid, "__d_move：id/parent 为空");
            }
            Element* element = root_ptr->find(id);
            Element* parent = root_ptr->find(parent_id);
            if (element == nullptr || parent == nullptr || element->parent() != parent) {
              return st::Json(false);  // 不在目标父下（已被替换/裁剪）：跳过
            }
            // 当前位置：找索引
            std::ptrdiff_t current = -1;
            for (std::size_t position = 0; position < parent->child_count(); ++position) {
              if (parent->child_at(position) == element) {
                current = static_cast<std::ptrdiff_t>(position);
                break;
              }
            }
            if (current < 0 || current == index) return st::Json(false);  // 已在位
            // 摘除 + 重新插入（插到目标位置）
            auto detached = parent->remove_child(element);
            if (detached == nullptr) return st::Json(false);
            const std::size_t at = index < 0 ? parent->child_count()
                                             : std::min(static_cast<std::size_t>(index),
                                                        parent->child_count());
            parent->insert_child(at, std::move(detached));
            parent->mark_layout_dirty();
            return st::Json(true);
          });
      !status) {
    return nullptr;
  }

  // 注入 declarative.js 前置（在 script_api 之后：它依赖 on/off/$ 等基础 API）。
  auto evaluated = script.eval_prelude(declarative_source(), "<declarative>");
  if (!evaluated) {
    log::error("DeclarativeHost：declarative.js 加载失败: {}", evaluated.error().message);
    return nullptr;
  }
  impl->ready = true;
  return host;
}

auto DeclarativeHost::run(std::string_view code, std::string_view filename) -> Status {
  if (impl_ == nullptr || !impl_->ready) {
    return st::unexpected(st::ErrorCode::Unsupported, "宿主未就绪");
  }
  auto outcome = impl_->script->eval(code, filename);
  if (!outcome) return st::unexpected(outcome.error().code, outcome.error().message);
  return st::ok();
}

auto DeclarativeHost::tick() -> bool {
  if (impl_ == nullptr || !impl_->ready) return false;
  // 先泵异步微任务（useResource 的结果落地会标脏）——再重组，同帧可见。
  (void)impl_->script->pump_jobs();
  auto outcome = impl_->script->call("__d_reconcile", {});
  if (!outcome) return false;
  bool rerun = st::json_get_i64(*outcome, "rerun", 0) > 0;

  // 重组中登记的 effect（useEffect）：**重组之后**执行——副作用里写状态是连锁写，
  // 隔帧生效；内联执行会被同帧的 dirty 清理吞掉（界面停在旧值）。
  // 返回 1 = effect 里写了状态（`dirty` 为真）⇒ 需要再重组一帧；
  // 返回 0 = 只是跑了副作用而没改界面 ⇒ 不多请求一帧。
  if (auto effects = impl_->script->call("__d_run_effects", {}); effects.has_value()) {
    if (st::json_as_i64(*effects, 0) > 0) rerun = true;
  }

  // 重组中新起的异步（如 useResource 因输入变化重发请求）→ 同帧再泵**一轮**并重组。
  // 只做一轮：防止"请求→重组→再请求"的链子把一帧拖成无限循环；没跑完的留给下一帧。
  if (impl_->script->pump_jobs() > 0) {
    if (auto second = impl_->script->call("__d_reconcile", {}); second.has_value()) {
      rerun = rerun || st::json_get_i64(*second, "rerun", 0) > 0;
    }
  }
  if (rerun) impl_->script->request_repaint();
  return rerun;
}

auto DeclarativeHost::unmount_declarative() -> bool {
  if (impl_ == nullptr || !impl_->ready) return false;
  auto outcome = impl_->script->call("__d_dispose", {});
  return outcome.has_value() && st::json_as_bool(*outcome);
}

auto DeclarativeHost::pump_jobs() -> std::size_t {
  if (impl_ == nullptr || !impl_->ready) return 0;
  return impl_->script->pump_jobs();
}

auto DeclarativeHost::stats() const -> st::Json {
  if (impl_ == nullptr) return st::Json();
  auto outcome = impl_->script->call("__d_stats", {});
  return outcome.has_value() ? *outcome : st::Json();
}

}  // namespace st::ui
