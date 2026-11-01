#include "fighter_art.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/math/path.hpp"

namespace gedou {

namespace {

using math::Color;
using math::Point;
using math::Rect;

/// 线段上的点（`t` = 0 在 `a`、1 在 `b`）。
[[nodiscard]] auto lerp_point(Point a, Point b, float t) -> Point {
  return Point{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

/// 两点距离。
[[nodiscard]] auto distance(Point a, Point b) -> float {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

/// 单位方向（零长度返回 +x，避免除零）。
[[nodiscard]] auto direction(Point from, Point to) -> Point {
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-4f) return Point{1.0f, 0.0f};
  return Point{dx / len, dy / len};
}

/// 垂直方向（左转 90°）。
[[nodiscard]] auto perpendicular(Point dir) -> Point { return Point{-dir.y, dir.x}; }

/// 带透明度缩放。
[[nodiscard]] auto with_alpha(Color color, float factor) -> Color {
  const auto clamp_byte = [](float v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
  };
  return Color{color.r, color.g, color.b,
               clamp_byte(static_cast<float>(color.a) * std::clamp(factor, 0.0f, 1.0f))};
}

/// 两色混合（`t` = 0 取 a、1 取 b）。
[[nodiscard]] auto mix_color(Color a, Color b, float t) -> Color {
  const auto blend = [t](std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t,
                   0.0f, 255.0f));
  };
  return Color{blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b), blend(a.a, b.a)};
}

/// 画一条**带描边的胶囊**（肢体）。
///
/// ## 为什么用"两条曲线拼一个胶囊"而不是直线 + 圆头
///
/// 直接画粗线只能得到**等宽**的棍子，看起来像火柴人。真实的肢体是
/// **上宽下窄**（上臂粗、手腕细），而且两条边是**外凸的弧**。
/// 做法：沿法线方向各偏 `width_a`/`width_b` 画两条二次贝塞尔（控制点再外扩
/// 一点做出鼓感），两端用圆弧连起来。
///
/// 描边先画（粗一圈）再填色——这是卡通渲染的标准做法，
/// 比"描边后画"稳（后者会在曲线相交处露白）。
void draw_limb(st::raster::Surface& canvas, Point from, Point to, float width_from, float width_to,
               Color fill, Color outline, float outline_width) {
  const Point dir = direction(from, to);
  const Point normal = perpendicular(dir);
  const float length = distance(from, to);
  if (length < 0.5f) return;

  // 控制点：在中点沿法线外扩，做出"肌肉鼓"的弧。
  const float bulge = std::min(width_from, width_to) * 0.35f;

  const auto build = [&](float widen) {
    const float wa = width_from + widen;
    const float wb = width_to + widen;
    const Point mid = lerp_point(from, to, 0.5f);
    math::Path path;
    // 上侧（法线正方向）
    path.move_to(Point{from.x + normal.x * wa, from.y + normal.y * wa});
    path.quad_to(Point{mid.x + normal.x * (wa + wb) * 0.5f + normal.x * bulge,
                       mid.y + normal.y * (wa + wb) * 0.5f + normal.y * bulge},
                 Point{to.x + normal.x * wb, to.y + normal.y * wb});
    // 末端圆头（半圆，从 +法线 侧转到 -法线 侧）
    {
      const float end_angle = std::atan2(dir.y, dir.x) * 57.29578f;
      path.add_arc(to, wb, end_angle - 90.0f, end_angle + 90.0f);
    }
    // 下侧
    path.quad_to(Point{mid.x - normal.x * (wa + wb) * 0.5f - normal.x * bulge,
                       mid.y - normal.y * (wa + wb) * 0.5f - normal.y * bulge},
                 Point{from.x - normal.x * wa, from.y - normal.y * wa});
    // 起点圆头（另半圈）
    {
      const float start_angle = std::atan2(dir.y, dir.x) * 57.29578f;
      path.add_arc(from, wa, start_angle + 90.0f, start_angle + 270.0f);
    }
    path.close();
    return path;
  };

  if (outline_width > 0.0f) {
    canvas.fill_path(build(outline_width), st::raster::Paint::solid(outline));
  }
  canvas.fill_path(build(0.0f), st::raster::Paint::solid(fill));
}

/// 画一个带描边的圆（关节球 / 头部）。
void draw_round(st::raster::Surface& canvas, Point center, float radius, Color fill, Color outline,
                float outline_width) {
  if (outline_width > 0.0f) {
    canvas.fill_circle(center, radius + outline_width, st::raster::Paint::solid(outline));
  }
  canvas.fill_circle(center, radius, st::raster::Paint::solid(fill));
}

/// 画一个带描边的圆角矩形（躯干/衣服片）。
void draw_rounded(st::raster::Surface& canvas, Rect rect, float radius, Color fill, Color outline,
                  float outline_width) {
  if (outline_width > 0.0f) {
    const Rect outer{rect.x - outline_width, rect.y - outline_width, rect.width + outline_width * 2,
                     rect.height + outline_width * 2};
    canvas.fill_rect(outer, st::raster::Paint::solid(outline), radius + outline_width);
  }
  canvas.fill_rect(rect, st::raster::Paint::solid(fill), radius);
}

/// 沿两点之间画一条细线（衣褶/装饰线）。
void draw_line(st::raster::Surface& canvas, Point from, Point to, float width, Color color) {
  const Point dir = direction(from, to);
  const Point normal = perpendicular(dir);
  math::Path path;
  path.move_to(Point{from.x + normal.x * width * 0.5f, from.y + normal.y * width * 0.5f});
  path.line_to(Point{to.x + normal.x * width * 0.5f, to.y + normal.y * width * 0.5f});
  path.line_to(Point{to.x - normal.x * width * 0.5f, to.y - normal.y * width * 0.5f});
  path.line_to(Point{from.x - normal.x * width * 0.5f, from.y - normal.y * width * 0.5f});
  path.close();
  canvas.fill_path(path, st::raster::Paint::solid(color));
}

}  // namespace

