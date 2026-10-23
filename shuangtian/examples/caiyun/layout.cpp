/// 裁云 · 脚本编排层实现（见 `layout.hpp` 的设计说明）。

#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/ext/json.hpp"
#include "st/ext/script.hpp"

namespace caiyun {

namespace {

/// 把 JS 返回的一个图层对象读成 `LayerDelta`。
///
/// ## 为何逐个字段"读不到就用默认值"而不是整块失败
///
/// AI 写脚本时漏一个字段是极常见的（也会写成 `null`）。
/// 若漏字段就整帧失败，那"写错一处、整片黑屏"——正是要避免的不稳。
/// 所以这里的原则是：**能读就读，读不到保持默认**（默认 = 不改基准值）。
///
/// 只有"整个对象不是对象"才算错——那是结构性错误，不该被静默吞掉。
[[nodiscard]] auto read_delta(const st::Json& node, LayerDelta& out) -> bool {
  if (!node.is_object()) return false;
  const auto number = [&node](std::string_view key, float fallback) -> float {
    const st::Json* field = st::json_find(node, key);
    if (field == nullptr || !field->is_number()) return fallback;
    const double value = field->get<double>();
    // 非有限值（NaN/Inf）会让渲染出现"整帧消失"之类难查的现象 → 当无效处理
    if (!std::isfinite(value)) return fallback;
    return static_cast<float>(value);
  };
  out.dx = number("dx", 0.0f);
  out.dy = number("dy", 0.0f);
  out.dw = number("dw", 0.0f);
  out.dh = number("dh", 0.0f);
  out.scale = number("scale", 1.0f);
  out.rotate = number("rotate", 0.0f);
  out.alpha = number("alpha", 1.0f);
  // 颜色：接受 "#RRGGBB" 字符串或 [r,g,b,a] 数组
  if (const st::Json* tint = st::json_find(node, "tint"); tint != nullptr) {
    if (tint->is_string()) {
      const std::string text = tint->get<std::string>();
      if (text.size() == 7 && text.front() == '#') {
        const auto hex = [&text](std::size_t offset) -> std::uint8_t {
          const auto digit = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
          };
          const int high = digit(text[offset]);
          const int low = digit(text[offset + 1]);
          if (high < 0 || low < 0) return 255U;
          return static_cast<std::uint8_t>(high * 16 + low);
        };
        out.tint = st::math::Color{hex(1), hex(3), hex(5), 255};
      }
    }
  }
  return true;
}

/// 解析 `#RRGGBB` / `#RRGGBBAA`。失败返回白色（宽容：脚本是外部输入）。
[[nodiscard]] auto parse_hex_color(std::string_view text) -> st::math::Color {
  st::math::Color color{255, 255, 255, 255};
  if (text.size() < 7 || text.front() != '#') return color;
  const auto hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  const auto pair = [&hex, text](std::size_t offset) -> int {
    const int high = hex(text[offset]);
    const int low = hex(text[offset + 1]);
    return (high < 0 || low < 0) ? -1 : high * 16 + low;
  };
  const int r = pair(1);
  const int g = pair(3);
  const int b = pair(5);
  if (r < 0 || g < 0 || b < 0) return color;
  color.r = static_cast<std::uint8_t>(r);
  color.g = static_cast<std::uint8_t>(g);
  color.b = static_cast<std::uint8_t>(b);
  if (text.size() >= 9) {
    const int a = pair(7);
    if (a >= 0) color.a = static_cast<std::uint8_t>(a);
  }
  return color;
};

}  // namespace

struct LayoutEngine::Impl {
  st::ext::ScriptEngine engine{};
  bool loaded{false};
  std::uint64_t failures{0};
  std::string last_error{};
  std::vector<ScriptShape> shapes{};
};

LayoutEngine::LayoutEngine() : impl_(std::make_unique<Impl>()) {}
LayoutEngine::~LayoutEngine() = default;
LayoutEngine::LayoutEngine(LayoutEngine&&) noexcept = default;
auto LayoutEngine::operator=(LayoutEngine&&) noexcept -> LayoutEngine& = default;

auto LayoutEngine::loaded() const noexcept -> bool { return impl_->loaded; }
auto LayoutEngine::failure_count() const noexcept -> std::uint64_t {
  return impl_->failures;
}
auto LayoutEngine::last_error() const -> const std::string& { return impl_->last_error; }

