#include "st/ui/script_bridge.hpp"

#include <algorithm>
#include <ranges>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/ext/json.hpp"
#include "st/ext/script.hpp"
#include "st/ui/actions.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/selector.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {
namespace {

/// 名字是否合法：非空、不以 `_` 开头、不含连续点、各段非空。
///
/// 为何拒下划线开头：`__` 是**内建扩展层**的命名段（`__d_*` / `__snapshot`…）。
/// 应用桥占用它就会与框架抢名字，而症状是"某个框架桥行为突然变了"——极难定位。
/// 与其等踩到再说，不如在注册这一步就拦住（并给出原因）。
auto valid_name(const std::string& name) -> std::optional<std::string> {
  if (name.empty()) return "名字不能为空";
  if (name.front() == '_') {
    return "名字不能以下划线开头（`__` 是框架内建扩展层的命名段）";
  }
  if (name.contains("..")) return "名字不能含连续的点";
  std::size_t begin = 0;
  while (true) {
    const std::size_t dot = name.find('.', begin);
    const std::size_t end = dot == std::string::npos ? name.size() : dot;
    if (end == begin) return "名字里不能有空段（如 `a..b` 或首尾是点）";
    if (dot == std::string::npos) break;
    begin = dot + 1;
  }
  if (name.back() == '.') return "名字不能以点结尾";
  return std::nullopt;
}

/// JS 标识符安全检查：段名要么是合法标识符，要么是数字（数组下标/对象键）。
///
/// 这里用**字符串拼 JS 源码**而不是逐字符构造对象：段名已经过白名单校验
/// （字母数字下划线），拼出来的源码不可能被注入。这条校验是那个前提。
auto is_identifier(const std::string& text) -> bool {
  if (text.empty()) return false;
  if (std::isdigit(static_cast<unsigned char>(text.front())) != 0) {
    // 数字段：只允许当键用（`obj[0]`），这里一并接受。
    return std::ranges::all_of(text, [](char ch) {
      return std::isdigit(static_cast<unsigned char>(ch)) != 0;
    });
  }
  return std::ranges::all_of(text, [](char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
  });
}

}  // namespace

struct ScriptBridge::Impl {
  ScriptHost* script{nullptr};
  UiRoot* root{nullptr};
  std::vector<std::string> exposed{};
  std::vector<std::string> loaded{};
  /// 最近一次失败原因（`expose`/`load` 的诊断面：只返回 bool 会把原因丢掉）。
  std::string last_error{};
  std::size_t sequence{0};

  /// 把一份脚本"检查 → 清场 → 重放"（热重载的内部件）。
  /// 与 `load` 的差别：不清场时旧闭包会继续被 handlers/timers 表强引用（见 `ui::HotReload`）。
  auto run_source(const std::string& source, const std::string& filename) -> Status {
    if (script == nullptr || !script->valid()) {
      return st::unexpected(st::ErrorCode::Unsupported, "脚本宿主未就绪");
    }
    auto result = script->eval(source, filename);
    if (!result.has_value()) {
      return st::unexpected(result.error().code,
                            std::format("{}：{}", filename, result.error().message));
    }
    return {};
  }
};