auto fighter_palette(std::size_t profile_index) -> const FighterPalette& {
  // 甜甜圈：美国国旗棒球帽 + 红色运动夹克（田径风）。
  static const FighterPalette kDonut{
      .skin = Color{0xF2, 0xC8, 0xA8, 0xFF},
      .skin_shadow = Color{0xD4, 0xA2, 0x82, 0xFF},
      .cloth = Color{0xD9, 0x2B, 0x3A, 0xFF},
      .cloth_shadow = Color{0xA8, 0x1C, 0x2C, 0xFF},
      .cloth_accent = Color{0xFA, 0xFA, 0xFA, 0xFF},
      .pants = Color{0x2E, 0x34, 0x42, 0xFF},
      .pants_shadow = Color{0x1C, 0x20, 0x2A, 0xFF},
      .shoes = Color{0xF2, 0xF2, 0xF2, 0xFF},
      .hair = Color{0x3A, 0x28, 0x1C, 0xFF},
      .outline = Color{0x14, 0x10, 0x18, 0xFF},
      .has_cap = true,
      .cap_base = Color{0x1F, 0x3A, 0x93, 0xFF},
      .cap_stripe = Color{0xBF, 0x0A, 0x30, 0xFF},
      .has_glasses = false,
      .long_sleeves = true,
      .signature = Color{0xD9, 0x2B, 0x3A, 0xFF},
  };
  // 户圣：亮片眼镜 + 灰色 polo。
  static const FighterPalette kHusheng{
      .skin = Color{0xF2, 0xC8, 0xA8, 0xFF},
      .skin_shadow = Color{0xD8, 0xA8, 0x88, 0xFF},
      .cloth = Color{0x9A, 0x9E, 0xA6, 0xFF},
      .cloth_shadow = Color{0x74, 0x78, 0x80, 0xFF},
      .cloth_accent = Color{0x7E, 0x82, 0x8A, 0xFF},
      .pants = Color{0x3A, 0x3E, 0x46, 0xFF},
      .pants_shadow = Color{0x26, 0x2A, 0x32, 0xFF},
      .shoes = Color{0x1E, 0x1E, 0x22, 0xFF},
      .hair = Color{0x22, 0x1A, 0x16, 0xFF},
      .outline = Color{0x14, 0x10, 0x18, 0xFF},
      .has_cap = false,
      .has_glasses = true,
      .glasses_frame = Color{0xC8, 0xA8, 0x50, 0xFF},
      .long_sleeves = false,
      .signature = Color{0x9A, 0x9E, 0xA6, 0xFF},
  };
  return profile_index == 0 ? kDonut : kHusheng;
}

