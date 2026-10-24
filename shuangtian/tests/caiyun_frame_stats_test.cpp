/// 裁云**画面质量度量**单测：静止占比 / 过亮帧 / 平均亮度 / 逐镜聚合。
///
/// ## 为什么这几条值得钉住
///
/// 这组数字替代的是「渲完抽几帧用眼睛看」。它错了**不会崩**，只会给出一个
/// 看起来合理的错数——然后有人照它去改片子（把不慢的镜头改短、把不亮的调暗），
/// 或者照它宣布"质检通过"。这是最坏的一类缺陷：**静默且被信任**。
///
/// 因此每条判据都配一个**真值可手算**的合成画布：
/// 纯色序列 → 静止 100%；定步长位移 → 静止 0%；全白 → 亮度 255；全黑 → 亮度 0。
/// 指标与真值方向不一致时，用例必须红。
///
/// ## 口径（与 `frame_stats.hpp` 的注释一致，改这里就要改那里）
///
/// - 帧差 = 相邻帧**降采样灰度图**的平均绝对差（0–255 尺度，块均值降采样）。
/// - 静止帧 = 帧差 < `static_threshold`（默认 1.2）。
/// - 静止占比 = 静止帧对数 / 总帧对数（单帧段为 0——单帧无法判动静）。

#include "examples/caiyun/frame_stats.cpp"

#include <cstdint>
#include <vector>

#include "st/test/test.hpp"

namespace {

using caiyun::QualityReport;
using caiyun::StatsAccumulator;

constexpr int kWidth = 320;
constexpr int kHeight = 180;

/// 整帧填一个 RGB 色（不透明）。
void fill(st::raster::Canvas& canvas, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  const std::uint32_t px = (static_cast<std::uint32_t>(255) << 24) |
                           (static_cast<std::uint32_t>(b) << 16) |
                           (static_cast<std::uint32_t>(g) << 8) |
                           static_cast<std::uint32_t>(r);
  for (auto& p : canvas.pixels()) p = px;
}

/// 在纯色底上画一块竖直居中的矩形（模拟「有内容在动」）。
void block(st::raster::Canvas& canvas, int x0, int w, std::uint8_t r, std::uint8_t g,
           std::uint8_t b) {
  const int W = canvas.physical_width();
  const int H = canvas.physical_height();
  const std::uint32_t px = (static_cast<std::uint32_t>(255) << 24) |
                           (static_cast<std::uint32_t>(b) << 16) |
                           (static_cast<std::uint32_t>(g) << 8) |
                           static_cast<std::uint32_t>(r);
  for (int y = H / 3; y < H * 2 / 3; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      if (x < 0 || x >= W) continue;
      canvas.pixels()[static_cast<std::size_t>(y) * W + x] = px;
    }
  }
}

}  // namespace

ST_TEST(frame_stats_luma_reads_the_right_channels) {
  // 通道顺序不能靠猜：Canvas 内部 `pixels()` 的打包顺序与 `to_rgba8()` 的字节序
  // 未必一致，读错通道会让「红色」被当成「蓝色」——灰度值随之算错，
  // 而静止占比/亮度全建立在这个灰度上（错了不会崩，只会给出看起来合理的错数）。
  //
  // 用 Surface 的公开 API（set_pixel）写入已知色，再与 to_rgba8 的字节比对，
  // 从而**从实现本身**确定 downscale_luma 该按什么顺序读。
  st::raster::Canvas canvas{8, 8, 1.0f};
  for (auto& px : canvas.pixels()) px = 0;
  canvas.set_pixel(3, 3, st::math::Color{0x11, 0x22, 0x33, 0xFF});

  const std::uint32_t raw = canvas.pixels()[3 * 8 + 3];
  const auto bytes = canvas.to_rgba8();
  const std::size_t at = (static_cast<std::size_t>(3) * 8 + 3) * 4;
  // to_rgba8 的契约是 RGBA 字节序
  ST_CHECK(bytes[at] == 0x11);
  ST_CHECK(bytes[at + 1] == 0x22);
  ST_CHECK(bytes[at + 2] == 0x33);
  ST_CHECK(bytes[at + 3] == 0xFF);

  // **实测布局：pixels() 是 0xRRGGBBAA**（raw=0x112233FF，字节 FF 33 22 11）。
  // 也就是说低字节是 A、最高字节是 R——与 to_rgba8 的 R,G,B,A 字节序**正好相反**。
  // 按 0xAARRGGBB 读会把 alpha 当成红（全不透明画面被算成纯红、灰度恒 76），
  // 而静止占比仍能算对，所以只有亮度判据偏——数值看起来"正常"，最难发现。
  const std::uint8_t b0 = static_cast<std::uint8_t>(raw & 0xFFU);
  const std::uint8_t b1 = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  const std::uint8_t b2 = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
  const std::uint8_t b3 = static_cast<std::uint8_t>((raw >> 24U) & 0xFFU);
  ST_CHECK(raw == 0x112233FFU);
  ST_CHECK(b3 == 0x11U);  // 最高字节 = R
  ST_CHECK(b2 == 0x22U);  // G
  ST_CHECK(b1 == 0x33U);  // B
  ST_CHECK(b0 == 0xFFU);  // 最低字节 = A
}

