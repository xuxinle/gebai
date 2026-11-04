#include "st/raster/three_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
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
    // - `TextEncoder`：`BufferGeometry.toJSON`、字体/文本相关路径会摸它。
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

  // 自动给最后一条“表达式语句”加 `return`。
  //
  // 只处理**最后一条非空且非注释行**，且该行：
  // - 不是 `return …`（已经写了）
  // - 不是以 `}` 结尾的块结构（if/for/function 的闭括号，加 return 会语法错）
  // - 不以 `const`/`let`/`var`/`function`/`class`/`import` 开头（声明无值）
  //
  // 这样两种常见写法都对：
  //   `...; scene;`                      ← 改成 `...; return scene;`
  //   `...; scene.add(mesh);`            ← 改成 `return scene.add(mesh);`（值是 mesh，
  //                                        不过下面还会去找全局名兜底）
  const auto as_iife = [](const std::string& source) -> std::string {
    // 找最后一个非空行
    std::size_t end = source.size();
    while (end > 0 && (source[end - 1] == '\n' || source[end - 1] == '\r' ||
                       source[end - 1] == ' ' || source[end - 1] == '\t')) {
      --end;
    }
    std::size_t line_start = source.rfind('\n', end == 0 ? 0 : end - 1);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
    std::string last_line = source.substr(line_start, end - line_start);
    // 去掉行尾分号（后面要重加）
    while (!last_line.empty() &&
           (last_line.back() == ';' || last_line.back() == ' ' || last_line.back() == '\t')) {
      last_line.pop_back();
    }
    // 首部去空白，用于关键字判定
    std::size_t first = last_line.find_first_not_of(" \t");
    const std::string head = first == std::string::npos ? "" : last_line.substr(first);

    bool needs_return = !head.empty();
    if (needs_return) {
      for (const char* keyword : {"return", "const ", "let ", "var ", "function", "class ",
                                 "import", "export", "if ", "for ", "while ", "switch",
                                 "try", "do ", "throw"}) {
        if (head.rfind(keyword, 0) == 0) {
          needs_return = false;
          break;
        }
      }
      // 以 `}` 或 `{` 结尾的：块结构，不碰
      if (needs_return && (head.back() == '}' || head.back() == '{')) needs_return = false;
    }

    std::string body = source.substr(0, line_start);
    if (needs_return) {
      body += "return " + head + ";\n";
    } else {
      body += source.substr(line_start, end - line_start);
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
      spin: (o.userData && o.userData.spin) || null,
      oscillate: (o.userData && o.userData.oscillate) || null
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
        // 环境光没有方向：累加到 `Lighting::ambient`（三色的均值，保持与旧路径一致的标量语义）
        ambient_from_light += intensity * (0.2126f * color.x + 0.7152f * color.y +
                                           0.0722f * color.z);
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

  return scene;
}

}  // namespace st::raster
