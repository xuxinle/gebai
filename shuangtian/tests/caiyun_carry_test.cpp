/// 裁云跨场承接层（carry）的单测。
///
/// ## 这组用例钉的是什么
///
/// 承接层的价值全在"**让两块画面之间有一个东西活下来**"这一件事上。
/// 它错了不会崩，只会退回到"每幕独立 + 交叉淡入"——也就是成熟动效片
/// 判定为幻灯片的那个形态。**画面上看不出错，只是分数上不去**。
///
/// 所以这里钉三件事：
/// 1. **求值是纯函数且边界精确**：起点那一帧与终点那一帧都必须 active
///    （否则边界两侧各缺一帧，而验收器恰好在那里取样）；
/// 2. **尺寸解析与图层同口径**：`"50%"` 与 `960` 在 1920 宽下必须等价，
///    否则承接会在某个画幅下"长到错的地方"；
/// 3. **不声明时不产生任何承接**：这是"不推翻现有架构"的可执行判据——
///    旧脚本必须逐像素不变，而"逐像素"没法在单测里直接断言，
///    所以断言它的**语义前提**（没有 active 的承接、collect 返回空）。

#include "examples/caiyun/carry.cpp"

#include "st/test/test.hpp"

namespace {

using caiyun::CarryOut;
using caiyun::CarryPose;

/// 一个典型的承接：从中央小卡长满整屏，时间轴 1.0s → 1.5s。
[[nodiscard]] auto make_carry() -> CarryOut {
  CarryOut carry{};
  carry.name = "hero";
  carry.at = 1.0;
  carry.dur = 0.5;
  carry.from.x = "38%";
  carry.from.y = "40%";
  carry.from.w = "24%";
  carry.from.h = "20%";
  carry.from.radius = 18.0f;
  carry.to.x = "0%";
  carry.to.y = "0%";
  carry.to.w = "100%";
  carry.to.h = "100%";
  carry.to.radius = 0.0f;
  carry.curve = "linear";  // 便于断言中间值
  return carry;
}

constexpr float kDesignW = 1920.0f;
constexpr float kDesignH = 1080.0f;

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 一、活动区间与端点（端点必须包含：验收器就在边界取样）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(carry_is_inactive_before_start) {
  const CarryOut carry = make_carry();
  ST_CHECK(!caiyun::evaluate_carry(carry, 0.9, kDesignW, kDesignH).active);
}

ST_TEST(carry_is_active_at_its_first_frame) {
  // 起点那一帧必须 active。若用 `time > at`（开区间），边界前最后一帧就没有承接，
  // 而验收器正好在那里取样 —— 一处 off-by-one 会让整个承接白做。
  const CarryPose pose = caiyun::evaluate_carry(make_carry(), 1.0, kDesignW, kDesignH);
  ST_CHECK(pose.active);
  ST_CHECK_NEAR(pose.k, 0.0f, 1e-5f);
  ST_CHECK_NEAR(pose.w, 1920.0f * 0.24f, 0.5f);
}

ST_TEST(carry_is_active_at_its_last_frame) {
  // 终点那一帧同理：边界后第一帧必须有承接，否则观众看到"长到一半消失"。
  const CarryPose pose = caiyun::evaluate_carry(make_carry(), 1.5, kDesignW, kDesignH);
  ST_CHECK(pose.active);
  ST_CHECK_NEAR(pose.k, 1.0f, 1e-5f);
  ST_CHECK_NEAR(pose.w, kDesignW, 0.5f);
}

ST_TEST(carry_is_inactive_after_end) {
  ST_CHECK(!caiyun::evaluate_carry(make_carry(), 1.55, kDesignW, kDesignH).active);
}

ST_TEST(carry_without_name_never_activates) {
  // 名字是验收器认身份的唯一依据；没名字的承接"画面上有、验收里不存在"，
  // 这种半吊子状态比不做更难查，所以直接不算。
  CarryOut carry = make_carry();
  carry.name.clear();
  ST_CHECK(!caiyun::evaluate_carry(carry, 1.2, kDesignW, kDesignH).active);
}

// ————————————————————————————————————————————————————————————————————————————
// 二、变形几何（起点 → 终点单调推进）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(carry_morphs_rect_monotonically) {
  const CarryOut carry = make_carry();
  const CarryPose early = caiyun::evaluate_carry(carry, 1.1, kDesignW, kDesignH);
  const CarryPose middle = caiyun::evaluate_carry(carry, 1.25, kDesignW, kDesignH);
  const CarryPose late = caiyun::evaluate_carry(carry, 1.4, kDesignW, kDesignH);
  // 宽度单调增长（承接的观感就是"长开"）
  ST_CHECK(early.w < middle.w);
  ST_CHECK(middle.w < late.w);
  // 位置单调向 0 靠（from.x 38% → to.x 0%）
  ST_CHECK(early.x > middle.x);
  ST_CHECK(middle.x > late.x);
  // 圆角单调减小（18 → 0）
  ST_CHECK(early.radius > middle.radius);
  ST_CHECK(middle.radius > late.radius);
}

