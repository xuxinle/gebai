#include "st/ext/script.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quickjs/quickjs.h"

#include "st/ext/json.hpp"

namespace st::ext {

/// 引擎状态：QuickJS 运行时/上下文 + 配额 + 宿主函数表。
///
/// `ScriptEngine` 持有它的 `unique_ptr`；指针同时经 `JS_SetContextOpaque` 挂到 JS 上下文上，
/// 让 C 函数指针形态的回调（中断处理、宿主函数桥）能找回来。
struct detail::ScriptState {
  JSRuntime* runtime{nullptr};
  JSContext* context{nullptr};
  ScriptLimits limits{};
  /// 本次执行的截止时刻（0 = 不限时）
  std::int64_t deadline_ms{0};
  /// 本次执行的指令预算（0 = 不限）
  std::uint64_t op_budget{0};
  std::uint64_t ops{0};
  bool timed_out{false};
  bool out_of_ops{false};
  std::vector<std::pair<std::string, ScriptHostFunction>> host_functions{};

  [[nodiscard]] auto find_host(std::string_view name) const -> const ScriptHostFunction* {
    for (const auto& [key, function] : host_functions) {
      if (key == name) return &function;
    }
    return nullptr;
  }

  /// 开始一次执行：重置计数并设定截止时刻。
  void begin_run(const ScriptLimits& effective) {
    timed_out = false;
    out_of_ops = false;
    ops = 0;
    op_budget = effective.max_interrupts;
    deadline_ms = effective.timeout.count() > 0
                      ? std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                                .count() +
                            effective.timeout.count()
                      : 0;
  }
};

namespace {

/// `JSValue` 的 RAII 持有。
///
/// QuickJS 是手工引用计数：**每一条提前 return 的分支都必须释放**，否则脚本反复执行会持续泄漏。
/// 用它把"所有出口"统一成析构，而不是在每个 `return unexpected(...)` 前记得补 `JS_FreeValue`。
class JsRef {
 public:
  JsRef() = default;
  JsRef(JSContext* ctx, JSValue value) : ctx_(ctx), value_(value) {}
  ~JsRef() {
    if (ctx_ != nullptr) JS_FreeValue(ctx_, value_);
  }
  JsRef(const JsRef&) = delete;
  auto operator=(const JsRef&) -> JsRef& = delete;
  JsRef(JsRef&& other) noexcept : ctx_(other.ctx_), value_(other.value_) { other.ctx_ = nullptr; }
  auto operator=(JsRef&& other) noexcept -> JsRef& {
    if (this != &other) {
      if (ctx_ != nullptr) JS_FreeValue(ctx_, value_);
      ctx_ = other.ctx_;
      value_ = other.value_;
      other.ctx_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] auto get() const noexcept -> JSValue { return value_; }
  /// 放弃所有权（把值交还给 QuickJS 的某个容器时用）。
  auto release() noexcept -> JSValue {
    ctx_ = nullptr;
    return value_;
  }

 private:
  JSContext* ctx_{nullptr};
  JSValue value_{};
};

// 互相递归的转换函数：先声明，再定义。
[[nodiscard]] auto convert_js_to_json(JSContext* ctx, JSValueConst value, std::uint32_t depth,
                                      std::uint32_t max_depth) -> Result<Json>;
[[nodiscard]] auto convert_json_to_js(JSContext* ctx, const Json& value, std::uint32_t depth,
                                      std::uint32_t max_depth) -> JSValue;

/// 中断回调：返回非 0 让 QuickJS 停手。
///
/// 这是"死循环/长跑脚本不会挂死调用方"的唯一实现手段——引擎在字节码循环里定期调用它，
/// 因此这里必须 O(1) 且**不做任何分配**（分配可能失败、可能反向触发内存上限回调）。
auto interrupt_handler(JSRuntime* /*runtime*/, void* opaque) -> int {
  auto* state = static_cast<detail::ScriptState*>(opaque);
  if (state == nullptr) return 0;
  ++state->ops;
  if (state->op_budget != 0 && state->ops > state->op_budget) {
    state->out_of_ops = true;
    return 1;
  }
  if (state->deadline_ms != 0) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (now > state->deadline_ms) {
      state->timed_out = true;
      return 1;
    }
  }
  return 0;
}

/// 宿主函数桥：JS 调用 → 查表 → C++ 闭包 → 结果转回 JS。
///
/// 返回一个抛出的 JS 异常（而非 `undefined`）表示宿主侧失败：脚本能用 `try/catch` 接住并决策。
auto host_bridge(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv,
                 int /*magic*/, JSValueConst* func_data) -> JSValue {
  auto* state = static_cast<detail::ScriptState*>(JS_GetContextOpaque(ctx));
  if (state == nullptr) return JS_ThrowInternalError(ctx, "脚本引擎状态丢失");
  // 函数名随函数对象一起保存（`JS_NewCFunctionData` 的 data 槽）
  const char* name_cstr = JS_ToCString(ctx, *func_data);
  const std::string name = name_cstr != nullptr ? name_cstr : "";
  if (name_cstr != nullptr) JS_FreeCString(ctx, name_cstr);
  const ScriptHostFunction* host = state->find_host(name);
  if (host == nullptr) return JS_ThrowInternalError(ctx, "宿主函数未注册: %s", name.c_str());

  std::vector<Json> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    auto converted = convert_js_to_json(ctx, argv[index], 0, state->limits.max_convert_depth);
    if (!converted) {
      return JS_ThrowTypeError(ctx, "参数 %d 无法转换为 JSON: %s", index,
                               converted.error().message.c_str());
    }
    arguments.push_back(std::move(*converted));
  }
  auto outcome = (*host)(arguments);
  if (!outcome) {
    return JS_ThrowInternalError(ctx, "宿主函数 %s 失败: %s", name.c_str(),
                                 outcome.error().message.c_str());
  }
  return convert_json_to_js(ctx, *outcome, 0, state->limits.max_convert_depth);
}

/// JS 值 → Json。
///
/// 深度上限不是"洁癖"：JS 对象可以有循环引用（`a.self = a`），无上限的递归转换会直接爆栈。
auto convert_js_to_json(JSContext* ctx, JSValueConst value, std::uint32_t depth,
                        std::uint32_t max_depth) -> Result<Json> {
  if (depth > max_depth) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("转换嵌套超过 {} 层（可能是循环引用）", max_depth));
  }
  if (JS_IsException(value)) return unexpected(ErrorCode::Parse, "JS 值处于异常状态");
  if (JS_IsUndefined(value) || JS_IsNull(value)) return Json();

  if (JS_IsBool(value)) return Json(JS_ToBool(ctx, value) != 0);

  if (JS_IsNumber(value)) {
    // 整数按 int64 存（这点很重要：`{"line": 3}` 不该变成 `3.0`），非整数按 double。
    std::int64_t as_int = 0;
    if (JS_ToInt64(ctx, &as_int, value) == 0) {
      double as_double = 0.0;
      if (JS_ToFloat64(ctx, &as_double, value) == 0 &&
          static_cast<double>(as_int) == as_double) {
        return Json(as_int);
      }
    }
    double as_double = 0.0;
    if (JS_ToFloat64(ctx, &as_double, value) != 0) {
      return unexpected(ErrorCode::Parse, "JS 数字无法读取");
    }
    if (std::isnan(as_double) || std::isinf(as_double)) {
      // JSON 没有 NaN/Infinity：明确报错，而不是悄悄变 null
      return unexpected(ErrorCode::Unsupported, "JSON 无法表示 NaN/Infinity");
    }
    return Json(as_double);
  }

  if (JS_IsBigInt(value)) {
    // **不能**用 `JS_ToBigInt64` 判溢出：它超出范围时照样返回成功并给出被截断的值
    // （实测 `123456789012345678901234567890n` 会变成 -4362896299872285998）。
    // 改为拿十进制文本，用 `from_chars` 精确判定能否落进 int64。
    const char* text = JS_ToCString(ctx, value);
    const std::string digits = text != nullptr ? text : "";
    if (text != nullptr) JS_FreeCString(ctx, text);
    std::string_view view(digits);
    if (!view.empty() && view.back() == 'n') view.remove_suffix(1);  // 防御：某些形态带 `n` 后缀
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), parsed);
    if (error == std::errc{} && end == view.data() + view.size()) return Json(parsed);
    // 超出 int64：退回字符串（保真优于静默取模）
    return Json(std::string(view));
  }

  if (JS_IsString(value)) {
    const char* text = JS_ToCString(ctx, value);
    if (text == nullptr) return unexpected(ErrorCode::Parse, "JS 字符串无法读取");
    std::string out(text);
    JS_FreeCString(ctx, text);
    return Json(out);
  }

  if (JS_IsArray(value)) {
    JsRef length_ref{ctx, JS_GetPropertyStr(ctx, value, "length")};
    std::int64_t length = 0;
    if (JS_ToInt64(ctx, &length, length_ref.get()) != 0 || length < 0) {
      return unexpected(ErrorCode::Parse, "JS 数组长度无法读取");
    }
    Json array = Json::array();
    for (std::int64_t index = 0; index < length; ++index) {
      JsRef item{ctx, JS_GetPropertyUint32(ctx, value, static_cast<std::uint32_t>(index))};
      auto converted = convert_js_to_json(ctx, item.get(), depth + 1, max_depth);
      if (!converted) return converted;
      array.push_back(std::move(*converted));
    }
    return array;
  }

  // 函数检查**必须早于对象分支**：JS 里函数也是对象，否则会被枚举成"没有可枚举键的对象"
  // 从而静默变成 `{}`——调用方会以为拿到了一份空数据，而实际是"这里有个函数没法序列化"。
  if (JS_IsFunction(ctx, value)) {
    return unexpected(ErrorCode::Unsupported, "函数没有 JSON 表示（请改为传调用结果）");
  }

  if (JS_IsObject(value)) {
    Json object = Json::object();
    JSPropertyEnum* table = nullptr;
    std::uint32_t count = 0;
    // 只取可枚举的字符串键：与 `Object.keys` 的直觉一致
    if (JS_GetOwnPropertyNames(ctx, &table, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) !=
        0) {
      return unexpected(ErrorCode::Parse, "无法枚举 JS 对象键");
    }
    for (std::uint32_t index = 0; index < count; ++index) {
      const char* key_cstr = JS_AtomToCString(ctx, table[index].atom);
      if (key_cstr == nullptr) continue;
      const std::string key(key_cstr);
      JS_FreeCString(ctx, key_cstr);
      JsRef item{ctx, JS_GetProperty(ctx, value, table[index].atom)};
      auto converted = convert_js_to_json(ctx, item.get(), depth + 1, max_depth);
      if (!converted) {
        JS_FreePropertyEnum(ctx, table, count);
        return converted;
      }
      object[key] = std::move(*converted);
    }
    JS_FreePropertyEnum(ctx, table, count);
    return object;
  }

  // 函数 / Symbol 之类没有 JSON 表示：明确报错，别静默变 null
  return unexpected(ErrorCode::Unsupported, "该 JS 值类型无法表示为 JSON");
}

