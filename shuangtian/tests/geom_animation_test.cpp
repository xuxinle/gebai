/// 关键帧动画的回归测试：四元数、缓动、采样、端到端。
///
/// ## 判据为什么不能只看"动起来了"
///
/// 动画的典型缺陷是**看起来在动、但动得不对**：
/// - 欧拉角插值绕远路（350°→10° 会转 340° 而不是 20°）；
/// - slerp 没做"最短路径翻转"（q 与 -q 表示同一旋转，不翻会绕过 360°-θ）；
/// - 缓动写在关键帧上但采样时没用（观感全是匀速）；
/// - 采样改了 local 但世界矩阵没重算（慢一帧）。
///
/// 这些全都能"画出一段动画"，所以每条都要有可复算的数字判据。
#include <cmath>
#include <string>
#include <vector>

#include "st/core/log.hpp"
#include "st/math/quaternion.hpp"
#include "st/raster/animation.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_graph.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Quat;
using st::math::Vec3;
using st::raster::Clip;
using st::raster::Easing;
using st::raster::Keyframe;
using st::raster::SceneNode;
using st::raster::Track;
using st::raster::TrackKind;

constexpr float kPi = 3.14159265358979f;

/// 数非背景像素（与其它集成测试同一口径）。
auto inked(const st::raster::Canvas& canvas) -> int {
  const st::math::Color background = canvas.pixel_at(0, 0);
  int count = 0;
  for (int y = 0; y < canvas.physical_height(); ++y) {
    for (int x = 0; x < canvas.physical_width(); ++x) {
      const st::math::Color pixel = canvas.pixel_at(x, y);
      const int dr = static_cast<int>(pixel.r) - static_cast<int>(background.r);
      const int dg = static_cast<int>(pixel.g) - static_cast<int>(background.g);
      const int db = static_cast<int>(pixel.b) - static_cast<int>(background.b);
      if (dr * dr + dg * dg + db * db > 400) ++count;
    }
  }
  return count;
}

auto render_at(const std::string& json, float time) -> int {
  auto spec = st::raster::parse_scene_spec_text(json);
  if (!spec) {
    st::log::err("解析失败：{}\n", spec.error().message);
    return -1;
  }
  st::raster::Canvas canvas(220, 200);
  const auto status =
      st::raster::render_scene_at(*spec, time, canvas, st::math::Color{14, 15, 19, 255});
  if (!status) {
    st::log::err("渲染失败：{}\n", status.error().message);
    return -1;
  }
  return inked(canvas);
}

}  // namespace

ST_TEST(quat_slerp_takes_the_short_path) {
  // ## 这条钉住"绕远路"这个经典缺陷
  //
  // 从 350° 到 10°（都绕 Z）应当走 **20°** 的短弧，中点约 0°。
  // 若 slerp 没做点积翻转，它会走 340° 的长弧，中点约 180°——
  // 画面表现是"物体先反向猛转大半圈再到位"，而短动画看不出来。
  const Quat a = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 350.0f * kPi / 180.0f);
  const Quat b = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 10.0f * kPi / 180.0f);
  const Quat mid = st::math::slerp(a, b, 0.5f);
  const Vec3 euler = mid.to_euler();
  // 中点应当在 0°（±5°）；若走了长路径会落在 ±180° 附近
  const float degrees = euler.z * 180.0f / kPi;
  st::log::out("[anim] slerp mid z={:.2f}deg\n", static_cast<double>(degrees));
  // 中点在 0° 附近（±5°）；若走长路径会落在 ±180°——用"到 0 的角距离"判
  const float wrapped = std::abs(std::remainder(degrees, 360.0f));
  ST_CHECK(wrapped < 5.0f);

  // 端点必须精确命中。
  //
  // ⚠ 判据要按**等价旋转**写，不能钉死 350°：`to_euler` 的 `atan2` 值域是
  // (-180°, 180°]，所以 350° 会被还原成 -10° —— 那是**同一个旋转**，
  // 精度上也完全正确（实测差 -359.99999°，归一化后为 0）。
  // 钉死 350 会得到一个"看着像失败、其实是对的"的红。
  const auto normalize_degrees = [](float angle) {
    float value = std::fmod(angle, 360.0f);
    if (value < 0.0f) value += 360.0f;
    return value;
  };
  const auto angular_distance = [&](float lhs, float rhs) {
    const float diff = std::abs(normalize_degrees(lhs) - normalize_degrees(rhs));
    return diff > 180.0f ? 360.0f - diff : diff;
  };
  const Vec3 start = st::math::slerp(a, b, 0.0f).to_euler();
  const Vec3 end = st::math::slerp(a, b, 1.0f).to_euler();
  ST_CHECK(angular_distance(start.z * 180.0f / kPi, 350.0f) < 1.0f);
  ST_CHECK(angular_distance(end.z * 180.0f / kPi, 10.0f) < 1.0f);
}

