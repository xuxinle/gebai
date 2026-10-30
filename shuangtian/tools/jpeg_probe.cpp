// JPEG 解码器验证：解真实 JPEG，把结果与"已知的图案"对照。
//
// ## 判据为什么必须是"内容对不对"而不是"解出来没崩"
//
// JPEG 解码最常见的错误不会崩：
// - 色度上采样错 → 整张图偏色 / 边缘出现彩色拉丝；
// - `receive_extend` 用普通补码 → 所有负系数差 1 → 暗部偏绿；
// - 之字形表反了 → 图变成雪花状纹理；
// - 量化表没转自然顺序 → 高频全丢（糊）或全乱。
//
// 这些都"能出图"。所以判据要钉**具体位置的颜色**：
// 生成一张已知渐变的 JPEG（红随 x 增、绿随 y 增、蓝为棋盘），
// 解码后检查同位置的像素是否落在合理区间。
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

#include "st/codec/image.hpp"
#include "st/codec/jpeg.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"

namespace {

struct Rgb {
  int r{0};
  int g{0};
  int b{0};
};

auto pixel_at(const std::vector<std::uint8_t>& rgba, int width, int x, int y) -> Rgb {
  const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                          static_cast<std::size_t>(x)) * 4U;
  if (at + 2U >= rgba.size()) return {};
  return Rgb{rgba[at], rgba[at + 1], rgba[at + 2]};
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> targets;
  for (int i = 1; i < argc; ++i) targets.emplace_back(argv[i]);
  if (targets.empty()) targets = {"tmp/eco/test.jpg"};

  int failures = 0;
  for (const std::string& path : targets) {
    st::log::out("\n=== {} ===\n", path);
    if (!st::fs::exists(path)) {
      st::log::out("  （不存在，跳过）\n");
      continue;
    }
    const auto bytes = st::fs::read_bytes(path);
    if (!bytes.has_value()) {
      st::log::out("  读取失败：{}\n", bytes.error().message);
      ++failures;
      continue;
    }
    st::log::out("  文件 {} 字节 · 识别格式 = {}\n", bytes->size(),
                 st::codec::sniff_image_format(*bytes));

    const auto decoded = st::codec::decode_image(*bytes);
    if (!decoded.has_value()) {
      st::log::out("  解码失败：{}\n", decoded.error().message);
      ++failures;
      continue;
    }
    st::log::out("  解码成功：{}×{} · {} 字节 RGBA · 格式 {}\n", decoded->width, decoded->height,
                 decoded->rgba.size(), decoded->format);

    // ── 判据 1：尺寸必须是 64×64（生成时就是这个） ──
    if (decoded->width != 64 || decoded->height != 64) {
      st::log::out("  [FAIL] 尺寸期望 64×64，实际 {}×{}\n", decoded->width, decoded->height);
      ++failures;
    } else {
      st::log::out("  [OK] 尺寸 64×64\n");
    }

    // ── 判据 2：红随 x 增、绿随 y 增（这是生成图的结构） ──
    //
    // 取四角检查趋势。JPEG 有损，所以给宽容差（±40）——但**趋势反了**就是解码错了。
    const auto tl = pixel_at(decoded->rgba, decoded->width, 2, 2);
    const auto tr = pixel_at(decoded->rgba, decoded->width, 61, 2);
    const auto bl = pixel_at(decoded->rgba, decoded->width, 2, 61);
    st::log::out("  TL=({},{},{}) TR=({},{},{}) BL=({},{},{})\n", tl.r, tl.g, tl.b, tr.r, tr.g,
                 tr.b, bl.r, bl.g, bl.b);

    const bool red_rises = tr.r > tl.r + 60;        // 右上比左上红得多
    const bool green_rises = bl.g > tl.g + 60;      // 左下比左上绿得多
    if (red_rises) {
      st::log::out("  [OK] 红通道随 x 递增（{} → {}）\n", tl.r, tr.r);
    } else {
      st::log::out("  [FAIL] 红通道未随 x 递增（{} → {}）\n", tl.r, tr.r);
      ++failures;
    }
    if (green_rises) {
      st::log::out("  [OK] 绿通道随 y 递增（{} → {}）\n", tl.g, bl.g);
    } else {
      st::log::out("  [FAIL] 绿通道未随 y 递增（{} → {}）\n", tl.g, bl.g);
      ++failures;
    }

    // ── 判据 3：整图不是单色（解码出常数说明熵解码失败） ──
    int min_luma = 255;
    int max_luma = 0;
    for (int y = 0; y < decoded->height; ++y) {
      for (int x = 0; x < decoded->width; ++x) {
        const auto pixel = pixel_at(decoded->rgba, decoded->width, x, y);
        const int luma = (pixel.r * 299 + pixel.g * 587 + pixel.b * 114) / 1000;
        min_luma = std::min(min_luma, luma);
        max_luma = std::max(max_luma, luma);
      }
    }
    if (max_luma - min_luma > 60) {
      st::log::out("  [OK] 亮度范围 {}..{}（有内容）\n", min_luma, max_luma);
    } else {
      st::log::out("  [FAIL] 亮度范围仅 {}..{}（疑似单色/解码失败）\n", min_luma, max_luma);
      ++failures;
    }

    // ── 判据 4：来回写 PNG（证明 RGBA 数据自洽） ──
    st::codec::PngImage png{};
    png.width = static_cast<std::uint32_t>(decoded->width);
    png.height = static_cast<std::uint32_t>(decoded->height);
    png.rgba = decoded->rgba;
    const std::string png_path = path + ".decoded.png";
    if (const auto written = st::codec::png_write_file(png_path, png); written.has_value()) {
      // 再读回来，确认能往返（尺寸与像素一致）
      const auto back = st::codec::decode_image_file(png_path);
      if (back.has_value() && back->width == decoded->width && back->height == decoded->height &&
          back->rgba == decoded->rgba) {
        st::log::out("  [OK] 转存 PNG 并读回一致：{}\n", png_path);
      } else {
        st::log::out("  [FAIL] PNG 往返不一致\n");
        ++failures;
      }
    }
  }

  st::log::out("\n{}：{}\n", failures == 0 ? "全部通过" : "有失败", failures);
  return failures == 0 ? 0 : 2;
}
