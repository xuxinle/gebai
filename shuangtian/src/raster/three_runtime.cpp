#include "st/raster/three_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "battery/embed.hpp"
#include "st/core/json.hpp"
#include "st/core/log.hpp"
// ⚠ **本文件是唯一包含 quickjs.h 的翻译单元之一**（另一个是 `src/ext/script.cpp`）。
//
// ## 为什么不用 `st::ext::ScriptEngine`
//
// 那个封装按"脚本 = 可执行代码，能力边界必须显式设计"的姿态设计的：宿主能力全部经
// `register_function`（**只传 Json**）。而 three.js 的场景抽取需要把
// `geometry.attributes.position.array`（一个 `Float32Array`，常见几千个 float）
// 取回来——走 Json 转换是逐元素建对象，实测 3.5 µs/顶点（1 万顶点 35 ms），
// 对每帧重算的场景是 28 fps 的上限。
//
// 更关键的是：three.js 的 `Scene`/`Mesh`/`Material` 是**活的 JS 对象图**，
// 根本没法序列化成 Json（循环引用、原型链、`Float32Array`）。
//
// 所以这里的做法是：**直接持有 QuickJS 上下文**，用 `JS_Eval` 跑脚本，
// 再用 `JS_GetTypedArrayBuffer` + `JS_GetArrayBuffer` 零拷贝读几何内存。
// 这不是绕过安全设计——本层给脚本的能力比 `ScriptEngine` **更少**：
// 没有注册任何宿主函数（脚本连"读时间"都做不到），只有 JS 语言本身 + three.js。
//
// ## 公共头里没有 quickjs.h
//
// 与 `st::ext::database` 对 `sqlite3.h` 的处理同一立场：第三方 C 头只出现在实现文件里，
// 公共头不背这个编译期成本，也不让 `JS_*` 符号随处可用。

extern "C" {
#include "quickjs/quickjs.h"
}

namespace st::raster {

namespace {

/// three.js 源码（编译期嵌入——与 `ui/declarative.js` 同一机制，不依赖运行时文件）。
[[nodiscard]] auto three_source() -> std::string_view {
  const auto file = b::embed<"third_party/three/three.cjs">();
  return std::string_view(file.data(), file.length());
}

/// RAII 包装：异常安全地释放 JSValue（QuickJS 是手写引用计数，漏一个就泄漏整个运行时）。
struct JsRef {
  JSContext* ctx{nullptr};
  JSValue value{JS_UNDEFINED};

  JsRef() = default;
  JsRef(JSContext* context, JSValue v) : ctx(context), value(v) {}
  ~JsRef() {
    if (ctx != nullptr) JS_FreeValue(ctx, value);
  }
  JsRef(const JsRef&) = delete;
  auto operator=(const JsRef&) -> JsRef& = delete;
  JsRef(JsRef&& other) noexcept : ctx(other.ctx), value(other.value) {
    other.ctx = nullptr;
    other.value = JS_UNDEFINED;
  }
  auto operator=(JsRef&& other) noexcept -> JsRef& {
    if (this != &other) {
      if (ctx != nullptr) JS_FreeValue(ctx, value);
      ctx = other.ctx;
      value = other.value;
      other.ctx = nullptr;
      other.value = JS_UNDEFINED;
    }
    return *this;
  }
  [[nodiscard]] auto get() const noexcept -> JSValue { return value; }
  [[nodiscard]] auto release() noexcept -> JSValue {
    ctx = nullptr;
    const auto out = value;
    value = JS_UNDEFINED;
    return out;
  }
  [[nodiscard]] auto is_exception() const noexcept -> bool { return JS_IsException(value); }
};

/// 把当前挂起的 JS 异常整形为可行动的错误消息（含堆栈）。
[[nodiscard]] auto take_exception_text(JSContext* ctx) -> std::string {
  JSValue exception = JS_GetException(ctx);
  JsRef owned{ctx, exception};
  std::string message;
  if (JS_IsObject(exception)) {
    // `Error` 对象：取 message + stack（stack 里带 three.js 侧行号，排错全靠它）
    JsRef msg{ctx, JS_GetPropertyStr(ctx, exception, "message")};
    if (JS_IsString(msg.get())) {
      const char* text = JS_ToCString(ctx, msg.get());
      if (text != nullptr) {
        message = text;
        JS_FreeCString(ctx, text);
      }
    }
    JsRef stack{ctx, JS_GetPropertyStr(ctx, exception, "stack")};
    if (JS_IsString(stack.get())) {
      const char* text = JS_ToCString(ctx, stack.get());
      if (text != nullptr) {
        if (!message.empty()) message += "\n";
        message += text;
        JS_FreeCString(ctx, text);
      }
    }
  }
  if (message.empty()) {
    const char* text = JS_ToCString(ctx, exception);
    if (text != nullptr) {
      message = text;
      JS_FreeCString(ctx, text);
    }
  }
  if (message.empty()) message = "(无描述的异常)";
  return message;
}

[[nodiscard]] auto get_double(JSContext* ctx, JSValueConst object, const char* key, double fallback)
    -> double {
  JsRef property{ctx, JS_GetPropertyStr(ctx, object, key)};
  if (JS_IsUndefined(property.get()) || property.is_exception() || !JS_IsNumber(property.get())) {
    return fallback;
  }
  double out = fallback;
  JS_ToFloat64(ctx, &out, property.get());
  return out;
}

[[nodiscard]] auto get_bool(JSContext* ctx, JSValueConst object, const char* key, bool fallback)
    -> bool {
  JsRef property{ctx, JS_GetPropertyStr(ctx, object, key)};
  if (JS_IsUndefined(property.get()) || property.is_exception()) return fallback;
  return JS_ToBool(ctx, property.get()) != 0;
}

[[nodiscard]] auto get_string(JSContext* ctx, JSValueConst object, const char* key) -> std::string {
  JsRef property{ctx, JS_GetPropertyStr(ctx, object, key)};
  if (!JS_IsString(property.get())) return {};
  const char* text = JS_ToCString(ctx, property.get());
  if (text == nullptr) return {};
  std::string out = text;
  JS_FreeCString(ctx, text);
  return out;
}

/// 把一个 typed array（Float32Array/Uint16Array/Uint32Array…）读成 C++ 数组。
///
/// ## 为什么分两个函数而不是一个模板
///
/// 元素宽度由**运行时**决定（three 的 index 是 Uint16 还是 Uint32 取决于顶点数），
/// 而目标容器类型不同（float / uint32）。分开写比在模板里做 `if (bytes == 2)` 清楚。
[[nodiscard]] auto read_as_float(JSContext* ctx, JSValueConst value) -> std::vector<float> {
  std::vector<float> out;
  if (JS_GetTypedArrayType(value) < 0) return out;
  std::size_t byte_offset = 0;
  std::size_t byte_length = 0;
  std::size_t bytes_per_element = 0;
  JsRef buffer{ctx, JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &byte_length,
                                          &bytes_per_element)};
  if (buffer.is_exception()) {
    JS_GetException(ctx);   // 清掉挂起异常
    return out;
  }
  std::size_t buffer_length = 0;
  std::uint8_t* raw = JS_GetArrayBuffer(ctx, &buffer_length, buffer.get());
  if (raw == nullptr) return out;
  if (bytes_per_element != sizeof(float)) return out;
  out.resize(byte_length / sizeof(float));
  if (!out.empty()) std::memcpy(out.data(), raw + byte_offset, byte_length);
  return out;
}

[[nodiscard]] auto read_as_index(JSContext* ctx, JSValueConst value) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> out;
  const int type = JS_GetTypedArrayType(value);
  if (type < 0) return out;
  std::size_t byte_offset = 0;
  std::size_t byte_length = 0;
  std::size_t bytes_per_element = 0;
  JsRef buffer{ctx, JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &byte_length,
                                          &bytes_per_element)};
  if (buffer.is_exception()) {
    JS_GetException(ctx);
    return out;
  }
  std::size_t buffer_length = 0;
  std::uint8_t* raw = JS_GetArrayBuffer(ctx, &buffer_length, buffer.get());
  if (raw == nullptr) return out;
  const std::size_t count = byte_length / bytes_per_element;
  out.resize(count);
  const std::uint8_t* base = raw + byte_offset;
  for (std::size_t i = 0; i < count; ++i) {
    if (bytes_per_element == 2) {
      std::uint16_t v = 0;
      std::memcpy(&v, base + i * 2, 2);
      out[i] = v;
    } else if (bytes_per_element == 4) {
      std::uint32_t v = 0;
      std::memcpy(&v, base + i * 4, 4);
      out[i] = v;
    } else {
      out.clear();
      return out;
    }
  }
  return out;
}

/// 从 three 的 `Color`（`toArray()`）读出线性 RGB。
///
/// ## 口径为什么可以直接用
///
/// three 在 `ColorManagement.enabled`（默认 true）时，`new Color(0xd08a50)` 会**立刻**
/// 把 sRGB 十六进制解码成线性工作空间（`workingColorSpace = "srgb-linear"`），
/// 于是 `toArray()` 给出的是**线性值**（实测 0xd08a50 → `[0.6308, 0.2542, 0.0802]`）。
///
/// 本框架 `Material::base_color` 也要求线性值。两边口径**天然一致**，不需要再转换
/// ——这是探针实测确认的，不是猜的（见 docs/three3d.md）。
[[nodiscard]] auto read_color(JSContext* ctx, JSValueConst array) -> math::Vec3 {
  math::Vec3 out{1.0f, 1.0f, 1.0f};
  for (int i = 0; i < 3; ++i) {
    JsRef component{ctx, JS_GetPropertyUint32(ctx, array, static_cast<std::uint32_t>(i))};
    double value = 1.0;
    JS_ToFloat64(ctx, &value, component.get());
    (&out.x)[i] = static_cast<float>(value);
  }
  return out;
}

}  // namespace

struct ThreeRuntime::Impl {
  JSRuntime* runtime{nullptr};
  JSContext* context{nullptr};
  bool ready{false};
  std::string failure_reason{};
  int timeout_ms{2000};
  std::size_t memory_limit{64U * 1024U * 1024U};
  std::string revision{};

  Impl() { boot(); }
  ~Impl() { shutdown(); }

  Impl(const Impl&) = delete;
  auto operator=(const Impl&) -> Impl& = delete;

  void shutdown() {
    if (context != nullptr) {
      JS_FreeContext(context);
      context = nullptr;
    }
    if (runtime != nullptr) {
      JS_FreeRuntime(runtime);
      runtime = nullptr;
    }
    ready = false;
  }

