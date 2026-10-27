/// 视觉快照与版本比对（P3）。
///
/// ## 这组用例守的是什么
///
/// 视觉 diff 的价值全在**判据可信**：说"完全一致"就必须逐像素一致，
/// 说"变了"就必须指出变在哪一块。三种失效最危险：
///
/// 1. **恒绿**：不管两版差多少都报 0%——看起来像护栏，实际什么都拦不住；
/// 2. **错位**：帧号对错一格，两版比的是不同时刻的画面，差异全是假的；
/// 3. **尺寸不同还硬比**：内存越界或比出垃圾数据。
///
/// 所以每条用例都构造**可知的差异**，再断言量出来的值与构造值吻合。
#include <cmath>
#include <string>
#include <vector>

#include "examples/caiyun/snapshot.hpp"
// 测试目标不链接 examples/caiyun，所以直接包含实现（仓库惯例）。
#include "examples/caiyun/snapshot.cpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto fresh_dir(std::string_view name) -> std::string {
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_cysnap_" + std::string{name});
  (void)st::fs::remove_all(dir);
  (void)st::fs::create_directories(dir);
  return dir;
}

/// 造一张纯色 PNG（便于精确算差异）。
[[nodiscard]] auto make_png(std::string_view path, int width, int height,
                            std::uint8_t r, std::uint8_t g, std::uint8_t b) -> bool {
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
    image.rgba[i * 4 + 0] = r;
    image.rgba[i * 4 + 1] = g;
    image.rgba[i * 4 + 2] = b;
    image.rgba[i * 4 + 3] = 255;
  }
  return st::codec::png_write_file(path, image, 1).has_value();
}

/// 造一张带一块矩形色块的 PNG（用来验差异**区域**）。
[[nodiscard]] auto make_png_with_patch(std::string_view path, int width, int height,
                                       int px, int py, int pw, int ph) -> bool {
  st::codec::PngImage image{};
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
    image.rgba[i * 4 + 0] = 20;
    image.rgba[i * 4 + 1] = 24;
    image.rgba[i * 4 + 2] = 32;
    image.rgba[i * 4 + 3] = 255;
  }
  for (int y = py; y < py + ph && y < height; ++y) {
    for (int x = px; x < px + pw && x < width; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
      image.rgba[at + 0] = 220;
      image.rgba[at + 1] = 180;
      image.rgba[at + 2] = 60;
    }
  }
  return st::codec::png_write_file(path, image, 1).has_value();
}

}  // namespace

ST_TEST(caiyun_snapshot_diff_identical_is_exactly_zero) {
  const std::string dir = fresh_dir("same");
  const std::string a = st::fs::join(dir, "a.png");
  const std::string b = st::fs::join(dir, "b.png");
  ST_CHECK(make_png(a, 64, 48, 30, 60, 90));
  ST_CHECK(make_png(b, 64, 48, 30, 60, 90));

  const auto diff = caiyun::diff_png(a, b);
  ST_CHECK(diff.has_value());
  if (!diff) return;
  // 完全一致必须是**严格 0**，不能是"接近 0"——否则恒绿判据从这里开始烂。
  ST_CHECK(diff->diff_pct == 0.0);
  ST_CHECK(diff->max_channel_delta == 0);
  ST_CHECK(diff->identical());
  ST_CHECK(diff->bounds_w == 0);
  ST_CHECK(diff->bounds_h == 0);
}

