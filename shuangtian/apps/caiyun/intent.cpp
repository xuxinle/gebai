#include "intent.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "director.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;

constexpr std::string_view kOpenTag = "<!-- caiyun:intent";
constexpr std::string_view kCloseTag = "-->";

}  // namespace

auto IntentDeviation::to_string() const -> std::string {
  return std::format("意图偏差 [{}]：期望 {}，实际 {}。{}", constraint, expected, actual, message);
}

auto parse_intent(std::string_view markdown) -> Result<IntentSpec> {
  IntentSpec spec{};
  const std::size_t open = markdown.find(kOpenTag);
  if (open == std::string_view::npos) return spec;  // 没有机器读区：合法

  const std::size_t body_start = markdown.find('\n', open);
  if (body_start == std::string_view::npos) {
    return st::unexpected(ErrorCode::Parse, "intent.md：`<!-- caiyun:intent` 后没有内容");
  }
  const std::size_t close = markdown.find(kCloseTag, body_start);
  if (close == std::string_view::npos) {
    return st::unexpected(ErrorCode::Parse,
                          "intent.md：`<!-- caiyun:intent` 没有闭合的 `-->`");
  }
  const std::string json_text{markdown.substr(body_start, close - body_start)};

  const auto parsed = st::json_parse(json_text);
  if (!parsed) {
    // 静默忽略会让"我写了约束但没生效"变成一个极难查的现象
    //（意图层不参与渲染，所以画面完全正常，只是校验从没生效过）。
    return st::unexpected(ErrorCode::Parse,
                          std::format("intent.md 的约束块不是合法 JSON：{}", parsed.error().message));
  }
  if (!parsed->is_object()) {
    return st::unexpected(ErrorCode::Parse, "intent.md 的约束块必须是 JSON 对象");
  }

  spec.raw = *parsed;
  if (const auto it = parsed->find("duration_s"); it != parsed->end() && it->is_number()) {
    spec.duration_s = it->get<double>();
  }
  if (const auto it = parsed->find("tolerance_s"); it != parsed->end() && it->is_number()) {
    spec.tolerance_s = std::max(0.0, it->get<double>());
  }
  if (const auto it = parsed->find("size"); it != parsed->end() && it->is_array() &&
                                                    it->size() == 2) {
    spec.size = std::pair<int, int>{static_cast<int>((*it)[0].get<double>()),
                                    static_cast<int>((*it)[1].get<double>())};
  }
  if (const auto it = parsed->find("must_show"); it != parsed->end() && it->is_array()) {
    for (const auto& item : *it) {
      if (item.is_string()) spec.must_show.push_back(item.get<std::string>());
    }
  }
  if (const auto it = parsed->find("tone"); it != parsed->end() && it->is_string()) {
    spec.tone = it->get<std::string>();
  }
  return spec;
}

auto render_intent_block(const IntentSpec& spec) -> std::string {
  st::Json out = st::Json::object();
  if (spec.duration_s) out["duration_s"] = *spec.duration_s;
  if (spec.tolerance_s != 0.5) out["tolerance_s"] = spec.tolerance_s;
  if (spec.size) {
    out["size"] = st::Json::array();
    out["size"].push_back(spec.size->first);
    out["size"].push_back(spec.size->second);
  }
  if (!spec.must_show.empty()) {
    out["must_show"] = st::Json::array();
    for (const std::string& item : spec.must_show) out["must_show"].push_back(item);
  }
  if (!spec.tone.empty()) out["tone"] = spec.tone;
  return std::format("<!-- caiyun:intent\n{}\n-->\n", out.dump(2));
}

auto check_intent(const IntentSpec& spec, const Script& script)
    -> std::vector<IntentDeviation> {
  std::vector<IntentDeviation> out;

  // 总时长 = 各幕时长之和（与渲染器的口径一致；转场不额外计时）。
  double total = 0.0;
  for (const Scene& scene : script.scenes) total += static_cast<double>(scene.duration);

  if (spec.duration_s) {
    const double expected = *spec.duration_s;
    if (std::abs(total - expected) > spec.tolerance_s) {
      out.push_back(IntentDeviation{
          .constraint = "duration_s",
          .actual = std::format("{:.2f}s", total),
          .expected = std::format("{:.2f}s ±{:.2f}s", expected, spec.tolerance_s),
          .message = "总时长与意图不符。若是有意调整，请同步更新 intent.md；"
                     "否则请回到意图声明的长度。",
      });
    }
  }

  if (spec.size) {
    const int want_w = spec.size->first;
    const int want_h = spec.size->second;
    if (script.width != want_w || script.height != want_h) {
      out.push_back(IntentDeviation{
          .constraint = "size",
          .actual = std::format("{}x{}", script.width, script.height),
          .expected = std::format("{}x{}", want_w, want_h),
          .message = "画幅与意图不符。画幅决定版式安全边距，换画幅应当重排而不是缩放。",
      });
    }
  }

  // must_show：在**全部文案**里做子串匹配（标题/副标/正文/字标/页脚）。
  //
  // 为何匹配文案而不是"图层名"：图层名常常是 `text1`/`rect3` 这类无意义标识，
  // 而意图里写的是"产品名""三个要点"这种**观众能看到的内容**。
  if (!spec.must_show.empty()) {
    std::string haystack;
    const auto append = [&](const std::string& text) {
      haystack += text;
      haystack += '\n';
    };
    append(script.name);
    append(script.brand_wordmark);
    append(script.brand_latin);
    append(script.footer_note);
    for (const Scene& scene : script.scenes) {
      append(scene.title);
      append(scene.subtitle);
      append(scene.footer_note);
      append(scene.carry.text);
      for (const std::string& line : scene.lines) append(line);
      for (const Layer& layer : scene.layers) {
        append(layer.text);
        append(layer.name);
      }
    }
    for (const std::string& need : spec.must_show) {
      if (need.empty()) continue;
      if (haystack.find(need) == std::string::npos) {
        out.push_back(IntentDeviation{
            .constraint = "must_show",
            .actual = "未找到",
            .expected = need,
            .message = std::format("意图要求出现「{}」，但脚本的全部文案里没有。", need),
        });
      }
    }
  }

  return out;
}

}  // namespace caiyun
