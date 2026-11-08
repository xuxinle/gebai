/// 启动卡品牌标记的候选：**同一个图标 6 个方案 × 3 个尺寸**，出一张对照图。
///
/// ## 为什么要先出候选再定
///
/// 图标是纯观感物（`CONVENTIONS.md` §9.2）：**验收标准长在用户身上**，不是"我觉得好看"。
/// 而当前那个"菱形套菱形"的标记被用户直接指出"太丑"——说明第一版的取法有问题：
/// 它只是把一个几何形状当标记用，没有任何可识别的意象。
///
/// 所以这里出的是**有出处的方案**，每个都对应一个可以讲的理由（见 `kCandidates` 的 note）：
///
/// | # | 意象 | 为什么可能与"霜天"相配 |
/// |---|---|---|
/// | 0 | 现状（菱形套菱形） | 基准，用来对比"到底差在哪" |
/// | 1 | 六角雪花（单笔折线） | "霜天"的字面意象；寒、锐、对称 |
/// | 2 | 上升折线 + 圆点 | "智能体在长"；工程感，不是装饰 |
/// | 3 | 双弧对合（"合"的抽象） | 两片合成一片 = 工具被收编成一体的隐喻 |
/// | 4 | 环形渐变 + 缺口 | 呼吸/加载的自然形态，与脉冲环同族 |
/// | 5 | 山形/天穹（一道弧 + 一线） | "天"的字面意象；最静，最不像科技公司 logo |
///
/// ## 出图规则
///
/// 每个方案画三行：**卡上真实尺寸**（1x / 1.5x / 2x 物理像素），因为图标在
/// 小尺寸下"糊不糊"和在大尺寸下"好不好看"是两件正交的事——
/// 只在大图上看，一定会选出小尺寸下认不出的方案（这是图标设计的常见坑）。
///
/// 用法：`brand_probe [输出目录]`（缺省 `build/brand`）。

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/math/color.hpp"
#include "st/math/path.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

constexpr double k_pi = 3.14159265358979323846;
constexpr float kCell = 140.0f;   ///< 每格逻辑边长

/// 一个候选：名字、一句话理由、绘制函数（在 `center` 处、`radius` 为外接半径）。
struct Candidate {
  const char* name;
  const char* note;
  void (*draw)(st::raster::Canvas&, st::math::Point center, float radius,
               st::math::Color accent, st::math::Color hole);
};

auto accent_of(st::math::Color color, float alpha) -> st::math::Color {
  return color.with_alpha_f(alpha);
}

// ── 0 现状：菱形套菱形（基准） ──
void draw_diamond(st::raster::Canvas& canvas, st::math::Point c, float r, st::math::Color accent,
                  st::math::Color hole) {
  auto diamond = [](st::math::Point center, float half_w, float half_h) {
    st::math::Path path;
    path.move_to({center.x, center.y - half_h});
    path.line_to({center.x + half_w, center.y});
    path.line_to({center.x, center.y + half_h});
    path.line_to({center.x - half_w, center.y});
    path.close();
    return path;
  };
  canvas.fill_path(diamond(c, r * 0.56f, r), st::raster::Paint::solid(accent));
  canvas.fill_path(diamond(c, r * 0.24f, r * 0.44f), st::raster::Paint::solid(hole));
}

// ── 1 六角雪花：一根竖轴 + 两对斜枝，单笔折线 ──
//
// 为什么不画"经典六角雪花"（六条枝 × 每条再分叉）：在小尺寸（14px）下那些分叉
// 会糊成一团。这里只保留**主干 + 一对短分叉**，14px 下仍能认出"六向对称"。
void draw_snowflake(st::raster::Canvas& canvas, st::math::Point c, float r, st::math::Color accent,
                    st::math::Color) {
  const st::math::Color ink = accent_of(accent, 0.95f);
  const float width = r * 0.17f;
  for (int axis = 0; axis < 3; ++axis) {
    const double angle = k_pi * static_cast<double>(axis) / 3.0;   // 0°, 60°, 120°
    const float dx = static_cast<float>(std::cos(angle));
    const float dy = static_cast<float>(std::sin(angle));
    st::math::Path spine;
    spine.move_to({c.x - dx * r, c.y - dy * r});
    spine.line_to({c.x + dx * r, c.y + dy * r});
    canvas.stroke_path(spine, st::raster::Paint::solid(ink), width);
  }
  // 六个枝尖的短分叉（只在两端各加一笔，保持"霜"的锐利）
  for (int tip = 0; tip < 6; ++tip) {
    const double angle = k_pi * static_cast<double>(tip) / 3.0;
    const float dx = static_cast<float>(std::cos(angle));
    const float dy = static_cast<float>(std::sin(angle));
    const st::math::Point end{c.x + dx * r, c.y + dy * r};
    // 分叉方向：主干方向旋转 ±40°
    for (int side = -1; side <= 1; side += 2) {
      const double offset = 0.70 * static_cast<double>(side);
      const float bx = static_cast<float>(std::cos(angle + offset));
      const float by = static_cast<float>(std::sin(angle + offset));
      st::math::Path barb;
      barb.move_to(end);
      barb.line_to({end.x + bx * r * 0.34f, end.y + by * r * 0.34f});
      canvas.stroke_path(barb, st::raster::Paint::solid(ink), width * 0.82f);
    }
  }
}