/// Json → JS 值。
auto convert_json_to_js(JSContext* ctx, const Json& value, std::uint32_t depth,
                        std::uint32_t max_depth) -> JSValue {
  if (depth > max_depth) {
    return JS_ThrowInternalError(ctx, "JSON 嵌套超过 %u 层", max_depth);
  }
  if (value.is_null()) return JS_NULL;
  if (value.is_boolean()) return JS_NewBool(ctx, value.get<bool>());
  if (value.is_number_integer()) return JS_NewInt64(ctx, value.get<std::int64_t>());
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    if (raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return JS_NewInt64(ctx, static_cast<std::int64_t>(raw));
    }
    // 超出 int64：JS 侧只能以 double 表达（JS 数字语义使然），此处如实降级
    return JS_NewFloat64(ctx, static_cast<double>(raw));
  }
  if (value.is_number_float()) return JS_NewFloat64(ctx, value.get<double>());
  if (value.is_string()) {
    const std::string& text = value.get_ref<const std::string&>();
    return JS_NewStringLen(ctx, text.data(), text.size());
  }
  if (value.is_array()) {
    JSValue array = JS_NewArray(ctx);
    std::uint32_t index = 0;
    for (const Json& item : value) {
      JSValue converted = convert_json_to_js(ctx, item, depth + 1, max_depth);
      if (JS_IsException(converted)) {
        JS_FreeValue(ctx, array);
        return converted;
      }
      JS_SetPropertyUint32(ctx, array, index, converted);
      ++index;
    }
    return array;
  }
  if (value.is_object()) {
    JSValue object = JS_NewObject(ctx);
    for (const auto& [key, item] : value.items()) {
      JSValue converted = convert_json_to_js(ctx, item, depth + 1, max_depth);
      if (JS_IsException(converted)) {
        JS_FreeValue(ctx, object);
        return converted;
      }
      JS_SetPropertyStr(ctx, object, key.c_str(), converted);
    }
    return object;
  }
  return JS_ThrowInternalError(ctx, "JSON 值无法转换为 JS");
}