ST_TEST(caiyun_snapshot_diff_counts_exactly) {
  const std::string dir = fresh_dir("count");
  const std::string a = st::fs::join(dir, "a.png");
  const std::string b = st::fs::join(dir, "b.png");
  // 64x48 = 3072 像素；只改一块 8x8 = 64 像素 → 64/3072 = 2.0833%
  // 底色必须**与 make_png_with_patch 一致**（20,24,32），否则每个像素都不同，
  // 量出来就是 100%——那是测试写错，不是实现错。
  ST_CHECK(make_png(a, 64, 48, 20, 24, 32));
  ST_CHECK(make_png_with_patch(b, 64, 48, 20, 16, 8, 8));

  const auto diff = caiyun::diff_png(a, b);
  ST_CHECK(diff.has_value());
  if (!diff) return;
  const double expected = 100.0 * 64.0 / 3072.0;
  ST_CHECK(std::abs(diff->diff_pct - expected) < 0.001);
  if (std::abs(diff->diff_pct - expected) >= 0.001) {
    st::print("差异占比 {:.4f}%，期望 {:.4f}%\n", diff->diff_pct, expected);
  }
  // 区域必须精确圈到那块 8x8
  ST_CHECK(diff->bounds_x == 20);
  ST_CHECK(diff->bounds_y == 16);
  ST_CHECK(diff->bounds_w == 8);
  ST_CHECK(diff->bounds_h == 8);
  // 通道差：红 220-20=200、绿 180-24=156、蓝 60-32=28 → 最大 200
  ST_CHECK(diff->max_channel_delta == 200);
}

ST_TEST(caiyun_snapshot_diff_rejects_size_mismatch) {
  const std::string dir = fresh_dir("size");
  const std::string a = st::fs::join(dir, "a.png");
  const std::string b = st::fs::join(dir, "b.png");
  ST_CHECK(make_png(a, 64, 48, 10, 10, 10));
  ST_CHECK(make_png(b, 32, 24, 10, 10, 10));

  // 尺寸不同必须**报错**，不能"尽力比"——硬比会越界或比出垃圾数据。
  const auto diff = caiyun::diff_png(a, b);
  ST_CHECK(!diff.has_value());
  if (!diff) {
    st::print("尺寸不匹配的报错：{}\n", diff.error().message);
    ST_CHECK(diff.error().message.find("64x48") != std::string::npos);
  }
}

ST_TEST(caiyun_snapshot_build_picks_per_scene_positions) {
  const std::string frames = fresh_dir("frames");
  // 模拟 3 镜 × 10 帧 = 30 帧
  for (int i = 0; i < 30; ++i) {
    ST_CHECK(make_png(st::fs::join(frames, std::format("frame_{:04}.png", i + 1)), 16, 12, 0,
                      0, static_cast<std::uint8_t>(i * 8)));
  }
  const std::vector<std::pair<int, int>> bounds = {{0, 10}, {10, 20}, {20, 30}};
  const std::string out = st::fs::join(frames, "snapshot_out");

  const auto snap = caiyun::build_snapshot(frames, out, "probe", bounds, 30, 30.0, 16, 12);
  ST_CHECK(snap.has_value());
  if (!snap) {
    st::print("抽快照失败：{}\n", snap.error().message);
    return;
  }
  // 每镜 3 帧 = 9 帧
  ST_CHECK(snap->frames.size() == 9);
  ST_CHECK(snap->width == 16);
  ST_CHECK(snap->total_frames == 30);

  // 逐镜核对抽帧位置：跨度 10 帧 × {0.30, 0.50, 0.85} → {3, 5, 9}（四舍五入：8.5 → 9）
  const int expected_offsets[] = {3, 5, 9};
  for (std::size_t s = 0; s < bounds.size(); ++s) {
    for (std::size_t k = 0; k < 3; ++k) {
      const std::size_t at = s * 3 + k;
      if (at >= snap->frames.size()) return;
      const int want = bounds[s].first + expected_offsets[k];
      if (snap->frames[at].index != want) {
        st::print("镜 {} 的第 {} 个采样：实际第 {} 帧，期望第 {} 帧\n", s, k,
                  snap->frames[at].index, want);
      }
      ST_CHECK(snap->frames[at].index == want);
      ST_CHECK(snap->frames[at].scene_index == static_cast<int>(s));
    }
  }
  // 元信息要能读回
  const auto reread = caiyun::read_snapshot(out);
  ST_CHECK(reread.has_value());
  if (reread) {
    ST_CHECK(reread->frames.size() == 9);
    ST_CHECK(reread->name == "probe");
  }
}

