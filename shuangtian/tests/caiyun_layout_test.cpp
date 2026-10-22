// 脚本编排层（`layout.hpp`）：**强能力 + 强护栏**的行为契约。
//
// ## 这组用例守的是"AI 用着稳"的四条
//
// ① **加载期就把错拦住**——语法错、没定义 `layout`、`layout` 不是函数，
//    三种都在 `load()` 时报，而不是渲到一半才崩；
// ② **失败降级不致命**——`evaluate` 失败时返回零增量（= 纯 JSON 行为），
//    而不是空白或崩溃；
// ③ **不写脚本时零影响**——`load("")` 后 `loaded()` 为假、`evaluate` 返回全零增量；
// ④ **增量语义正确**——`apply_delta` 是"相对基准"，不是"覆盖绝对值"。
//
// 第 ② 条最容易被实现成"抛异常"，而那正是"一处脚本错误毁掉整片"的来源。

#include "examples/caiyun/layout.cpp"

#include <cmath>
#include <cstdio>

#include "st/test/test.hpp"

namespace {

using caiyun::LayoutEngine;
using caiyun::LayoutScript;
using caiyun::LayerDelta;
using caiyun::LayerState;

[[nodiscard]] auto script_of(std::string source) -> LayoutScript {
  LayoutScript script{};
  script.source = std::move(source);
  script.filename = "<test>";
  script.timeout_ms = 200;
  return script;
}

[[nodiscard]] auto make_states(std::size_t count) -> std::vector<LayerState> {
  std::vector<LayerState> states;
  states.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    LayerState state{};
    state.x = 100.0f;
    state.y = 200.0f;
    state.w = 50.0f;
    state.h = 40.0f;
    state.alpha = 1.0f;
    state.scale = 1.0f;
    states.push_back(state);
  }
  return states;
}

}  // namespace

ST_TEST(layout_engine_rejects_syntax_error_at_load_time) {
  // 语法错必须在**加载时**就报，带定位信息。
  // 若只在渲染时失败，AI 写错一处要等渲到那一帧才知道。
  LayoutEngine engine;
  const auto status = engine.load(script_of("function layout(t) { return [; }"));
  ST_CHECK(!status.has_value());
  ST_CHECK(!engine.loaded());
}

ST_TEST(layout_engine_rejects_missing_layout_function) {
  // 只写了别的函数、忘了 `layout` —— 这是"能加载但永远不生效"的坑，
  // 必须在加载时就拦下（否则每帧静默失败，画面看起来"就是没动画"）。
  LayoutEngine engine;
  const auto status = engine.load(script_of("function other(t) { return []; }"));
  ST_CHECK(!status.has_value());
  ST_CHECK(!engine.loaded());
}

ST_TEST(layout_engine_rejects_non_function_layout) {
  // `layout` 存在但不是函数（常见笔误：写成了对象或数组）
  LayoutEngine engine;
  const auto status = engine.load(script_of("const layout = { a: 1 };"));
  ST_CHECK(!status.has_value());
  ST_CHECK(!engine.loaded());
}

ST_TEST(layout_engine_empty_script_is_disabled_not_error) {
  // 不写脚本 = 不启用（不是错误）。这是老脚本零改动的关键。
  LayoutEngine engine;
  ST_CHECK(engine.load(LayoutScript{}).has_value());
  ST_CHECK(!engine.loaded());

  std::vector<LayerDelta> deltas;
  auto states = make_states(3);
  ST_CHECK(engine.evaluate(1.0f, 4.0f, 1920.0f, 1080.0f, states, deltas).has_value());
  ST_CHECK(deltas.size() == 3);
  for (const auto& delta : deltas) {
    // 零增量：一切保持基准
    ST_CHECK(delta.dx == 0.0f);
    ST_CHECK(delta.dy == 0.0f);
    ST_CHECK(delta.scale == 1.0f);
    ST_CHECK(delta.alpha == 1.0f);
  }
}

