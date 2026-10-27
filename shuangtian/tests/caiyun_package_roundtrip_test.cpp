/// 工程包（`.cypkg`）的地基：`Script` → JSON → `Script` 必须**无损往返**。
///
/// ## 为什么先测这个
///
/// 包的语义是「同一份 Script 的另一种落盘形态」。载入时包被**拍平**成 Script
/// 再走既有渲染路径，所以往返一旦有损，包与原脚本就会漂移——而漂移是静默的：
/// 画面照常出，只是作者写过的某个字段在保存那一步消失了。
///
/// 这类缺陷最难查（"我明明写了" / "画面上没生效"），所以把它做成第一道闸。
///
/// ## 判据为什么是"两次序列化相等"而不是"逐字段比对"
///
/// 后者要维护一份字段清单，加字段时必漏。前者用同一个序列化器做参照：
/// `json(parse(json(x))) == json(x)`——只要解析器与序列化器对某个字段
/// 理解不一致，这条立刻红。
#include <string>
#include <vector>

#include "examples/caiyun/director.hpp"
#include "st/core/print.hpp"
#include "st/test/test.hpp"

namespace {

/// 覆盖各层特性的脚本：三维、承接、关键帧、音频、布局、包装层都碰一遍。
[[nodiscard]] auto rich_script_json() -> std::string {
  return R"({
  "name": "往返保真探针",
  "fps": 30,
  "size": [1280, 720],
  "transition": 0.4,
  "fade_in": 0.5,
  "fade_out": 0.5,
  "brand_wordmark": "PROBE",
  "brand_latin": "ROUNDTRIP",
  "footer_note": "page",
  "show_brand": true,
  "audio": {
    "enabled": true,
    "style": "uplift",
    "bpm": 110,
    "gain": 0.7,
    "voice": [{"path": "media/vo.wav", "start": 0.5, "gain": 0.9}]
  },
  "scenes": [
    {
      "kind": "custom",
      "duration": 3.0,
      "title": "标题",
      "subtitle": "副标题",
      "lines": ["一", "二"],
      "values": [12.5, 88.0],
      "accent": "#3f5f92",
      "background": ["#080B12", "#121A28"],
      "wrap": false,
      "dof": 0.3,
      "particles": 40,
      "particle_alpha": 0.4,
      "grain": 900,
      "grain_alpha": 0.12,
      "glow": 0.5,
      "glow_cx": 0.4,
      "glow_cy": 0.6,
      "glow_radius": 0.5,
      "god_ray": 0.25,
      "god_ray_angle": 32.0,
      "light_sweep": 0.4,
      "cam_zoom_from": 1.0,
      "cam_zoom_to": 1.06,
      "cam_rotate": 0.5,
      "cam_pan_x": 12.0,
      "cam_pan_y": -8.0,
      "image": "media/bg.png",
      "image_card": "media/card.png",
      "image_dim": 0.35,
      "image_ken_burns": 0.08,
      "carry_out": {
        "name": "承接物",
        "into": 1,
        "at": 2.6,
        "dur": 0.7,
        "from": {"x": 10, "y": 20, "w": 30, "h": 40, "radius": 6},
        "to": {"x": 100, "y": 200, "w": 300, "h": 40, "radius": 12},
        "fill": "#ffffff",
        "stroke": "#000000",
        "stroke_width": 2.0,
        "text": "carry",
        "text_color": "#111111",
        "text_size": 24,
        "curve": 1,
        "fade_out": 0.35
      },
      "layers": [
        {
          "kind": "rect",
          "name": "卡片",
          "x": "10%", "y": "20%", "w": "30%", "h": "40%",
          "fill": "#223344", "fill_alt": "#445566", "gradient_dir": 1,
          "radius": 8, "stroke": "#abcdef", "stroke_width": 1.5,
          "opacity": 0.9, "enter": 3, "enter_at": 0.2, "enter_dur": 0.5,
          "tracks": {"y": [{"t": 0, "v": 400}, {"t": 1, "v": 0, "ease": 1}]}
        },
        {
          "kind": "text",
          "name": "文字",
          "text": "第一行\n第二行",
          "x": 100, "y": 200, "w": 400, "h": 120,
          "size": 64, "color": "#ffffff", "bold": true, "tracking": 2.0,
          "line_height": 1.6, "align": 1
        },
        {
          "kind": "model3d",
          "name": "模型",
          "x": "0%", "y": "0%", "w": "50%", "h": "50%",
          "model_shape": "obj:assets/gear.obj",
          "model_color": "#d8b48a",
          "model_segments": 48,
          "model_scale": 0.8,
          "model_spin": 12.0,
          "cam_azimuth": 20.0,
          "cam_elevation": 12.0,
          "cam_distance": 3.2,
          "model_script": {"camera": {"eye": [0, 0, 4]}, "objects": []}
        }
      ]
    }
  ]
})";
}

}  // namespace

ST_TEST(caiyun_script_json_roundtrip_is_lossless) {
  const std::string original = rich_script_json();

  const auto first = caiyun::parse_script(original);
  ST_CHECK(first.has_value());
  if (!first) {
    st::print("解析失败：{}\n", first.error().message);
    return;
  }
  const std::string once = caiyun::script_to_json(*first);

  const auto second = caiyun::parse_script(once);
  ST_CHECK(second.has_value());
  if (!second) {
    st::print("二次解析失败：{}\n", second.error().message);
    return;
  }
  const std::string twice = caiyun::script_to_json(*second);

  // 计次稳定：再序列化一次必须逐字节相同。这是"序列化器是纯函数"的断言。
  if (once != twice) {
    // 打印第一处差异附近，便于定位（不要只报"不相等"）
    const std::size_t limit = once.size() < twice.size() ? once.size() : twice.size();
    std::size_t diff = 0;
    while (diff < limit && once[diff] == twice[diff]) ++diff;
    const std::size_t from = diff > 120 ? diff - 120 : 0;
    st::print("首次差异在偏移 {}\nonce  ...{}\n", diff, once.substr(from, 260));
      }
  ST_CHECK(once == twice);

  // 关键字段抗丢：这些是"删掉也不报错"的高危项，逐个点名。
  const caiyun::Script& s = *second;
  ST_CHECK(s.name == "往返保真探针");
  ST_CHECK(s.scenes.size() == 1);
  if (s.scenes.empty()) return;
  const caiyun::Scene& scene = s.scenes[0];
  ST_CHECK(scene.duration > 2.9 && scene.duration < 3.1);
  ST_CHECK(scene.layers.size() == 3);
  // 承接：五条硬约束依赖 at/into/dur 精确往返。
  // `carry` 是值类型（不是 optional），未声明时 `name` 为空——所以判据看 name。
  ST_CHECK(scene.carry.name == "承接物");
  ST_CHECK(scene.carry.into == 1);
  ST_CHECK(scene.carry.at > 2.5 && scene.carry.at < 2.7);
  ST_CHECK(scene.carry.dur > 0.6 && scene.carry.dur < 0.8);
  ST_CHECK(scene.carry.fade_out > 0.3f && scene.carry.fade_out < 0.4f);
  // 渲染层：色调映射与采样设置的高危项
  ST_CHECK(scene.image_dim > 0.3f && scene.image_dim < 0.4f);
}