  /// 宿主函数：`console.*` 的落地。
  ///
  /// ## 为何日志要真的打出来（而不是丢掉）
  ///
  /// 沙箱里给一个“能调但不输出”的 `console` 能消除报错，但会造出新的困惑源：
  /// 用户打了 `console.log(geo)` 查到的东西是空的，而**没有任何解释**。
  /// 而这类场景（写三维脚本）里，看属性结构是最主要的调试手段。
  ///
  /// 输出走 `st::log`（与框架其他部分同一出口，会被 `--log` 收进日志）。
  static auto console_log(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                          JSValueConst* argv) -> JSValue {
    std::string line;
    for (int i = 0; i < argc; ++i) {
      if (i > 0) line += ' ';
      // 用 toString 而不是 JSON：三维对象里大量循环引用，stringify 会抛。
      const char* text = JS_ToCString(ctx, argv[i]);
      if (text != nullptr) {
        line += text;
        JS_FreeCString(ctx, text);
      }
    }
    ST_LOG_INFO("[three-script] {}", line);
    return JS_UNDEFINED;
  }

  void boot() {
    const auto source = three_source();
    if (source.empty()) {
      failure_reason = "three.js 源码为空（嵌入式资源缺失：third_party/three/three.cjs）";
      return;
    }
    runtime = JS_NewRuntime();
    if (runtime == nullptr) {
      failure_reason = "QuickJS 运行时创建失败";
      return;
    }
    JS_SetMemoryLimit(runtime, memory_limit);
    JS_SetMaxStackSize(runtime, 4U * 1024U * 1024U);
    if (timeout_ms > 0) {
      JS_SetInterruptHandler(runtime, &Impl::on_interrupt, this);
    }
    context = JS_NewContext(runtime);
    if (context == nullptr) {
      failure_reason = "QuickJS 上下文创建失败";
      return;
    }

    // ## three.js 的加载环境
    //
    // three.cjs 是 **CJS**（`exports.xxx = ...`）。给它一个假 `module`/`exports`，
    // 加载完从 `module.exports` 取命名空间。
    //
    // 同时给几个**必需的桩**（都是纯 JS，不涉及系统能力）：
    // - `self`/`window`：three 在若干地方探"是不是浏览器"；
    // - `performance.now`：动画/时钟用（给了之后 `Clock` 可用）；
    // - `TextEncoder`：`BufferGeometry.toJSON`、字体/文本相关路径会摸它；
    // - `console`：场景脚本最普通的排查手段（缺它会让整份脚本报错中断）；
    // - `ATOB`：three 的 `Loader`/`toArray` 相关路径会用它解码 base64。
    //
    // 最后一个 `__st_console_log` 是**宿主函数**（下面注册），把日志送到 `st::log`。
    {
      JSValue global = JS_GetGlobalObject(context);
      JS_SetPropertyStr(context, global, "__st_console_log",
                        JS_NewCFunction(context, &Impl::console_log, "__st_console_log", 2));
      JS_FreeValue(context, global);
    }
    static constexpr std::string_view kShim = R"JS(
var self = globalThis;
var window = globalThis;
var module = { exports: {} };
var exports = module.exports;
if (typeof performance === 'undefined') {
  var __now_base = 0;
  performance = { now: function () { return __now_base; } };
}
if (typeof TextEncoder === 'undefined') {
  TextEncoder = function () {
    this.encode = function (text) {
      var bytes = [];
      for (var i = 0; i < text.length; i++) {
        var code = text.charCodeAt(i);
        if (code < 0x80) {
          bytes.push(code);
        } else if (code < 0x800) {
          bytes.push(0xC0 | (code >> 6), 0x80 | (code & 0x3F));
        } else {
          bytes.push(0xE0 | (code >> 12), 0x80 | ((code >> 6) & 0x3F), 0x80 | (code & 0x3F));
        }
      }
      return new Uint8Array(bytes);
    };
  };
}
// ## `console`：不能只修“报错”，还要让日志真的看得到
//
// 场景脚本里写 `console.log(...)` 排查问题是最普通的手法，而沙箱里原本没有它
// → 一句 `console is not defined` 就把整个脚本打断（连场景都出不来）。
// three 自己也在若干路径上打 warning（例如未知属性）。
//
// 输出口径：走宿主日志（`st::log`）而不是丢弃——丢弃会让“我打了日志但什么都看不到”
// 变成新的困惑源。
if (typeof console === 'undefined') {
  console = {
    log: function () { __st_console_log('log', arguments); },
    info: function () { __st_console_log('info', arguments); },
    warn: function () { __st_console_log('warn', arguments); },
    error: function () { __st_console_log('error', arguments); },
    debug: function () { __st_console_log('debug', arguments); },
  };
}
)JS";
    // ## 顺序很关键：**先打桩，再加载 three**
    //
    // 第一版写反了（先加载后打桩），报错 `exports is not defined`。原因：
    // three.cjs 是 CJS 产物，**顶层就在读写 `exports`/`module`**——桩没到位时
    // 第一行就抛异常，后面再打桩已经来不及。
    if (!eval_string(std::string(kShim), "<three-shim>")) {
      failure_reason = "three 环境桩注入失败: " + last_error;
      return;
    }
    if (!eval_into(source, "three.cjs")) {
      failure_reason = "three.js 加载失败: " + last_error;
      return;
    }
    if (!eval_string("var THREE = module.exports; window.THREE = THREE;", "<three-bind>")) {
      failure_reason = "three 命名空间绑定失败: " + last_error;
      return;
    }
    // 自检：读一次 REVISION，确认命名空间真的可用（并记下来供诊断）
    {
      JSValue value = JS_Eval(context, "THREE.REVISION", 14, "<three-revision>",
                              JS_EVAL_TYPE_GLOBAL);
      JsRef owned{context, value};
      if (owned.is_exception()) {
        failure_reason = "three 运行时自检失败: " + take_exception_text(context);
        return;
      }
      const char* text = JS_ToCString(context, owned.get());
      if (text != nullptr) {
        revision = text;
        JS_FreeCString(context, text);
      }
    }
    ready = true;
  }

  /// 最近一次失败的描述（`evaluate` 把它带进 `Result` 的错误消息）。
  std::string last_error{};

  // ── 超时控制 ──
  //
  // ## 为什么用中断回调而不是“外部杀线程”
  //
  // 外部杀线程会留下半执行的 JS 堆（引用计数错乱、后续求值全崩）。QuickJS 提供的
  // `JS_SetInterruptHandler` 是在解释器循环里周期回调，返回 1 就是“抛个异常停下来”，
  // 堆仍然一致。这就是本仓 `ScriptLimits::timeout` 的同一条机制。
  //
  // ## 端点：**跳过中间调用回帧**
  //
  // 回调里不能做重活（它每几千条指令就被叫一次）。这里只读一个单调时钟。
  std::chrono::steady_clock::time_point deadline{};
  bool timing{false};

  /// 中断回调：超过时限就打断（死循环不会挂死导出）。
  static auto on_interrupt(JSRuntime* /*runtime*/, void* opaque) -> int {
    auto* self = static_cast<Impl*>(opaque);
    if (!self->timing) return 0;
    return std::chrono::steady_clock::now() >= self->deadline ? 1 : 0;
  }

  /// 进入一次受时限保护的求值。
  void begin_timing() {
    if (timeout_ms <= 0) {
      timing = false;
      return;
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    timing = true;
  }
  void end_timing() { timing = false; }

  [[nodiscard]] auto eval_string(std::string code, std::string filename) -> bool {
    last_error.clear();
    JSValue value = JS_Eval(context, code.c_str(), code.size(), filename.c_str(),
                            JS_EVAL_TYPE_GLOBAL);
    JsRef owned{context, value};
    if (owned.is_exception()) {
      last_error = take_exception_text(context);
      return false;
    }
    return true;
  }

  [[nodiscard]] auto eval_into(std::string_view source, std::string filename) -> bool {
    last_error.clear();
    JSValue value = JS_Eval(context, source.data(), source.size(), filename.c_str(),
                            JS_EVAL_TYPE_GLOBAL);
    JsRef owned{context, value};
    if (owned.is_exception()) {
      last_error = take_exception_text(context);
      return false;
    }
    return true;
  }

  [[nodiscard]] auto eval_value(std::string_view source, std::string filename) -> JSValue {
    last_error.clear();
    JSValue value = JS_Eval(context, source.data(), source.size(), filename.c_str(),
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
      last_error = take_exception_text(context);
    }
    return value;
  }
};

ThreeRuntime::ThreeRuntime() : impl_(std::make_unique<Impl>()) {}
ThreeRuntime::~ThreeRuntime() = default;

auto ThreeRuntime::available() -> bool {
  try {
    auto& rt = instance();
    return rt.impl_ != nullptr && rt.impl_->ready;
  } catch (...) {
    return false;
  }
}

auto ThreeRuntime::unavailable_reason() -> std::string {
  try {
    auto& rt = instance();
    if (rt.impl_ == nullptr) return "运行时未初始化";
    return rt.impl_->ready ? std::string() : rt.impl_->failure_reason;
  } catch (const std::exception& error) {
    return std::string("运行时构造抛出异常: ") + error.what();
  }
}

auto ThreeRuntime::instance() -> ThreeRuntime& {
  thread_local ThreeRuntime runtime;
  return runtime;
}

void ThreeRuntime::set_timeout_ms(int milliseconds) { impl_->timeout_ms = milliseconds; }

void ThreeRuntime::set_memory_limit(std::size_t bytes) {
  // ## 为什么内存上限只能在构造前生效
  //
  // `JS_SetMemoryLimit` 是对**已存在的运行时**设的，改不了已经分配的堆。
  // 本实现把初始值跟在 `boot()` 里设的那一个对齐；若调用方在构造后改，
  // 只能影响**下次**重新建运行时（本层没有提供重建入口——重建会把已求值的
  // 场景对象全丢掉，语义上是另一个运行时）。
  //
  // 所以这里只更新记录值并**如实记日志**，不假装生效。
  if (impl_->runtime != nullptr && impl_->ready) {
    ST_LOG_WARN("three 运行时：内存上限在运行时已启动后不再生效（当前 {} MiB）；"
                "要改请在首次求值前设置",
                impl_->memory_limit / (1024 * 1024));
    return;
  }
  impl_->memory_limit = bytes;
}