ST_TEST(quat_euler_round_trip_is_stable) {
  // 欧拉 → 四元数 → 欧拉 往返：非万向锁区间必须还原
  const Vec3 cases[] = {{0.3f, 0.5f, 0.7f}, {-1.2f, 0.4f, 2.1f}, {0.0f, 0.0f, 0.0f},
                        {1.5f, -0.8f, -2.5f}};
  for (const Vec3& euler : cases) {
    const Vec3 back = Quat::from_euler(euler).to_euler();
    ST_CHECK_NEAR(back.x, euler.x, 0.02f);
    ST_CHECK_NEAR(back.y, euler.y, 0.02f);
    ST_CHECK_NEAR(back.z, euler.z, 0.02f);
  }
  // 四元数转矩阵与 `Mat4::rotation` 同口径：绕 Z 转 90° 把 +X 映到 +Y
  const Quat q = Quat::from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, kPi * 0.5f);
  const st::math::Mat4 m = q.to_matrix();
  const Vec3 mapped{m.value(0, 0), m.value(1, 0), m.value(2, 0)};
  ST_CHECK_NEAR(mapped.x, 0.0f, 1e-4f);
  ST_CHECK_NEAR(mapped.y, 1.0f, 1e-4f);
}

ST_TEST(anim_easing_shapes_the_curve) {
  // 缓动必须是"曲线"，而不是被忽略的装饰：同一段 0→1，
  // ease-in 的中点 < 0.5、ease-out 的中点 > 0.5、linear 恰为 0.5。
  ST_CHECK_NEAR(st::raster::apply_easing(Easing::Linear, 0.5f), 0.5f, 1e-5f);
  ST_CHECK(st::raster::apply_easing(Easing::EaseIn, 0.5f) < 0.4f);
  ST_CHECK(st::raster::apply_easing(Easing::EaseOut, 0.5f) > 0.6f);
  ST_CHECK_NEAR(st::raster::apply_easing(Easing::EaseInOut, 0.5f), 0.5f, 1e-5f);
  // 端点恒等（任何缓动都不该修饰首尾）
  for (const Easing easing : {Easing::Linear, Easing::EaseIn, Easing::EaseOut,
                              Easing::EaseInOut}) {
    ST_CHECK_NEAR(st::raster::apply_easing(easing, 0.0f), 0.0f, 1e-5f);
    ST_CHECK_NEAR(st::raster::apply_easing(easing, 1.0f), 1.0f, 1e-5f);
  }
  // 阶跃：整段保持前值（在关键帧处才跳）
  ST_CHECK_NEAR(st::raster::apply_easing(Easing::Constant, 0.99f), 0.0f, 1e-5f);
  // 名字往返 + 别名
  ST_CHECK(st::raster::parse_easing("ease-in-out").value() == Easing::EaseInOut);
  ST_CHECK(st::raster::parse_easing("Smooth").value() == Easing::EaseInOut);
  ST_CHECK(!st::raster::parse_easing("bounce").has_value());
  ST_CHECK(st::raster::parse_track_kind("position").value() == TrackKind::Translation);
  ST_CHECK(st::raster::parse_track_kind("rotation").value() == TrackKind::Rotation);
  ST_CHECK(!st::raster::parse_track_kind("colour").has_value());
}