auto LayoutEngine::shapes() const -> const std::vector<ScriptShape>& { return impl_->shapes; }

auto LayoutEngine::register_function(std::string name, st::ext::ScriptHostFunction function)
    -> st::Status {
  return impl_->engine.register_function(std::move(name), std::move(function));
}

auto LayoutEngine::load(const LayoutScript& script) -> st::Status {
  impl_->loaded = false;
  if (script.empty()) return st::Status{};   // 空脚本 = 不启用（不是错误）

  st::ext::ScriptLimits limits{};
  limits.timeout = std::chrono::milliseconds{std::clamp(script.timeout_ms, 10, 30'000)};
  impl_->engine = st::ext::ScriptEngine{limits};
  // 注册宿主函数（白名单，逐个显式）。必须在执行脚本**之前**——
  // 否则脚本顶层就可能调不到它们。
  register_host_functions(*this);

  // ① 语法校验：**加载期**就拦住语法错（AI 最常犯的错之一，必须当场报）
  if (const auto syntax = impl_->engine.check_syntax(script.source); !syntax) {
    impl_->last_error = syntax.error().message;
    return st::unexpected(syntax.error().code,
                          std::format("脚本编排层语法错误：{}", syntax.error().message));
  }

  // ② 执行一遍顶层代码：捕获"调用了不存在的宿主函数"这类运行期错误。
  //    顶层只应该定义函数与常量，不应该有重计算——若有，超时会在这里暴露。
  if (const auto run = impl_->engine.eval(script.source, script.filename); !run) {
    impl_->last_error = run.error().message;
    return st::unexpected(run.error().code,
                          std::format("脚本编排层初始化失败：{}", run.error().message));
  }

  // ③ 确认 `layout` 真的存在：否则每帧都失败，那是"能加载但永远不生效"的坑
  if (const auto probe = impl_->engine.eval("typeof layout", script.filename); !probe) {
    impl_->last_error = probe.error().message;
    return st::unexpected(probe.error().code, "脚本编排层无法检查 layout 函数");
  } else if (!probe.value().is_string() || probe.value().get<std::string>() != "function") {
    const std::string actual =
        probe.value().is_string() ? probe.value().get<std::string>() : "未知";
    impl_->last_error = std::format("layout 不是函数（实际是 {}）", actual);
    return st::unexpected(
        st::ErrorCode::Invalid,
        std::format("脚本编排层要求定义 function layout(t, duration, canvas, layers)，"
                    "但检测到 layout 是 {}",
                    actual));
  }

  impl_->loaded = true;
  impl_->failures = 0;
  impl_->last_error.clear();

  // 包装脚本：把 `layout` 套一层，**在 JS 侧清洗非有限值**。
  //
  // ## 为何必须在 JS 侧洗
  //
  // `NaN` / `Infinity` 不是合法 JSON 数字，JSON 转换层会直接报错，
  // 结果是 **一整帧失败**（而不是那个字段无效）。实测：AI 写出
  // `dy: NaN` 这种常见笔误，会让整帧降级——而预期只是那一项无效。
  //
  // 在 JS 侧把非有限值换成 `undefined`（= 字段缺失 = 保持基准），
  // 就变成“一个值坏了不影响其他”。这是“AI 用着稳”的具体保障之一。
  const std::string wrapper =
      "globalThis.__caiyun_layout_safe = function (t, duration, canvas, layers) {"
      "  const clean = (v) => (typeof v === 'number' && !Number.isFinite(v)) ? undefined : v;"
      "  const cleanItem = (item) => {"
      "    if (item === null || typeof item !== 'object') return item;"
      "    const out = {};"
      "    for (const key of Object.keys(item)) out[key] = clean(item[key]);"
      "    return out;"
      "  };"
      "  const result = layout(t, duration, canvas, layers);"
      "  if (Array.isArray(result)) return result.map(cleanItem);"
      // 新形态：{ states, shapes } —— 两边都要清洗（shapes 里同样可能出 NaN）
      "  if (result && typeof result === 'object') {"
      "    return {"
      "      states: Array.isArray(result.states) ? result.states.map(cleanItem) : [],"
      "      shapes: Array.isArray(result.shapes) ? result.shapes.map(cleanItem) : [],"
      "    };"
      "  }"
      "  return result;"
      "};"
      // ⚠ 末尾必须有 `void 0`：赋值表达式的值是**函数本身**，
      // 而函数没有 JSON 表示 → `eval` 会报“函数没有 JSON 表示”。
      // 实测踩过：报错文案完全没指向“你忘了把返回值抹掉”。
      "void 0;";
  if (const auto wrap = impl_->engine.eval(wrapper, "<layout-safe-wrapper>"); !wrap) {
    impl_->loaded = false;
    impl_->last_error = wrap.error().message;
    return st::unexpected(wrap.error().code,
                          std::format("脚本编排层包装失败：{}", wrap.error().message));
  }
  return st::Status{};
}

auto LayoutEngine::evaluate(float time, float duration, float canvas_w, float canvas_h,
                            const std::vector<LayerState>& layers,
                            std::vector<LayerDelta>& out) -> st::Status {
  out.assign(layers.size(), LayerDelta{});
  if (!impl_->loaded) return st::Status{};   // 未装载 = 零增量

  // 把图层基准状态交给脚本。**一次一个数组**，不是每图层一次跨界。
  st::Json input = st::Json::array();
  for (const LayerState& state : layers) {
    st::Json item = st::Json::object();
    item["x"] = state.x;
    item["y"] = state.y;
    item["w"] = state.w;
    item["h"] = state.h;
    item["alpha"] = state.alpha;
    item["scale"] = state.scale;
    item["offset_y"] = state.offset_y;
    input.push_back(std::move(item));
  }

  st::Json canvas = st::Json::object();
  canvas["width"] = canvas_w;
  canvas["height"] = canvas_h;

  auto produced =
      impl_->engine.call("__caiyun_layout_safe", {st::Json(time), st::Json(duration), canvas, input});
  if (!produced.has_value()) {
    ++impl_->failures;
    impl_->last_error = produced.error().message;
    return st::unexpected(produced.error().code, produced.error().message);
  }
  const st::Json& result = produced.value();
  // 两种返回形态都支持：
  //   · `return [ {...}, ... ]`            —— 纯状态数组（旧写法）
  //   · `return { states: [...], shapes: [...] }` —— 带临时图元
  //
  // 为何不强制新写法：已有剧本用旧形态，改契约会让它们全失效。
  const st::Json* states_json = &result;
  impl_->shapes.clear();
  if (result.is_object()) {
    if (const st::Json* nested = st::json_find(result, "states"); nested != nullptr) {
      states_json = nested;
    }
    if (const st::Json* shapes = st::json_find(result, "shapes");
        shapes != nullptr && shapes->is_array()) {
      impl_->shapes.reserve(shapes->size());
      for (const st::Json& entry : *shapes) {
        if (!entry.is_object()) continue;
        ScriptShape shape{};
        const auto number = [&entry](std::string_view key, float fallback) -> float {
          const st::Json* field = st::json_find(entry, key);
          if (field == nullptr || !field->is_number()) return fallback;
          const double value = field->get<double>();
          return std::isfinite(value) ? static_cast<float>(value) : fallback;
        };
        shape.kind = static_cast<int>(std::clamp(number("kind", 0.0f), 0.0f, 3.0f));
        shape.x = number("x", 0.0f);
        shape.y = number("y", 0.0f);
        shape.w = number("w", 0.0f);
        shape.h = number("h", 0.0f);
        shape.x2 = number("x2", 0.0f);
        shape.y2 = number("y2", 0.0f);
        shape.radius = number("radius", 0.0f);
        shape.stroke = number("stroke", 0.0f);
        shape.rotate = number("rotate", 0.0f);
        shape.text = st::json_get_string(entry, "text");
        const float alpha = std::clamp(number("alpha", 1.0f), 0.0f, 1.0f);
        const std::string color_text = st::json_get_string(entry, "color", "#ffffff");
        shape.color = parse_hex_color(color_text);
        shape.color.a = static_cast<std::uint8_t>(std::lround(alpha * 255.0f));
        impl_->shapes.push_back(std::move(shape));
      }
    }
  }
  if (!states_json->is_array()) {
    ++impl_->failures;
    impl_->last_error = "layout 没有返回数组";
    return st::unexpected(st::ErrorCode::Invalid,
                          "layout 必须返回一个状态数组（与传入的图层等长）");
  }

  // 长度不符时按较短者填：缺失项保持零增量（= 不改基准值），
  // 多出来的忽略。**不当作错误**——一处疏漏不该毁掉整帧。
  const std::size_t count = std::min(states_json->size(), out.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (!read_delta((*states_json)[index], out[index])) {
      ++impl_->failures;
      impl_->last_error = std::format("layout 返回的第 {} 项不是对象", index);
      // 单项结构错误：保持该项零增量，继续处理其余项
      out[index] = LayerDelta{};
    }
  }
  return st::Status{};
}
/// 取本线程的编排引擎（`thread_local` 池）。
auto layout_engine() -> LayoutEngine& {
  static thread_local LayoutEngine engine;
  return engine;
}

void register_host_functions(LayoutEngine& engine) {
  // ## 为何全部是“纯函数”
  //
  // 注册进引擎的函数是脚本唯一的宿主能力面。这里刻意只给
  // **无副作用、无 IO、无状态**的：拿到同样输入就得到同样输出。
  // 一旦开放“读时钟/随机数/文件”，确定性就没了，
  // 而确定性是“同一帧渲两次逐像素相同”的前提（批量导出靠它）。

  // ease(name, t)：标准缓动曲线。
  // 脚本里算缓动是极高频需求，而写对 easeOutBack 这种需要经验。
  (void)engine.register_function(
      "ease", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        if (args.size() < 2 || !args[0].is_string() || !args[1].is_number()) {
          return st::unexpected(st::ErrorCode::Invalid, "ease(name, t) 需要 (字符串, 数字)");
        }
        const std::string name = args[0].get<std::string>();
        const float t = std::clamp(static_cast<float>(args[1].get<double>()), 0.0f, 1.0f);
        if (name == "linear") return st::Json(static_cast<double>(t));
        if (name == "in") return st::Json(static_cast<double>(t * t * t));
        if (name == "out") {
          const float inverse = 1.0f - t;
          return st::Json(static_cast<double>(1.0f - inverse * inverse * inverse));
        }
        if (name == "in_out") {
          const float value = t < 0.5f
                                  ? 4.0f * t * t * t
                                  : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
          return st::Json(static_cast<double>(value));
        }
        if (name == "back") {
          // easeOutBack：先越过再回弹（“有质量”的关键曲线）
          const float c1 = 1.70158f;
          const float c3 = c1 + 1.0f;
          const float inverse = t - 1.0f;
          return st::Json(static_cast<double>(1.0f + c3 * inverse * inverse * inverse +
                                              c1 * inverse * inverse));
        }
        if (name == "elastic") {
          if (t <= 0.0f) return st::Json(0.0);
          if (t >= 1.0f) return st::Json(1.0);
          constexpr float kTwoPi = 6.283185307179586f;
          const float c4 = kTwoPi / 3.0f;
          return st::Json(static_cast<double>(std::pow(2.0f, -10.0f * t) *
                                                  std::sin((t * 10.0f - 0.75f) * c4) +
                                              1.0f));
        }
        return st::unexpected(st::ErrorCode::Invalid,
                              "未知缓动：" + name + "（可用：linear/in/out/in_out/back/elastic）");
      });

  // lerp(a, b, t)：线性插值
  (void)engine.register_function(
      "lerp", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        if (args.size() < 3 || !args[0].is_number() || !args[1].is_number() ||
            !args[2].is_number()) {
          return st::unexpected(st::ErrorCode::Invalid, "lerp(a, b, t) 需要 3 个数字");
        }
        const double a = args[0].get<double>();
        const double b = args[1].get<double>();
        const double t = args[2].get<double>();
        return st::Json(a + (b - a) * t);
      });

  // clamp(v, lo, hi)：限幅
  (void)engine.register_function(
      "clamp", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        if (args.size() < 3 || !args[0].is_number() || !args[1].is_number() ||
            !args[2].is_number()) {
          return st::unexpected(st::ErrorCode::Invalid, "clamp(v, lo, hi) 需要 3 个数字");
        }
        return st::Json(std::clamp(args[0].get<double>(), args[1].get<double>(),
                                   args[2].get<double>()));
      });

  // snap(t, fps)：吸附到帧。
  //
  // 为何需要它：手工写帧精度动画时，“让一个动作恰好落在第 N 帧”
  // 是常见需求；脚本自己算会因为浮点误差差一帧。
  (void)engine.register_function(
      "snap", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        if (args.size() < 2 || !args[0].is_number() || !args[1].is_number()) {
          return st::unexpected(st::ErrorCode::Invalid, "snap(t, fps) 需要 2 个数字");
        }
        const double fps = args[1].get<double>();
        if (fps <= 0.0) return st::Json(args[0].get<double>());
        const double time = args[0].get<double>();
        return st::Json(std::round(time * fps) / fps);
      });

  // noise(seed)：**确定性**伪随机（同种子同结果）。
  //
  // 为何不给 Math.random：那会破坏“同帧两次渲逐像素相同”。
  // 给确定性版本就能做“每个图标偶发抖动”这类效果而仍可复现。
  (void)engine.register_function(
      "noise", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        const double seed =
            (args.empty() || !args[0].is_number()) ? 0.0 : args[0].get<double>();
        // 经典 sin-hash：确定、无状态、分布够用
        const double value = std::sin(seed * 12.9898) * 43758.5453;
        return st::Json(value - std::floor(value));
      });
}