// ── 2 上升折线 + 圆点（"在长"的工程感） ──
void draw_growth(st::raster::Canvas& canvas, st::math::Point c, float r, st::math::Color accent,
                 st::math::Color) {
  const float width = r * 0.19f;
  st::math::Path line;
  line.move_to({c.x - r * 0.86f, c.y + r * 0.52f});
  line.line_to({c.x - r * 0.30f, c.y - r * 0.10f});
  line.line_to({c.x + r * 0.10f, c.y + r * 0.20f});
  line.line_to({c.x + r * 0.84f, c.y - r * 0.62f});
  canvas.stroke_path(line, st::raster::Paint::solid(accent_of(accent, 0.95f)), width);
  // 端点圆点：给"还在往上走"一个落点（折线本身没有终点感）
  canvas.fill_circle({c.x + r * 0.84f, c.y - r * 0.62f}, width * 1.05f,
                     st::raster::Paint::solid(accent));
}

// ── 3 双弧对合：两片合成一片 ──
void draw_merge(st::raster::Canvas& canvas, st::math::Point c, float r, st::math::Color accent,
                st::math::Color) {
  const float width = r * 0.18f;
  for (int side = -1; side <= 1; side += 2) {
    st::math::Path arc;
    const float shift = static_cast<float>(side) * r * 0.30f;
    arc.move_to({c.x + shift, c.y - r * 0.86f});
    arc.cubic_to({c.x + shift + r * 0.92f * static_cast<float>(side), c.y - r * 0.40f},
                 {c.x + shift + r * 0.92f * static_cast<float>(side), c.y + r * 0.40f},
                 {c.x + shift, c.y + r * 0.86f});
    canvas.stroke_path(arc, st::raster::Paint::solid(accent_of(accent, 0.92f)), width);
  }
  // 中间一道细横线：把"两片"读成"一片"
  st::math::Path bar;
  bar.move_to({c.x - r * 0.34f, c.y});
  bar.line_to({c.x + r * 0.34f, c.y});
  canvas.stroke_path(bar, st::raster::Paint::solid(accent_of(accent, 0.55f)), width * 0.62f);
}

// ── 4 环形 + 缺口（与脉冲环同族） ──
void draw_ring(st::raster::Canvas& canvas, st::math::Point c, float r, st::math::Color accent,
               st::math::Color) {
  st::math::Path ring = st::math::Path{}.add_circle(c, r * 0.80f);
  // 缺口靠"画一弧"实现：add_arc 从 -60° 到 240°（留 60° 的口）
  st::math::Path open;
  open.add_arc(c, r * 0.80f, -55.0f, 235.0f);
  (void)ring;
  canvas.stroke_path(open, st::raster::Paint::solid(accent_of(accent, 0.95f)), r * 0.20f);
  // 缺口里放一个点，指向"正在转"
  const double gap_angle = -55.0 * k_pi / 180.0;
  canvas.fill_circle({c.x + r * 0.80f * static_cast<float>(std::cos(gap_angle)),
                      c.y + r * 0.80f * static_cast<float>(std::sin(gap_angle))},
                     r * 0.15f, st::raster::Paint::solid(accent));
}

