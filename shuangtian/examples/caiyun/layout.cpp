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

  // ④ **探针帧**：用真实形状试跑一帧，把“能装载但每帧都失败”这一类**提前说出来**。
  //
  // ## 为何需要它（实测代价）
  //
  // 上面三步只能证明：语法对、顶层能跑、`layout` 是函数。它们**拦不住**
  // 函数体里引用了不存在的名字（典型：漏定义一个缓动函数）。那种错会让
  // **每一帧**都失败 ⇒ `shapes` 恒空 ⇒ 画面只剩背景，而渲染照常退出码 0、
  // **一条 warn 都没有**。实测用二分试了十几轮才定位到。
  //
  // ## 为何**不**在装载期拒绝（这一层容易做错）
  //
  // 本框架已有契约：脚本运行期抛错/超时 ⇒ `load()` 成功、`evaluate()` 报错 +
  // 零增量 + 继续出片（`layout_engine_throwing_script_degrades_not_crashes` /
  // `..._timeout_degrades_to_zero_deltas` / ease 参数错）。这是**刻意的**：
  // AI 写的一处错不应该毁掉整片。实测：把探针做成“失败就拒绝装载”，
  // 直接打破上述三条契约。
  //
  // 而脚本引擎对外把“抛错”统一报成 `ErrorCode::Parse`，
  // 因此 `throw new Error("boom")` 与“漏定义函数”在错误码上**完全不可区分**——
  // 想只拒绝后者而放行前者，做不到。
  //
  // 所以探针的职责收敛为：**把错说在前面**（装载时一条明确 warn + 原文），
  // 而**不改行为**（仍照常降级）。渲完的 `failure_count()` 汇总再补一刀，
  // 两道合起来把“静默”堵死。
  {
    st::Json probe_canvas = st::Json::object();
    probe_canvas["width"] = 1920.0;
    probe_canvas["height"] = 1080.0;
    // 空图层数组：脚本若假定“至少一层”就可能在这里报错——而这**正是**要报的
    // （真实场景若没有图层，它每帧都会这样死，与探针一致）。
    const auto probe = impl_->engine.call("__caiyun_layout_safe",
                                          {st::Json(0.0), st::Json(1.0), probe_canvas,
                                           st::Json::array()});
    if (!probe.has_value()) {
      // 不拒绝装载，但绝不再安静：原文 + 后果都写清楚。
      impl_->last_error = probe.error().message;
      // 措辞要克制：探针帧只能证明“**第 0 帧、空图层**这个输入下失败了”。
      // 对漏定义函数这类确定性缺陷它会每帧都失败；而 `throw` / 超时可能是数据依赖的，
      // 那正是本框架允许“运行期降级、不拒装载”的理由。所以只说“首次探测就失败了”。
      ST_LOG_WARN(
          "脚本编排层首次探测（t=0，空图层）就失败了，这通常是每帧都会失败——"
          "若确实如此，动效将全部丢失（画面只剩 JSON 图层）。原因：{}",
          probe.error().message);
    } else {
      // 返回值形态：既不是数组也不是 {states, shapes} 是纯写法错误，
      // 且不会随时间变好（不像 throw 可能是数据依赖的）——这一类拒绝装载。
      const st::Json& value = probe.value();
      if (!value.is_array() && !value.is_object()) {
        impl_->loaded = false;
        impl_->last_error = "layout 的返回值不是数组也不是对象";
        return st::unexpected(
            st::ErrorCode::Invalid,
            "脚本编排层探针帧发现 layout() 返回值的形态不合法："
            "应返回增量数组，或 { states: [...], shapes: [...] }");
      }
    }
  }

  impl_->loaded = true;
  impl_->failures = 0;
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
        shape.kind = static_cast<int>(std::clamp(number("kind", 0.0f), 0.0f, 4.0f));
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
        // 多边形顶点：接受扁平数组 [x0,y0,x1,y1,…] 或 [[x,y],…] 两种写法。
        //
        // 两种都接受的理由：扁平数组是 JS 里算出来的自然形式（循环 push），
        // 而 `[[x,y],…]` 是手写脚本时的自然形式。只支持一种会让另一边写得很别扭，
        // 而脚本层出错是静默的（渲染器只降级不报错，见 docs/BACKLOG.md G8）。
        if (const st::Json* pts = st::json_find(entry, "points");
            pts != nullptr && pts->is_array()) {
          shape.points.reserve(pts->size() * 2);
          for (const st::Json& item : *pts) {
            if (item.is_number()) {
              const double v = item.get<double>();
              if (std::isfinite(v)) shape.points.push_back(static_cast<float>(v));
            } else if (item.is_array()) {
              for (const st::Json& sub : item) {
                if (!sub.is_number()) continue;
                const double v = sub.get<double>();
                if (std::isfinite(v)) shape.points.push_back(static_cast<float>(v));
              }
            }
          }
          // 顶点数为奇数时丢掉末项（否则后面的线段会配错点）
          if (shape.points.size() % 2 != 0) shape.points.pop_back();
        }
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
        // expo_out：前段极快、后段几乎不动（t80 ≈ 0.23）。
        //
        // 用途：**硬切点/抢拍**。与 `out`（三次）相比它“几乎瞬到”，
        // 但又不像线性那样结尾硬停——“一下就到位然后稳住”是专业动效的常规语感。
        if (name == "expo_out") {
          if (t <= 0.0f) return st::Json(0.0);
          if (t >= 1.0f) return st::Json(1.0);
          constexpr float kRoof = 1.0f - 0.0009765625f;  // 1 - 2^-10
          return st::Json(static_cast<double>((1.0f - std::pow(2.0f, -10.0f * t)) / kRoof));
        }
        // expo_in_out：前一半加速、后一半减速，两端都很缓。
        //
        // 用途：**长距离运镜**（推拉/横移）。中段快、两端缓，看起来像有人推着走，
        // 而不是匀速传送带。
        if (name == "expo_in_out") {
          if (t <= 0.0f) return st::Json(0.0);
          if (t >= 1.0f) return st::Json(1.0);
          const auto expo_in = [](float v) -> float {
            return (std::pow(2.0f, 10.0f * v) - 1.0f) / 1023.0f;
          };
          const float value =
              t < 0.5f ? expo_in(2.0f * t) * 0.5f : 1.0f - expo_in(2.0f - 2.0f * t) * 0.5f;
          return st::Json(static_cast<double>(value));
        }
        // smoother（smootherstep）：五次多项式，两阶导数在端点都为 0。
        //
        // 与 `in_out`（三次）的差别：**起步与收尾更“沉”**。
        // 三次曲线在端点仍有加速度突变，大距离位移时能看出“忽然启动”；
        // 五次没有，所以它是“物体自己动起来”而不是“被谁推了一下”。
        if (name == "smoother") {
          const float value = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
          return st::Json(static_cast<double>(value));
        }
                return st::unexpected(st::ErrorCode::Invalid,
                              "未知缓动：" + name +
                                  "（可用：linear/in/out/in_out/back/elastic/expo_out/"
                                  "expo_in_out/smoother）");
      });

  // ── 弹簧族：运动质量的本体 ──
  //
  // ## 为何单开一个函数，而不把它当成一种 ease
  //
  // `ease(name, t)` 的 `t` 是**归一化进度**（0~1），它描述“走到终点了吗”。
  // 而弹簧的自变量是**时间**：它没有预设终点时刻，靠 ζ/ω 自己决定何时停。
  // 把弹簧压成 `ease` 会丢掉这件事（“到 1 之后就不动了”），也就丢掉了
  // 超调后的二次回弹——而那正是“有质量”的来源。
  //
  // ## 参数的意义（写脚本时只需要记住这两个数）
  //
  // · `zeta`（阻尼比）< 1 会过冲：0.4 很跳，0.6 舒服，0.8 稳重，1 临界（不回头）
  // · `omega`（角频率）越大越快：14 慢，20 中，26 快，32 很急
  //
  // 返回值恒从 0 起、趋于 1（阶跃响应），所以能直接当“入场进度”用：
  //
  // ```js
  // const k = spring(t - 0.4, 0.62, 20);   // 0.4s 起跳，舒服的过冲
  // return [{ dx: (1 - k) * -60, alpha: k }];
  // ```
  (void)engine.register_function(
      "spring", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        if (args.empty() || !args[0].is_number()) {
          return st::unexpected(st::ErrorCode::Invalid,
                                "spring(tau, [zeta], [omega]) 需要至少 1 个数字");
        }
        // 负数 tau = 还没开始 ⇒ 返回 0（静止在原位），而不是让它振荡回头。
        const double tau = args[0].get<double>();
        if (tau <= 0.0) return st::Json(0.0);
        const double zeta = args.size() > 1 && args[1].is_number() ? args[1].get<double>() : 0.6;
        const double omega = args.size() > 2 && args[2].is_number() ? args[2].get<double>() : 20.0;
        // 参数护栏：ζ <= 0 会变成永不衰减的振荡（画面永远在抖），
        // ω <= 0 永远不动。夹到合理区间，而不是返回错误——
        // 动效脚本里“参数写夸张了”比“逻辑错”常见得多，直接失效更影响创作。
        const double z = std::clamp(zeta, 0.05, 2.0);
        const double w = std::clamp(omega, 1.0, 80.0);
        double value = 0.0;
        if (z < 1.0) {
          // 欠阻尼：衰减余弦 + 正弦（经典二阶阶跃响应）
          const double wd = w * std::sqrt(1.0 - z * z);
          value = 1.0 - std::exp(-z * w * tau) *
                            (std::cos(wd * tau) + (z * w / wd) * std::sin(wd * tau));
        } else if (z == 1.0) {
          value = 1.0 - std::exp(-w * tau) * (1.0 + w * tau);
        } else {
          // 过阻尼：两个实根，不振荡但很滞
          const double q = w * std::sqrt(z * z - 1.0);
          const double r1 = -z * w + q;
          const double r2 = -z * w - q;
          value = 1.0 - (r2 * std::exp(r1 * tau) - r1 * std::exp(r2 * tau)) / (r2 - r1);
        }
        // 非有限值（极端参数下的数值意外）归 1：宁可“已到位”，不可让 NaN 传进布局
        // （NaN 会让元素直接消失，而且不报错）。
        if (!std::isfinite(value)) return st::Json(1.0);
        return st::Json(value);
      });

  // spring_vel(tau, zeta, omega)：弹簧响应的**导数**（每秒进度）。
  //
  // 用在哪里：**由速度驱动的形变**（squash & stretch）。落地瞬间进度最快，
  // 此时沿运动方向拉长、垂直方向压薄；停稳后速度归零，形变也归零。
  // 没它就只能“固定幅度地压一下”，看起来像预设动画而不是碰撞。
  (void)engine.register_function(
      "spring_vel", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        if (args.empty() || !args[0].is_number()) {
          return st::unexpected(st::ErrorCode::Invalid,
                                "spring_vel(tau, [zeta], [omega]) 需要至少 1 个数字");
        }
        const double tau = args[0].get<double>();
        if (tau <= 0.0) return st::Json(0.0);
        const double zeta = args.size() > 1 && args[1].is_number() ? args[1].get<double>() : 0.6;
        const double omega = args.size() > 2 && args[2].is_number() ? args[2].get<double>() : 20.0;
        const double z = std::clamp(zeta, 0.05, 2.0);
        const double w = std::clamp(omega, 1.0, 80.0);
        double value = 0.0;
        if (z < 1.0) {
          const double s = std::sqrt(1.0 - z * z);
          value = (w / s) * std::exp(-z * w * tau) * std::sin(w * s * tau);
        } else if (z == 1.0) {
          value = w * w * tau * std::exp(-w * tau);
        } else {
          const double q = w * std::sqrt(z * z - 1.0);
          const double r1 = -z * w + q;
          const double r2 = -z * w - q;
          value = w * w * (std::exp(r1 * tau) - std::exp(r2 * tau)) / (r1 - r2);
        }
        if (!std::isfinite(value)) return st::Json(0.0);
        return st::Json(value);
      });

  // settle(zeta, omega, eps)：弹簧**多久之后稳定**（秒）。
  //
  // 为何需要它：写“跳→落定→停留”的节奏时，必须知道这一弹要占多久，
  // 否则要么提前切镜（动作被砍断）、要么留太多空白（节奏散了）。
  // 手算临界阻尼的 4/ω 只对 ζ=1 成立，欠阻尼的衰减时间与 ζ、ω 都相关。
  (void)engine.register_function(
      "settle", [](const std::vector<st::Json>& args) -> st::Result<st::Json> {
        const double zeta = args.size() > 0 && args[0].is_number() ? args[0].get<double>() : 0.6;
        const double omega = args.size() > 1 && args[1].is_number() ? args[1].get<double>() : 20.0;
        const double eps = args.size() > 2 && args[2].is_number() ? args[2].get<double>() : 0.02;
        const double z = std::clamp(zeta, 0.05, 2.0);
        const double w = std::clamp(omega, 1.0, 80.0);
        // 解析上界：包络 e^{-zωt} 衰减到 eps 即可保证 |响应-1| < eps。
        // 取 ln(1/eps)/(zω) 再留 20% 余量（包络是保守估计，但不是等价）。
        const double envelope = std::log(1.0 / std::clamp(eps, 0.001, 0.5)) / (z * w);
        return st::Json(envelope * 1.2);
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
    script.raw_field = reference;
    script.from_file = true;
    return script;
  }

  if (field->is_object()) {
    script.source = st::json_get_string(*field, "source");
    script.filename = st::json_get_string(*field, "filename", "<inline>");
    const double timeout = st::json_get_double(*field, "timeout_ms", 400.0);
    script.timeout_ms = static_cast<int>(std::clamp(timeout, 10.0, 30'000.0));
    script.raw_field = script.source;
    script.from_file = false;
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