void draw_fighter(st::raster::Surface& canvas, const Pose& raw_pose, const FighterPalette& palette,
                  const FighterDrawOptions& options) {
  // ── 身高归一化 ──
  //
  // 矢量造型里所有尺寸都由 `head_radius` 派生（头 0.82、肩 1.32、上臂 0.50…），
  // 所以"整体多大"完全取决于这一个值。骨架给的 `head_radius` 是按**判定盒**
  // 定的（约 19 像素 ⇒ 身高仅约 120px），而立绘按骨架的"脚底到头顶"摆位
  //（约 240px）——两者差一倍，切画法时人物会突然变小（实测截图里就是这样）。
  //
  // 这里把 `head_radius` 统一放大到"与骨架实际身高匹配"：
  // 从 foot.y 到 (head.y - radius) 是真实身高，除以 6.0（矢量的头身比）即为目标半径。
  Pose pose = raw_pose;
  {
    const float foot_y = std::max(raw_pose.foot_front.y, raw_pose.foot_back.y);
    const float top_y = raw_pose.head.y - raw_pose.head_radius;
    const float span = foot_y - top_y;
    if (span > 10.0f) {
      // 6.0：从脚底到头顶约等于 6 个头半径（这里画出来头约 0.82 半径，
      // 即头顶到脚底 ≈ 7.3 个头半径 —— 取 6.0 让角色显得更"Q"一点，
      // 与立绘的动漫比例接近）。
      const float target_radius = span / 6.0f;
      if (target_radius > raw_pose.head_radius * 0.5f) {
        pose.head_radius = target_radius;
      }
    }
  }
  const auto tint = [&](Color color) {
    // 受击闪白：把颜色往白色推（保留 alpha）。
    if (options.flash > 0.001f) {
      color = mix_color(color, Color{0xFF, 0xF4, 0xE8, color.a}, options.flash);
    }
    return with_alpha(color, options.alpha);
  };

  const Color outline = tint(palette.outline);
  const float ow = 2.6f;   // 描边宽度（像素）

  // ── 落地阴影（先画，在最底层）──
  {
    const float ground_y = std::max(pose.foot_front.y, pose.foot_back.y);
    const float middle_x = (pose.foot_front.x + pose.foot_back.x) * 0.5f;
    const float radius = pose.head_radius * 1.5f * options.shadow_scale;
    math::Path shadow;
    shadow.add_ellipse(Rect{middle_x - radius, ground_y - radius * 0.20f, radius * 2.0f,
                            radius * 0.40f});
    canvas.fill_path(shadow,
                     st::raster::Paint::solid(Color{0x00, 0x00, 0x00,
                                                    static_cast<std::uint8_t>(
                                                        200.0f * options.shadow_alpha)}));
  }

  // ── **后侧**肢体（先画，被身体挡住一部分）──
  //
  // 先画后侧是"立体感"的关键：后手/后腿被躯干遮住一截，
  // 眼睛会自动读成"两条胳膊在前后不同深度"。全画在最上层就变成"千手观音"。
  // 肢体粗细：这些数字决定"像人"还是"像火柴人"。
  // 实测 head_radius 约 19px（kBodyScale=1.6 后约 30px），
  // 上臂约头半径的 0.42、前臂 0.34、手腕 0.28 —— 这是按人体比例来的。
  const float limb_width_upper = pose.head_radius * 0.50f;
  const float limb_width_lower = pose.head_radius * 0.40f;
  const float limb_width_wrist = pose.head_radius * 0.30f;
  const Color skin_shadow = tint(palette.skin_shadow);
  const Color pants_shadow = tint(palette.pants_shadow);
  const Color cloth_shadow = tint(palette.cloth_shadow);

  // 后腿
  draw_limb(canvas, pose.hip, pose.knee_back, limb_width_upper * 1.05f, limb_width_lower,
            pants_shadow, outline, ow);
  draw_limb(canvas, pose.knee_back, pose.foot_back, limb_width_lower, limb_width_wrist,
            pants_shadow, outline, ow);
  // 后脚（鞋：扁的圆角矩形，朝朝向那一侧伸出去）
  {
    const float side_f = options.facing_right ? 1.0f : -1.0f;
    const float shoe_len = pose.head_radius * 0.62f;
    const float shoe_h = pose.head_radius * 0.26f;
    const Rect shoe{pose.foot_back.x - shoe_len * 0.35f + (side_f > 0 ? 0.0f : -shoe_len * 0.3f),
                    pose.foot_back.y - shoe_h * 0.35f, shoe_len, shoe_h};
    draw_rounded(canvas, shoe, shoe_h * 0.45f,
                 tint(mix_color(palette.shoes, Color{0x00, 0x00, 0x00, 0xFF}, 0.42f)), outline, ow);
  }
  // 后臂（袖子 + 前臂）
  {
    // 袖子：长袖到手腕、短袖到上臂中段。分两段画，接缝处才有"布"的感觉。
    const Point sleeve_end_back =
        palette.long_sleeves ? lerp_point(pose.elbow_back, pose.hand_back, 0.88f)
                             : lerp_point(pose.shoulder_back, pose.elbow_back, 0.62f);
    const Color sleeve_color = palette.long_sleeves ? cloth_shadow : skin_shadow;
    draw_limb(canvas, pose.shoulder_back, sleeve_end_back, limb_width_upper,
              limb_width_lower * 0.92f, sleeve_color, outline, ow);
    // 露出的前臂
    draw_limb(canvas, sleeve_end_back, pose.hand_back, limb_width_lower * 0.92f,
              limb_width_wrist, skin_shadow, outline, ow);
    draw_round(canvas, pose.hand_back, limb_width_lower * 0.95f, skin_shadow, outline, ow);
  }

  // ── 躯干 ──
  //
  // 用一个**上宽下窄的四边形**而不是矩形：肩膀比腰宽，这是人的体态。
  // 再叠一条明暗分界（左半边压暗），躯干就有圆柱感。
  const Point hip = pose.hip;
  const Point chest = pose.chest;
  const Point dir_torso = direction(hip, chest);
  const Point normal_torso = perpendicular(dir_torso);
  // 躯干宽度：肩比腰宽（人的体态）。头半径做参照，
  // 真人肩宽约 1.6 个头宽、腰约 1.2 个头宽。
  const float shoulder_half = pose.head_radius * 1.32f;
  const float hip_half = pose.head_radius * 1.02f;

  const auto torso_polygon = [&](float widen) -> math::Path {
    const float sh = shoulder_half + widen;
    const float hp = hip_half + widen;
    // 躯干顶端稍微超过 chest（让肩线自然）
    const Point top = Point{chest.x + dir_torso.x * pose.head_radius * 0.22f,
                            chest.y + dir_torso.y * pose.head_radius * 0.22f};
    math::Path path;
    path.move_to(Point{top.x + normal_torso.x * sh, top.y + normal_torso.y * sh});
    path.quad_to(Point{chest.x + normal_torso.x * sh * 1.05f,
                       chest.y + normal_torso.y * sh * 1.05f},
                 Point{hip.x + normal_torso.x * hp, hip.y + normal_torso.y * hp});
    path.line_to(Point{hip.x - normal_torso.x * hp, hip.y - normal_torso.y * hp});
    path.quad_to(Point{chest.x - normal_torso.x * sh * 1.05f,
                       chest.y - normal_torso.y * sh * 1.05f},
                 Point{top.x - normal_torso.x * sh, top.y - normal_torso.y * sh});
    path.close();
    return path;
  };

  canvas.fill_path(torso_polygon(ow), st::raster::Paint::solid(outline));
  canvas.fill_path(torso_polygon(0.0f), st::raster::Paint::solid(tint(palette.cloth)));

  // 躯干的明暗分界：沿法线偏一半处画一条暗色带（圆柱感）
  {
    const Point shade_dir = normal_torso;
    math::Path shade;
    const Point top = Point{chest.x + dir_torso.x * pose.head_radius * 0.18f,
                            chest.y + dir_torso.y * pose.head_radius * 0.18f};
    // 取"背光侧"（与朝向相反的一侧）作为暗部
    const float side = options.facing_right ? 1.0f : -1.0f;
    shade.move_to(Point{top.x + shade_dir.x * shoulder_half * 0.25f * side,
                        top.y + shade_dir.y * shoulder_half * 0.25f * side});
    shade.line_to(Point{top.x + shade_dir.x * shoulder_half * 1.1f * side,
                        top.y + shade_dir.y * shoulder_half * 1.1f * side});
    shade.line_to(Point{hip.x + shade_dir.x * hip_half * 1.1f * side,
                        hip.y + shade_dir.y * hip_half * 1.1f * side});
    shade.line_to(Point{hip.x + shade_dir.x * hip_half * 0.25f * side,
                        hip.y + shade_dir.y * hip_half * 0.25f * side});
    shade.close();
    canvas.fill_path(shade, st::raster::Paint::solid(tint(palette.cloth_shadow)));
  }

  // 上衣装饰：夹克是两条竖向跑动条，polo 是门襟 + 领口
  {
    const float side = options.facing_right ? 1.0f : -1.0f;
    if (palette.long_sleeves) {
      // 两条跑动条（沿躯干方向，偏正面那一侧）
      for (const float offset : {-0.42f, 0.42f}) {
        const Point top = lerp_point(chest, hip, 0.22f);
        const Point bottom = lerp_point(chest, hip, 0.88f);
        const Point shift{normal_torso.x * shoulder_half * offset * side,
                          normal_torso.y * shoulder_half * offset * side};
        draw_line(canvas, Point{top.x + shift.x, top.y + shift.y},
                  Point{bottom.x + shift.x * 0.9f, bottom.y + shift.y * 0.9f},
                  pose.head_radius * 0.16f, tint(palette.cloth_accent));
      }
      // 拉链：中间一条细暗线
      draw_line(canvas, lerp_point(chest, hip, 0.20f), lerp_point(chest, hip, 0.95f),
                pose.head_radius * 0.06f, tint(palette.cloth_shadow));
    } else {
      // polo：门襟 + 领口翻边
      draw_line(canvas, lerp_point(chest, hip, 0.22f), lerp_point(chest, hip, 0.62f),
                pose.head_radius * 0.10f, tint(palette.cloth_accent));
      // 领子：胸上方一个 V
      const Point left{chest.x - normal_torso.x * shoulder_half * 0.55f,
                       chest.y - normal_torso.y * shoulder_half * 0.55f};
      const Point right{chest.x + normal_torso.x * shoulder_half * 0.55f,
                        chest.y + normal_torso.y * shoulder_half * 0.55f};
      const Point v{chest.x + dir_torso.x * pose.head_radius * 0.30f,
                    chest.y + dir_torso.y * pose.head_radius * 0.30f};
      math::Path collar;
      collar.move_to(left);
      collar.line_to(right);
      collar.line_to(v);
      collar.close();
      canvas.fill_path(collar, st::raster::Paint::solid(tint(palette.cloth_accent)));
    }
  }

  // ── 前腿 ──
  draw_limb(canvas, pose.hip, pose.knee_front, limb_width_upper * 1.05f, limb_width_lower,
            tint(palette.pants), outline, ow);
  draw_limb(canvas, pose.knee_front, pose.foot_front, limb_width_lower, limb_width_wrist,
            tint(palette.pants), outline, ow);
  // 前脚
  {
    const float side_f = options.facing_right ? 1.0f : -1.0f;
    const float shoe_len = pose.head_radius * 0.66f;
    const float shoe_h = pose.head_radius * 0.28f;
    const Rect shoe{pose.foot_front.x - shoe_len * 0.35f + (side_f > 0 ? 0.0f : -shoe_len * 0.3f),
                    pose.foot_front.y - shoe_h * 0.35f, shoe_len, shoe_h};
    draw_rounded(canvas, shoe, shoe_h * 0.45f, tint(palette.shoes), outline, ow);
    // 鞋底一条暗线（"有厚度的鞋"）
    draw_line(canvas, Point{shoe.x + shoe.width * 0.1f, shoe.bottom() - shoe_h * 0.12f},
              Point{shoe.right() - shoe.width * 0.1f, shoe.bottom() - shoe_h * 0.12f},
              shoe_h * 0.22f, tint(palette.pants_shadow));
  }

  // ── 头与脖子 ──
  {
    // 脖子（一小段，把头与躯干连起来；缺了会"头飘着"）
    draw_limb(canvas, chest, pose.head, pose.head_radius * 0.28f, pose.head_radius * 0.30f,
              skin_shadow, outline, ow);

    const Point head = pose.head;
    // 绘制用的头半径比骨架略小：骨架的 head_radius 是按"判定盒"给的（偏大），
    // 直接用它画出来是"大头娃娃"（实测：头占身体近 1/3）。
    // 0.82 让头占比接近真人（约 1/6 身高）。
    const float radius = pose.head_radius * 0.82f;

    // 脸（圆形 + 明暗）
    draw_round(canvas, head, radius, tint(palette.skin), outline, ow);
    // 暗部：下半侧一小片弧（下巴/颌线）
    {
      math::Path jaw;
      jaw.add_ellipse(Rect{head.x - radius * 0.86f, head.y + radius * 0.12f, radius * 1.72f,
                           radius * 0.86f});
      math::Path clip_circle;
      clip_circle.add_circle(head, radius);
      canvas.push_clip_path(clip_circle);
      canvas.fill_path(jaw, st::raster::Paint::solid(tint(palette.skin_shadow)));
      canvas.pop_clip();
    }

    const float side = options.facing_right ? 1.0f : -1.0f;

    // 头发（在帽子下面时只露出鬓角）
    if (!palette.has_cap) {
      // 上半球 + 后脑
      math::Path hair;
      hair.add_ellipse(Rect{head.x - radius * 1.04f, head.y - radius * 1.16f, radius * 2.08f,
                            radius * 1.5f});
      math::Path clip_head;
      clip_head.add_circle(head, radius * 1.02f);
      canvas.push_clip_path(clip_head);
      canvas.fill_path(hair, st::raster::Paint::solid(tint(palette.hair)));
      canvas.pop_clip();
      // 鬓角
      const Point temple{head.x - side * radius * 0.82f, head.y - radius * 0.10f};
      draw_limb(canvas, Point{temple.x, temple.y - radius * 0.3f},
                Point{temple.x - side * radius * 0.06f, temple.y + radius * 0.35f},
                radius * 0.16f, radius * 0.10f, tint(palette.hair), outline, ow * 0.7f);
    }

    // 五官（侧脸：眼睛偏向朝向一侧）
    {
      const float eye_y = head.y - radius * 0.08f;
      // 近侧眼（朝向那一侧）
      const Point near_eye{head.x + side * radius * 0.36f, eye_y};
      const Point far_eye{head.x - side * radius * 0.26f, eye_y};
      const Color eye_color = tint(Color{0x1A, 0x16, 0x1C, 0xFF});

      // 远侧眼（略小、略淡——透视暗示）
      canvas.fill_circle(far_eye, radius * 0.085f,
                         st::raster::Paint::solid(with_alpha(eye_color, 0.75f * options.alpha)));
      // 近侧眼：眼白 + 瞳孔 + 高光
      canvas.fill_circle(near_eye, radius * 0.13f,
                         st::raster::Paint::solid(tint(Color{0xFA, 0xFA, 0xFA, 0xFF})));
      canvas.fill_circle(Point{near_eye.x + side * radius * 0.03f, near_eye.y},
                         radius * 0.085f, st::raster::Paint::solid(eye_color));
      canvas.fill_circle(Point{near_eye.x + side * radius * 0.06f, near_eye.y - radius * 0.04f},
                         radius * 0.032f,
                         st::raster::Paint::solid(tint(Color{0xFF, 0xFF, 0xFF, 0xFF})));

      // 眉（一条短斜线，给表情）
      draw_line(canvas, Point{near_eye.x - radius * 0.13f, eye_y - radius * 0.24f},
                Point{near_eye.x + side * radius * 0.12f, eye_y - radius * 0.28f},
                radius * 0.075f, tint(palette.hair));

      // 嘴
      draw_line(canvas, Point{head.x + side * radius * 0.16f, head.y + radius * 0.46f},
                Point{head.x + side * radius * 0.40f, head.y + radius * 0.44f},
                radius * 0.065f, tint(Color{0x9C, 0x5A, 0x50, 0xFF}));
    }

    // 亮片眼镜（户圣）
    if (palette.has_glasses) {
      const float eye_y = head.y - radius * 0.08f;
      const Color lens = tint(Color{0x2A, 0x24, 0x3C, 0xFF});
      for (const float offset : {-0.26f, 0.36f}) {
        const Point center{head.x + side * radius * offset, eye_y};
        // 镜片（圆角矩形）
        draw_rounded(canvas, Rect{center.x - radius * 0.22f, center.y - radius * 0.17f,
                                  radius * 0.44f, radius * 0.34f},
                     radius * 0.10f, lens, tint(palette.glasses_frame), ow * 0.6f);
        // 亮片（小白点，沿镜片边缘——"bling"的来源）
        for (int i = 0; i < 5; ++i) {
          const float angle = static_cast<float>(i) * 1.2566f + (offset > 0 ? 0.3f : 0.0f);
          canvas.fill_circle(Point{center.x + std::cos(angle) * radius * 0.15f,
                                   center.y + std::sin(angle) * radius * 0.11f},
                             radius * 0.022f,
                             st::raster::Paint::solid(tint(Color{0xFF, 0xFF, 0xFF, 0xFF})));
        }
        // 高光斜条
        draw_line(canvas, Point{center.x - radius * 0.12f, center.y - radius * 0.10f},
                  Point{center.x + radius * 0.06f, center.y + radius * 0.08f}, radius * 0.035f,
                  with_alpha(Color{0xFF, 0xFF, 0xFF, 0xFF}, 0.45f * options.alpha));
      }
      // 鼻梁
      draw_line(canvas, Point{head.x + side * radius * 0.10f, eye_y - radius * 0.02f},
                Point{head.x + side * radius * 0.14f, eye_y - radius * 0.02f}, radius * 0.055f,
                tint(palette.glasses_frame));
    }

    // 棒球帽（甜甜圈）
    if (palette.has_cap) {
      // 帽体：覆盖头顶的弧
      math::Path cap;
      cap.add_ellipse(Rect{head.x - radius * 1.06f, head.y - radius * 1.20f, radius * 2.12f,
                           radius * 1.55f});
      math::Path clip_cap;
      clip_cap.add_circle(head, radius * 1.05f);
      canvas.push_clip_path(clip_cap);
      canvas.fill_path(cap, st::raster::Paint::solid(tint(palette.cap_base)));
      // 红白横条（国旗意象）：两条横带
      for (const auto& [offset_ratio, color] :
           {std::pair{0.10f, palette.cap_stripe}, std::pair{0.26f, palette.cloth_accent}}) {
        const Rect band{head.x - radius * 1.06f, head.y - radius * (0.62f - offset_ratio),
                        radius * 2.12f, radius * 0.11f};
        canvas.fill_rect(band, st::raster::Paint::solid(tint(color)));
      }
      canvas.pop_clip();
      // 帽体描边（只画上半弧——整圈会切到脸）
      {
        math::Path arc;
        arc.add_ellipse(Rect{head.x - radius * 1.06f, head.y - radius * 1.20f, radius * 2.12f,
                             radius * 1.55f});
        math::Path clip_arc;
        clip_arc.add_circle(head, radius * 1.06f);
        canvas.push_clip_path(clip_arc);
        canvas.stroke_path(arc, st::raster::Paint::solid(outline), ow, st::raster::StrokeStyle{});
        canvas.pop_clip();
      }
      // 帽檐（朝向那一侧伸出）
      {
        const Point brim_start{head.x + side * radius * 0.30f, head.y - radius * 0.42f};
        const Point brim_end{head.x + side * radius * 1.42f, head.y - radius * 0.34f};
        draw_limb(canvas, brim_start, brim_end, radius * 0.20f, radius * 0.13f,
                  tint(palette.cap_base), outline, ow);
        // 帽檐下的阴影（让"檐"有厚度）
        draw_line(canvas, Point{brim_start.x, brim_start.y + radius * 0.12f},
                  Point{brim_end.x, brim_end.y + radius * 0.10f}, radius * 0.055f,
                  with_alpha(outline, 0.5f * options.alpha));
      }
    }
  }

  // ── **前侧**手臂（最后画，压在躯干之上）──
  {
    const Point sleeve_end_front =
        palette.long_sleeves ? lerp_point(pose.elbow_front, pose.hand_front, 0.86f)
                             : lerp_point(pose.shoulder_front, pose.elbow_front, 0.60f);
    draw_limb(canvas, pose.shoulder_front, sleeve_end_front, limb_width_upper,
              limb_width_lower * 0.95f, tint(palette.cloth), outline, ow);
    // 袖口：一道暗边（布料的"收口"）
    {
      const Point dir_arm = direction(pose.shoulder_front, sleeve_end_front);
      const Point cuff_normal = perpendicular(dir_arm);
      draw_line(canvas,
                Point{sleeve_end_front.x + cuff_normal.x * limb_width_lower * 0.45f,
                      sleeve_end_front.y + cuff_normal.y * limb_width_lower * 0.45f},
                Point{sleeve_end_front.x - cuff_normal.x * limb_width_lower * 0.45f,
                      sleeve_end_front.y - cuff_normal.y * limb_width_lower * 0.45f},
                limb_width_lower * 0.22f, tint(palette.cloth_shadow));
    }
    // 露出的前臂与手
    draw_limb(canvas, sleeve_end_front, pose.hand_front, limb_width_lower * 0.95f,
              limb_width_wrist, tint(palette.skin), outline, ow);
    // 手（拳头：比前臂略大，读起来有"握拳"的分量）
    draw_round(canvas, pose.hand_front, limb_width_lower * 0.98f, tint(palette.skin), outline, ow);
    // 指节线（一小段，暗示握拳）
    draw_line(canvas, Point{pose.hand_front.x - limb_width_lower * 0.5f,
                            pose.hand_front.y - limb_width_lower * 0.15f},
              Point{pose.hand_front.x + limb_width_lower * 0.5f,
                    pose.hand_front.y - limb_width_lower * 0.15f},
              limb_width_lower * 0.16f, skin_shadow);
  }
}

}  // namespace gedou