ST_TEST(anim_sample_holds_endpoints_and_interpolates) {
  // 端点保持：范围外不循环、不外推
  Track track{};
  track.node = "n";
  track.kind = TrackKind::Translation;
  track.keys = {Keyframe{0.0f, Vec3{0.0f, 0.0f, 0.0f}, Easing::Linear},
                Keyframe{2.0f, Vec3{2.0f, 0.0f, 0.0f}, Easing::Linear}};
  Clip clip{};
  clip.name = "test";
  clip.tracks = {track};

  const auto at = [&](float time) {
    const auto sampled = st::raster::sample_clip(clip, time);
    return sampled.empty() ? Vec3{} : sampled.front().local.translation;
  };
  ST_CHECK_NEAR(at(-1.0f).x, 0.0f, 1e-5f);   // 之前：取首值
  ST_CHECK_NEAR(at(0.0f).x, 0.0f, 1e-5f);
  ST_CHECK_NEAR(at(1.0f).x, 1.0f, 1e-5f);    // 中点
  ST_CHECK_NEAR(at(2.0f).x, 2.0f, 1e-5f);
  ST_CHECK_NEAR(at(9.0f).x, 2.0f, 1e-5f);    // 之后：取末值（不循环）

  // 缓动真的被采样器消费（同一段、换成 ease-out，中点值必须变大）
  clip.tracks[0].keys[0].easing = Easing::EaseOut;
  ST_CHECK(at(1.0f).x > 1.2f);
  clip.tracks[0].keys[0].easing = Easing::EaseIn;
  ST_CHECK(at(1.0f).x < 0.8f);
}

ST_TEST(anim_rotation_uses_slerp_not_euler_lerp) {
  // 同一段 350°→10° 的旋转轨道：采样中点的 Z 角必须在 0 附近。
  // 若这里退化成"欧拉角逐分量线性插值"，中点会是 180°（绕远路）。
  Track track{};
  track.node = "n";
  track.kind = TrackKind::Rotation;
  track.keys = {
      Keyframe{0.0f, Vec3{0.0f, 0.0f, 350.0f * kPi / 180.0f}, Easing::Linear},
      Keyframe{1.0f, Vec3{0.0f, 0.0f, 10.0f * kPi / 180.0f}, Easing::Linear}};
  Clip clip{};
  clip.tracks = {track};
  const auto sampled = st::raster::sample_clip(clip, 0.5f);
  ST_REQUIRE(!sampled.empty());
  const float degrees = sampled.front().local.rotation.z * 180.0f / kPi;
  st::log::out("[anim] clip mid rotate z={:.2f}deg\n", static_cast<double>(degrees));
  const float wrapped = std::abs(std::remainder(degrees, 360.0f));
  ST_CHECK(wrapped < 6.0f);
}

ST_TEST(anim_visible_track_is_stepwise) {
  // 可见性轨道：整段保持前值，在关键帧处跳变（不是渐变）
  Track track{};
  track.node = "n";
  track.kind = TrackKind::Visible;
  track.keys = {Keyframe{0.0f, Vec3{1.0f, 0.0f, 0.0f}, Easing::Linear},
                Keyframe{1.0f, Vec3{0.0f, 0.0f, 0.0f}, Easing::Linear}};
  Clip clip{};
  clip.tracks = {track};
  ST_CHECK(st::raster::sample_clip(clip, 0.0f).front().visible);
  ST_CHECK(st::raster::sample_clip(clip, 0.99f).front().visible);
  ST_CHECK(!st::raster::sample_clip(clip, 1.0f).front().visible);
}

ST_TEST(anim_validate_rejects_bad_tracks) {
  SceneNode root{};
  root.name = "arm";
  root.has_geometry = false;

  Clip clip{};
  Track missing{};
  missing.node = "ghost";
  missing.kind = TrackKind::Translation;
  missing.keys = {Keyframe{0.0f, Vec3{}, Easing::Linear}};
  clip.tracks = {missing};
  const auto not_found = st::raster::validate_clip(clip, {root});
  ST_REQUIRE(!not_found.has_value());
  ST_CHECK(not_found.error().message.find("ghost") != std::string::npos);

  // 同一节点同一属性两条轨道 → 拒绝（"谁赢"取决于顺序，不可复现）
  clip.tracks.clear();
  Track a{};
  a.node = "arm";
  a.kind = TrackKind::Rotation;
  a.keys = {Keyframe{0.0f, Vec3{}, Easing::Linear}};
  Track b{};
  b.node = "arm";
  b.kind = TrackKind::Rotation;
  b.keys = {Keyframe{1.0f, Vec3{1.0f, 0.0f, 0.0f}, Easing::Linear}};
  clip.tracks = {a, b};
  const auto duplicated = st::raster::validate_clip(clip, {root});
  ST_REQUIRE(!duplicated.has_value());
  ST_CHECK(duplicated.error().message.find("重复") != std::string::npos);

  // 空关键帧 → 拒绝
  clip.tracks.clear();
  Track empty{};
  empty.node = "arm";
  empty.kind = TrackKind::Scale;
  clip.tracks = {empty};
  ST_REQUIRE(!st::raster::validate_clip(clip, {root}).has_value());
}

