// `ScriptEngine` 的**空实现**：在排除 `src/ext/script.cpp`（QuickJS 整链）时替代它。
//
// ## 为什么需要这个文件
//
// `st.pkg` 允许目标排除 `src/ext/script.cpp` + `third_party/quickjs/*`（省 786KB）。
// 但排除后 `ScriptEngine` 的所有符号都不存在，而**框架库源里仍有引用**：
// `src/ui/script_host.cpp` 无条件写 `engine->eval(...)` 之类。
//
// ## 为什么不能靠 `#ifdef ST_NO_SCRIPT`
//
// `targets.<名>.defines` **只发给目标自己的源文件**，而 `script_host.cpp`
// 是框架库源——实测：往 `app.cpp` 顶部插 `#ifdef ST_NO_SCRIPT static_assert(false,…)`，
// gbcode 构建**不报错**，说明宏压根没到编译器（`app.cpp.o` 里 `nm` 仍能看到
// `unique_ptr<ScriptHost>` 的实例化）。
//
// 而“把目标宏也发给框架单元”这条路**与框架对象缓存的设计直接冲突**：
// 见 `build.cpp` 的 `make_framework_flags` 长注释——框架缓存不能被目标宏污染，
// 否则每换一个宏值就全量重编框架源（实测每轮 2m30s）。
//
// ## 本文件的做法
//
// 提供一份**同名同签名的空实现**，由构建清单决定编不编它：
//
//   "exclude_sources": ["src/ext/script.cpp", …]   // 不含引擎实现
//   "sources": [ …, "src/ext/script_stub.cpp"]     // 含空实现
//
// 链接期符号齐全（不再报 undefined），运行期 `valid()` 恒 false，
// `Application::start()` 里 `ScriptHost` 初始化失败 → 按既有逻辑回 "未编入"。
// **不碰编译标志、不干扰框架缓存**，两侧的设计意图都保住。

#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/script.hpp"
// `script.hpp` 只前向声明 `Json`（避免把 25,526 行的 nlohmann 拉进每个包含方）。
// 这份空实现要**定义**返回 `Result<Json>` 的函数，必须拿到完整类型。
#include "st/core/json.hpp"

namespace st::ext {

namespace detail {
// `script.hpp` 只前向声明 `ScriptState`（真定义在 `script.cpp` 里，含 QuickJS 上下文）。
// 而 `ScriptEngine` 持有 `unique_ptr<ScriptState>`——**析构/移动都需要完整类型**，
// 所以这份空实现必须给出一个空定义（没有任何成员，纯粹为了让
// `unique_ptr` 的删除器能实例化）。
struct ScriptState {};

}  // namespace detail

namespace {

[[nodiscard]] auto unavailable(std::string_view what) -> Status {
  return st::unexpected(ErrorCode::Unsupported,
                        std::format("脚本引擎未编入本产物（{} 不可用；"
                                    "构建清单排除了 src/ext/script.cpp）",
                                    what));
}

/// `Result<T>` 一族的失败值（`Status` 不能隐式转成 `Result<Json>`）。
template <typename T>
[[nodiscard]] auto unavailable_value(std::string_view what) -> Result<T> {
  return st::unexpected(ErrorCode::Unsupported,
                        std::format("脚本引擎未编入本产物（{} 不可用；"
                                    "构建清单排除了 src/ext/script.cpp）",
                                    what));
}

}  // namespace

ScriptEngine::ScriptEngine() = default;
ScriptEngine::ScriptEngine(ScriptLimits) {}
ScriptEngine::~ScriptEngine() = default;
ScriptEngine::ScriptEngine(ScriptEngine&&) noexcept = default;
auto ScriptEngine::operator=(ScriptEngine&&) noexcept -> ScriptEngine& = default;

auto ScriptEngine::valid() const noexcept -> bool { return false; }

auto ScriptEngine::eval(std::string_view, std::string_view) -> Result<Json> {
  return unavailable_value<Json>("eval");
}

auto ScriptEngine::call(std::string_view, const std::vector<Json>&) -> Result<Json> {
  return unavailable_value<Json>("call");
}

auto ScriptEngine::register_function(std::string, ScriptHostFunction) -> Status {
  return unavailable("register_function");
}

auto ScriptEngine::register_binary_function(std::string, ScriptBinaryHostFunction) -> Status {
  return unavailable("register_binary_function");
}

auto ScriptEngine::set_global(std::string_view, const Json&) -> Status {
  return unavailable("set_global");
}

auto ScriptEngine::global(std::string_view) -> Result<Json> { return unavailable_value<Json>("global"); }

auto ScriptEngine::check_syntax(std::string_view) -> Status { return unavailable("check_syntax"); }

auto ScriptEngine::pump_jobs() -> std::size_t { return 0; }

auto ScriptEngine::version() -> std::string_view { return "（未编入脚本引擎）"; }

}  // namespace st::ext
