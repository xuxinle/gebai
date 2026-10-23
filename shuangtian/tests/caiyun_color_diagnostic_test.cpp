/// 脚本诊断：**非法输入必须报出来，而不是静默回退**。
///
/// ## 为什么这组用例值得单独存在
///
/// 裁云的诊断是给"写脚本的人 / AI"看的。一条诊断不报，写者就会得到
/// 一个**看起来能跑、但画得不对**的画面，而且没有任何线索指向原因。
///
/// 实测代价（这条诊断就是这么来的）：一份 480 个格子的脚本全用了
/// `rgb(30, 40, 50)` 这种写法，裁云只接受 `#rrggbb`——
/// 于是整屏变成白色方阵，**没有任何报错**，花了很久才定位到是颜色格式。
///
/// 所以这里钉的不是"诊断文案"，而是**这几类错误输入都必须产生 error 级诊断**。

#include <string>
#include <string_view>

#include "examples/caiyun/director.hpp"
#include "st/test/test.hpp"

namespace {

using caiyun::Script;

/// 从一个图层数组片段构造脚本（其余部分用最小可运行骨架）。
auto parse_layers_json(const std::string& layers_json) -> Script {
  const std::string text = std::format(
      "{{\"name\":\"diag\",\"fps\":30,\"size\":[1920,1080],\"scenes\":["
      "{{\"kind\":\"custom\",\"duration\":1.0,\"layers\":{}}}]}}",
      layers_json);
  const auto parsed = caiyun::parse_script(text);
  if (!parsed.has_value()) {
    Script empty{};
    return empty;
  }
  return parsed.value();
}

auto has_error_mentioning(const Script& script, std::string_view needle) -> bool {
  for (const caiyun::Diagnostic& diag : script.diagnostics) {
    if (diag.is_error && diag.message.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

ST_TEST(diagnostic_rejects_rgb_function_colour) {
  // `rgb()` 是 CSS 写法，裁云不认。必须报 error（而不是静默变白）。
  const Script script = parse_layers_json(
      "[{\"kind\":\"rect\",\"x\":0,\"y\":0,\"w\":100,\"h\":100,"
      "\"fill\":\"rgb(30, 40, 50)\"}]");
  ST_CHECK(has_error_mentioning(script, "色值"));
}

ST_TEST(diagnostic_rejects_short_hex_colour) {
  // `#ff` / `#12345` 位数不对。3 位简写（`#f00`）也**不接受**——
  // 裁云的约定是 6 或 8 位；接受 3 位会让"#fff"与"#ffffff"两套写法并存，
  // 而两套写法迟早会有一处写错。
  ST_CHECK(has_error_mentioning(parse_layers_json("[{\"kind\":\"rect\",\"fill\":\"#ff\"}]"),
                                "色值"));
  ST_CHECK(has_error_mentioning(parse_layers_json("[{\"kind\":\"rect\",\"fill\":\"#12345\"}]"),
                                "色值"));
}

ST_TEST(diagnostic_accepts_valid_hex_colour) {
  // 合法色值**不得**产生诊断（否则真问题会被淹没在噪音里）。
  const Script script = parse_layers_json(
      "[{\"kind\":\"rect\",\"fill\":\"#1e2a3e\"},"
      "{\"kind\":\"rect\",\"fill\":\"#1e2a3eff\"},"
      "{\"kind\":\"text\",\"text\":\"x\",\"color\":\"#FFFFFF\"}]");
  ST_CHECK(!has_error_mentioning(script, "色值"));
}

ST_TEST(diagnostic_rejects_colour_without_hash) {
  // 裸十六进制 `1e2a3e` 是常见错法（从设计工具复制时丢了 #）。
  ST_CHECK(has_error_mentioning(parse_layers_json("[{\"kind\":\"rect\",\"fill\":\"1e2a3e\"}]"),
                                "色值"));
}

ST_TEST(diagnostic_reports_bad_colour_per_layer_with_path) {
  // 诊断必须能**定位到具体图层**（`scenes[0].layers[1]`），
  // 否则 480 个图层里有几个写错时等于没报。
  const Script script = parse_layers_json(
      "[{\"kind\":\"rect\",\"fill\":\"#112233\"},"
      "{\"kind\":\"rect\",\"fill\":\"rgb(1,2,3)\"}]");
  bool found_with_path = false;
  for (const caiyun::Diagnostic& diag : script.diagnostics) {
    if (!diag.is_error) continue;
    if (diag.path.find("layers[1]") != std::string::npos) found_with_path = true;
  }
  ST_CHECK(found_with_path);
}