ST_TEST(carry_uses_named_curve) {
  // 同一个时间点，`linear` 与 `expo_out` 必须给出**不同的** k。
  // 若缓动名没被接上（静默退化成同一条曲线），这条会红。
  CarryOut linear = make_carry();
  linear.curve = "linear";
  CarryOut expo = make_carry();
  expo.curve = "expo_out";
  const float k_linear = caiyun::evaluate_carry(linear, 1.25, kDesignW, kDesignH).k;
  const float k_expo = caiyun::evaluate_carry(expo, 1.25, kDesignW, kDesignH).k;
  ST_CHECK_NEAR(k_linear, 0.5f, 0.02f);
  ST_CHECK(k_expo > k_linear + 0.15f);  // expo_out 前段更快
}

ST_TEST(carry_unknown_curve_falls_back_to_expo_out) {
  CarryOut carry = make_carry();
  carry.curve = "definitely_not_a_curve";
  CarryOut reference = make_carry();
  reference.curve = "expo_out";
  ST_CHECK_NEAR(caiyun::evaluate_carry(carry, 1.25, kDesignW, kDesignH).k,
                caiyun::evaluate_carry(reference, 1.25, kDesignW, kDesignH).k, 1e-5f);
}

// ————————————————————————————————————————————————————————————————————————————
// 三、尺寸解析（与图层同口径）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(carry_dim_percent_matches_design_pixels) {
  // `"50%"` 在 1920 宽下必须等于 960 —— 两个写法是同一件事。
  ST_CHECK_NEAR(caiyun::resolve_carry_dim("50%", 1920.0f, -1.0f), 960.0f, 0.01f);
  ST_CHECK_NEAR(caiyun::resolve_carry_dim("960", 1920.0f, -1.0f), 960.0f, 0.01f);
  ST_CHECK_NEAR(caiyun::resolve_carry_dim(" 100 % ", 1920.0f, -1.0f), 1920.0f, 0.01f);
}

ST_TEST(carry_dim_garbage_falls_back) {
  // 非法输入必须落到 fallback（而不是 0）：承接落到 0 会让它消失，
  // 而"消失的承接"在画面上表现为"这一帧漏了一块"，很难归因。
  ST_CHECK_NEAR(caiyun::resolve_carry_dim("abc", 1920.0f, 7.0f), 7.0f, 0.01f);
  ST_CHECK_NEAR(caiyun::resolve_carry_dim("50px", 1920.0f, 7.0f), 7.0f, 0.01f);
  ST_CHECK_NEAR(caiyun::resolve_carry_dim("", 1920.0f, 7.0f), 7.0f, 0.01f);
}