auto ThreeRuntime::evaluate(std::string_view script, std::string_view filename)
    -> Result<ThreeScene> {
  if (impl_ == nullptr) {
    return unexpected(ErrorCode::Unsupported, "three 运行时未初始化");
  }
  if (!impl_->ready) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("three.js 不可用: {}", impl_->failure_reason));
  }

  ThreeScene scene;

  // ── 让脚本跑起来 ──
  //
  // ## 为什么要包在 IIFE 里（这一条是踩出来的）
  //
  // 直接 `JS_Eval` 脚本是在**全局作用域**执行，于是模型写的
  // `const scene = new THREE.Scene();` 会把 `scene` 声明到全局。
  // 后果：同一个运行时（线程内单例）里**第二次**求值一份同样写法的脚本时，
  // 报 `redeclaration of 'scene'`——而错误点指向用户完全无辜的第一行。
  //
  // 实测就是这样：测试里第一次跑通了，隔两个用例再跑就报重声明。
  // 而真实使用（裁云逐帧、预览+导出二次渲染）必然多次求值。
  //
  // 修法：把脚本包进 `(function(){ ... })()`， `const`/`let` 落在函数作用域里，
  // 每次求值天然独立。同时把**最后一个表达式的值**作为返回值——
  // 这是在 IIFE 里保留“语句列表最后一句就是返回值”这个有用语义的唯一办法。
  //
  // 怎么做：把最后一行（或末尾那个非空表达式语句）自动改写为 `return <expr>;`。
  // 不是盲目加 return：若最后一句本身是 `return`/块/声明，就不改（那本来就无值）。
  const std::string original(script);
  const std::string script_name(filename);

  // ## 每次求值前清掉上一轮的 bpy 资产（**跨场景不泄露**）
  //
  // `THREE` 命名空间属于线程内单例运行时，会跨求值残留。资产是**场景级**的
  // （属于“这一次求值”），不清就会：上一个带资产的场景跑完，下一个不带资产的
  // 脚本仍能拿到（实测踩到：反证用例——单独跑组装脚本应当失败——恒绿，
  // 因为它拿到了上一轮残留的几何）。
  //
  // 一句 `= {}` 的开销可忽略，换来的语义是“场景之间互不影响”。
  (void)impl_->eval_string("if (typeof THREE !== 'undefined') THREE.__assets = undefined;",
                          "<reset-assets>");

  // 自动给最后一条“表达式语句”加 `return`。
  //
  // 详细约定见下面 lambda 顶部那段：要点是**按语句边界**识别（不是按行），
  // 因为跨行表达式与一行多语句都是合法写法。
    // ══════════════════════════════════════════════════════════════════
    // 取「最后一条顶层语句」并给它加 `return`
    // ══════════════════════════════════════════════════════════════════
    //
    // ## 为何不能“只看最后一行”（旧实现的真缺陷）
    //
    // 旧代码只取最后一行、去掉行尾 `;`、然后在行首插 `return`。三种常见写法全坏：
    //
    // | 写法 | 旧实现的结果 | 现象 |
    // |---|---|---|
    // | 表达式**跨行**（`new THREE.Mesh(\n  geo, mat)`） | `return new THREE.Mesh(` + 换行 | `unexpected token in expression` |
    // | 一行里多个语句（`s.add(l);s.add(x);s;`） | `return s.add(l);s.add(x);s;` | 只返回第一个结果 |
    // | 末行是块结尾（`闭花括号` 后面的换行） | 识别为空行 → 不加 return | 场景丢 |
    //
    // 实测：一组 20 条能力脚本里有 9 条因此报“语法错”，而它们写的都是合法 JS。
    // 最坑的是错误信息指向用户完全无辜的最后一行。
    //
    // ## 现在怎做
    //
    // 逐字符扫描（跳过字符串/模板/正则/注释），按**顶层 `;`与顶层平衡的 `闭花括号`**
    // 切出语句边界，取最后一段非空语句。然后分三种情形：
    //
    // 1. 以 `;` 结尾 → 它就是一条语句，直接加 `return `；
    // 2. 以 `闭花括号` 结尾（块结构，如 `for(...)开花括号...闭花括号`）→ **不加** return：
    //    块无值，而且 `return 开花括号...闭花括号` 会把一个块当对象字面量解析（语义完全不同）；
    // 3. 其余（末尾无分号）→ 也加 `return `（ASI 风格的最后一行表达式）。
    //
    // 已有 `return`/声明/控制流关键字的，一律不加。
    const auto as_iife = [](const std::string& source) -> std::string {
      // ── 扫描器：找到“最后一个顶层语句”的起始位置 ──
          std::size_t statement_start = 0;
      // 块语句的起点（`for (...) 开花括号 ... 闭花括号` 的 `for` 位置）。
      //
      // ## 为何需要单独一个游标（实测代价）
      //
      // 顶层 `闭花括号` 也意味着“一条语句结束”，但它结束的那条语句 **从块的头部开始**，
      // 不是从 `闭花括号` 之后。旧写法直接 `statement_start = i + 1`，于是遇到
      // “末行是 `闭花括号` 后面的换行”时就把最后一条顶层语句当成了空串——
      // 场景因此丢掉（而错误里什么都没有，最难查）。
      //
      // 现在两个游标分工：
      // · `statement_start`：最近一条**已结束**语句之后的游标（给下一句用）；
      // · `block_statement_start`：最近一个**顶层块语句的开始位置**。
      std::size_t block_statement_start = 0;
      std::size_t last_completed_statement = 0;   // 最后一条**非空**语句的起点
      int depth_paren_last = 0;
      (void)depth_paren_last;
    int depth_paren = 0;   // ( )
      int depth_brace = 0;   // 开花括号 闭花括号
      int depth_bracket = 0; // [ ]
      bool in_string = false;
      bool in_template = false;
      bool in_line_comment = false;
      bool in_block_comment = false;
      char quote = '\0';
      for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (in_line_comment) {
          if (ch == '\n') in_line_comment = false;
          continue;
        }
        if (in_block_comment) {
          if (ch == '*' && next == '/') {
            in_block_comment = false;
            ++i;
          }
          continue;
        }
        if (in_string || in_template) {
          if (ch == '\\') {
            ++i;   // 跳过转义的下一个字符
            continue;
          }
          if (ch == quote) {
            in_string = false;
            in_template = false;
            quote = '\0';
          }
          continue;
        }
        if (ch == '/' && next == '/') {
          in_line_comment = true;
          ++i;
          continue;
        }
        if (ch == '/' && next == '*') {
          in_block_comment = true;
          ++i;
          continue;
        }
        if (ch == '\'' || ch == '"' || ch == '`') {
          in_string = true;
          in_template = (ch == '`');
          quote = ch;
          continue;
        }
        switch (ch) {
          case '(': ++depth_paren; break;
          case ')': --depth_paren; break;
          case '[': ++depth_bracket; break;
          case ']': --depth_bracket; break;
          case '{':
            // 顶层 `开花括号` **第一次**出现的位置就是 block 语句的开头（`for (...) 开花括号`）。
            if (depth_paren == 0 && depth_brace == 0 && depth_bracket == 0) {
              block_statement_start = statement_start;
            }
            ++depth_brace;
            break;
          case '}': --depth_brace; break;
          case ';':
            // **只在全部深度归零时**才是一条顶层语句的边界。
            // 少了这个判定，`new THREE.Mesh(a, b);` 里的分号（若在括号内）
            // 会把一条语句劈成两半。
            if (depth_paren == 0 && depth_brace == 0 && depth_bracket == 0) {
              last_completed_statement = statement_start;
              statement_start = i + 1;
            }
            break;
          default: break;
        }
        // 顶层 `闭花括号` 收尾一个块（`for 开花括号 ... 闭花括号` / `function 开花括号 ... 闭花括号`）：块本身就是一条语句。
        // 它结束的语句**从块头开始**（不是从 `闭花括号` 之后）——这是上面两个游标存在的原因。
        if (ch == '}' && depth_paren == 0 && depth_brace == 0 && depth_bracket == 0) {
          last_completed_statement = block_statement_start;
          statement_start = i + 1;
        }
        if (depth_paren < 0 || depth_brace < 0 || depth_bracket < 0) {
          // 不平衡（用户写错了）：放弃改造，让真编译器报原错（错误信息才准）。
          return "(function(){ 'use strict';\n" + source + "\n})()";
        }
      }
      // ── 最后一段（末尾无分号的尾巴）与“最后一条已结束语句”取更靠后的那个 ──
      //
      // 三种尾巴都要对：
      // · `scene;`      → 尾巴是空白，用 `last_completed_statement`（即 `scene`）
      // · `scene`       → 尾巴就是 `scene`
      // · `for()开花括号...闭花括号` → `闭花括号` 已把 `last_completed_statement` 设为块头
      const std::size_t trailing_start = statement_start;
      const std::size_t last_statement_start =
          source.find_first_not_of(" \t\r\n", trailing_start) == std::string::npos
              ? last_completed_statement
              : trailing_start;
    // ⚠ **`return ` 后面紧跟 `开花括号` 会把块变成对象字面量**——所以这里的目标是“最后一条
    // 顶层语句”，而块结构（以 `开花括号` 开头、`闭花括号` 结尾）本身就不该加 return。
    // 旧实现里那条 “以 `闭花括号` 或 `开花括号` 结尾就不碰” 的判定保留下来，但现在它作用在
    // **整条语句**上（而不是一行），于是多行块也能正确识别。
      auto begin = last_statement_start;
      while (begin < source.size() &&
             (source[begin] == ' ' || source[begin] == '\t' || source[begin] == '\n' ||
              source[begin] == '\r')) {
        ++begin;
      }
      // 末尾向前跳过空白
      auto end = source.size();
      while (end > begin && (source[end - 1] == '\n' || source[end - 1] == '\r' ||
                             source[end - 1] == ' ' || source[end - 1] == '\t')) {
        --end;
      }
      std::string_view tail(source.data() + begin, end > begin ? end - begin : 0);

      // 尾部去掉行尾注释（`scene; // 注释`），否则关键字判定会被注释干扰。
      if (const std::size_t comment = tail.rfind("//"); comment != std::string_view::npos &&
          tail.find('\n', comment) == std::string_view::npos) {
        tail = tail.substr(0, comment);
        while (!tail.empty() && (tail.back() == ' ' || tail.back() == '\t')) {
          tail.remove_suffix(1);
        }
      }
      if (tail.empty()) {
        return "(function(){ 'use strict';\n" + source + "\n})()";
      }

      const std::string text(tail);
      auto head = text;
      while (!head.empty() && (head.back() == ';' || head.back() == ' ' || head.back() == '\t')) {
        head.pop_back();
      }
      std::size_t first = head.find_first_not_of(" \t");
      const std::string head_trimmed = first == std::string::npos ? "" : head.substr(first);

      bool needs_return = !head_trimmed.empty();
      if (needs_return) {
        for (const char* keyword : {"return", "const ", "let ", "var ", "function", "class ",
                                    "import", "export", "if ", "if(", "for ", "for(", "while ",
                                    "while(", "switch", "try", "do ", "do{", "throw", "else",
                                    "case ", "default:", "async "}) {
          if (head_trimmed.rfind(keyword, 0) == 0) {
            needs_return = false;
            break;
          }
        }
        // 以 `闭花括号` 结尾：块结构无值。**必须不加**——`return 开花括号 … 闭花括号` 会被解析成
        // 对象字面量，而块里的语句全部失效（语义静默地变了）。
        if (needs_return && (head_trimmed.back() == '}' || head_trimmed.back() == '{')) {
          needs_return = false;
        }
      }

      std::string body = source.substr(0, begin);
      if (needs_return) {
        // 保留原缩进与换行（`text` 里可能有换行）：只在前面插 `return `。
        body += "return " + text + ";\n";
      } else {
        body += source.substr(begin, end - begin);
        body += '\n';
      }
      return "(function(){ 'use strict';\n" + body + "})()";
    };

  impl_->begin_timing();
  JSValue result = impl_->eval_value(as_iife(original), script_name);
  impl_->end_timing();
  JsRef result_owned{impl_->context, result};
  if (result_owned.is_exception()) {
    std::string message = impl_->last_error;
    // 行号偏差：包了一层 IIFE 与 `'use strict'`，用户看到的行号会 +1。
    // 不在这里做代偿减法——那会让“错误行号”与“报告行号”分叉，更难查。
    // 真实代价是“行号差一行”，而源文件名是对的，足够定位。
    const bool timed_out = message.find("interrupted") != std::string::npos ||
                           message.find("out of memory") != std::string::npos;
    return unexpected(timed_out ? ErrorCode::Timeout : ErrorCode::Parse, message);
  }

  // 找到场景根：优先用返回值，其次找全局 `scene` / `root` / `group`
  JsRef scene_root{impl_->context, JS_UNDEFINED};
  const auto looks_like_scene = [&](JSValueConst value) -> bool {
    if (!JS_IsObject(value)) return false;
    return get_bool(impl_->context, value, "isScene", false) ||
           get_bool(impl_->context, value, "isGroup", false) ||
           get_bool(impl_->context, value, "isMesh", false) ||
           get_bool(impl_->context, value, "isObject3D", false);
  };

  if (looks_like_scene(result_owned.get())) {
    scene_root = JsRef{impl_->context, JS_DupValue(impl_->context, result_owned.get())};
  } else if (JS_IsArray(result_owned.get())) {
    // 数组 = 多个根：包一个 Group 统一处理
    if (!impl_->eval_string("({ root: new THREE.Group() })", "<wrap>")) {
      return unexpected(ErrorCode::Parse, impl_->last_error);
    }
    // 简化：数组情况让下面的脚本自己合并（见 extract）
    scene_root = JsRef{impl_->context, JS_DupValue(impl_->context, result_owned.get())};
  } else {
    // 全局名兜底（兼容“脚本最后一句是调用、没有返回值”的写法）：
    // 因为脚本已包进 IIFE，`const scene` **不会**落到全局。所以这里换成
    // “让脚本把根暴露到 window.__three_root__”：先看 IIFE 的返回值，
    // 没有就在隔离环境里重跑一次并显式抓常用变量名。
    for (const char* candidate : {"scene", "root", "group", "world"}) {
      const std::string probe = "(function(){ 'use strict'; try { return " +
                                std::string(candidate) + "; } catch(e) { return undefined; } })()";
      JSValue probe_value = impl_->eval_value(probe, "<probe>");
      if (JS_IsException(probe_value)) {
        JS_FreeValue(impl_->context, JS_GetException(impl_->context));
        JS_FreeValue(impl_->context, probe_value);
        continue;
      }
      if (looks_like_scene(probe_value)) {
        scene_root = JsRef{impl_->context, probe_value};
        break;
      }
      JS_FreeValue(impl_->context, probe_value);
    }
  }
  if (!JS_IsObject(scene_root.get())) {
    return unexpected(ErrorCode::Invalid,
                      "脚本没有产出三维场景：最后一个表达式应是 three 的 Scene/Group/Mesh，"
                      "或定义一个名为 scene 的全局变量");
  }

  // ── 抽取：把整个场景翻译成 C++ 结构 ──
  //
  // 为什么一次 eval 铺平成数组：跨界每多一次就多一次类型转换与引用计数的机会。
  // 这里让 JS 侧先整理成"纯数据"，C++ 只需按固定字段读。
  {
    JsRef global{impl_->context, JS_GetGlobalObject(impl_->context)};
    JS_SetPropertyStr(impl_->context, global.get(), "__three_root__",
                      JS_DupValue(impl_->context, scene_root.get()));
  }

  static constexpr std::string_view kExtract = R"JS(
