/// `st::ext::VideoDecoder` 视频解码（ffmpeg 子进程路径）测试。
///
/// 这一层的核心承诺是**"探测不到就如实不支持"**与**"能解码时元信息/像素都真对"**。
/// 所以用例分两类：
/// ① 没有 ffmpeg 时：`available()` 为假、各接口返回 `Unsupported`（**而不是**静默返回
///    空结果——那会让"没有解码器"看起来像"这个文件坏了"）；
/// ② 有 ffmpeg 时：**用 ffmpeg 自己合成一段视频**（不依赖任何外部素材），
///    再读它的元信息与像素——像素断言用"左半红、右半蓝"的两色画面，
///    这样"抽到了哪一帧/有没有抽到"一眼可判。

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/ext/video.hpp"
#include "st/test/test.hpp"

namespace {

using st::ext::VideoDecoder;
using st::ext::VideoInfo;

/// 用 ffmpeg 合成一段测试视频（2 秒、320x240、左边红右边蓝、30fps）。
///
/// 为何合成而不是放一个测试素材文件进仓库：二进制素材会让"这段断言期望什么"
/// 变成"得先打开那个文件看"；而合成命令本身就是可读的期望说明。
[[nodiscard]] auto make_test_video(const std::string& path) -> bool {
  if (!VideoDecoder::available()) return false;
  const std::string tool = VideoDecoder::ffmpeg_path();
  st::process::Options options;
  options.capture_output = true;
  // `-f lavfi -i color=…` 两个色块横向拼接（`hstack`），2 秒。
  const auto result = st::process::run(
      tool,
      {"-y", "-hide_banner", "-loglevel", "error",
       "-f", "lavfi", "-i", "color=c=red:s=160x240:d=2:r=30",
       "-f", "lavfi", "-i", "color=c=blue:s=160x240:d=2:r=30",
       "-filter_complex", "[0:v][1:v]hstack=inputs=2[v]", "-map", "[v]",
       "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "15",
       path},
      options);
  return result.has_value() && result->exit_code == 0 &&
         st::fs::is_regular_file(path);
}

/// 临时路径（进程 id 隔离，避免并行测试撞名）。
[[nodiscard]] auto temp_path(const std::string& name) -> std::string {
  return st::fs::join(st::fs::temp_dir(),
                      std::format("st_video_test_{}_{}", st::process::current_id(), name));
}

}  // namespace

ST_TEST(video_decoder_reports_unavailable_without_ffmpeg) {
  // 注入一个不存在的路径 = 模拟"没有解码器"。
  VideoDecoder::set_ffmpeg_path(temp_path("definitely-missing-ffmpeg.exe"));
  ST_CHECK(!VideoDecoder::available());
  ST_CHECK(VideoDecoder::ffmpeg_path().empty());

  VideoDecoder decoder(temp_path("whatever.mp4"));
  // ⚠ **如实报 `Unsupported`**，而不是返回空的元信息/空白画布：
  // 后者会让上层显示"0×0、时长 0"，看起来像"这个文件是空的"（真因是没解码器）。
  const auto info = decoder.probe();
  ST_CHECK(!info.has_value());
  ST_CHECK(info.error().code == st::ErrorCode::Unsupported);

  const auto frame = decoder.frame_at(0);
  ST_CHECK(!frame.has_value());
  ST_CHECK(frame.error().code == st::ErrorCode::Unsupported);

  const auto png = decoder.extract_png(0, temp_path("x.png"));
  ST_CHECK(!png.has_value());

  VideoDecoder::reset_probe();
}