ST_TEST(layout_engine_produces_deltas_for_every_layer) {
  // 一次调用算出**整帧所有图层**（不是每图层一次跨界）
  LayoutEngine engine;
  const auto status = engine.load(script_of(R"js(
    function layout(t, duration, canvas, layers) {
      return layers.map((L, i) => ({ dx: i * 10, dy: t * 100, alpha: 0.5 }));
    }
  )js"));
  if (!status.has_value()) {
    std::printf("[layout load error] %s\n", status.error().message.c_str());
  }
  ST_CHECK(status.has_value());
  ST_CHECK(engine.loaded());
  ST_CHECK(engine.loaded());

  std::vector<LayerDelta> deltas;
  auto states = make_states(4);
  ST_CHECK(engine.evaluate(2.0f, 4.0f, 1920.0f, 1080.0f, states, deltas).has_value());
  ST_CHECK(deltas.size() == 4);
  ST_CHECK(std::abs(deltas[0].dx - 0.0f) < 1e-3f);
  ST_CHECK(std::abs(deltas[2].dx - 20.0f) < 1e-3f);
  ST_CHECK(std::abs(deltas[0].dy - 200.0f) < 1e-3f);
  ST_CHECK(std::abs(deltas[0].alpha - 0.5f) < 1e-3f);
}

ST_TEST(layout_engine_timeout_degrades_to_zero_deltas) {
  // **最要紧的一条**：脚本死循环必须被打断，且**返回零增量**而非崩溃。
  // 这是"一键 AI 写错不毁整片"的实际保障。
  LayoutEngine engine;
  const auto status = engine.load(script_of(R"js(
    function layout(t) { while (true) {} }
  )js"));
  ST_CHECK(status.has_value());

  std::vector<LayerDelta> deltas;
  auto states = make_states(2);
  const auto evaluated = engine.evaluate(1.0f, 4.0f, 1920.0f, 1080.0f, states, deltas);
  ST_CHECK(!evaluated.has_value());          // 报错（调用方可记日志）
  ST_CHECK(engine.failure_count() >= 1);
  // 但增量必须是**零**——降级到纯 JSON 行为，而不是空白
  for (const auto& delta : deltas) {
    ST_CHECK(delta.dx == 0.0f);
    ST_CHECK(delta.dy == 0.0f);
    ST_CHECK(delta.scale == 1.0f);
  }
}

ST_TEST(layout_engine_throwing_script_degrades_not_crashes) {
  // 脚本主动抛错也是同一处置：报错 + 零增量，不崩。
  LayoutEngine engine;
  ST_CHECK(engine.load(script_of(R"js(
    function layout(t) { throw new Error("boom"); }
  )js")).has_value());

  std::vector<LayerDelta> deltas;
  auto states = make_states(2);
  ST_CHECK(!engine.evaluate(1.0f, 4.0f, 1920.0f, 1080.0f, states, deltas).has_value());
  ST_CHECK(deltas.size() == 2);
  ST_CHECK(deltas[0].scale == 1.0f);
  // 错误原因要留着——否则"为什么没动画"无从查起
  ST_CHECK(engine.last_error().find("boom") != std::string::npos);
}

ST_TEST(layout_engine_tolerates_shorter_and_longer_results) {
  // 数组长度不符**不算错**：按较短者填，缺失项保持基准。
  // 一处疏漏不该毁掉整帧（AI 常犯：忘了 map 全部图层）。
  LayoutEngine engine;
  ST_CHECK(engine.load(script_of(R"js(
    function layout(t, duration, canvas, layers) { return [{ dx: 5 }]; }
  )js")).has_value());

  std::vector<LayerDelta> deltas;
  auto states = make_states(3);
  ST_CHECK(engine.evaluate(1.0f, 4.0f, 1920.0f, 1080.0f, states, deltas).has_value());
  ST_CHECK(deltas.size() == 3);
  ST_CHECK(std::abs(deltas[0].dx - 5.0f) < 1e-3f);   // 有的项生效
  ST_CHECK(deltas[1].dx == 0.0f);                    // 缺的项保持基准
  ST_CHECK(deltas[2].dx == 0.0f);
}

ST_TEST(layout_engine_ignores_nan_and_infinity) {
  // NaN/Inf 会让渲染出现"整帧消失"这类难查现象 → 当无效值，保持默认。
  LayoutEngine engine;
  ST_CHECK(engine.load(script_of(R"js(
    function layout(t, duration, canvas, layers) {
      return [{ dx: NaN, dy: Infinity, scale: 2 }];
    }
  )js")).has_value());

  std::vector<LayerDelta> deltas;
  auto states = make_states(1);
  ST_CHECK(engine.evaluate(1.0f, 4.0f, 1920.0f, 1080.0f, states, deltas).has_value());
  ST_CHECK(std::isfinite(deltas[0].dx));
  ST_CHECK(std::isfinite(deltas[0].dy));
  ST_CHECK(deltas[0].dx == 0.0f);
  ST_CHECK(std::abs(deltas[0].scale - 2.0f) < 1e-3f);   // 合法值仍生效
}

ST_TEST(layout_apply_delta_is_relative_not_absolute) {
  // 增量语义：`apply_delta` 是"加在基准上"，不是"覆盖成这个值"。
  // 这条是"脚本出错退化到基准值"的前提——若实现成覆盖，
  // 脚本一旦返回小数值，图层就会跑到画面原点。
  LayerState state{};
  state.x = 100.0f;
  state.y = 200.0f;
  state.w = 50.0f;
  state.h = 40.0f;
  state.alpha = 1.0f;
  state.scale = 1.0f;

  LayerDelta delta{};
  delta.dx = 30.0f;
  delta.dy = -40.0f;
  delta.scale = 2.0f;
  delta.alpha = 0.5f;
  caiyun::apply_delta(state, delta);

  ST_CHECK(std::abs(state.x - 130.0f) < 1e-3f);
  ST_CHECK(std::abs(state.y - 160.0f) < 1e-3f);
  ST_CHECK(std::abs(state.scale - 2.0f) < 1e-3f);
  ST_CHECK(std::abs(state.alpha - 0.5f) < 1e-3f);
}

ST_TEST(layout_engine_is_deterministic) {
  // 确定性：同一脚本 + 同一输入 ⇒ 逐位相同的结果。
  // 批量导出要靠这条（否则同一帧两次渲出来不一样，回归无法比对）。
  LayoutEngine engine;
  ST_CHECK(engine.load(script_of(R"js(
    function layout(t, duration, canvas, layers) {
      return layers.map((L, i) => ({ dy: Math.sin(t * 3 + i) * 50, scale: 1 + t * 0.1 }));
    }
  )js")).has_value());

  std::vector<LayerDelta> first;
  std::vector<LayerDelta> second;
  auto states = make_states(5);
  ST_CHECK(engine.evaluate(1.7f, 4.0f, 1920.0f, 1080.0f, states, first).has_value());
  ST_CHECK(engine.evaluate(1.7f, 4.0f, 1920.0f, 1080.0f, states, second).has_value());
  ST_CHECK(first.size() == second.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    ST_CHECK(first[index].dy == second[index].dy);
    ST_CHECK(first[index].scale == second[index].scale);
  }
}