(function () {
  var root = window.__three_root__;
  if (!root) throw new Error('场景根丢失');

  // 数组形态：多个根合成一个临时 Group（不修改调用方的对象图）
  var container = root;
  if (Array.isArray(root)) {
    container = new THREE.Group();
    for (var i = 0; i < root.length; i++) container.add(root[i]);
  }
  container.updateMatrixWorld(true);

  var out = { objects: [], warnings: [], revision: THREE.REVISION,
              camera: null, lights: [] };

  // —— 相机 ——
  var camera = null;
  if (container.isCamera) {
    camera = container;
  } else {
    container.traverse(function (o) { if (!camera && o.isCamera) camera = o; });
  }
  if (!camera) out.warnings.push('场景里没有相机（试试 scene.add(camera)）');
  else {
    // ## 从 `matrixWorld` 直接取朝向，不从 quaternion 反算
    //
    // 实测踩到的 QuickJS 数值缺陷：`camera.lookAt(...)` 之后，
    // `new Vector3(0,0,-1).applyQuaternion(camera.quaternion)` 得到的方向**角度普遍减半**
    // （lookAt 到 45° 的位置，抽出来只有 ~22°）。而手写 `quaternion.setFromAxisAngle`
    // 却是准的——说明问题在 `setFromRotationMatrix` 这条路径上。
    //
    // 症状：场景里写的 `camera.lookAt(...)` **完全不起作用**，渲染出来是默认朝向。
    // 而 three_check 用的是同一个抽取——所以两边一起错，很难察觉。
    //
    // 修法：视角方向取 `matrixWorld` 的第三列（相机局部 +Z 在世界里的方向），
    // 这是渲染管线自己的口径，不经过四元数。相机看向 -Z，故取反。
    camera.updateWorldMatrix(true, false);
    var e = camera.matrixWorld.elements;
    // 列主序：第 3 列是 (e[8], e[9], e[10])，即相机局部 +Z 在世界的方向
    var dir = { x: -e[8], y: -e[9], z: -e[10] };
    var eye = camera.position;
    var target = [eye.x + dir.x, eye.y + dir.y, eye.z + dir.z];
    var isOrtho = !!camera.isOrthographicCamera;
    out.camera = {
      eye: [eye.x, eye.y, eye.z],
      target: target,
      up: [camera.up.x, camera.up.y, camera.up.z],
      fov: camera.fov !== undefined ? camera.fov : 45,
      near: camera.near !== undefined ? camera.near : 0.1,
      far: camera.far !== undefined ? camera.far : 1000,
      orthographic: isOrtho,
      // 正交相机的"垂直可视高度"= (top - bottom) / zoom
      ortho_scale: isOrtho ? ((camera.top - camera.bottom) / (camera.zoom || 1)) : 2,
      aspect: camera.aspect !== undefined ? camera.aspect : 1
    };
  }

  // —— 灯光 ——
  container.traverse(function (o) {
    if (!o.isLight) return;
    var kind = 'directional';
    if (o.isPointLight) kind = 'point';
    else if (o.isSpotLight) kind = 'spot';
    else if (o.isAmbientLight) kind = 'ambient';
    else if (o.isHemisphereLight) kind = 'hemisphere';
    var world = new THREE.Vector3();
    o.getWorldPosition(world);
    var dir = new THREE.Vector3();
    if (o.isDirectionalLight || o.isSpotLight) {
      // 平行光/聚光：three 的语义是"从 position 射向 target"
      var t = new THREE.Vector3();
      if (o.target && o.target.getWorldPosition) o.target.getWorldPosition(t);
      dir.copy(t).sub(world);
      if (dir.lengthSq() < 1e-9) dir.set(0, -1, 0);
      dir.normalize();
    }
    out.lights.push({
      kind: kind,
      intensity: o.intensity || 0,
      color: o.color ? o.color.toArray() : [1, 1, 1],
      position: [world.x, world.y, world.z],
      direction: [dir.x, dir.y, dir.z],
      distance: o.distance || 0,
      angle: o.angle !== undefined ? o.angle : 0,
      penumbra: o.penumbra !== undefined ? o.penumbra : 0,
      groundColor: o.groundColor ? o.groundColor.toArray() : [0, 0, 0]
    });
  });

  // —— 网格 ——
  container.traverse(function (o) {
    if (!o.isMesh || !o.geometry || !o.geometry.attributes.position) return;
    if (!o.visible) return;
    var g = o.geometry;
    var m = o.material;
    // 材质可能是数组（多材质网格）：当前只取第一个（多材质要拆组，见文档的边界说明）
    if (Array.isArray(m)) m = m[0];
    var side = 0;   // 0=Front 1=Back 2=Double
    if (m) {
      if (m.side === THREE.DoubleSide) side = 2;
      else if (m.side === THREE.BackSide) side = 1;
    }
    out.objects.push({
      name: o.name || '',
      position: g.attributes.position.array,
      normal: g.attributes.normal ? g.attributes.normal.array : null,
      uv: g.attributes.uv ? g.attributes.uv.array : null,
      index: g.index ? g.index.array : null,
      world: o.matrixWorld.elements,
      color: m && m.color ? m.color.toArray() : [1, 1, 1],
      metalness: m && m.metalness !== undefined ? m.metalness : 0,
      roughness: m && m.roughness !== undefined ? m.roughness : 1,
      opacity: m && m.opacity !== undefined ? m.opacity : 1,
      transparent: m ? !!m.transparent : false,
      depthWrite: m && m.depthWrite !== undefined ? !!m.depthWrite : true,
      emissive: m && m.emissive ? m.emissive.toArray() : [0, 0, 0],
      emissiveIntensity: m && m.emissiveIntensity !== undefined ? m.emissiveIntensity : 0,
      side: side,
      // 记录材质类型：MeshBasicMaterial 不参与光照（unlit），语义要带到渲染侧
      materialType: m ? (m.type || '') : '',
      // ## 运动意图（`userData`）
      //
      // 轮系/摆轮这类“只在转”的动画不该每帧重求值整个场景。
      // 这里只把意图（轴/速度/振幅/频率）带出去，实际旋转由渲染侧逐帧算。
      // 字段缺省时给 null，C++ 侧按 0 处理。
      //
      // ### 向上继承：查自身 + 所有祖先
      //
      // three 的习惯写法是把“这一组一起转”标在**组**上：
      //
      //     const rotor = new THREE.Group();
      //     rotor.userData.spin = { axis: [1,0,0], speed: 0.5 };
      //     rotor.add(fanBlade); rotor.add(compressorBlade); ...
      //
      // 那是**符合直觉的**：同一级的叶片是刚体，转的是转子而不是叶片。
      // 但抽取器只遍历 Mesh —— 不向上看，标记就**静默丢掉**：
      // 场景照常渲染、物体数正确、`three_anim_probe` 报“自转 0 个”，
      // 而画面就是不动。本仓实测就是这样（涡轮发动机双转子）。
      //
      // 所以这里向上找最近一个带标记的祖先。自己身上的优先。
      spin: (function () {
        for (var node = o; node; node = node.parent) {
          if (node.userData && node.userData.spin) return node.userData.spin;
        }
        return null;
      })(),
      oscillate: (function () {
        for (var node = o; node; node = node.parent) {
          if (node.userData && node.userData.oscillate) return node.userData.oscillate;
        }
        return null;
      })(),
      // 取景排除标记（同样向上继承：一块背景板可能是个组）
      noFrame: (function () {
        for (var node = o; node; node = node.parent) {
          if (node.userData && node.userData.noFrame) return true;
        }
        return false;
      })()
    });
  });

  return out;
})()
)JS";

  JSValue extracted = JS_UNDEFINED;
  impl_->begin_timing();
  extracted = impl_->eval_value(kExtract, "<three-extract>");
  impl_->end_timing();
  JsRef extracted_owned{impl_->context, extracted};
  if (extracted_owned.is_exception()) {
    return unexpected(ErrorCode::Parse,
                      std::format("场景抽取失败: {}", impl_->last_error));
  }

  scene.three_revision = get_string(impl_->context, extracted_owned.get(), "revision");

  // ── 对齐模式：`globalThis.renderer = { compat: 'three' }` / `scene.compat` ──
  //
  // ⚠ **必须在光照抽取之前**：光照段要看 `aligned` 才能把 three 的
  // “没写 AmbientLight 就是 0”和“背景不产生 IBL”两条语义落实。
  // 第一版把它写在函数末尾，于是光照段读到的还是旧默认值，结果背光面
  // 仍然被抬到 87（期望 76，差的正好是那个 0.35 常量环境）。
  bool aligned = false;
  {
    JsRef global_root{impl_->context, JS_GetGlobalObject(impl_->context)};
    JsRef renderer{impl_->context,
                   JS_GetPropertyStr(impl_->context, global_root.get(), "renderer")};
    const auto compat_of = [&](JSValueConst owner) -> bool {
      if (!JS_IsObject(owner)) return false;
      JsRef flag{impl_->context, JS_GetPropertyStr(impl_->context, owner, "compat")};
      if (!JS_IsString(flag.get())) return false;
      const char* raw = JS_ToCString(impl_->context, flag.get());
      if (raw == nullptr) return false;
      const std::string text{raw};
      JS_FreeCString(impl_->context, raw);
      return text == "three";
    };
    aligned = compat_of(renderer.get()) || compat_of(scene_root.get());
    if (aligned) {
      scene.three_parity = true;
      // three 没有“边缘光”；留着它会让轮廓一侧凭空亮 25%。
      scene.lighting.rim = 0.0f;
      // three 的纯色 `scene.background` **不产生 IBL**：环境反射必须关。
      scene.lighting.env_strength = 0.0f;
      scene.lighting.env_sky = math::Vec3{0.0f, 0.0f, 0.0f};
      scene.lighting.env_ground = math::Vec3{0.0f, 0.0f, 0.0f};
    }
  }

  // ── 相机 ──
  {
    JsRef camera{impl_->context,
                 JS_GetPropertyStr(impl_->context, extracted_owned.get(), "camera")};
    if (JS_IsObject(camera.get())) {
      const auto vec3 = [&](const char* key) {
        JsRef array{impl_->context, JS_GetPropertyStr(impl_->context, camera.get(), key)};
        return read_color(impl_->context, array.get());
      };
      const auto eye = vec3("eye");
      const auto target = vec3("target");
      scene.camera.eye = eye;
      scene.camera.target = target;
      // ## `up` 必须读（2026-11-05 修）——漏了这行整个 Z-up 场景会“躺倒”
      //
      // three 的相机默认 `up = (0, 1, 0)`（Y 轴朝上），而三维美术与 bpy
      // 的通用约定是 **Z 轴朝上**。脚本里写 `camera.up.set(0,0,1)` 就是为了
      // 让相机按 Z-up 算视图矩阵。
      //
      // 而这里原本**只读 eye 与 target，完全忽略 up**——于是脚本设的
      // `camera.up` 被默默丢弃，视图矩阵仍按 Y-up 算。
      //
      // 后果（小卖部示例实测）：竖直的侧墙渲染成一条从左上到右下的**斜板**，
      // 地台变成两个 L 形白块，屋顶横插进货架上方。
      //
      // 它**看起来像「几何建错了」或「相机角度不对」**——我为此改了三轮
      // 相机位置与 `lookAt` 目标，甚至怀疑过资产里的 z 坐标约定，全都没用。
      // 真正的错因是这里少了一行。
      //
      // 注：抽取端（`three_runtime` 的 JS 段第 683 行）**一直是输出 up 的**，
      // 断链只在这一处。
      {
        JsRef up_array{impl_->context,
                       JS_GetPropertyStr(impl_->context, camera.get(), "up")};
        if (JS_IsObject(up_array.get()) && !JS_IsUndefined(up_array.get())) {
          const math::Vec3 up = read_color(impl_->context, up_array.get());
          // 全零的 up 会让 `look_at` 退化（叉积为零）——只在长度足够时才采用
          const float length_sq = up.x * up.x + up.y * up.y + up.z * up.z;
          if (length_sq > 1e-6f) scene.camera.up = up;
        }
      }
      scene.camera.fov_y_degrees = static_cast<float>(get_double(impl_->context, camera.get(),
                                                                 "fov", 45.0));
      scene.camera.near_z = static_cast<float>(get_double(impl_->context, camera.get(), "near",
                                                         0.1));
      scene.camera.far_z = static_cast<float>(get_double(impl_->context, camera.get(), "far",
                                                         1000.0));
      if (get_bool(impl_->context, camera.get(), "orthographic", false)) {
        scene.camera.projection = ProjectionKind::Orthographic;
        scene.camera.ortho_scale = static_cast<float>(
            get_double(impl_->context, camera.get(), "ortho_scale", 2.0));
      }
    } else {
      scene.warnings.emplace_back("没有找到相机，使用默认相机");
    }
  }

  // ── 灯光 ──
  //
  // three 与霜天的语义差（**这一层必须翻译，不能照搬**）：
  //
  // | 项 | three | 霜天 |
  // |---|---|---|
  // | 光的方向 | `position → target`（**射出**方向） | `Light::direction` = **指向光源** |
  // | 环境光 | 独立的 `AmbientLight` 对象 | `Lighting::ambient`（标量强度） |
  // | 强度单位 | 无单位（美术调） | 乘在漫反射项上 |
  //
  // 方向差一个负号是这里最容易错的地方：漏了它，**灯永远照不到东西**，
  // 而画面表现为"一片黑"——看起来像"渲染器不工作"。（本仓的聚光曾经就踩过这个坑。）
  {
    JsRef lights{impl_->context,
                 JS_GetPropertyStr(impl_->context, extracted_owned.get(), "lights")};
    JsRef length_value{impl_->context, JS_GetPropertyStr(impl_->context, lights.get(), "length")};
    std::uint32_t light_count = 0;
    JS_ToUint32(impl_->context, &light_count, length_value.get());

    float ambient_from_light = 0.0f;
    bool saw_ambient_light = false;
    int directional_index = 0;
    int extra_index = 0;
    for (std::uint32_t i = 0; i < light_count; ++i) {
      JsRef light{impl_->context,
                  JS_GetPropertyUint32(impl_->context, lights.get(), i)};
      const std::string kind = get_string(impl_->context, light.get(), "kind");
      const float intensity = static_cast<float>(get_double(impl_->context, light.get(),
                                                            "intensity", 0.0));
      JsRef color_array{impl_->context,
                        JS_GetPropertyStr(impl_->context, light.get(), "color")};
      const math::Vec3 color = read_color(impl_->context, color_array.get());

      if (kind == "ambient") {
        // 环境光没有方向：累加到 `Lighting::ambient`（三色的均值，保持与旧路径一致的标量语义）。
        //
        // ⚠ 对齐模式（`Model::Three`）下不能只留标量：three 的 `AmbientLight`
        // 是 `intensity × color` 的**逐通道** irradiance，冷/暖环境光在两边会长得不同。
        // 所以同时把颜色**按亮度归一**地存进 `ambient_color`：
        //   ambient      = intensity × luma(color)
        //   ambient_color= color / luma(color)   ⇒ intensity × color 可重建
        // 旧模型不看 `ambient_color`，所以对它零影响。
        const float luma = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
        ambient_from_light += intensity * luma;
        saw_ambient_light = true;
        if (luma > 1e-6f) {
          scene.lighting.ambient_color =
              math::Vec3{color.x / luma, color.y / luma, color.z / luma};
        }
        continue;
      }
      if (kind == "hemisphere") {
        // 半球光：本框架没有这个类型。用"环境项 + 弱的顶部平行光"近似，
        // 而不是直接丢掉（丢掉会让模型写的场景整体偏暗，而它不知道为什么）。
        JsRef ground_array{impl_->context,
                           JS_GetPropertyStr(impl_->context, light.get(), "groundColor")};
        const math::Vec3 ground = read_color(impl_->context, ground_array.get());
        const float sky_luma = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
        const float ground_luma =
            0.2126f * ground.x + 0.7152f * ground.y + 0.0722f * ground.z;
        ambient_from_light += intensity * (sky_luma + ground_luma) * 0.5f;
        continue;
      }

      JsRef direction_array{impl_->context,
                            JS_GetPropertyStr(impl_->context, light.get(), "direction")};
      const math::Vec3 outgoing = read_color(impl_->context, direction_array.get());
      // ⚠ 取反：three 的方向是"射出去"，霜天要的是"指向光源"
      const math::Vec3 toward_light{-outgoing.x, -outgoing.y, -outgoing.z};
      JsRef position_array{impl_->context,
                           JS_GetPropertyStr(impl_->context, light.get(), "position")};
      const math::Vec3 position = read_color(impl_->context, position_array.get());

      if (kind == "directional" && directional_index == 0) {
        // 第一个平行光当主光（与本框架的 `light_from` 语义一致）
        scene.lighting.light_from = toward_light;
        scene.lighting.diffuse = intensity;
        // 主光颜色：three 的 `Irradiance = dotNL × color × intensity`，
        // 颜色必须单独带下去（旧模型只看亮度）。
        scene.lighting.light_color = color;
        ++directional_index;
        continue;
      }
      if (extra_index >= static_cast<int>(kMaxExtraLights)) {
        scene.warnings.push_back(std::format(
            "灯光数量超过上限（{}），第 {} 个及以后被忽略", kMaxExtraLights, extra_index + 1));
        break;
      }
      Light& slot = scene.lighting.extra_lights[static_cast<std::size_t>(extra_index)];
      slot.color = color;
      slot.intensity = intensity;
      const float range = static_cast<float>(get_double(impl_->context, light.get(), "distance",
                                                        0.0));
      if (kind == "point") {
        slot.kind = LightKind::Point;
        slot.position = position;
        slot.radius = range;   // three 的 distance 就是"有效半径"
      } else if (kind == "spot") {
        slot.kind = LightKind::Spot;
        slot.position = position;
        slot.direction = toward_light;
        slot.radius = range;
        // three 的 angle 是**半角（弧度）**；霜天用度
        const double angle = get_double(impl_->context, light.get(), "angle", 0.5236);
        const double penumbra = get_double(impl_->context, light.get(), "penumbra", 0.0);
        slot.spot_inner_degrees = static_cast<float>(angle * (1.0 - penumbra) * 180.0 / 3.14159265);
        slot.spot_outer_degrees = static_cast<float>(angle * 180.0 / 3.14159265);
      } else {
        // directional 的第二个及以后：当作附加的平行光
        slot.kind = LightKind::Directional;
        slot.direction = toward_light;
      }
      ++extra_index;
    }
    if (ambient_from_light > 0.0f) scene.lighting.ambient = ambient_from_light;
    // ## 没写 `AmbientLight` 时环境项必须是 **0**（对齐模式，2026-11-06）
    //
    // three 里环境光**只来自 `AmbientLight` / `HemisphereLight` 对象**；
    // 不写就是 0（背光面全黑）。而 `Lighting::ambient` 的默认值是 0.35，
    // 那是一层"无端的环境保底"——实测让背光面从 0 抬到 **84/255**，
    // 也是文献里"本框架完全没有暗部"的根因。
    //
    // 旧模型不改（存量画面靠它），只在对齐模式下归零。
    if (aligned && !saw_ambient_light) scene.lighting.ambient = 0.0f;
  }

  // ── 世界背景 → 环境反射 ──
  //
  // ## 这一块是 bpy 路早就有的，three 路漏了（2026-11-05 补）
  //
  // 环境反射是**金属出色的唯一来源**。而 `Lighting::env_sky` 的默认值是
  // `(0.52, 0.58, 0.68)` —— 一个明亮的天空。
  //
  // bpy 路已经改成“从脚本的世界背景推导”（`bpy_runtime.cpp`），而
  // three 路一直没做——后果就是同一个场景、同一个渲染器：
  //
  // | 脚本 | 主体均值 |
  // |---|---|
  // | bpy (`bpy_watch.py`) | **69.5** |
  // | three (`watch_gears.js`) | **149.5** |
  //
  // 差 80 级。而调 `DirectionalLight.intensity`（0.8125 → 0.12）只降
  // 5 级——证明亮度不来自主光，来自那个不存在的亮天空。
  //
  // `scene.background` 在 three 里可能是 `Color` 对象，也可能是贴图/
  // 立方体贴图。只有 `Color` 能推导环境反射；其余保持默认。
  {
    // ⚠ 用 `scene_root`（脚本返回的 Scene 对象）而不是全局 `scene`
    // ——`three_script` 里 `scene` 是脚本内 `const`，不进全局。
    //
    // 第一次改的时候用了 `JS_GetGlobalObject` 取 `scene`，结果永远是
    // undefined，改了等于没改（数值一位不动）。
    JsRef background{impl_->context,
                     JS_GetPropertyStr(impl_->context, scene_root.get(), "background")};
    if (!JS_IsUndefined(background.get()) && !JS_IsNull(background.get())) {
      // `Color` 有 `isColor === true`（three 的惯例）
      JsRef is_color{impl_->context,
                     JS_GetPropertyStr(impl_->context, background.get(), "isColor")};
      const bool looks_like_color = JS_ToBool(impl_->context, is_color.get()) == 1;
      if (looks_like_color) {
        // `Color.toArray()` → [r, g, b]（已是线性工作空间值）
        JsRef to_array{impl_->context,
                       JS_GetPropertyStr(impl_->context, background.get(), "toArray")};
        if (JS_IsFunction(impl_->context, to_array.get())) {
          JsRef array{impl_->context,
                      JS_Call(impl_->context, to_array.get(), background.get(), 0, nullptr)};
          if (!JS_IsException(array.get()) && !JS_IsUndefined(array.get())) {
            const math::Vec3 linear = read_color(impl_->context, array.get());
            // ## 环境必须有**方向性对比**，否则金属没有质感（2026-11-05）
            //
            // 第一版把 `env_ground` 取 `env_sky × 0.5`——对 0.05 的背景，
            // 那是 0.05 vs 0.025，差 0.025。反射出来就是一片均匀暗灰，
            // 齿轮看起来像**剪影平涂**。
            //
            // 而真实金属的质感来自“反射到的东西有明暗层次”：上方的亮
            // 天空、下方的暗地面、侧面的中间调。
            //
            // 实测（扫 `environmentIntensity` 0.45 → 3.5）：
            // 均值 104.2 → 127.0，但**只是变亮，质感不变**（壳部仍为 0）。
            // 原因就在这里：把同样均匀的暗灰放大 8 倍，仍然均匀。
            //
            // 所以这里把地面拉升到 `天空 × 2.6`（亮于天空），同时天空保持
            // 背景色。这样垂直方向上 2.6 倍的对比，金属表面就能看到
            // “上暗下亮”的渐变——那正是金属反射的标志。
            //
            // ## 为何不是把天空提亮
            //
            // 天空色同时是**背景色**（`begin_frame` 用它清屏）。提亮它会
            // 直接把画面底色改了，四格里就无法与参照对比。而 `env_ground`
            // 只影响反射，不碰背景——所以动它。
            scene.lighting.env_sky = linear;
            scene.lighting.env_ground =
                math::Vec3{linear.x * 2.6f, linear.y * 2.6f, linear.z * 2.6f};
            // 清屏色：three 就是拿 `scene.background` 当清屏色（线性值）。
            scene.background_linear = linear;
            scene.background_set = true;
            // 环境强度：与 bpy 路同值（0.45，分位数标定得出）
            //
            // ⚠ 对齐模式下**不推导**：three 的纯色 `scene.background` 不产生 IBL，
            // 金属只能反射直接光。旧路径凭空多一层环境反射，实测让背光面从 0
            // 抬到 84/255。
            if (!aligned && scene.lighting.env_strength <= 0.0f) {
              scene.lighting.env_strength = 0.45f;
            }
          }
        }
      }
    }
  }

  // ## 环境探针：`scene.environmentProbes`（2026-11-05 新增）
  //
  // 脚本可以给一组方向 + 颜色，表达“环境里有窗户、有天光、有地面”：
  //
  //     scene.environmentProbes = [
  //       { direction: [0, 1, 0],   color: [0.10, 0.11, 0.13] },  // 天光
  //       { direction: [0.8, 0.3, -0.5], color: [0.35, 0.33, 0.30] }, // 窗户
  //       { direction: [0, -1, 0],  color: [0.02, 0.02, 0.02] },  // 地面
  //     ];
  //
  // 为何需要它：只用 `background`（纯色）推环境时，同一平面上的金属
  // 反射完全一样（法线相同）——平面看起来是均匀色块。有探针后，不同
  // 朝向的微面反射到不同探针，“金属跟着角度变”才成立。
  {
    JsRef probes{impl_->context,
                 JS_GetPropertyStr(impl_->context, scene_root.get(), "environmentProbes")};
    if (JS_IsArray(probes.get())) {
      JsRef length_value{impl_->context, JS_GetPropertyStr(impl_->context, probes.get(), "length")};
      std::uint32_t length = 0;
      JS_ToUint32(impl_->context, &length, length_value.get());
      const std::size_t capacity = Lighting::kMaxEnvProbes;
      std::size_t taken = 0;
      for (std::uint32_t i = 0; i < length && taken < capacity; ++i) {
        JsRef item{impl_->context, JS_GetPropertyUint32(impl_->context, probes.get(), i)};
        if (!JS_IsObject(item.get())) continue;
        JsRef dir_array{impl_->context,
                        JS_GetPropertyStr(impl_->context, item.get(), "direction")};
        JsRef col_array{impl_->context,
                        JS_GetPropertyStr(impl_->context, item.get(), "color")};
        if (!JS_IsObject(dir_array.get()) || !JS_IsObject(col_array.get())) continue;
        scene.lighting.env_probes[taken].direction = read_color(impl_->context, dir_array.get());
        scene.lighting.env_probes[taken].color = read_color(impl_->context, col_array.get());
        ++taken;
      }
      scene.lighting.env_probe_count = taken;
      // 锐利度：脚本可选（`scene.environmentProbeSharpness`）
      JsRef sharpness{impl_->context,
                      JS_GetPropertyStr(impl_->context, scene_root.get(),
                                        "environmentProbeSharpness")};
      if (!JS_IsUndefined(sharpness.get()) && !JS_IsNull(sharpness.get())) {
        double value = 0.0;
        if (JS_ToFloat64(impl_->context, &value, sharpness.get()) == 0 && value > 0.0) {
          scene.lighting.env_probe_sharpness = static_cast<float>(value);
        }
      }
    }
  }

  // ── `scene.environmentIntensity`（three r155+ 的真实 API）──
  //
  // ## 为什么需要这个口子
  //
  // `scene.background = Color(0.05…)` 是**几乎全黑**的背景。用它当环境
  // 反射源时，金属没有任何东西可反射 —— 于是齿轮渲染出来是**平涂剪影**。
  //
  // 而**真 three.js 也是这个效果**（`ExtrudeGeometry` 平面法线 + 纯色背景，
  // 实测均值 70.2、壳部 3.9%）。也就是说“无金属质感”与真渲染器是**一致**的，
  // 不是缺陷。
  //
  // 但“一致”不等于“好看”。用户对四格的视觉排序是：
  // 本框架js > 真js > 真py > 本框架py，而它恰好与**亮部占比**完全一致
  //（8.8% > 3.9% > 2.9% > 0.0%）——人眼读的是高光丰富度。
  //
  // 所以给脚本一个**显式控制口**：想有金属质感就调高 `environmentIntensity`
  // （真 three.js 里它乘在环境贴图的贡献上；我们这里乘在纯色环境的反射
  // 贡献上）。默认不设 = 旧行为（0.45），完全兼容。
  //
  // ## 为何不靠 `scene.environment = envMap`
  //
  // 真 three.js 需要一张 PMREM 环境贴图。本框架没有贴图/PMREM 管线，
  // 所以提供一个**标量**口子——语义上就是“环境反射的强度倍数”。
  {
    JsRef intensity{impl_->context,
                    JS_GetPropertyStr(impl_->context, scene_root.get(), "environmentIntensity")};
    if (!JS_IsUndefined(intensity.get()) && !JS_IsNull(intensity.get())) {
      double value = 0.0;
      if (JS_ToFloat64(impl_->context, &value, intensity.get()) == 0 && value > 0.0) {
        scene.lighting.env_strength = static_cast<float>(value);
      }
    }
  }

  // ── 物体 ──
  {
    JsRef objects{impl_->context,
                  JS_GetPropertyStr(impl_->context, extracted_owned.get(), "objects")};
    JsRef length_value{impl_->context, JS_GetPropertyStr(impl_->context, objects.get(), "length")};
    std::uint32_t object_count = 0;
    JS_ToUint32(impl_->context, &object_count, length_value.get());
    scene.objects.reserve(object_count);

    for (std::uint32_t i = 0; i < object_count; ++i) {
      JsRef source{impl_->context,
                   JS_GetPropertyUint32(impl_->context, objects.get(), i)};
      ThreeObject object;
      object.name = get_string(impl_->context, source.get(), "name");

      // 几何：position 必需，其余可选
      JsRef position_array{impl_->context,
                           JS_GetPropertyStr(impl_->context, source.get(), "position")};
      const auto positions = read_as_float(impl_->context, position_array.get());
      if (positions.size() < 3) continue;   // 无几何，跳过（而不是崩）

      JsRef normal_array{impl_->context,
                         JS_GetPropertyStr(impl_->context, source.get(), "normal")};
      auto normals = read_as_float(impl_->context, normal_array.get());
      JsRef uv_array{impl_->context, JS_GetPropertyStr(impl_->context, source.get(), "uv")};
      auto uvs = read_as_float(impl_->context, uv_array.get());
      JsRef index_array{impl_->context, JS_GetPropertyStr(impl_->context, source.get(), "index")};
      auto indices = read_as_index(impl_->context, index_array.get());

      const std::size_t vertex_count = positions.size() / 3;
      const bool has_normals = normals.size() == vertex_count * 3;
      object.mesh.vertices.resize(vertex_count * Mesh::kVertexFloats);
      for (std::size_t v = 0; v < vertex_count; ++v) {
        const std::size_t base = v * Mesh::kVertexFloats;
        object.mesh.vertices[base + 0] = positions[v * 3 + 0];
        object.mesh.vertices[base + 1] = positions[v * 3 + 1];
        object.mesh.vertices[base + 2] = positions[v * 3 + 2];
        if (has_normals) {
          object.mesh.vertices[base + 3] = normals[v * 3 + 0];
          object.mesh.vertices[base + 4] = normals[v * 3 + 1];
          object.mesh.vertices[base + 5] = normals[v * 3 + 2];
        } else {
          object.mesh.vertices[base + 3] = 0.0f;
          object.mesh.vertices[base + 4] = 0.0f;
          object.mesh.vertices[base + 5] = 0.0f;
        }
        // 顶点色填白：颜色由材质基色决定（three 的语义也是"材质给颜色"）
        object.mesh.vertices[base + 6] = 1.0f;
        object.mesh.vertices[base + 7] = 1.0f;
        object.mesh.vertices[base + 8] = 1.0f;
      }
      if (uvs.size() == vertex_count * 2) object.mesh.uvs = std::move(uvs);

      if (!indices.empty()) {
        object.mesh.indices = std::move(indices);
      } else {
        // 非索引几何（three 的 ExtrudeGeometry 常见）：按顺序当三角形列表
        object.mesh.indices.resize(vertex_count);
        for (std::size_t v = 0; v < vertex_count; ++v) {
          object.mesh.indices[v] = static_cast<std::uint32_t>(v);
        }
      }

      // 世界矩阵：three 的 `elements` 与霜天 `Mat4` 都是**列主序**，可直接 memcpy。
      // （两边都是 `m[column * 4 + row]`，这与 OpenGL/GLSL 的 mat4 布局一致。）
      JsRef world_array{impl_->context,
                        JS_GetPropertyStr(impl_->context, source.get(), "world")};
      for (int k = 0; k < 16; ++k) {
        JsRef component{impl_->context, JS_GetPropertyUint32(impl_->context, world_array.get(),
                                                             static_cast<std::uint32_t>(k))};
        double value = 0.0;
        JS_ToFloat64(impl_->context, &value, component.get());
        object.world.m[k] = static_cast<float>(value);
      }

      // 材质
      JsRef color_array{impl_->context,
                        JS_GetPropertyStr(impl_->context, source.get(), "color")};
      object.material.base_color = read_color(impl_->context, color_array.get());
      object.material.base_color_set = true;
      object.material.metallic = static_cast<float>(get_double(impl_->context, source.get(),
                                                               "metalness", 0.0));
      object.material.roughness = static_cast<float>(get_double(impl_->context, source.get(),
                                                                "roughness", 1.0));
      object.material.opacity = static_cast<float>(get_double(impl_->context, source.get(),
                                                              "opacity", 1.0));
      // 高光：three 的 Standard/Physical 恒有 GGX 高光 → 本框架要显式打开（默认是 0）
      object.material.specular = 1.0f;
      const double emissive_intensity = get_double(impl_->context, source.get(),
                                                   "emissiveIntensity", 0.0);
      object.material.emission = static_cast<float>(emissive_intensity);
      JsRef emissive_array{impl_->context,
                           JS_GetPropertyStr(impl_->context, source.get(), "emissive")};
      const math::Vec3 emissive = read_color(impl_->context, emissive_array.get());
      object.material.emissive_color = emissive;
      object.material.emissive_color_set = emissive_intensity > 0.0;

      // 材质类型 → 是否受光（MeshBasicMaterial / MeshNormalMaterial 在 three 里不受光）
      const std::string material_type = get_string(impl_->context, source.get(), "materialType");
      if (material_type == "MeshBasicMaterial") {
        // three 的 Basic = 纯色不受光。用"自发光且 unlit"表达，颜色取基色。
        object.material.emission = 1.0f;
        object.material.emissive_color = object.material.base_color;
        object.material.emissive_color_set = true;
        object.material.emission_unlit = true;
      }

      // 面的朝向：three 的 side → 霜天的剔除模式
      // Front(0) 只画正面 ⇒ 剔背面；Back(1) 只画背面 ⇒ 剔正面；Double(2) ⇒ 不剔
      const int side = static_cast<int>(get_double(impl_->context, source.get(), "side", 0.0));
      object.cull = side == 2 ? CullMode::None
                              : (side == 1 ? CullMode::Front : CullMode::Back);

      // ── 运动意图（见 `ThreeObject::spin_axis` 的说明）──
      const auto read_axis = [&](JSValueConst holder, const char* key, math::Vec3 fallback) {
        JsRef array{impl_->context,
                    JS_GetPropertyStr(impl_->context, holder, key)};
        if (!JS_IsArray(array.get())) return fallback;
        // 复用 `read_color` 读三元组（它做的正是“读 3 个数字”）
        return read_color(impl_->context, array.get());
      };
      {
        JsRef spin{impl_->context, JS_GetPropertyStr(impl_->context, source.get(), "spin")};
        if (JS_IsObject(spin.get())) {
          object.spin_speed = static_cast<float>(
              get_double(impl_->context, spin.get(), "speed", 0.0));
          if (object.spin_speed != 0.0f) {
            object.spin_axis = read_axis(spin.get(), "axis", math::Vec3{0.0f, 0.0f, 1.0f});
          }
        }
        JsRef oscillate{impl_->context,
                        JS_GetPropertyStr(impl_->context, source.get(), "oscillate")};
        if (JS_IsObject(oscillate.get())) {
          object.oscillate_amplitude = static_cast<float>(
              get_double(impl_->context, oscillate.get(), "amplitude", 0.0));
          object.oscillate_frequency = static_cast<float>(
              get_double(impl_->context, oscillate.get(), "frequency", 1.0));
          object.oscillate_phase = static_cast<float>(
              get_double(impl_->context, oscillate.get(), "phase", 0.0));
          if (object.oscillate_amplitude != 0.0f) {
            object.oscillate_axis =
                read_axis(oscillate.get(), "axis", math::Vec3{0.0f, 0.0f, 1.0f});
          }
        }
        // 取景排除标记（背景板/地面板这类氛围件）
        {
          JsRef flag{impl_->context,
                     JS_GetPropertyStr(impl_->context, source.get(), "noFrame")};
          object.no_frame = JS_ToBool(impl_->context, flag.get()) > 0;
        }
      }

      // 透明：three 要求 `transparent: true` 才按 opacity 混合。
      //
      // ## 为何要把 `transparent` 带过来
      //
      // three 的语义是：`opacity < 1` 但 `transparent: false` 时**仍然不透明**
      //（`opacity` 被忽略）。而本框架的语义是“`opacity < 1` 就混合”。
      // 两边不同，不显式翻译的话，一个写了 `opacity: 0.5` 却忘了 `transparent: true`
      // 的玻璃会变成全实心——模型很难看出这是“忘了设 transparent”。
      const bool transparent = get_bool(impl_->context, source.get(), "transparent", false);
      if (!transparent) {
        // 非透明：把 opacity 拉回 1（与 three 的渲染行为一致）
        object.material.opacity = 1.0f;
      } else if (object.material.opacity >= 1.0f) {
        // 声明为透明但 opacity 仍是 1：按 three 的语义它看起来不透明，但要走透明队列
        // （因为同一物体上可能有贴图 alpha）。本框架下保持 1.0 等价于不透明，无需特判。
        object.material.opacity = 1.0f;
      }

      // depthWrite：three 的 `depthWrite: false` 常用于“透明物体不写深度”。
      // 本框架的 `Material::depth_write` 管同一件事，直接带过来。
      object.material.depth_write = get_bool(impl_->context, source.get(), "depthWrite", true);
      if (object.material.opacity < 1.0f) object.material.depth_write = false;

      scene.objects.push_back(std::move(object));
    }
  }

  // 警告：抽不到物体是个常见且难自查的情况（相机在物体内、全部在取景框外等）
  if (scene.objects.empty()) {
    scene.warnings.emplace_back(
        "场景里没有可画的网格：确认写了 new THREE.Mesh(...) 且 scene.add(...)");
  }

  {
    JsRef warnings{impl_->context,
                   JS_GetPropertyStr(impl_->context, extracted_owned.get(), "warnings")};
    JsRef length_value{impl_->context, JS_GetPropertyStr(impl_->context, warnings.get(),
                                                        "length")};
    std::uint32_t warning_count = 0;
    JS_ToUint32(impl_->context, &warning_count, length_value.get());
    for (std::uint32_t i = 0; i < warning_count; ++i) {
      JsRef item{impl_->context, JS_GetPropertyUint32(impl_->context, warnings.get(), i)};
      if (!JS_IsString(item.get())) continue;
      const char* text = JS_ToCString(impl_->context, item.get());
      if (text != nullptr) {
        scene.warnings.emplace_back(text);
        JS_FreeCString(impl_->context, text);
      }
    }
  }

  // ── 色调映射：读 `renderer.toneMapping` / `scene.toneMapping`（2026-11-05）
  //
  // ## 为何需要这个口子
  //
  // 本框架的 `Lighting::tone_map` 默认是 **ACES**。ACES 是电影工业的
  // 显示变换：它把整个画面压进一个 S 曲线，**代价是降低饱和度**——
  // 尤其对低亮度的颜色，会把它们洗成灰。
  //
  // 实测（小卖部示例）：一套本来饱和度为 0.8~0.96 的配色（暖白墙、红招牌、
  // 木作、蓝周转箱），渲染出来整张图看着是**灰的**。
  //
  // 而 three.js 里 `renderer.toneMapping` 是**脚本自己控制**的真实 API
  //（`THREE.NoToneMapping` / `ACESFilmicToneMapping` / `AgXToneMapping`）。
  // 之前本框架完全忽略它——无论脚本写什么，都用 ACES。
  //
  // 现在读两个位置（兼容两种写法）：
  //   1. 全局 `renderer.toneMapping`（three 的标准用法）
  //   2. `scene.toneMapping`（方便脚本不必造一个 renderer）
  //
  // 取值与 three 的常量对齐：0=None, 1=Linear(=None), 2=Reinhard,
  // 3=Cineon(=Filmic), 4=ACESFilmic, 5/6=AgX, 7=Neutral(≈AgX)
  {
    int tone = -1;
    JsRef global_root{impl_->context, JS_GetGlobalObject(impl_->context)};
    JsRef renderer{impl_->context,
                   JS_GetPropertyStr(impl_->context, global_root.get(), "renderer")};
    if (JS_IsObject(renderer.get())) {
      tone = static_cast<int>(get_double(impl_->context, renderer.get(), "toneMapping", -1.0));
    }
    if (tone < 0) {
      tone = static_cast<int>(get_double(impl_->context, scene_root.get(), "toneMapping", -1.0));
    }
    if (tone >= 0) {
      switch (tone) {
        case 0:   // THREE.NoToneMapping
          // three 的 NoToneMapping 是**真的什么都不做**（连 `saturate` 都不做，
          // 那是输出编码里的 clamp）。所以这里必须映射到 `None`，
          // 不能退到任何 S 曲线。
          scene.lighting.tone_map = ToneMap::None;
          break;
        case 1:   // THREE.LinearToneMapping = saturate(exposure * color)
          scene.lighting.tone_map = ToneMap::None;
          break;
        case 2:   // THREE.ReinhardToneMapping
          scene.lighting.tone_map = ToneMap::Reinhard;
          break;
        case 3:   // THREE.CineonToneMapping（= OptimizedCineon，与 Hable 不同）
          scene.lighting.tone_map = ToneMap::Filmic;
          break;
        case 4:   // THREE.ACESFilmicToneMapping
          // ⚠ 必须用 `AcesThree`（逐式同构），不能用 `Aces`（Narkowicz 近似）。
          // 两者在中间调差 12~14 级（见 `color_pipeline.hpp` 的表）。
          scene.lighting.tone_map = ToneMap::AcesThree;
          break;
        default:  // 5/6 = AgXToneMapping，7 = NeutralToneMapping
          scene.lighting.tone_map = ToneMap::Agx;
          break;
      }
    }
  }

  // ── 对齐模式下的色调映射默认值 ──
  //
  // three 的默认是 `toneMapping = NoToneMapping`。对齐模式下若脚本没显式
  // 设过，就按 three 的默认走（**而不是**本框架的 ACES）；显式设过的由上面
  // 那段 `renderer.toneMapping` 映射处理（含 ACESFilmic → `AcesThree`）。
  if (aligned) {
    bool explicit_tone = false;
    JsRef global_root{impl_->context, JS_GetGlobalObject(impl_->context)};
    JsRef renderer{impl_->context,
                   JS_GetPropertyStr(impl_->context, global_root.get(), "renderer")};
    if (JS_IsObject(renderer.get())) {
      JsRef tone{impl_->context,
                 JS_GetPropertyStr(impl_->context, renderer.get(), "toneMapping")};
      explicit_tone = !JS_IsUndefined(tone.get()) && !JS_IsNull(tone.get());
    }
    if (!explicit_tone) {
      JsRef tone{impl_->context,
                 JS_GetPropertyStr(impl_->context, scene_root.get(), "toneMapping")};
      explicit_tone = !JS_IsUndefined(tone.get()) && !JS_IsNull(tone.get());
    }
    if (!explicit_tone) scene.lighting.tone_map = ToneMap::None;
  }

  return scene;
}