// ── 5 山形/天穹：一道弧 + 一条底座线（最静） ──
void draw_sky(st::raster::Canvas& canvas, st::math::Point c, float r, st::math::Color accent,
              st::math::Color) {
  const float width = r * 0.19f;
  st::math::Path dome;
  dome.move_to({c.x - r * 0.92f, c.y + r * 0.44f});
  dome.cubic_to({c.x - r * 0.72f, c.y - r * 0.78f}, {c.x + r * 0.72f, c.y - r * 0.78f},
                {c.x + r * 0.92f, c.y + r * 0.44f});
  canvas.stroke_path(dome, st::raster::Paint::solid(accent_of(accent, 0.95f)), width);
  st::math::Path base;
  base.move_to({c.x - r * 0.62f, c.y + r * 0.74f});
  base.line_to({c.x + r * 0.62f, c.y + r * 0.74f});
  canvas.stroke_path(base, st::raster::Paint::solid(accent_of(accent, 0.62f)), width * 0.66f);
}

const Candidate kCandidates[] = {
    {"0 现状 菱形套菱形", "基准，用来对比", &draw_diamond},
    {"1 六角雪花", "霜天的字面意象；寒、锐、对称", &draw_snowflake},
    {"2 上升折线", "智能体在长；工程感", &draw_growth},
    {"3 双弧对合", "两片合成一片 = 收编成一体", &draw_merge},
    {"4 环形缺口", "与脉冲环同族的呼吸感", &draw_ring},
    {"5 山形/天穹", "天的字面意象；最静", &draw_sky},
};

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "build/brand";
  (void)st::fs::create_directories(out_dir);

  constexpr int kCount = static_cast<int>(sizeof(kCandidates) / sizeof(kCandidates[0]));
  const std::vector<float> scales{1.0f, 1.5f, 2.0f};
  const int cols = kCount;
  const int rows = static_cast<int>(scales.size());
  const int sheet_w = static_cast<int>(kCell) * cols;
  const int sheet_h = static_cast<int>(kCell) * rows;

  // 每个候选各画一行三档；背景用**深色卡底**（与真启动卡一致，避免"深色图标画在白底上"的假评估）
  st::raster::Canvas sheet(sheet_w, sheet_h, 1.0f);
  sheet.clear(st::math::Color::rgb(0x0B, 0x0B, 0x0E));
  const st::math::Color accent = st::math::Color::rgb(0x7C, 0x8C, 0xFF);
  const st::math::Color hole = st::math::Color::rgb(0x0B, 0x0B, 0x0E);

  for (int col = 0; col < cols; ++col) {
    for (int row = 0; row < rows; ++row) {
      const float scale = scales[static_cast<std::size_t>(row)];
      // 三档的"卡上真实尺寸"：启动卡里标记外接半径 = 34 逻辑像素。
      const float radius_logical = 34.0f;
      const float radius = radius_logical * scale;
      const st::math::Point center{kCell * static_cast<float>(col) + kCell * 0.5f,
                                   kCell * static_cast<float>(row) + kCell * 0.5f};
      kCandidates[col].draw(sheet, center, radius * 0.42f, accent, hole);
    }
  }

  // 写 PNG（预乘 → 非预乘）
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(sheet_w);
  image.height = static_cast<std::uint32_t>(sheet_h);
  image.rgba.resize(static_cast<std::size_t>(sheet_w) * static_cast<std::size_t>(sheet_h) * 4U);
  const auto pixels = sheet.pixels();
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const std::uint32_t v = pixels[i];
    const std::uint32_t a = v & 0xFFU;
    const auto un = [a](std::uint32_t ch) -> std::uint8_t {
      if (a == 0U) return 0;
      const float f = static_cast<float>(ch) * 255.0f / static_cast<float>(a);
      return static_cast<std::uint8_t>(f > 255.0f ? 255.0f : f);
    };
    image.rgba[i * 4U + 0U] = un((v >> 24U) & 0xFFU);
    image.rgba[i * 4U + 1U] = un((v >> 16U) & 0xFFU);
    image.rgba[i * 4U + 2U] = un((v >> 8U) & 0xFFU);
    image.rgba[i * 4U + 3U] = static_cast<std::uint8_t>(a);
  }
  const std::string path = st::fs::join(out_dir, "brand-candidates.png");
  if (auto written = st::codec::png_write_file(path, image); !written) {
    std::printf("写 PNG 失败：%s\n", written.error().message.c_str());
    return 1;
  }

  std::printf("候选对照图：%s（%d 列 × %d 行：每列一个方案，每行 1x/1.5x/2x 物理像素）\n",
              path.c_str(), cols, rows);
  for (int col = 0; col < cols; ++col) {
    std::printf("  %-22s %s\n", kCandidates[col].name, kCandidates[col].note);
  }
  return 0;
}