ScriptBridge::ScriptBridge(ScriptHost& script, UiRoot* root) : impl_(std::make_unique<Impl>()) {
  impl_->script = &script;
  impl_->root = root;
  if (root == nullptr) return;
  // 元素便利函数：**薄包装** `ui::apply_properties` / `ui::invoke_element`，
  // 不另立一套语义（与协议 `set`/`invoke` 同一份实现）。
  expose("ui.set", [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.size() < 2) return st::unexpected(st::ErrorCode::Invalid, "ui.set 需要 (id, props)");
    const std::string id = st::json_as_string(args[0]);
    Element* element = root->find(id);
    if (element == nullptr) {
      return st::unexpected(st::ErrorCode::NotFound, std::format("找不到元素 {}", id));
    }
    return ui::apply_properties(*root, *element, args[1]);
  });
  expose("ui.invoke", [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.size() < 2) {
      return st::unexpected(st::ErrorCode::Invalid, "ui.invoke 需要 (id, action[, argument])");
    }
    const std::string id = st::json_as_string(args[0]);
    Element* element = root->find(id);
    if (element == nullptr) {
      return st::unexpected(st::ErrorCode::NotFound, std::format("找不到元素 {}", id));
    }
    const std::string action = st::json_as_string(args[1]);
    const std::string argument = args.size() > 2 ? st::json_as_string(args[2]) : std::string{};
    return st::Json(ui::invoke_element(*root, *element, action, argument));
  });
  expose("ui.query", [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "ui.query 需要选择器");
    const auto selector = Selector::parse(st::json_as_string(args[0]));
    if (!selector.has_value()) {
      return st::unexpected(st::ErrorCode::Invalid, "选择器语法错误");
    }
    const std::size_t limit = args.size() > 1 ? static_cast<std::size_t>(args[1].get<double>()) : 0;
    st::Json out = st::Json::array();
    for (Element* element : root->query(*selector, limit)) {
      out.push_back(ui::element_to_json(*element));
    }
    return out;
  });
  expose("ui.exists", [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "ui.exists 需要 id");
    return st::Json(root->find(st::json_as_string(args[0])) != nullptr);
  });
  // —— 动态结构：建 / 插 / 清子元素 ——
  //
  // 为何要有这一组：XML 只能定**静态**结构，而表驱动界面（清单/表格/树）必然要在
  // 运行时按数据增删行。没有它，纯 XML 写的应用只能做静态页面
  // （实测：任务清单示例的“行”无路可走）。组件工厂 `dsl::make_element` 是同一张表，
  // 所以 XML 里能写的类型这里也能建，建出来的元素同类、照样能被选择器寻址。
  auto build_fn = [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "ui.build 需要类型名");
    const std::string requested = st::json_as_string(args[0]);
    // ⚠ **别名也要认**：XML 里能写 `<Row>`/`<Column>`/`<Icon>`，而组件工厂只认注册名
    //（`Panel`/`Icon`）。建成“XML 能写、JS 不能建”会让同一个类型在两种入口下不一致
    //——实测：`ui.build('Row')` 报“没有这个组件类型”（而 XML 里 `<Row>` 明明是合法的）。
    std::string type = requested;
    bool row_direction = false;
    if (requested == "Row" || requested == "Column" || requested == "Toolbar" ||
        requested == "Group") {
      type = "Panel";
      row_direction = requested == "Row";
    } else if (requested == "Separator") {
      type = "Divider";
    } else if (requested == "Progress") {
      type = "ProgressBar";
    }
    auto owned = dsl::make_element(type);
    if (owned == nullptr) {
      return st::unexpected(st::ErrorCode::Unsupported,
                            std::format("没有这个组件类型：{}", requested));
    }
    if (row_direction) {
      if (auto* panel = dynamic_cast<Panel*>(owned.get()); panel != nullptr) {
        panel->style().direction = FlexDirection::Row;
      }
    }
    Element* host = root->content();
    if (host == nullptr) {
      return st::unexpected(st::ErrorCode::Unsupported, "根内容为空，无法建元素");
    }
    // 新元素必须**先有宿主**才能被 `find` 到（选择器只遍历挂在树上的元素）；
    // 先挂到根下、由调用方随后 `ui.append` 挪走（与声明式宿主的 pending 队列同一思路）。
    Element* raw = host->add_child(std::move(owned));
    if (const st::Json props = args.size() > 1 ? args[1] : st::Json::object();
        props.is_object() && !props.empty()) {
      // ⚠ `id` 要**单独抽出来先设**：它是元素元数据（不在 `property_names()` 里），
      // 而 `ui::apply_properties` 只认“元素自己声明的属性面”——忘抽的后果是
      // 传进来的 id 被静默忽略、然后被自动 id 覆盖（`auto-Panel`），
      // 而调用方接下来用自己那个 id 去 `find` 就找不到（实测踩到）。
      if (props.contains("id") && props["id"].is_string()) {
        raw->set_id(st::json_as_string(props["id"]));
      }
      st::Json rest = props;
      rest.erase("id");
      if (!rest.empty()) (void)ui::apply_properties(*root, *raw, rest);
    }
    // ⚠ 补一个**保证唯一**的自动 id：JS 建元素时通常不给 id，而后续
    // `ui.append`/`set` 都按 id 寻址（声明式宿主用的是同一手法：临时 id → 挂树后正式 id）。
    // 先 `find` 验证唯一，撞名就加序号——同名会让后续 `find` 拿到错误元素。
    if (raw->id().empty()) {
      for (int suffix = 1; suffix < 100000; ++suffix) {
        const std::string candidate =
            suffix == 1 ? std::format("auto-{}", raw->type())
                        : std::format("auto-{}-{}", raw->type(), suffix);
        raw->set_id(candidate);
        // `find` 可能拿到自己（那就是没撞名）。
        if (root->find(candidate) == raw) break;
      }
    }
    st::Json out = st::Json::object();
    out["id"] = raw->id();
    out["type"] = std::string(raw->type());
    return out;
  };
  auto append_fn = [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.size() < 2) {
      return st::unexpected(st::ErrorCode::Invalid, "ui.append 需要 (父 id, 子 id)");
    }
    Element* parent = root->find(st::json_as_string(args[0]));
    Element* child = root->find(st::json_as_string(args[1]));
    if (parent == nullptr || child == nullptr) {
      return st::unexpected(st::ErrorCode::NotFound, "ui.append：父或子元素不存在");
    }
    // `remove_child` 返回所有权，再 `add_child` 挂到新父下——这是**移动**
    // （元素与它的 id / 事件绑定都保留），不是销毁重建。
    Element* old_parent = child->parent();
    if (old_parent == nullptr) {
      return st::unexpected(st::ErrorCode::Invalid, "ui.append：子元素不在树上");
    }
    auto owned = old_parent->remove_child(child);
    if (owned == nullptr) {
      return st::unexpected(st::ErrorCode::Internal, "ui.append：摘除失败");
    }
    (void)parent->add_child(std::move(owned));
    return st::Json(true);
  };
  auto clear_fn = [root](const std::vector<st::Json>& args) -> Result<st::Json> {
    if (args.empty()) return st::unexpected(st::ErrorCode::Invalid, "ui.clear 需要 id");
    Element* parent = root->find(st::json_as_string(args[0]));
    if (parent == nullptr) {
      return st::unexpected(st::ErrorCode::NotFound, "ui.clear：元素不存在");
    }
    // 逐个摘除并释放：子元素一起走（包括它们的事件绑定）。
    while (parent->child_count() > 0) {
      (void)parent->remove_child(parent->child_at(0));
    }
    return st::Json(true);
  };
  expose("ui.build", build_fn);
  expose("ui.append", append_fn);
  expose("ui.clear", clear_fn);
  // 脚本侧方法链（`$('#x').append(…)`）直调这三个名字：它们**不经命名空间**，
  // 所以不能补 `app.` 前缀与点号映射——走内部通道原样注册到全局。
  //
  // ⚠ 必须走 `expose_internal` 而不是 `expose`：下划线开头会被 `valid_name` 拒。
  // 实测根因就是这个——现象是“JS 里 `__bridgeBuild` 不是函数”，
  // 而 `expose` 只返回 bool，注册失败静默无痕。
  expose_internal("__bridgeBuild", build_fn);
  expose_internal("__bridgeAppend", append_fn);
  expose_internal("__bridgeClear", clear_fn);
}