ST_TEST(frame_stats_static_frames_report_full_still) {
  // 纯色 + 固定块，逐帧完全一致 → 静止占比必须 100%。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  for (int i = 0; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    block(canvas, 20, 40, 224, 224, 224);
    acc.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  const QualityReport report = acc.report(1.0, false);
  ST_CHECK(report.frames == 10);
  ST_CHECK(report.still_ratio > 99.0);
  // 一个在动的帧都没有 → 变化密度 0
  ST_CHECK(report.change_density < 1e-9);
}

ST_TEST(frame_stats_moving_block_reports_no_still) {
  // 每帧位移 20px（远超静止阈值）→ 静止占比必须 0%。
  // 这条与上一条互为对照：同一条代码路径，必须能分辨两个已知不同的信号。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  for (int i = 0; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    block(canvas, 20 + i * 20, 40, 224, 224, 224);
    acc.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  const QualityReport report = acc.report(1.0, false);
  ST_CHECK(report.still_ratio < 1.0);
  ST_CHECK(report.change_density > 5.0);
}

ST_TEST(frame_stats_brightness_ordering) {
  // 全白 → 亮度接近 255 且全部计入过亮帧；全黑 → 亮度 0 且无过亮帧。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  {
    StatsAccumulator acc{};
    for (int i = 0; i < 5; ++i) {
      fill(canvas, 255, 255, 255);
      acc.push(canvas, i, static_cast<double>(i) / 10.0);
    }
    const QualityReport white = acc.report(0.5, false);
    ST_CHECK(white.mean_luma > 250.0);
    ST_CHECK(white.bright_ratio > 99.0);
  }
  {
    StatsAccumulator acc{};
    for (int i = 0; i < 5; ++i) {
      fill(canvas, 0, 0, 0);
      acc.push(canvas, i, static_cast<double>(i) / 10.0);
    }
    const QualityReport black = acc.report(0.5, false);
    ST_CHECK(black.mean_luma < 5.0);
    ST_CHECK(black.bright_ratio < 1.0);
  }
}

ST_TEST(frame_stats_per_shot_separates_still_and_moving) {
  // 逐镜是这条能力的关键：全片平均会把「某一镜 81% 静止」冲淡成 48%。
  // 造 0–1s 静止 + 1–2s 运动，逐镜必须分别给出 ~100% 与 ~0%。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  int index = 0;
  for (int i = 0; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    block(canvas, 20, 40, 224, 224, 224);
    acc.push(canvas, index++, static_cast<double>(i) / 10.0);
  }
  for (int i = 0; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    block(canvas, 20 + i * 20, 40, 224, 224, 224);
    acc.push(canvas, index++, 1.0 + static_cast<double>(i) / 10.0);
  }
  std::vector<StatsAccumulator::ShotSpan> spans{};
  spans.push_back({11, 0, "shot-static", 0.0, 1.0});
  spans.push_back({12, 1, "shot-moving", 1.0, 2.0});
  acc.attach_spans(spans);
  const QualityReport report = acc.report(2.0, false);

  ST_CHECK(report.shots.size() == 2);
  const auto& a = report.shots[0];
  const auto& b = report.shots[1];
  ST_CHECK(a.frames > 0);
  ST_CHECK(b.frames > 0);
  ST_CHECK(a.frame_pairs >= 8);
  ST_CHECK(b.frame_pairs >= 8);
  ST_CHECK(a.still_ratio() > 85.0);   // 静止镜
  ST_CHECK(b.still_ratio() < 60.0);   // 运动镜明显更低
  // 最静一镜必须指向静止镜（这就是「慢在哪」的答案）
  ST_CHECK(report.worst_still_clip_id == 11);
}