// ————————————————————————————————————————————————————————————————————————————
// 四、淡出让位（`fade_out`）：长到终点时不能再盖住下一幕的内容
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(carry_fade_out_gives_way_at_the_end) {
  // 实测缺陷：承接长满整屏后，下一幕已进场的文字被它完全遮住——
  // 观众看到 0.4 秒空屏（第 160 帧全空）。
  // 修法：末段淡出，把画面让给真正的页面内容。
  CarryOut carry = make_carry();
  carry.fade_out = 0.4f;  // 最后 40% 的时间淡出

  // 前半段：完全不透明（它就是画面主体）
  ST_CHECK_NEAR(caiyun::evaluate_carry(carry, 1.1, kDesignW, kDesignH).alpha, 1.0f, 1e-4f);
  // 中点（淡出窗口前）：仍不透明
  ST_CHECK_NEAR(caiyun::evaluate_carry(carry, 1.25, kDesignW, kDesignH).alpha, 1.0f, 1e-4f);
  // 末段中点（raw_k = 0.8）：已淡到一半
  ST_CHECK_NEAR(caiyun::evaluate_carry(carry, 1.4, kDesignW, kDesignH).alpha, 0.5f, 0.02f);
  // 终点：完全透明（让位完成）
  ST_CHECK_NEAR(caiyun::evaluate_carry(carry, 1.5, kDesignW, kDesignH).alpha, 0.0f, 0.02f);

  // 不声明 `fade_out` 时恒为不透明（旧行为不变）
  CarryOut plain = make_carry();
  ST_CHECK_NEAR(caiyun::evaluate_carry(plain, 1.5, kDesignW, kDesignH).alpha, 1.0f, 1e-6f);
}

ST_TEST(carry_fade_out_is_monotonic) {
  // 淡出必须单调不增：中间反弹一下会被看成“闪了一下”。
  CarryOut carry = make_carry();
  carry.fade_out = 0.3f;
  float previous = 1.0f;
  for (int step = 0; step <= 30; ++step) {
    const double t = 1.0 + 0.5 * static_cast<double>(step) / 30.0;
    const float alpha = caiyun::evaluate_carry(carry, t, kDesignW, kDesignH).alpha;
    ST_CHECK(alpha <= previous + 1e-5f);
    previous = alpha;
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 五、"不声明时零影响"（不推翻现有架构的可执行判据）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(carry_collect_is_empty_when_no_scene_declares_one) {
  // 旧脚本没有 carry_out ⇒ collect 必须返回空 ⇒ 合成层一笔不画。
  // 这是"逐像素不变"的**语义前提**：只要这里空，就不可能有像素被改动。
  caiyun::Script script{};
  caiyun::Scene first{};
  first.duration = 1.0;
  caiyun::Scene second{};
  second.duration = 1.0;
  script.scenes = {first, second};
  const auto carries = caiyun::collect_active_carries(script, 0.5, kDesignW, kDesignH);
  ST_CHECK(carries.empty());
}

ST_TEST(carry_collect_reports_the_active_one) {
  caiyun::Script script{};
  caiyun::Scene first{};
  first.duration = 1.0;
  first.carry = make_carry();
  caiyun::Scene second{};
  second.duration = 1.0;
  script.scenes = {first, second};

  // 活动区间内：必须被收集到，且带上名字与当时的矩形
  const auto during = caiyun::collect_active_carries(script, 1.25, kDesignW, kDesignH);
  ST_REQUIRE(during.size() == 1);
  ST_CHECK_EQ(during[0].name, std::string("hero"));
  ST_CHECK(during[0].w > 0.0f);

  // 区间外：必须不收（否则验收器会看到一个"永远在场"的身份，分数虚高）
  ST_CHECK(caiyun::collect_active_carries(script, 0.5, kDesignW, kDesignH).empty());
  ST_CHECK(caiyun::collect_active_carries(script, 2.0, kDesignW, kDesignH).empty());
}