ST_TEST(anim_script_animation_moves_the_model) {
  // 端到端：肩关节在 0/1/2 秒摆动，渲染三个时刻必须得到**三个不同的画面**。
  const std::string json = R"j({
  "camera": {
    "eye": [
      1.2,
      1.0,
      2.4
    ],
    "target": [
      0.0,
      0.5,
      0.0
    ],
    "fov": 40
  },
  "lighting": {
    "ambient": 0.35,
    "diffuse": 0.8
  },
  "objects": [
    {
      "name": "base",
      "shape": "cylinder",
      "color": "#8a8f99",
      "translate": [
        0.0,
        0.05,
        0.0
      ],
      "size": [
        0.4,
        0.1
      ]
    },
    {
      "name": "shoulder",
      "group": true,
      "translate": [
        0.0,
        0.1,
        0.0
      ],
      "children": [
        {
          "name": "upper",
          "shape": "capsule",
          "color": "#d4b07a",
          "translate": [
            0.0,
            0.3,
            0.0
          ],
          "size": [
            0.14,
            0.7
          ]
        }
      ]
    }
  ],
  "animation": {
    "name": "swing",
    "duration": 2.0,
    "tracks": [
      {
        "node": "shoulder",
        "kind": "rotation",
        "keys": [
          {
            "t": 0.0,
            "value": [
              0,
              0,
              0
            ],
            "easing": "ease-in-out"
          },
          {
            "t": 1.0,
            "value": [
              0,
              0,
              0.9
            ]
          },
          {
            "t": 2.0,
            "value": [
              0,
              0,
              0
            ]
          }
        ]
      }
    ]
  }
})j";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  ST_REQUIRE(spec->animation.has_value());
  ST_CHECK_EQ(spec->animation->tracks.size(), 1U);
  ST_CHECK_NEAR(spec->animation->duration(), 2.0f, 1e-5f);

  const int ink_start = render_at(json, 0.0f);
  const int ink_mid = render_at(json, 1.0f);
  const int ink_end = render_at(json, 2.0f);
  st::log::out("[anim] ink t=0 {} t=1 {} t=2 {}\n", ink_start, ink_mid, ink_end);
  ST_CHECK(ink_start > 0);
  ST_CHECK(ink_mid > 0);
  // 摆到 0.9 rad 时覆盖像素必然变化（手臂位置不同）
  ST_CHECK(ink_mid != ink_start);
  // 回到原位：与起始帧**一致**（这是"回到起点"的判据，不是"也变了")
  ST_CHECK_EQ(ink_end, ink_start);
}

ST_TEST(anim_script_without_time_matches_static) {
  // 没有动画的场景：`render_scene` 与 `render_scene_at(任意 t)` 必须逐像素相同。
  // 这条保"渐进采用"：加了动画层不能改变静态场景的出图结果。
  const std::string json = R"j({
  "camera": {
    "eye": [
      0.0,
      0.6,
      3.0
    ],
    "target": [
      0.0,
      0.0,
      0.0
    ],
    "fov": 42
  },
  "objects": [
    {
      "shape": "uv_sphere",
      "color": "#c8a07a",
      "size": 1.2
    }
  ]
})j";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  ST_CHECK(!spec->animation.has_value());

  st::raster::Canvas a(160, 160);
  st::raster::Canvas b(160, 160);
  ST_CHECK(st::raster::render_scene(*spec, a, st::math::Color{10, 10, 12, 255}).has_value());
  ST_CHECK(
      st::raster::render_scene_at(*spec, 1.7f, b, st::math::Color{10, 10, 12, 255}).has_value());
  const auto pa = a.to_rgba8();
  const auto pb = b.to_rgba8();
  ST_CHECK_EQ(pa.size(), pb.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
    if (pa[i] != pb[i]) ++differing;
  }
  ST_CHECK_EQ(static_cast<int>(differing), 0);
}

