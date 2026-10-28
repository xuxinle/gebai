/// 意图层（`intent.md`）：机器可读约束 + 与实现核对。
///
/// ## 这组用例守的是什么
///
/// 意图层**不参与渲染**——所以它出问题时画面完全正常，只是校验从没生效过。
/// 这是本项目里最难查的一类缺陷（"我明明写了约束"），所以每一条路径都要测：
/// 标记缺失、标记在但 JSON 坏、约束写了但脚本不符。
#include <string>
#include <vector>

#include "examples/caiyun/director.hpp"
#include "examples/caiyun/intent.hpp"
// 测试目标不链接 examples/caiyun，所以直接包含实现（仓库惯例）。
#include "examples/caiyun/intent.cpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto make_script(double total_seconds, int width, int height,
                               std::string_view title) -> caiyun::Script {
  caiyun::Script script{};
  script.name = "意图测试";
  script.width = width;
  script.height = height;
  caiyun::Scene scene{};
  scene.kind = caiyun::SceneKind::Custom;
  scene.duration = static_cast<float>(total_seconds);
  scene.title = std::string{title};
  script.scenes.push_back(std::move(scene));
  return script;
}

}  // namespace

ST_TEST(caiyun_intent_absent_block_is_not_an_error) {
  // 只有人读区、没有机器读区：合法。一个刚建的工程可能还没写约束。
  const auto spec = caiyun::parse_intent("# 意图\n\n给内部看的短片。\n");
  ST_CHECK(spec.has_value());
  if (!spec) return;
  ST_CHECK(!spec->duration_s.has_value());
  ST_CHECK(spec->must_show.empty());
}

ST_TEST(caiyun_intent_parses_all_fields) {
  const std::string text = R"md(# 意图
说明文字。

<!-- caiyun:intent
{"duration_s": 6.0, "tolerance_s": 0.25, "size": [1920, 1080],
 "must_show": ["产品名", "三个要点"], "tone": "冷静"}
-->
)md";
  const auto spec = caiyun::parse_intent(text);
  ST_CHECK(spec.has_value());
  if (!spec) return;
  ST_CHECK(spec->duration_s.has_value());
  if (spec->duration_s) ST_CHECK(*spec->duration_s > 5.9 && *spec->duration_s < 6.1);
  ST_CHECK(spec->tolerance_s > 0.24 && spec->tolerance_s < 0.26);
  ST_CHECK(spec->size.has_value());
  if (spec->size) {
    ST_CHECK(spec->size->first == 1920);
    ST_CHECK(spec->size->second == 1080);
  }
  ST_CHECK(spec->must_show.size() == 2);
  ST_CHECK(spec->tone == "冷静");
}

ST_TEST(caiyun_intent_broken_json_is_an_error) {
  // 标记在但 JSON 坏：**必须报错**。
  // 静默忽略 = "我写了约束但没生效"，而画面完全正常，这类问题极难查。
  const auto spec = caiyun::parse_intent(
      "text\n<!-- caiyun:intent\n{duration_s: 6.0}\n-->\n");
  ST_CHECK(!spec.has_value());
  if (!spec) st::log::out("坏 JSON 的报错：{}\n", spec.error().message);
}

ST_TEST(caiyun_intent_unclosed_block_is_an_error) {
  const auto spec = caiyun::parse_intent("text\n<!-- caiyun:intent\n{\"a\":1}\n");
  ST_CHECK(!spec.has_value());
}

ST_TEST(caiyun_intent_detects_duration_drift) {
  caiyun::IntentSpec spec{};
  spec.duration_s = 6.0;
  spec.tolerance_s = 0.5;

  // 符合（6.0 vs 6.0）
  const auto ok = caiyun::check_intent(spec, make_script(6.0, 1920, 1080, "a"));
  ST_CHECK(ok.empty());

  // 容差内（6.4 vs 6.0，差 0.4 < 0.5）
  const auto within = caiyun::check_intent(spec, make_script(6.4, 1920, 1080, "a"));
  ST_CHECK(within.empty());

  // 超出容差（10.0 vs 6.0）
  const auto drift = caiyun::check_intent(spec, make_script(10.0, 1920, 1080, "a"));
  ST_CHECK(drift.size() == 1);
  if (!drift.empty()) {
    ST_CHECK(drift[0].constraint == "duration_s");
    // 消息里要同时有期望与实际，否则人无法判断差多少
    ST_CHECK(drift[0].to_string().find("6.00") != std::string::npos);
    ST_CHECK(drift[0].to_string().find("10.00") != std::string::npos);
  }
}

ST_TEST(caiyun_intent_detects_size_and_missing_content) {
  caiyun::IntentSpec spec{};
  spec.size = std::pair<int, int>{1080, 1920};  // 竖版
  spec.must_show = {"产品名", "不存在的词"};

  const auto devs = caiyun::check_intent(spec, make_script(6.0, 1920, 1080, "产品名在这里"));
  // 两条：画幅不符 + "不存在的词"缺失（"产品名"在标题里，应当找到）
  ST_CHECK(devs.size() == 2);
  std::size_t size_hits = 0;
  std::size_t must_hits = 0;
  for (const auto& dev : devs) {
    if (dev.constraint == "size") ++size_hits;
    if (dev.constraint == "must_show") {
      ++must_hits;
      ST_CHECK(dev.expected == "不存在的词");
    }
  }
  ST_CHECK(size_hits == 1);
  ST_CHECK(must_hits == 1);
}

ST_TEST(caiyun_intent_no_constraints_means_no_deviations) {
  // 一个字段都不写 = 什么都不校验。
  // 这条防的是"未声明被当成默认值硬套"——那会让没写时长的工程被判失败。
  const caiyun::IntentSpec empty{};
  const auto devs = caiyun::check_intent(empty, make_script(999.0, 640, 480, "x"));
  ST_CHECK(devs.empty());
}

ST_TEST(caiyun_intent_render_block_roundtrips) {
  caiyun::IntentSpec spec{};
  spec.duration_s = 12.5;
  spec.size = std::pair<int, int>{1280, 720};
  spec.must_show = {"甲", "乙"};
  spec.tone = "轻快";

  const std::string block = caiyun::render_intent_block(spec);
  // 生成的块必须能被自己解析回来（否则"生成骨架"与"解析"两套口径会漂移）
  const auto parsed = caiyun::parse_intent("# 意图\n\n" + block);
  ST_CHECK(parsed.has_value());
  if (!parsed) return;
  ST_CHECK(parsed->duration_s.has_value());
  if (parsed->duration_s) ST_CHECK(*parsed->duration_s > 12.4 && *parsed->duration_s < 12.6);
  ST_CHECK(parsed->size.has_value());
  if (parsed->size) ST_CHECK(parsed->size->first == 1280);
  ST_CHECK(parsed->must_show.size() == 2);
  ST_CHECK(parsed->tone == "轻快");
}