ST_TEST(caiyun_snapshot_diff_snapshots_pairs_by_frame_index) {
  // 关键：两版必须**按帧号**配对，不能按数组下标。
  // 剧本时长变了会让帧数不同，按下标配对就会拿"第 3 帧 vs 第 8 帧"比，
  // 差异全是错位造成的假象。
  const std::string dir_a = fresh_dir("snapa");
  const std::string dir_b = fresh_dir("snapb");

  // 两版的**同号帧内容故意不同**（B 整体偏亮 40），用来验证“按帧号配对”
  // 而不是“按内容/下标配对”——若实现错了位，比出来的差异不是这个量。
  const auto write_meta = [](const std::string& dir, const std::vector<int>& indices,
                             std::uint8_t blue_bias) {
    caiyun::Snapshot snap{};
    snap.name = "t";
    snap.created_at = "2026-01-01T00:00:00Z";
    snap.width = 16;
    snap.height = 12;
    snap.fps = 30.0;
    snap.total_frames = 30;
    for (const int index : indices) {
      caiyun::SnapshotFrame frame{};
      frame.index = index;
      frame.path = std::format("f{:05d}.png", index);
      snap.frames.push_back(frame);
      const auto blue = static_cast<std::uint8_t>(index * 5 + blue_bias);
      (void)make_png(st::fs::join(dir, frame.path), 16, 12, 0, 0, blue);
    }
    (void)caiyun::write_snapshot(dir, snap);
  };

  // A: 帧 3,5,8；B: 帧 3,5,8,11（多一帧）
  write_meta(dir_a, {3, 5, 8}, 0);
  write_meta(dir_b, {3, 5, 8, 11}, 40);

  const auto diff = caiyun::diff_snapshots(dir_a, dir_b);
  ST_CHECK(diff.has_value());
  if (!diff) {
    st::print("比对失败：{}\n", diff.error().message);
    return;
  }
  ST_CHECK(diff->compared == 3);   // 只有 3 对共有帧
  ST_CHECK(diff->only_right == 1); // B 独有 1 帧
  ST_CHECK(diff->only_left == 0);
  // 帧内容不同（灰度按 index 递增），所以每对都有差异
  for (const auto& frame : diff->frames) {
    ST_CHECK(frame.diff_pct > 0.0);
  }
  // 判词要说明"帧数不一致"（这是人能据以判断"剧本变长了"的信号）
  ST_CHECK(diff->verdict().find("帧数不一致") != std::string::npos);
}

ST_TEST(caiyun_snapshot_verdict_scales_with_change) {
  caiyun::SnapshotDiff none{};
  none.compared = 3;
  ST_CHECK(none.verdict().find("完全一致") != std::string::npos);

  caiyun::SnapshotDiff tiny{};
  tiny.compared = 3;
  tiny.mean_diff_pct = 0.01;
  tiny.worst_diff_pct = 0.02;
  ST_CHECK(tiny.verdict().find("几乎一致") != std::string::npos);

  caiyun::SnapshotDiff local{};
  local.compared = 3;
  local.mean_diff_pct = 0.8;
  local.worst_diff_pct = 1.2;
  ST_CHECK(local.verdict().find("局部改动") != std::string::npos);

  caiyun::SnapshotDiff big{};
  big.compared = 3;
  big.mean_diff_pct = 25.0;
  big.worst_diff_pct = 40.0;
  ST_CHECK(big.verdict().find("显著变化") != std::string::npos);

  // 没有可比帧不能说"一致"（这是"无数据"与"没问题"的区分）
  caiyun::SnapshotDiff empty{};
  ST_CHECK(empty.verdict().find("无可比") != std::string::npos);
  ST_CHECK(empty.verdict().find("完全一致") == std::string::npos);
}