ST_TEST(anim_script_errors_carry_paths) {
  // 动画的每类错误都要指到字段：这是生成代码的模型唯一能自我修正的线索
  const auto parse = [](const std::string& json) {
    return st::raster::parse_scene_spec_text(json);
  };
  // 1) track 没写 node
  auto no_node = parse(R"j({"objects":[],"animation":{"tracks":[{"kind":"rotation","keys":[]}]}})j");
  ST_REQUIRE(!no_node.has_value());
  ST_CHECK(no_node.error().message.find("tracks[0].node") != std::string::npos);
  // 2) kind 拼错 → 给可用清单
  auto bad_kind = parse(
      R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"colour","keys":[]}]}})j");
  ST_REQUIRE(!bad_kind.has_value());
  ST_CHECK(bad_kind.error().message.find("tracks[0].kind") != std::string::npos);
  ST_CHECK(bad_kind.error().message.find("rotation") != std::string::npos);
  // 3) key 缺时间
  auto no_time = parse(
      R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"rotation","keys":[{"value":[0,0,0]}]}]}})j");
  ST_REQUIRE(!no_time.has_value());
  ST_CHECK(no_time.error().message.find("keys[0].t") != std::string::npos);
  // 4) easing 拼错 → 给可用清单
  auto bad_easing = parse(
      R"j({"objects":[],"animation":{"tracks":[{"node":"a","kind":"rotation","keys":[{"t":0,"value":[0,0,0],"easing":"bounce"}]}]}})j");
  ST_REQUIRE(!bad_easing.has_value());
  ST_CHECK(bad_easing.error().message.find("easing") != std::string::npos);
  ST_CHECK(bad_easing.error().message.find("ease-in-out") != std::string::npos);
  // 5) 轨道指向不存在的节点（这条在**渲染**时才查，因为解析时节点还没建树）
  auto spec = parse(
      R"j({"objects":[{"name":"a","shape":"cube"}],"animation":{"tracks":[{"node":"ghost","kind":"rotation","keys":[{"t":0,"value":[0,0,0]}]}]}})j");
  ST_REQUIRE(spec.has_value());
  st::raster::Canvas canvas(64, 64);
  const auto rendered =
      st::raster::render_scene_at(*spec, 0.0f, canvas, st::math::Color{0, 0, 0, 255});
  ST_REQUIRE(!rendered.has_value());
  ST_CHECK(rendered.error().message.find("ghost") != std::string::npos);
}

ST_TEST(anim_keyframes_are_sorted_independent_of_input_order) {
  // 手写 JSON 常按叙事顺序写关键帧（不按时间）。解析必须排序，
  // 否则采样会取到错的分段（症状：动画抖一下然后跳）。
  const std::string json = R"j({
  "objects": [
    {
      "name": "box",
      "shape": "cube",
      "size": [
        0.8,
        1.0
      ]
    }
  ],
  "animation": {
    "tracks": [
      {
        "node": "box",
        "kind": "translate",
        "keys": [
          {
            "t": 2.0,
            "value": [
              2,
              0,
              0
            ]
          },
          {
            "t": 0.0,
            "value": [
              0,
              0,
              0
            ]
          },
          {
            "t": 1.0,
            "value": [
              1,
              0,
              0
            ]
          }
        ]
      }
    ]
  }
})j";
  auto spec = st::raster::parse_scene_spec_text(json);
  ST_REQUIRE(spec.has_value());
  ST_REQUIRE(spec->animation.has_value());
  const auto& keys = spec->animation->tracks[0].keys;
  ST_REQUIRE(keys.size() == 3U);
  ST_CHECK_NEAR(keys[0].time, 0.0f, 1e-6f);
  ST_CHECK_NEAR(keys[1].time, 1.0f, 1e-6f);
  ST_CHECK_NEAR(keys[2].time, 2.0f, 1e-6f);

  const auto sampled = st::raster::sample_clip(*spec->animation, 0.5f);
  ST_REQUIRE(!sampled.empty());
  ST_CHECK_NEAR(sampled.front().local.translation.x, 0.5f, 1e-5f);
}