// ————————————————————————————————————————————————————————————————————————
// bpy × three.js 混合：把 bpy 几何写成 `BufferGeometry` 挂到 `THREE.__assets`
// ————————————————————————————————————————————————————————————————————————
//
// ## 为何是“挂到 THREE 上”而不是给脚本传参
//
// three.js 脚本是**用户写的普通 JS**（它不知道本框架的存在）。要让它在不学新 API
// 的前提下拿到 bpy 几何，最自然的入口就是它已经熟悉的命名空间：
//
// ```js
// const gear = new THREE.Mesh(THREE.__assets.gear_42t, mat);   // ← 就像内置几何一样用
// ```
//
// 传参方案（`function(scene, assets)`）会强制所有人改写法，而“用户写的就是它已经
// 会写的东西”是这条路线的全部价值（见 `three_runtime.hpp` 顶部）。
//
// ## 为何用 `eval_string` 拼一段 JS 而不是直接建对象
//
// QuickJS 的 BufferAttribute/BufferGeometry 构造是纯 JS 逻辑（属性拷贝、包围盒、
// 着色器 uniform 推导都在 JS 侧）。从 C++ 逐属性写会把那套逻辑重实现一遍，
// 而任何一处漏项（`itemSize`、`normalized`、`boundingSphere`）都是**静默**的：
// 几何看起来对、光照/拾取却错。拼一段 JS 让 three 自己走完整构造。
namespace {

/// 把 float 数组拼成 JS 字面量（保留 6 位有效小数：资产精度够，体积可控）。
void append_floats(std::string& out, const std::vector<float>& values) {
  out += '[';
  char buffer[32];
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    const int written = std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(values[i]));
    if (written > 0) out.append(buffer, static_cast<std::size_t>(written));
  }
  out += ']';
}