ScriptBridge::~ScriptBridge() = default;
ScriptBridge::ScriptBridge(ScriptBridge&&) noexcept = default;
auto ScriptBridge::operator=(ScriptBridge&&) noexcept -> ScriptBridge& = default;


auto ScriptBridge::expose(std::string name, ScriptBridgeFn function) -> bool {
  if (impl_ == nullptr || impl_->script == nullptr) return false;
  if (const auto problem = valid_name(name); problem.has_value()) {
    // 失败原因必须能查到（只返回 bool 会把“名字非法”与“引擎拒绝”压成同一个失败）。
    impl_->last_error = *problem;
    return false;
  }
  // ⚠ **应用暴露的名字一律挂在 `app.` 之下**（含不带点号的）：
  //
  // 契约是「脚本侧看到的是命名空间化的全局对象 `app.…`」（见头文件的用法示例与
  // `expose("fs.read")` → `app.fs.read`）。而实现原先只在名字**含点号**时才挂命名空间，
  // 于是 `expose("add")` 变成裸全局 `add` —— 脚本里写 `app.add(1,2,3)` 报
  // `TypeError: not a function`。
  //
  // 这个不一致原先**看不见**：那个用例随后 `eval_ok("total").get<int>()` 在 null 上
  // 抛异常，而测试框架当时不接异常 ⇒ 整个测试进程 terminate，
  // 报出来的是"跑到某处崩了"而不是"app.add 不是函数"（缺陷被崩溃掩盖）。
  //
  // 命名空间化的理由与头文件一致：不污染全局、不与内建 API 撞名、
  // 且"这个函数是谁提供的"在脚本里一眼可读。
  // 已在 `app.` 之下的就不再套（避免 `app.app.x`）；其余一律归到 `app.` 下。
  const bool already_qualified = name.rfind("app.", 0) == 0;
  const std::string qualified = already_qualified ? name : "app." + name;
  if (!register_one(std::move(qualified), std::move(function))) return false;
  // 清单记**原始名**（见头文件 `exposed()` 的说明）；`app.` 前缀是调用方自己写的
  // 就按原样记（他写什么就报什么）。
  if (const auto found = std::ranges::find(impl_->exposed, name);
      found == impl_->exposed.end()) {
    impl_->exposed.push_back(name);
  }
  return true;
}

