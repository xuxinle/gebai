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

}  // namespace

struct LayoutEngine::Impl {
  st::ext::ScriptEngine engine{};
  bool loaded{false};
  std::uint64_t failures{0};
  std::string last_error{};
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

auto LayoutEngine::load(const LayoutScript& script) -> st::Status {
  impl_->loaded = false;
  if (script.empty()) return st::Status{};   // 空脚本 = 不启用（不是错误）

  st::ext::ScriptLimits limits{};
  limits.timeout = std::chrono::milliseconds{std::clamp(script.timeout_ms, 10, 30'000)};
  impl_->engine = st::ext::ScriptEngine{limits};

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
      "  const result = layout(t, duration, canvas, layers);"
      "  if (!Array.isArray(result)) return result;"
      "  return result.map((item) => {"
      "    if (item === null || typeof item !== 'object') return item;"
      "    const out = {};"
      "    for (const key of Object.keys(item)) out[key] = clean(item[key]);"
      "    return out;"
      "  });"
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
  if (!result.is_array()) {
    ++impl_->failures;
    impl_->last_error = "layout 没有返回数组";
    return st::unexpected(st::ErrorCode::Invalid,
                          "layout 必须返回一个状态数组（与传入的图层等长）");
  }

  // 长度不符时按较短者填：缺失项保持零增量（= 不改基准值），
  // 多出来的忽略。**不当作错误**——一处疏漏不该毁掉整帧。
  const std::size_t count = std::min(result.size(), out.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (!read_delta(result[index], out[index])) {
      ++impl_->failures;
      impl_->last_error = std::format("layout 返回的第 {} 项不是对象", index);
      // 单项结构错误：保持该项零增量，继续处理其余项
      out[index] = LayerDelta{};
    }
  }
  return st::Status{};
}
/// 取本线程的编排引擎（`thread_local` 池）。
///
/// 导出是 16 路并行：每线程一个实例，各自编译一次脚本。
/// 换来的是**无锁**——QuickJS 运行时非线程安全，共享就得全程加锁，
/// 而每帧每图层都要过一遍锁，那比多编译几次贵得多。
auto layout_engine() -> LayoutEngine& {
  static thread_local LayoutEngine engine;
  return engine;
}

auto parse_layout_script(const st::Json& node, std::string_view asset_basis)
    -> st::Result<LayoutScript> {
  LayoutScript script{};
  const st::Json* field = st::json_find(node, "layout");
  if (field == nullptr) return script;   // 不写 = 不启用

  if (field->is_string()) {
    // 文件形式：相对脚本目录解析
    const std::string reference = field->get<std::string>();
    std::string path = reference;
    if (!st::fs::is_absolute(path) && !asset_basis.empty()) {
      const std::string joined = st::fs::join(std::string(asset_basis), reference);
      if (st::fs::exists(joined)) path = joined;
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