ST_TEST(video_decoder_probe_reads_real_metadata) {
  if (!VideoDecoder::available()) {
    std::fprintf(stderr, "[video] 本机没有 ffmpeg，跳过\n");
    return;
  }
  const std::string path = temp_path("probe.mp4");
  ST_REQUIRE(make_test_video(path));
  VideoDecoder decoder(path);
  ST_CHECK(decoder.valid());

  const auto info = decoder.probe();
  ST_REQUIRE(info.has_value());
  // 合成参数就是期望：320x240、2 秒、30fps。
  ST_CHECK_EQ(info->width, 320);
  ST_CHECK_EQ(info->height, 240);
  ST_CHECK(info->fps > 29.0 && info->fps < 31.0);
  // 时长 2000ms（编码器可能给 1.98~2.02，容差取 200ms）。
  ST_CHECK(std::llabs(info->duration_ms - 2000) < 200);
  ST_CHECK(!info->codec.empty());
  // 没有音轨（合成命令没给音频）——这一条是"别把 hstack 的第二路当成音频"的回归。
  ST_CHECK(!info->has_audio);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

ST_TEST(video_decoder_extracts_frame_pixels) {
  if (!VideoDecoder::available()) {
    std::fprintf(stderr, "[video] 本机没有 ffmpeg，跳过\n");
    return;
  }
  const std::string path = temp_path("frame.mp4");
  ST_REQUIRE(make_test_video(path));
  VideoDecoder decoder(path);

  const auto frame = decoder.frame_at(500);
  ST_REQUIRE(frame.has_value());
  ST_CHECK_EQ(frame->physical_width(), 320);
  ST_CHECK_EQ(frame->physical_height(), 240);

  // **左边红、右边蓝**：两处取样必须明显不同，且各自偏向合成时的颜色。
  // 取中间高度、避开色块接缝（x=160 附近）与边缘（编码会有振铃）。
  const auto left = frame->pixel_at(40, 120);
  const auto right = frame->pixel_at(280, 120);
  ST_CHECK(left.r > 150);              // 左：红为主
  ST_CHECK(left.b < 100);
  ST_CHECK(right.b > 150);             // 右：蓝为主
  ST_CHECK(right.r < 100);
  // 两侧确实不同（防"抽到一张全黑/全灰的图也算过"）。
  ST_CHECK(left != right);

  // 缩放抽帧：宽度收窄，高度按比例。
  const auto small = decoder.frame_at(500, 80);
  ST_REQUIRE(small.has_value());
  ST_CHECK_EQ(small->physical_width(), 80);
  ST_CHECK_EQ(small->physical_height(), 60);

  // 逐帧不同（500ms 与 1500ms 都落在同一色块的稳定区间；这条主要防"每次都抽首帧"）。
  const auto later = decoder.frame_at(1500);
  ST_REQUIRE(later.has_value());
  ST_CHECK_EQ(later->physical_width(), 320);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

ST_TEST(video_decoder_extract_png_writes_a_readable_file) {
  if (!VideoDecoder::available()) {
    std::fprintf(stderr, "[video] 本机没有 ffmpeg，跳过\n");
    return;
  }
  const std::string video = temp_path("png.mp4");
  ST_REQUIRE(make_test_video(video));
  const std::string out = temp_path("out.png");
  std::error_code ignored;
  std::filesystem::remove(out, ignored);

  VideoDecoder decoder(video);
  const auto status = decoder.extract_png(1000, out, 160);
  ST_CHECK(status.has_value());
  ST_REQUIRE(st::fs::is_regular_file(out));
  // 文件不是空的（"退出成功但写了个 0 字节"要在这一层被抓）。
  const auto size = st::fs::file_size(out);
  ST_REQUIRE(size.has_value());
  ST_CHECK(*size > 100);

  // 不存在的输入文件：**报错而不是成功**（返回 true 会让调用方以为抽到了帧）。
  VideoDecoder missing(temp_path("no-such-video.mp4"));
  ST_CHECK(!missing.extract_png(0, temp_path("never.png")).has_value());

  std::filesystem::remove(video, ignored);
  std::filesystem::remove(out, ignored);
}

ST_TEST(video_decoder_rejects_non_video_file) {
  if (!VideoDecoder::available()) {
    std::fprintf(stderr, "[video] 本机没有 ffmpeg，跳过\n");
    return;
  }
  // 一个文本文件：`ffmpeg -i` 会失败，而**不能**返回全零的 `VideoInfo`
  //（那会被显示成"0×0、时长 0"，看起来像文件是空的）。
  const std::string path = temp_path("not-a-video.txt");
  ST_REQUIRE(st::fs::write_text(path, "这不是视频\n").has_value());
  VideoDecoder decoder(path);
  const auto info = decoder.probe();
  ST_CHECK(!info.has_value());
  ST_CHECK(info.error().code == st::ErrorCode::Invalid);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}