auto ScriptBridge::expose_internal(std::string name, ScriptBridgeFn function) -> bool {
  // 框架自己的运行时原语（`__bridgeBuild` 等）：**跳过名字校验**。
  //
  // 下划线前缀是**框架保留段**——这条规则是给应用守的，而这里就是框架本人在用。
  // 所以它走单独一个入口，而不是给 `expose` 加个“允许下划线”的开关
  //（那个开关迟早会被应用摸到）。
  if (impl_ == nullptr || impl_->script == nullptr) return false;
  if (name.empty() || function == nullptr) return false;
  // 框架原语走**裸全局名**（`__bridgeBuild` 由 `script_api.js` 直接按名字调），
  // 因此不加 `app.` 前缀；清单里也记它的原名。
  if (!register_one(name, std::move(function))) return false;
  if (const auto found = std::ranges::find(impl_->exposed, name);
      found == impl_->exposed.end()) {
    impl_->exposed.push_back(std::move(name));
  }
  return true;
}

auto ScriptBridge::register_one(std::string name, ScriptBridgeFn function) -> bool {
  if (impl_ == nullptr || impl_->script == nullptr) return false;
  if (function == nullptr) return false;

  // 桥函数名：
  //
  // · **带点号的命名空间名**（`fs.read`）→ 引擎里用唯一内部名（`__bridge_N`），
  //   再把真名挂到 `app.fs.read`——因为引擎不允许重复注册同名，而内部名唯一
  //   才能让“同名重新 expose”不报错。
  // · **不带点号的全局名**（`__bridgeBuild`）→ **原样注册**：它本来就要成为
  //   全局函数（`script_api.js` 直接按名字调），再包一层内部名就找不到了。
  //
  // ⚠ 这个分支是必需的——实测踩过：两者都走“内部名 + 命名空间挂载”时，
  // `globalThis.$('#list').append()` 报 `__bridgeBuild is not a function`，
  // 因为注册出来的是 `__bridge_6`。
  const bool namespaced = name.find('.') != std::string::npos;
  const std::string internal =
      namespaced ? std::format("__bridge_{}", impl_->sequence++) : name;
  auto status = impl_->script->register_function(
      internal, [function = std::move(function)](const std::vector<st::Json>& args)
                    -> Result<st::Json> { return function(args); });
  if (!status) return false;
  if (!namespaced) {
    // 清单由 `expose` 负责记（它知道调用方传入的原始名）——这里不再插一份，
    // 否则同一个函数会在清单里出现两次（限定名 + 原始名）。
    return true;
  }

  // 命名空间挂载：`fs.read` → `app.fs.read`。
  //
  // 为何要命名空间而不是直接注册全局名：不污染全局、不与内建 API 撞名、
  // 也让"这个函数是谁提供的"在脚本里一眼可读。
  //
  // ⚠ 每一步都要 **`|| {}` 兼容既有值**，而根对象要先建出来：
  // 漏建根的反面是 `undefined.fs = …` 直接抛 TypeError——而 `expose` 返回 `false`
  // 时调用方很容易以为“名字不合法”，实际是挂载炸了（实测踩到）。
  // ⚠ 挂载起点取决于名字是否已带 `app.` 前缀：
  //   · `app.add`（`expose` 加的前缀）→ 起点就是 `globalThis.app`，从 `add` 开始挂；
  //   · 别的根名（如 `tools.x`，调用方自己写的）→ 也要能挂，起点是 `globalThis`。
  //
  // 第一版没区分，于是 `app.add` 被挂成 `globalThis.app.app.add`——
  // 而脚本调 `app.add` 仍然 `not a function`（挂到了一个嵌套的同名对象上）。
  const bool rooted_at_app = name.rfind("app.", 0) == 0;
  std::string source = rooted_at_app
                           ? "globalThis.app = globalThis.app || {};"
                           : "globalThis.app = globalThis.app || {};";
  std::size_t begin = rooted_at_app ? 4 : 0;   // 跳过 `app.`
  std::string accessor = rooted_at_app ? "globalThis.app" : "globalThis";
  while (true) {
    const std::size_t dot = name.find('.', begin);
    const bool last = dot == std::string::npos;
    const std::string segment = name.substr(begin, last ? std::string::npos : dot - begin);
    if (!is_identifier(segment)) return false;
    if (last) {
      source += std::format("{}.{} = {};", accessor, segment, internal);
      break;
    }
    // 中间层：不存在就建、已存在（且是对象）就复用。
    source += std::format("{}.{} = {}.{} || {{}};", accessor, segment, accessor, segment);
    accessor += "." + segment;
    begin = dot + 1;
  }
  // ⚠ **末尾要接一个可 JSON 化的值**（`;0`）：
  //
  // `ScriptEngine::eval` 会把**最后一句表达式的值**转成 JSON 返回。而赋值语句
  // （`a.b = fn`）的值是那个**函数对象**，它没有 JSON 表示 → 整个 `eval` 被判失败，
  // 于是 `expose` 报警称“失败”——而属性其实已经挂上去了。
  // 实测根因就是这两个字：`<bridge>：函数没有 JSON 表示（请改为传调用结果）`。
  //
  // 为何不把“结尾表达式不可表示”当成功：那会把真正的 eval 错误也放过去
  // （失败与成功不可分）。截坐在这里、让每次挂载都有一个哑值返回，更小也更精确。
  source += "0;";
  // 清单由 `expose` 记（见上）。
  return impl_->run_source(source, "<bridge>").has_value();
}