/// 取出 JS 异常的可读描述（消息 + 首行调用栈，含行号）。
///
/// 特例：**内存耗尽**时 QuickJS 往往连异常对象都构造不出来（构造它也要分配内存），
/// `JS_GetException` 只给出 null/undefined。此时不能把 `null` 当作错误消息丢给用户——
/// 那等于什么都没说，得直接指向最可能的原因（内存上限）与具体数值。
[[nodiscard]] auto describe_exception(JSContext* ctx, const ScriptLimits& limits) -> std::string {
  JsRef exception{ctx, JS_GetException(ctx)};
  if (JS_IsNull(exception.get()) || JS_IsUndefined(exception.get())) {
    return std::format("引擎内部异常（异常对象不可用，通常是内存超限：上限 {} MiB）",
                       limits.memory_bytes / (1024U * 1024U));
  }
  std::string description = "JS 异常";
  const char* text = JS_ToCString(ctx, exception.get());
  if (text != nullptr) {
    description = text;
    JS_FreeCString(ctx, text);
  }
  JsRef stack{ctx, JS_GetPropertyStr(ctx, exception.get(), "stack")};
  if (JS_IsString(stack.get())) {
    const char* stack_text = JS_ToCString(ctx, stack.get());
    if (stack_text != nullptr) {
      const std::string_view view(stack_text);
      const std::size_t newline = view.find('\n');
      const std::string_view first =
          newline == std::string_view::npos ? view : view.substr(0, newline);
      if (!first.empty()) description.append("\n  ").append(first);
      JS_FreeCString(ctx, stack_text);
    }
  }
  return description;
}

