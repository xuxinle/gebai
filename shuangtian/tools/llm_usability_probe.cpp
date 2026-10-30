// 大模型可用性探针：**模拟大模型最常犯的错**，量出错误信息够不够自我修正。
//
// ## 为什么用"犯错"来测可用性
//
// 大模型写场景脚本时，成功路径不是问题（照文档抄就行）。真正决定成败的是
// **第一次写错之后能不能自己改对**——而那只取决于一件事：错误信息是否包含
// "哪里错、错在哪、正确写法是什么"。
//
// 所以这个探针不测"能不能跑通"，而是逐类喂入典型错误，检查报错质量：
//
// | 错误类型 | 大模型会怎么犯 | 理想报错 |
// |---|---|---|
// | 字段名拼错 | `raduis` / `translaet` | 指出"未知字段 raduis"，**并给出相近的候选** |
// | 形状名拼错 | `cubee` | 列出可用形状 |
// | 类型错 | `"radius": "0.5"` | 指出期望类型 |
// | 缺必填 | 没有 `objects` | 指出缺什么 |
// | 取值越界 | `segments: -5` | 指出合法范围 |
//
// ## 判据
//
// 每条错误消息都必须包含**定位**（字段路径）。这一条是硬门槛：
// 只说"解析失败"的报错，对大模型等于没有信息。
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

struct Case {
  const char* label;
  std::string json;
  /// 期望错误里出现的关键词（任一命中即算"有信息量"）
  std::vector<std::string> expect_any;
  /// 是否期望**被拒绝**（false = 允许接受，但要记下来）
  bool expect_reject{true};
};

}  // namespace

int main() {
  const std::vector<Case> cases = {
      // ── 字段名拼错（大模型最高频的错误）──
      {"radius 拼成 raduis", R"j({"objects":[{"shape":"cube","raduis":0.5}]})j",
       {"raduis", "radius", "未知", "unknown"}, true},
      {"translate 拼成 translaet", R"j({"objects":[{"shape":"cube","translaet":[0,1,0]}]})j",
       {"translaet", "translate", "未知", "unknown"}, true},
      {"顶层字段拼错 camra", R"j({"camra":{},"objects":[]})j", {"camra", "camera", "未知", "unknown"},
       true},
      // ── 形状名拼错 ──
      {"shape 拼成 cubee", R"j({"objects":[{"shape":"cubee"}]})j", {"cubee", "cube", "可用"}, true},
      // ── 类型错 ──
      {"radius 写成字符串", R"j({"objects":[{"shape":"cube","radius":"0.5"}]})j",
       {"radius", "数字", "number", "类型"}, true},
      {"objects 写成对象", R"j({"objects":{"shape":"cube"}})j", {"objects", "数组", "array"}, true},
      // ── 缺必填 ──
      {"缺 objects", R"j({"camera":{}})j", {"objects"}, true},
      // ── 取值越界 ──
      {"segments 为负", R"j({"objects":[{"shape":"uv_sphere","segments":-5}]})j",
       {"segments", "负", "范围", "至少"}, true},
      // ── 动画相关 ──
      {"kind 拼错 colour", R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"colour","keys":[]}]}})j",
       {"kind", "rotation", "可用"}, true},
      {"关键帧缺 t", R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"rotation","keys":[{"value":[0,0,0]}]}]}})j",
       {"t", "时间"}, true},
      // ── 正确写法（必须通过，否则前面的判据无意义）──
      {"正确脚本", R"j({"camera":{"eye":[0,1,3]},"objects":[{"name":"a","shape":"cube","radius":0.5}]})j",
       {}, false},
  };

  int rejected = 0;
  int informative = 0;
  int silent_accept_bad = 0;
  int correct_accepted = 0;

  for (const Case& item : cases) {
    const auto spec = st::raster::parse_scene_spec_text(item.json);
    const bool rejected_case = !spec.has_value();
    std::string message = spec.has_value() ? "(接受)" : spec.error().message;

    // 判据 1：该拒的拒了 / 该过的过了
    bool verdict_ok = rejected_case == item.expect_reject;
    if (rejected_case) {
      ++rejected;
    } else if (item.expect_reject) {
      ++silent_accept_bad;   // 该拒却接受 = 静默错误，最危险
    } else {
      ++correct_accepted;
    }

    // 判据 2：错误消息是否有信息量（含定位关键词）
    bool has_info = true;
    if (rejected_case && !item.expect_any.empty()) {
      has_info = false;
      for (const std::string& keyword : item.expect_any) {
        if (message.find(keyword) != std::string::npos) {
          has_info = true;
          break;
        }
      }
      if (has_info) ++informative;
    }

    st::log::out("[{}] {:<26} {}\n", verdict_ok && has_info ? "OK  " : "差  ", item.label,
                 message);
    if (!verdict_ok) {
      st::log::out("        ↑ 期望{}，实际{}\n", item.expect_reject ? "拒绝" : "接受",
                   rejected_case ? "拒绝" : "接受");
    }
    if (rejected_case && !item.expect_any.empty() && !has_info) {
      st::log::out("        ↑ 报错缺少可自我修正的关键词（期望其一：{}）\n",
                   [&] {
                     std::string joined;
                     for (const auto& k : item.expect_any) {
                       if (!joined.empty()) joined += "/";
                       joined += k;
                     }
                     return joined;
                   }());
    }
  }

  st::log::out("\n=== 汇总 ===\n");
  st::log::out("错误用例 {} 条，报错有信息量 {} 条\n", rejected, informative);
  st::log::out("静默接受错误 {} 条（越少越好；>0 说明有字段被忽略而不报）\n", silent_accept_bad);
  st::log::out("正确脚本被接受 {} 条（应为 1）\n", correct_accepted);
  return (silent_accept_bad == 0 && correct_accepted >= 1) ? 0 : 2;
}