ST_TEST(frame_stats_single_frame_shot_reports_zero_not_hundred) {
  // 单帧段无法判动静：报 0 而不是 100%。
  // 报 100% 会让「片尾多留一帧」这类无害情形被误判成"静止拖沓"。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  fill(canvas, 16, 24, 32);
  acc.push(canvas, 0, 0.0);
  std::vector<StatsAccumulator::ShotSpan> spans{};
  spans.push_back({7, 0, "one-frame", 0.0, 0.1});
  acc.attach_spans(spans);
  const QualityReport report = acc.report(0.1, false);
  ST_CHECK(report.shots.size() == 1);
  ST_CHECK(report.shots[0].frames == 1);
  ST_CHECK(report.shots[0].frame_pairs == 0);
  ST_CHECK(report.shots[0].still_ratio() < 1e-9);
}

ST_TEST(frame_stats_non_adjacent_frames_are_not_compared) {
  // 并行渲染时同一 worker 拿到的 index 可能不连续。**不相邻的帧不能相比**——
  // 拿两个相隔很远的帧相减会得到"伪运动"，从而把静止段量成在动。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  fill(canvas, 16, 24, 32);
  block(canvas, 20, 40, 224, 224, 224);
  acc.push(canvas, 0, 0.0);
  // 跳到 index=5（中间的帧在别的 worker 上）：这一帧的帧差应标记为"无相邻前帧"
  fill(canvas, 16, 24, 32);
  block(canvas, 200, 40, 224, 224, 224);
  const auto stats = acc.push(canvas, 5, 0.5);
  ST_CHECK(stats.diff_prev < 0.0);  // -1 = 无相邻前帧
}