auto parse_layout_script(const st::Json& node, std::string_view asset_basis)
    -> st::Result<LayoutScript> {
  LayoutScript script{};
  const st::Json* field = st::json_find(node, "layout");
  if (field == nullptr) return script;   // 不写 = 不启用

  if (field->is_string()) {
    // 文件形式。路径解析顺序：
    // ① 绝对路径 → 直接用；
    // ② 给定 `asset_basis`（脚本目录）→ 拼起来；
    // ③ 都失败就按当前工作目录试一次（兼容“在脚本目录下运行”的用法）。
    //
    // ⚠ 实测踩过：只按工作目录解析会让“写在一个子目录里的脚本”永远载入失败，
    // 而错误又被 `value_or` 吞掉——表现是“脚本完全没生效但没有任何报错”。
    const std::string reference = field->get<std::string>();
    std::string path = reference;
    if (!st::fs::is_absolute(path)) {
      bool resolved = false;
      if (!asset_basis.empty()) {
        const std::string joined = st::fs::join(std::string(asset_basis), reference);
        if (st::fs::exists(joined)) {
          path = joined;
          resolved = true;
        }
      }
      if (!resolved && !st::fs::exists(path)) {
        return st::unexpected(st::ErrorCode::NotFound,
                              std::format("脚本编排层文件读不到：{}", reference));
      }
    }
    auto text = st::fs::read_text(path);
    if (!text.has_value()) {
      return st::unexpected(st::ErrorCode::NotFound,
                            std::format("脚本编排层文件读不到：{}", path));
    }
    script.source = std::move(text.value());
    script.filename = path;
    return script;
  }

  if (field->is_object()) {
    script.source = st::json_get_string(*field, "source");
    script.filename = st::json_get_string(*field, "filename", "<inline>");
    const double timeout = st::json_get_double(*field, "timeout_ms", 400.0);
    script.timeout_ms = static_cast<int>(std::clamp(timeout, 10.0, 30'000.0));
    return script;
  }

  return st::unexpected(st::ErrorCode::Invalid,
                        "layout 必须是字符串（文件路径）或对象（内联 source）");
}

/// 把脚本返回的增量叠到图层状态上。
///
/// ## 为何是“增量”而不是“绝对值”
///
/// JSON 里的 `x/y/w/h` 仍是**基准**，脚本只表达“相对基准变化多少”。
/// 好处有两个：① 两种写法天然可以混用（同一脚本里部分图层走 JSON、部分走 JS）；
/// ② 脚本出错时退化到基准值，而不是“变成 0 跑到画面外”。
///
/// ## 为何放在这里（而不是 motion.cpp）
///
/// 它是**编排层的语义**（“增量怎么用”是这套接口的一部分），
/// 而且测试要能直接调它验证“相对而非绝对”。放在绘制文件的匿名命名空间里就测不到。
void apply_delta(LayerState& state, const LayerDelta& delta) {
  state.x += delta.dx;
  state.y += delta.dy;
  state.w = std::max(0.0f, state.w + delta.dw);
  state.h = std::max(0.0f, state.h + delta.dh);
  state.scale *= delta.scale;
  state.alpha = std::clamp(state.alpha * delta.alpha, 0.0f, 1.0f);
  if (std::isfinite(delta.rotate)) state.rotate += delta.rotate;
  if (delta.tint.a != 0) state.tint = delta.tint;
}

}  // namespace caiyun