/// 执行收尾：记录统计（内存/耗时/指令数）。
void record_stats(detail::ScriptState& state, ScriptStats& stats,
                  std::chrono::steady_clock::time_point started) {
  const auto stopped = std::chrono::steady_clock::now();
  stats.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(stopped - started);
  stats.timed_out = state.timed_out;
  stats.ops = state.ops;
  JSMemoryUsage usage{};
  JS_ComputeMemoryUsage(state.runtime, &usage);
  stats.memory_used = static_cast<std::size_t>(std::max<std::int64_t>(0, usage.malloc_size));
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 生命周期
// ————————————————————————————————————————————————————————————————————————————

ScriptEngine::ScriptEngine() : ScriptEngine(ScriptLimits{}) {}

ScriptEngine::ScriptEngine(ScriptLimits limits)
    : impl_(std::make_unique<detail::ScriptState>()), limits_(limits) {
  impl_->limits = limits;
  impl_->runtime = JS_NewRuntime();
  if (impl_->runtime == nullptr) return;
  // 配额在**运行时层**生效：脚本无从绕过（不是"入口处检查参数"那种纸面约束）
  JS_SetMemoryLimit(impl_->runtime, limits.memory_bytes);
  JS_SetMaxStackSize(impl_->runtime, limits.stack_bytes);
  JS_SetInterruptHandler(impl_->runtime, interrupt_handler, impl_.get());
  impl_->op_budget = limits.max_interrupts;
  impl_->context = JS_NewContext(impl_->runtime);
  if (impl_->context == nullptr) {
    JS_FreeRuntime(impl_->runtime);
    impl_->runtime = nullptr;
    return;
  }
  // 把 C++ 状态挂到上下文：C 回调据此找回宿主函数表
  JS_SetContextOpaque(impl_->context, impl_.get());
}

ScriptEngine::~ScriptEngine() {
  if (impl_ == nullptr) return;
  if (impl_->context != nullptr) {
    JS_SetContextOpaque(impl_->context, nullptr);
    JS_FreeContext(impl_->context);
    impl_->context = nullptr;
  }
  if (impl_->runtime != nullptr) {
    JS_FreeRuntime(impl_->runtime);
    impl_->runtime = nullptr;
  }
}

ScriptEngine::ScriptEngine(ScriptEngine&& other) noexcept
    : impl_(std::move(other.impl_)), limits_(other.limits_), stats_(other.stats_) {}

auto ScriptEngine::operator=(ScriptEngine&& other) noexcept -> ScriptEngine& {
  if (this != &other) {
    impl_ = std::move(other.impl_);
    limits_ = other.limits_;
    stats_ = other.stats_;
  }
  return *this;
}

auto ScriptEngine::valid() const noexcept -> bool {
  return impl_ != nullptr && impl_->runtime != nullptr && impl_->context != nullptr;
}

auto ScriptEngine::version() -> std::string_view { return "0.17.0 (quickjs-ng)"; }

// ————————————————————————————————————————————————————————————————————————————
// 执行
// ————————————————————————————————————————————————————————————————————————————

auto ScriptEngine::eval(std::string_view source, std::string_view filename) -> Result<Json> {
  if (!valid()) return unexpected(ErrorCode::Unsupported, "脚本引擎未就绪（运行时创建失败）");
  const auto started = std::chrono::steady_clock::now();
  impl_->begin_run(limits_);

  const std::string file_name(filename);
  JsRef result{impl_->context,
               JS_Eval(impl_->context, source.data(), source.size(), file_name.c_str(),
                       JS_EVAL_TYPE_GLOBAL)};

  if (JS_IsException(result.get())) {
    const std::string detail = describe_exception(impl_->context, limits_);
    record_stats(*impl_, stats_, started);
    if (impl_->timed_out) {
      return unexpected(ErrorCode::Timeout,
                        std::format("脚本执行超时（{} ms）：{}", limits_.timeout.count(), detail));
    }
    if (impl_->out_of_ops) {
      return unexpected(ErrorCode::Timeout,
                        std::format("脚本超出中断轮询预算（{} 次）：{}", limits_.max_interrupts, detail));
    }
    return unexpected(ErrorCode::Parse, std::format("脚本错误: {}", detail));
  }

  auto converted = convert_js_to_json(impl_->context, result.get(), 0, limits_.max_convert_depth);
  record_stats(*impl_, stats_, started);
  if (!converted) return forward_error(converted.error());
  return Json(std::move(*converted));
}

auto ScriptEngine::call(std::string_view function, const std::vector<Json>& arguments)
    -> Result<Json> {
  if (!valid()) return unexpected(ErrorCode::Unsupported, "脚本引擎未就绪");
  const auto started = std::chrono::steady_clock::now();
  impl_->begin_run(limits_);

  JsRef global{impl_->context, JS_GetGlobalObject(impl_->context)};
  const std::string name(function);
  JsRef target{impl_->context, JS_GetPropertyStr(impl_->context, global.get(), name.c_str())};
  if (!JS_IsFunction(impl_->context, target.get())) {
    record_stats(*impl_, stats_, started);
    return unexpected(ErrorCode::NotFound, std::format("脚本中未定义函数: {}", name));
  }

  std::vector<JSValue> raw_args;
  raw_args.reserve(arguments.size());
  for (const Json& argument : arguments) {
    raw_args.push_back(convert_json_to_js(impl_->context, argument, 0, limits_.max_convert_depth));
  }
  JsRef result{impl_->context,
               JS_Call(impl_->context, target.get(), JS_UNDEFINED, static_cast<int>(raw_args.size()),
                       raw_args.data())};
  for (const JSValue raw : raw_args) JS_FreeValue(impl_->context, raw);

  if (JS_IsException(result.get())) {
    const std::string detail = describe_exception(impl_->context, limits_);
    record_stats(*impl_, stats_, started);
    if (impl_->timed_out) {
      return unexpected(ErrorCode::Timeout,
                        std::format("脚本执行超时（{} ms）：{}", limits_.timeout.count(), detail));
    }
    if (impl_->out_of_ops) {
      return unexpected(ErrorCode::Timeout,
                        std::format("脚本超出中断轮询预算（{} 次）：{}", limits_.max_interrupts, detail));
    }
    return unexpected(ErrorCode::Parse, std::format("脚本错误: {}", detail));
  }

  auto converted = convert_js_to_json(impl_->context, result.get(), 0, limits_.max_convert_depth);
  record_stats(*impl_, stats_, started);
  if (!converted) return forward_error(converted.error());
  return Json(std::move(*converted));
}

auto ScriptEngine::check_syntax(std::string_view source) -> Status {
  if (!valid()) return unexpected(ErrorCode::Unsupported, "脚本引擎未就绪");
  // 只编译不执行：配置加载期校验语法，不产生副作用
  JsRef compiled{impl_->context,
                 JS_Eval(impl_->context, source.data(), source.size(), "<syntax>",
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY)};
  if (JS_IsException(compiled.get())) {
    const std::string detail = describe_exception(impl_->context, limits_);
    return unexpected(ErrorCode::Parse, std::format("脚本语法错误: {}", detail));
  }
  return ok();
}

// ————————————————————————————————————————————————————————————————————————————
// 宿主绑定
// ————————————————————————————————————————————————————————————————————————————

auto ScriptEngine::pump_jobs() -> std::size_t {
  if (!valid()) return 0;
  std::size_t executed = 0;
  // 把已入队的微任务全部跑完（job 里再排新 job 也一并处理）——挂起 job 不驱动就
  // 永远不会跑（Promise.then 无人调用）。
  JSContext* context = nullptr;
  while (JS_IsJobPending(impl_->runtime)) {
    if (JS_ExecutePendingJob(impl_->runtime, &context) < 0) {
      // 不引 core/log（ext 层此前零日志依赖）——微任务失败经 stderr 可见即可
      std::fprintf(stderr, "[script] 微任务失败: %s\n",
                   describe_exception(impl_->context, limits_).c_str());
      break;
    }
    ++executed;
  }
  return executed;
}

auto ScriptEngine::register_function(std::string name, ScriptHostFunction function) -> Status {
  if (!valid()) return unexpected(ErrorCode::Unsupported, "脚本引擎未就绪");
  if (name.empty()) return unexpected(ErrorCode::Invalid, "宿主函数名不能为空");
  if (!function) return unexpected(ErrorCode::Invalid, "宿主函数为空");
  // 不覆盖既有绑定：静默覆盖会让"谁改了这个函数"无从追查
  if (impl_->find_host(name) != nullptr) {
    return unexpected(ErrorCode::Busy, std::format("宿主函数已存在: {}", name));
  }
  impl_->host_functions.emplace_back(name, std::move(function));

  // 名字随函数对象传递（C 回调签名不携带自定义上下文，只有 magic 与 data 槽可用）
  JSValue data[1] = {JS_NewStringLen(impl_->context, name.data(), name.size())};
  JSValue function_value =
      JS_NewCFunctionData(impl_->context, host_bridge, 0, /*magic=*/0, /*data_len=*/1, data);
  JS_FreeValue(impl_->context, data[0]);
  if (JS_IsException(function_value)) {
    impl_->host_functions.pop_back();
    return unexpected(ErrorCode::Parse, "宿主函数注册失败（无法创建 JS 函数对象）");
  }
  JsRef owned{impl_->context, function_value};
  JsRef global{impl_->context, JS_GetGlobalObject(impl_->context)};
  JS_SetPropertyStr(impl_->context, global.get(), name.c_str(), owned.release());
  return ok();
}

auto ScriptEngine::set_global(std::string_view name, const Json& value) -> Status {
  if (!valid()) return unexpected(ErrorCode::Unsupported, "脚本引擎未就绪");
  if (name.empty()) return unexpected(ErrorCode::Invalid, "全局名不能为空");
  const std::string key(name);
  JSValue converted = convert_json_to_js(impl_->context, value, 0, limits_.max_convert_depth);
  if (JS_IsException(converted)) {
    const std::string detail = describe_exception(impl_->context, limits_);
    return unexpected(ErrorCode::Unsupported, std::format("值无法转换为 JS: {}", detail));
  }
  JsRef owned{impl_->context, converted};
  JsRef global{impl_->context, JS_GetGlobalObject(impl_->context)};
  JS_SetPropertyStr(impl_->context, global.get(), key.c_str(), owned.release());
  return ok();
}

auto ScriptEngine::global(std::string_view name) -> Result<Json> {
  if (!valid()) return unexpected(ErrorCode::Unsupported, "脚本引擎未就绪");
  const std::string key(name);
  JsRef global{impl_->context, JS_GetGlobalObject(impl_->context)};
  JsRef value{impl_->context, JS_GetPropertyStr(impl_->context, global.get(), key.c_str())};
  if (JS_IsUndefined(value.get())) {
    return unexpected(ErrorCode::NotFound, std::format("脚本全局不存在: {}", key));
  }
  return convert_js_to_json(impl_->context, value.get(), 0, limits_.max_convert_depth);
}

auto run_script(std::string_view source, const ScriptLimits& limits) -> Result<Json> {
  ScriptEngine engine(limits);
  return engine.eval(source);
}

}  // namespace st::ext