auto ScriptBridge::load(const std::string& path) -> Status {
  if (impl_ == nullptr) return st::unexpected(st::ErrorCode::Unsupported, "桥未就绪");
  const auto text = st::fs::read_text(path);
  if (!text.has_value()) {
    // 失败原因必须具体到"哪一步"——"脚本出错"这种话没法排查。
    return st::unexpected(text.error().code,
                          std::format("读不到脚本 {}：{}", path, text.error().message));
  }
  const auto status = impl_->run_source(*text, path);
  if (!status.has_value()) return status;
  impl_->loaded.push_back(path);
  return {};
}

auto ScriptBridge::run(std::string_view source, std::string_view filename) -> Status {
  if (impl_ == nullptr) return st::unexpected(st::ErrorCode::Unsupported, "桥未就绪");
  return impl_->run_source(std::string(source), std::string(filename));
}

auto ScriptBridge::exposed() const -> const std::vector<std::string>& {
  static const std::vector<std::string> empty{};
  return impl_ != nullptr ? impl_->exposed : empty;
}

auto ScriptBridge::loaded_files() const -> const std::vector<std::string>& {
  static const std::vector<std::string> empty{};
  return impl_ != nullptr ? impl_->loaded : empty;
}

auto ScriptBridge::last_error() const -> std::string {
  return impl_ != nullptr ? impl_->last_error : std::string{};
}

auto ScriptBridge::call(const std::string& function, const std::vector<st::Json>& arguments)
    -> Result<st::Json> {
  if (impl_ == nullptr || impl_->script == nullptr) {
    return st::unexpected(st::ErrorCode::Unsupported, "桥未就绪");
  }
  return impl_->script->call(function, arguments);
}

}  // namespace st::ui
