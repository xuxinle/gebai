// **JS 编排能不能替代 JSON 描述**——先验证底子。
//
// ## 要验的不是"JS 能跑"，而是四件具体的事
//
// ① **C++ 函数能被脚本调用**（这是"编排"的全部前提）；
// ② **一遍 eval 能产出完整图层列表**（不是一图层一调用——那样每帧几百次跨界）；
// ③ **配额真的生效**（死循环会被打断，而不是挂死导出）；
// ④ **失败有可行动的报错**（行号 + 消息，而不是"渲染出错"）。
//
// 任一条不成立，这条路就不该推。

#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/ext/json.hpp"
#include "st/ext/script.hpp"
#include "st/probe/probe.hpp"

namespace {

using st::Json;

/// 一个最小的"裁云运行时"：脚本通过它算位置与透明度。
///
/// 真实实现里这些会直接落到 `LayerState`；这里只验通路。
struct MiniRuntime {
  double t{0.0};
  double duration{4.0};
};

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "JS 编排裁云图层（脚本能否替代 JSON 描述）",
      .criterion = "C++ 函数可调用、一遍 eval 产出图层表、配额生效、报错带行号",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("caiyun_js_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  st::ext::ScriptLimits limits{};
  limits.timeout = std::chrono::milliseconds{300};   // 故意短，用来验超时
  st::ext::ScriptEngine engine{limits};
  if (!engine.valid()) {
    return st::probe::main_of(spec, opts, st::probe::skip("脚本引擎不可用"));
  }
  st::print("QuickJS 版本：{}\n", st::ext::ScriptEngine::version());

  // ── ① 注册宿主函数（脚本能调的 C++ 能力）──
  auto runtime = std::make_shared<MiniRuntime>();
  // 时间与时长：脚本每帧要读，走最便宜的标量通道
  const auto now_fn = [runtime](const std::vector<Json>&) -> st::Result<Json> {
    return Json(runtime->t);
  };
  if (!engine.register_function("now", now_fn)) {
    verdict.add("注册 now()", false, "注册失败");
    verdict.conclude();
    return st::probe::main_of(spec, opts, verdict);
  }
  const auto dur_fn = [runtime](const std::vector<Json>&) -> st::Result<Json> {
    return Json(runtime->duration);
  };
  (void)engine.register_function("duration", dur_fn);

  // 一个带参数、带返回值的函数：验"脚本 → C++ → 回 JS"整条链
  const auto clamp_fn = [](const std::vector<Json>& args) -> st::Result<Json> {
    if (args.size() < 3) return st::unexpected(st::ErrorCode::Invalid, "clamp 需要 3 个参数");
    const double v = args[0].get<double>();
    const double lo = args[1].get<double>();
    const double hi = args[2].get<double>();
    return Json(std::clamp(v, lo, hi));
  };
  if (!engine.register_function("clamp", clamp_fn)) {
    verdict.add("注册 clamp()", false, "注册失败");
  } else {
    verdict.add("注册 clamp()", true, "");
  }

  // ── ② 一遍 eval 产出完整图层列表 ──
  //
  // 这是"编排"的关键形态：脚本里可以写循环、条件、数学，
  // 最后 return 一个图层数组。宿主只做一次跨界。
  constexpr std::string_view kSceneScript = R"js(
    // 用户写法：像写代码一样描述动画
    const layers = [];
    const T = duration();

    // 一排图标，按索引错峰入场（JSON 里要写 N 遍，这里一个循环）
    for (let i = 0; i < 5; i++) {
      const phase = i * 0.18;
      const local = Math.max(0, now() - phase);
      layers.push({
        kind: "model3d",
        name: "icon-" + i,
        x: (0.18 + i * 0.14) * 1920,
        y: 400 + Math.sin(local * 3) * 60,
        // 用宿主函数算：C++ 侧的 clamp
        scale: clamp(0.2 + local * 0.05, 0.2, 0.45),
      });
    }

    // 条件分支：JSON 表达不了这种"根据时长决定做几幕"
    if (T > 3) {
      layers.push({ kind: "text", name: "outro", text: "done", x: 1920 * 0.5, y: 200 });
    }
    layers;
  )js";

  auto produced = engine.eval(kSceneScript, "scene.js");
  if (!produced.has_value()) {
    verdict.add("一遍 eval 产出图层表", false, produced.error().message);
    verdict.conclude();
    return st::probe::main_of(spec, opts, verdict);
  }
  const Json& layers = produced.value();
  const bool is_array = layers.is_array();
  const std::size_t count = is_array ? layers.size() : 0;
  st::print("\n脚本产出 {} 个图层（含条件分支的 outro）\n", count);
  verdict.add("一遍 eval 产出图层表", is_array && count == 6,
              std::format("产出 {} 个（期望 6 = 5 图标 + 1 outro）", count));

  // 抽查一个：第 0 个图标的 scale 必须被 C++ 的 clamp 限制在 [0.2, 0.45]
  if (is_array && count > 0) {
    const Json& first = layers[0];
    const double scale = first["scale"].get<double>();
    const bool clamped_ok = scale >= 0.199 && scale <= 0.451;
    verdict.add("C++ 函数返回值被脚本采用（clamp 生效）", clamped_ok,
                std::format("scale={:.3f}", scale));
    // 时间读到了：now()=0 时 local=0，y 应为 400
    const double y = first["y"].get<double>();
    verdict.add("宿主 now() 可读", std::abs(y - 400.0) < 1.0,
                std::format("y={:.1f}（now=0 时应为 400）", y));
  }

  // 每帧重新求值：改 t 再 eval，位置必须跟着变
  runtime->t = 1.0;
  auto second = engine.eval(kSceneScript, "scene.js");
  if (second.has_value() && second.value().is_array() && second.value().size() > 0) {
    const double y2 = second.value()[0]["y"].get<double>();
    verdict.add("改时间后重新求值生效", std::abs(y2 - 400.0) > 1.0,
                std::format("t=1 时 y={:.1f}（应与 t=0 的 400 不同）", y2));
  }

  // ── ③ 配额：死循环必须被打断 ──
  auto spin = engine.eval("while (true) {}");
  const bool timed_out = !spin.has_value() &&
                         spin.error().code == st::ErrorCode::Timeout;
  verdict.add("死循环被超时打断（不会挂死导出）", timed_out,
              timed_out ? "按预期超时" : "未被中断！");

  // ── ④ 报错带行号 ──
  auto bad = engine.eval("const a = 1;\nconst b = ;\n", "broken.js");
  const bool has_line = !bad.has_value() && bad.error().message.find("broken.js") !=
                                                 std::string::npos;
  st::print("\n语法错误消息：{}\n", bad.has_value() ? "(无错误)" : bad.error().message);
  verdict.add("语法错误带文件名/行号", has_line,
              has_line ? "含 broken.js" : "消息缺少定位信息");

  verdict.conclude();
  st::print("\n=== 逐项结果 ===\n");
  for (const auto& item : verdict.checks) {
    st::print("  [{}] {} — {}\n", item.passed ? "PASS" : "FAIL", item.name, item.detail);
  }
  return st::probe::main_of(spec, opts, verdict);
}
