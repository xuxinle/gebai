/// 文字抗锯齿**对照工具**（仅验证用，不进框架构建、不进 `st.pkg`）。
///
/// 用途：「字看着糊 / 带着彩边」这类观感问题的第一现场是**像素**。本工具把同一段文字
/// 按三种形态各渲一张 PNG —— 灰度抗锯齿 / 亚像素(5-tap 滤波) / 亚像素(原始) ——
/// 并打印某个扫描行从第一处墨迹起的**边缘剖面**（逐像素 RGB）。
/// 于是「是灰度还是亚像素」「滤波有没有生效」「彩边在哪个通道」都可以直接读出来，
/// 不需要靠肉眼猜，也不需要改框架代码。
///
/// 手工编译（Linux，复用框架已构建的对象；**必须排除各 target 的 main 与测试对象**）：
///
/// ```bash
/// cd shuangtian
/// ./build/bin/st build st --profile dev          # 先确保框架对象是新的
/// OBJS=$(ls build/dev/obj/*.o | grep -v -E "(main\.cpp\.o|_test\.cpp\.o|test_runner\.cpp\.o|_vendor_|examples_)")
/// g++ -std=c++20 -O1 -Iinclude -Ithird_party tools/lcd_compare.cpp $OBJS -o /tmp/lcd_compare -lpthread -ldl -lm
/// /tmp/lcd_compare /tmp/lcd-shots                  # 输出三张 PNG + 边缘剖面
/// ```
///
/// 无头/窗口一致：字形位图同源，只有边缘合成方式不同（这正是要看的东西）。
/// Windows 机器上另有更省事的路径：`<app> --headless --text-lcd on` + 控制通道 `capture`。
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/text/text.hpp"

using st::math::Color;
using st::math::Point;
using st::raster::Canvas;
using st::text::FontStack;
using st::text::TextRenderer;

namespace {

constexpr int kWidth = 900;
constexpr int kHeight = 130;
constexpr Color kBackground{0x26, 0x2A, 0x33, 0xFF};   // 深色界面底
constexpr Color kForeground{0xE8, 0xEA, 0xED, 0xFF};

constexpr std::string_view kLine1 = "霜天 vsedit 文字渲染 概览 面板 win32";
constexpr std::string_view kLine2 = "霜天 概览 win32";
constexpr std::string_view kLine3 = "Handgloves Illegible 0123456789";

struct Mode {
  const char* name;
  bool lcd;
  bool filter;
};

const Mode kModes[3] = {
    {"gray", false, true},
    {"lcd-filter", true, true},
    {"lcd-raw", true, false},
};

/// 三种模式下布局完全相同（同一段文字、同一坐标、同一字号档）。
void draw_all(Canvas& canvas, TextRenderer& renderer) {
  renderer.draw(canvas, kLine1, Point{6.0f, 26.0f}, 13.5f, kForeground);
  renderer.draw(canvas, kLine2, Point{6.0f, 60.0f}, 22.0f, kForeground);
  renderer.draw(canvas, kLine3, Point{6.0f, 100.0f}, 15.0f, kForeground);
}

void render_mode(const FontStack& stack, const Mode& mode, const std::string& path) {
  Canvas canvas{kWidth, kHeight};
  canvas.clear(kBackground);
  TextRenderer renderer(stack, 1.25f);  // 125% DPI 档（与反馈现场一致）
  renderer.set_subpixel(mode.lcd);
  renderer.set_subpixel_filter(mode.filter);
  draw_all(canvas, renderer);

  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(kWidth);
  image.height = static_cast<std::uint32_t>(kHeight);
  image.rgba = canvas.to_rgba8();
  const auto status = st::codec::png_write_file(path, image);
  st::print("{} → {}\n", path, status.has_value() ? "ok" : status.error().message);
}

/// 打印某一行、某个 x 窗口内的原始像素（边缘剖面的原始证据）。
void print_profile(const FontStack& stack, const Mode& mode, int row, int from, int to) {
  Canvas canvas{kWidth, kHeight};
  canvas.clear(kBackground);
  TextRenderer renderer(stack, 1.25f);
  renderer.set_subpixel(mode.lcd);
  renderer.set_subpixel_filter(mode.filter);
  draw_all(canvas, renderer);
  st::print("  {:<11} ", mode.name);
  for (int x = from; x <= to; ++x) {
    const Color pixel = canvas.pixel_at(x, row);
    st::print("({:3},{:3},{:3}) ", static_cast<int>(pixel.r), static_cast<int>(pixel.g),
              static_cast<int>(pixel.b));
  }
  st::print("\n");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";
  auto stack = FontStack::system_default();
  if (!stack) {
    st::print("未找到可用字体（ST_FONT_LATIN/ST_FONT_CJK 可显式指定）\n");
    return 1;
  }
  for (const Mode& mode : kModes) {
    render_mode(*stack, mode, out_dir + "/" + mode.name + ".png");
  }

  // 找一行墨迹最多的行，再从第一处墨迹起打印 12 个像素的边缘剖面
  Canvas probe{kWidth, kHeight};
  probe.clear(kBackground);
  TextRenderer gray(*stack, 1.25f);
  draw_all(probe, gray);
  int best_row = 0;
  std::size_t best_ink = 0;
  for (int y = 0; y < kHeight; ++y) {
    std::size_t ink = 0;
    for (int x = 0; x < kWidth; ++x) {
      if (probe.pixel_at(x, y).r != kBackground.r) ++ink;
    }
    if (ink > best_ink) {
      best_ink = ink;
      best_row = y;
    }
  }
  int first_ink = 0;
  for (int x = 0; x < kWidth; ++x) {
    if (probe.pixel_at(x, best_row).r != kBackground.r) {
      first_ink = x;
      break;
    }
  }
  st::print("边缘剖面（y={}，x={}..{}，13.5px 正文）：\n", best_row, first_ink - 2, first_ink + 9);
  for (const Mode& mode : kModes) {
    print_profile(*stack, mode, best_row, first_ink - 2, first_ink + 9);
  }
  return 0;
}