ST_TEST(anim_pivot_and_animation_compose) {
  // ## 这条钉住一个真缺陷：枢轴预设把动画结果覆盖掉了
  //
  // `world_matrix_of` 原先在算枢轴预设时从 spec **重新**构造节点
  //（`to_node(*found->second)`），于是动画刚采好的 `local` 被场景里写死的
  // 初始值覆盖。症状极具误导性：只有**设了 `pivot_at` 的节点**纹丝不动，
  // 其它节点正常动画，而采样日志里角度明明在变。
  //
  // 判据：同一份模型，带动画的两个时刻渲染结果必须不同，**且**该节点同时有
  // `pivot_at`（否则这条测不到那个覆盖路径）。
  const std::string json = R"j({
  "camera": {
    "eye": [
      0.9,
      0.75,
      1.7
    ],
    "target": [
      0.0,
      0.5,
      0.0
    ],
    "fov": 36
  },
  "objects": [
    {
      "name": "pivot_node",
      "group": true,
      "translate": [
        0.0,
        0.1,
        0.0
      ],
      "pivot_at": "bottom",
      "children": [
        {
          "name": "arm",
          "shape": "capsule",
          "color": "#d4b07a",
          "translate": [
            0.0,
            0.3,
            0.0
          ],
          "size": [
            0.14,
            0.6
          ]
        }
      ]
    }
  ],
  "animation": {
    "tracks": [
      {
        "node": "pivot_node",
        "kind": "rotation",
        "keys": [
          {
            "t": 0.0,
            "value": [
              0,
              0,
              0
            ]
          },
          {
            "t": 1.0,
            "value": [
              0,
              0,
              1.2
            ]
          }
        ]
      }
    ]
  }
})j";
  const int ink_start = render_at(json, 0.0f);
  const int ink_end = render_at(json, 1.0f);
  st::log::out("[anim] pivot+anim ink t=0 {} t=1 {}\n", ink_start, ink_end);
  ST_CHECK(ink_start > 0);
  ST_CHECK(ink_end > 0);
  // 关键判据：设了 pivot_at 的节点也必须真的动
  ST_CHECK(ink_start != ink_end);
}

ST_TEST(anim_resolved_node_carries_sampled_local) {
  // `ResolvedNode::local` 必须等于**动画后**的局部值（而不是场景初始值）。
  // 它是"枢轴重算/导出当前姿态"的唯一正确来源，退回初始值就会静默错位。
  SceneNode root{};
  root.name = "n";
  root.has_geometry = true;
  root.local.rotation = st::math::Vec3{0.0f, 0.0f, 0.2f};   // 场景里写死的初始值
  std::vector<SceneNode> roots{root};

  Track track{};
  track.node = "n";
  track.kind = TrackKind::Rotation;
  track.keys = {Keyframe{0.0f, st::math::Vec3{0.0f, 0.0f, 0.0f}, Easing::Linear},
                Keyframe{1.0f, st::math::Vec3{0.0f, 0.0f, 1.0f}, Easing::Linear}};
  Clip clip{};
  clip.tracks = {track};

  const auto sampled = st::raster::sample_clip(clip, 0.5f);
  const auto animated = st::raster::apply_sample(roots, sampled);
  ST_REQUIRE(!animated.empty());
  // 采样中点约 0.5 rad（与场景里的 0.2 不同）
  ST_CHECK_NEAR(animated[0].local.rotation.z, 0.5f, 0.02f);
  const auto tree = st::raster::resolve(animated);
  ST_REQUIRE(tree.has_value());
  ST_REQUIRE(!tree->nodes.empty());
  ST_CHECK_NEAR(tree->nodes[0].local.rotation.z, 0.5f, 0.02f);   // 带出的是采样值
  // 世界矩阵也必须跟着（绕 Z 转 0.5 rad 的 sin 分量）
  ST_CHECK_NEAR(tree->nodes[0].world.value(0, 1), -std::sin(0.5f), 0.02f);
}