ST_TEST(frame_stats_merge_keeps_all_frames) {
  // 并行归并：两个线程各渲一半，归并后帧数必须是两半之和（不能丢帧）。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator a{};
  StatsAccumulator b{};
  for (int i = 0; i < 5; ++i) {
    fill(canvas, 16, 24, 32);
    a.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  for (int i = 5; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    b.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  a.merge(b);
  const QualityReport report = a.report(1.0, false);
  ST_CHECK(report.frames == 10);
}

ST_TEST(frame_stats_empty_reports_clearly) {
  // 没渲任何帧时不能编数字：frames=0、summary 明说「未渲染」。
  StatsAccumulator acc{};
  const QualityReport report = acc.report(1.0, false);
  ST_CHECK(report.frames == 0);
  ST_CHECK(report.summary.find("未渲染") != std::string::npos);
  ST_CHECK(report.still_ratio == 0.0);
}

ST_TEST(frame_stats_partial_flag_is_reported) {
  // 局部渲染（只渲一段做动效确认）时，结论只覆盖那段——必须标记出来，
  // 否则会被当成全片质检结论。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  for (int i = 0; i < 3; ++i) {
    fill(canvas, 16, 24, 32);
    acc.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  const QualityReport report = acc.report(0.3, true);
  ST_CHECK(report.partial);
  ST_CHECK(report.summary.find("局部渲染") != std::string::npos);
}

ST_TEST(frame_stats_parallel_workers_still_have_adjacent_pairs) {
  // 导出是 16 路并行，每路 `fetch_add` 抢帧号：同一路拿到的是 0、16、32…，
  // **相邻帧永远在不同 worker 上**。若帧差只在「本路上一帧」之间算，
  // 归并后有效帧对数恒为 0——实测一次 16 秒导出报「静止 0.0%（帧差 <1.2，**0 对**）」，
  // 指标恒为 0 而摘要毫无异常。这条用例造两个工人抢奇偶帧号，
  // 断言归并后帧对数 = 帧数-1 且能分辨已知的「动」与「不动」。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator even{};
  StatsAccumulator odd{};
  for (int i = 0; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    block(canvas, 20 + i * 20, 40, 224, 224, 224);
    (i % 2 == 0 ? even : odd).push(canvas, i, static_cast<double>(i) / 10.0);
  }
  even.merge(odd);
  const QualityReport moving = even.report(1.0, false);
  ST_CHECK(moving.frames == 10);
  // 分母必须凑得出来（9 对）——这正是旧实现丢失的东西
  ST_CHECK(moving.frame_pairs == 9);
  ST_CHECK(moving.still_ratio < 1.0);
  ST_CHECK(moving.change_density > 5.0);

  // 对照组：同样交错喂入，但画面逐帧完全一致 → 必须判 100% 静止。
  // 没有这一组，「帧对数对了」可以靠「随便把差算得很大」蒙过去。
  StatsAccumulator still_even{};
  StatsAccumulator still_odd{};
  for (int i = 0; i < 10; ++i) {
    fill(canvas, 16, 24, 32);
    block(canvas, 20, 40, 224, 224, 224);
    (i % 2 == 0 ? still_even : still_odd).push(canvas, i, static_cast<double>(i) / 10.0);
  }
  still_even.merge(still_odd);
  const QualityReport still = still_even.report(1.0, false);
  ST_CHECK(still.frame_pairs == 9);
  ST_CHECK(still.still_ratio > 99.0);
}

ST_TEST(frame_stats_zero_pairs_is_flagged) {
  // 分母为 0 时不能闷不吱声：只渲了一帧就出报告，必须明说不可信。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  fill(canvas, 16, 24, 32);
  acc.push(canvas, 0, 0.0);
  const QualityReport report = acc.report(0.033, false);
  ST_CHECK(report.frames == 1);
  ST_CHECK(report.frame_pairs == 0);
  ST_CHECK(report.summary.find("无有效相邻帧对") != std::string::npos);
}

ST_TEST(frame_stats_definition_is_in_summary) {
  // 阈值必须随数一起给出：换阈值就换答案，不给阈值的百分比无法复核。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  for (int i = 0; i < 4; ++i) {
    fill(canvas, 16, 24, 32);
    acc.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  const QualityReport report = acc.report(0.4, false);
  // summary 里要能看见「帧差 <阈值」这一句
  ST_CHECK(report.summary.find("帧差 <") != std::string::npos);
  ST_CHECK(report.static_threshold > 0.0);
}

ST_TEST(frame_stats_json_roundtrip_shape) {
  // JSON 是给智能体/e2e 读的唯一形状——字段名改了就断了消费者。
  st::raster::Canvas canvas{kWidth, kHeight, 1.0f};
  StatsAccumulator acc{};
  for (int i = 0; i < 4; ++i) {
    fill(canvas, 16, 24, 32);
    acc.push(canvas, i, static_cast<double>(i) / 10.0);
  }
  std::vector<StatsAccumulator::ShotSpan> spans{};
  spans.push_back({3, 0, "shot-a", 0.0, 0.4});
  acc.attach_spans(spans);
  const QualityReport report = acc.report(0.4, false);
  const std::string json = caiyun::quality_to_json(report);
  for (const char* key : {"\"still_ratio\"", "\"bright_ratio\"", "\"mean_luma\"",
                          "\"change_density\"", "\"static_threshold\"", "\"shots\"",
                          "\"worst_still_clip_id\"", "\"partial\"", "\"summary\"",
                          "\"frame_pairs\""}) {
    ST_CHECK(json.find(key) != std::string::npos);
  }
  // 必须能解析回来（自研 Json 的往返）
  const auto parsed = st::json_parse(json);
  ST_CHECK(parsed.has_value());
}