/// 把 uint32 数组拼成 JS 字面量。
void append_indices(std::string& out, const std::vector<std::uint32_t>& values) {
  out += '[';
  char buffer[16];
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ',';
    const int written = std::snprintf(buffer, sizeof(buffer), "%u", values[i]);
    if (written > 0) out.append(buffer, static_cast<std::size_t>(written));
  }
  out += ']';
}

/// JS 字符串字面量（只逸引号/反斜杠/控制符——名字来自脚本，可能是中文）。
void append_js_string(std::string& out, std::string_view text) {
  out += '"';
  for (const char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch;
    }
  }
  out += '"';
}

}  // namespace

auto ThreeRuntime::evaluate_with_assets(std::string_view script, const ThreeAssets& assets,
                                        std::string_view filename) -> Result<ThreeScene> {
  if (impl_ == nullptr || !impl_->ready) return evaluate(script, filename);   // 走原路报错

  // ## 零资产也要走本条路（**清掉上一轮的残留**）
  //
  // 曾经这里写的是 `if (assets.empty()) return evaluate(...)`——那看似最安全
  // （零资产 = 旧行为），但它不会清 `THREE.__assets`，而上一个场景可能刚把
  // 几何写进去。于是“不带资产的脚本”能读到上一场景的几何去组装，
  // 而现象是“换了场景但模型还是老的”。
  //
  // 代价：零资产时多跑一句 `THREE.__assets = {}`（微秒级），换来的语义是
  // **每次求值都是干净的**。

  // ## 先把资产装进一个**前缀脚本**，再与用户脚本一起求值
  //
  // 不能在 `evaluate` 之后装：那时的场景已经抽完了（`evaluate` 内部一次求值到底）。
  // 也不能改 `evaluate` 的签名（它被大量现有调用方引用）——所以这条混合路径
  // 用一个独立入口，内部拼一段前导 JS。
  std::string header;
  header.reserve(1024);
  // ⚠ **每次都重置**：`ThreeRuntime` 是线程内**单例**（QuickJS 运行时不能重建），
  // 所以 `THREE` 命名空间上的东西会跨求值残留。不重置的后果：上一次带资产的
  // 场景跑完，下一个**不带资产**的脚本仍能拿到 `THREE.__assets`——
  // 于是它默默用上一场景的几何去组装，而现象是“换了场景但模型还是老的”（实测踩到：
  // 反证用例（单独跑组装脚本应当失败）一直绿，因为它拿到了上一轮残留的资产）。
  header += "THREE.__assets = {};\n";
  for (const auto& asset : assets.items) {
    const Mesh& mesh = asset.mesh;
    if (mesh.vertices.empty()) continue;
    const std::size_t stride = Mesh::kVertexFloats;
    std::vector<float> positions;
    std::vector<float> normals;
    positions.reserve(mesh.vertices.size() / stride * 3);
    normals.reserve(mesh.vertices.size() / stride * 3);
    for (std::size_t i = 0; i + stride <= mesh.vertices.size(); i += stride) {
      positions.push_back(mesh.vertices[i + 0]);
      positions.push_back(mesh.vertices[i + 1]);
      positions.push_back(mesh.vertices[i + 2]);
      normals.push_back(mesh.vertices[i + 3]);
      normals.push_back(mesh.vertices[i + 4]);
      normals.push_back(mesh.vertices[i + 5]);
    }
    if (positions.empty()) continue;

    header += "(function(){\n  var g = new THREE.BufferGeometry();\n";
    header += "  g.setAttribute('position', new THREE.BufferAttribute(new Float32Array(";
    append_floats(header, positions);
    header += "), 3));\n";
    header += "  g.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(";
    append_floats(header, normals);
    header += "), 3));\n";
    if (!mesh.indices.empty()) {
      header += "  g.setIndex(new THREE.BufferAttribute(new Uint32Array(";
      append_indices(header, mesh.indices);
      header += "), 1));\n";
    }
    header += "  THREE.__assets[";
    append_js_string(header, asset.name);
    header += "] = g;\n})();\n";
  }

  std::string combined = header;
  combined += script;
  return evaluate(combined, filename);}

}  // namespace st::raster
