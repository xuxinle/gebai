#include "st/ext/video.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/string.hpp"
#include "st/codec/png.hpp"

namespace st::ext {

namespace {

/// 探测缓存（进程级）。
///
/// lint-allow: L8 这三项是**探测缓存**（与 `log.cpp` 的进程级配置同类）：
/// 它们只回答"本机有没有 ffmpeg"，不参与业务判断，且探测本身要起子进程
///（每帧抽图都重探一次的话，光探测就比解码贵）。注入 `Context` 会让每个
/// 调用点都得传一对"只给探测看"的状态——那是把成本转嫁给所有人。
///
/// 用**函数内静态**而不是文件作用域全局：初始化顺序明确（首次调用时），
/// 也不会被别的翻译单元的静态初始化意外读到。
struct ProbeCache {
  int probed{-1};   ///< `-1` = 未探测 / `0` = 不可用 / `1` = 可用
  std::string ffmpeg{};
  std::string override_path{};   ///< 显式覆盖（测试注入；空 = 走 PATH 探测）
};
[[nodiscard]] auto probe_cache() -> ProbeCache& {
  static ProbeCache cache;
  return cache;
}

/// 在 PATH 里找 ffmpeg（找不到返回空）。
[[nodiscard]] auto probe_ffmpeg() -> std::string {
  if (!probe_cache().override_path.empty()) return probe_cache().override_path;
  if (const auto found = process::which("ffmpeg"); found.has_value()) return *found;
  // Windows 上常见把 ffmpeg.exe 放在非 PATH 目录；不猜路径——
  // 猜错了会得到"看起来可用但一跑就报错"的状态，比"不可用"更糟。
  return {};
}

/// 从 `ffmpeg -i` 的 stderr 里抠元信息。
///
/// ⚠ ffmpeg 把文件信息写在 **stderr** 且**以非零码退出**（因为没给输出目标）——
/// 所以不能"退出码非零就判失败"，那会把每个正常文件都判成坏的（这就是为什么
/// 这里自己解析而不是找一个更友好的接口）。
[[nodiscard]] auto parse_probe_output(const std::string& text) -> VideoInfo {
  VideoInfo info;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t eol = text.find('\n', begin);
    const std::string line =
        text.substr(begin, eol == std::string::npos ? std::string::npos : eol - begin);
    begin = eol == std::string::npos ? text.size() : eol + 1;

    const std::size_t colon = line.find(": ");
    if (colon == std::string::npos) continue;
    const std::string key = line.substr(0, colon);
    const std::string value = line.substr(colon + 2);

    if (key.find("Duration") != std::string::npos) {
      // `Duration: 00:00:05.28, start: …` → 毫秒。
      const std::size_t comma = value.find(',');
      const std::string time = value.substr(0, comma);
      // `std::sscanf` 的整数转换说明符与**平台上的类型名**绑定，不认 `std::int64_t`：
      // LP64（Linux/macOS）下 `int64_t` 是 `long`、LLP64（Windows）下是 `long long`，
      // 而 `%lld` 只接受后者——差一个字就是 `-Wformat` 警告，配 `-Werror` 直接构建失败
      // （实测：Windows 自举只警告，WSL 上同一份代码刷屏）。故用 `long long` 承接后显式转换。
      long long hours = 0;
      long long minutes = 0;
      double seconds = 0.0;
      if (std::sscanf(time.c_str(), "%lld:%lld:%lf", &hours, &minutes, &seconds) == 3) {
        info.duration_ms = (static_cast<std::int64_t>(hours) * 3600 +
                            static_cast<std::int64_t>(minutes) * 60) *
                               1000 +
                           static_cast<std::int64_t>(seconds * 1000.0);
      }
    } else if (key.find("Stream #") != std::string::npos) {
      if (value.find("Video:") != std::string::npos) {
        // `h264 (High) (avc1 / 0x31637661), yuv420p, 1920x1080 [SAR 1:1 …], 30 fps, …`
        const std::size_t codec_end = value.find_first_of(" (,");
        if (codec_end != std::string::npos) info.codec = value.substr(0, codec_end);
        // 分辨率：找 `NNNxNNN`（形如 `1920x1080`）。
        for (std::size_t index = 0; index + 2 < value.size(); ++index) {
          if (value[index] != 'x') continue;
          if (!std::isdigit(static_cast<unsigned char>(value[index - 1]))) continue;
          std::size_t left = index;
          while (left > 0 && std::isdigit(static_cast<unsigned char>(value[left - 1]))) --left;
          std::size_t right = index + 1;
          while (right < value.size() && std::isdigit(static_cast<unsigned char>(value[right]))) {
            ++right;
          }
          if (right - index <= 1 || index - left < 2) continue;   // 太短不像分辨率
          info.width = std::stoi(value.substr(left, index - left));
          info.height = std::stoi(value.substr(index + 1, right - index - 1));
          break;
        }
        // 帧率：`30 fps` 或 `29.97 fps`。
        const std::size_t fps_at = value.find(" fps");
        if (fps_at != std::string::npos) {
          std::size_t start = fps_at;
          while (start > 0 && (std::isdigit(static_cast<unsigned char>(value[start - 1])) ||
                               value[start - 1] == '.')) {
            --start;
          }
          try {
            info.fps = std::stod(value.substr(start, fps_at - start));
          } catch (...) {
            info.fps = 0.0;
          }
        }
      } else if (value.find("Audio:") != std::string::npos) {
        info.has_audio = true;
      }
    } else if (key.find("Input #") != std::string::npos) {
      // `Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'x.mp4':` → 容器类型。
      const std::size_t from = line.find(", from ");
      if (from != std::string::npos) {
        const std::string head = line.substr(0, from);
        const std::size_t comma = head.find(", ");
        if (comma != std::string::npos) {
          info.container = head.substr(comma + 2);
          const std::size_t next = info.container.find(", from");
          if (next != std::string::npos) info.container.resize(next);
        }
      }
    }
  }
  return info;
}

}  // namespace

auto VideoDecoder::available() -> bool {
  ProbeCache& cache = probe_cache();
  if (cache.probed < 0) {
    cache.ffmpeg = probe_ffmpeg();
    // ⚠ 路径存在 ≠ 可用：覆盖路径可能写错、PATH 里那个可能是同名的别的东西。
    // 所以判据是"**文件真的在**"（`which` 返回的就是存在的路径，而覆盖值要自己验）。
    const bool exists = !cache.ffmpeg.empty() && fs::is_regular_file(cache.ffmpeg);
    if (!exists) cache.ffmpeg.clear();
    cache.probed = exists ? 1 : 0;
  }
  return cache.probed == 1;
}

auto VideoDecoder::ffmpeg_path() -> std::string {
  (void)available();
  return probe_cache().ffmpeg;
}

void VideoDecoder::set_ffmpeg_path(std::string path) {
  probe_cache().override_path = std::move(path);
  probe_cache().probed = -1;   // 下次重新判定
}

void VideoDecoder::reset_probe() {
  ProbeCache& cache = probe_cache();
  cache.override_path.clear();
  cache.probed = -1;
  cache.ffmpeg.clear();
}

auto VideoDecoder::probe() const -> Result<VideoInfo> {
  if (path_.empty()) return unexpected(ErrorCode::Invalid, "未指定视频文件");
  const std::string tool = available() ? ffmpeg_path() : std::string{};
  if (tool.empty()) {
    return unexpected(ErrorCode::Unsupported, "本机没有可用的 ffmpeg（视频解码不可用）");
  }
  process::Options options;
  options.capture_output = true;
  // 不指定输出目标 ⇒ ffmpeg 只打印信息后退出（**退出码非零是正常的**，见解析函数的说明）。
  const auto result = process::run(tool, {"-hide_banner", "-i", path_}, options);
  if (!result.has_value()) {
    last_error_ = result.error().message;
    return unexpected(ErrorCode::Io, "无法运行 ffmpeg：" + last_error_);
  }
  last_error_ = result->stderr_text.empty() ? result->stdout_text : result->stderr_text;
  if (last_error_.size() > 2000) last_error_.resize(2000);
  const VideoInfo info = parse_probe_output(last_error_);
  if (info.width <= 0 || info.height <= 0) {
    // 有输出但没解析出分辨率 ⇒ 不是视频文件（或格式不支持）。
    // **如实报错**而不是返回全零结构（那会被显示成"0×0、时长 0"，
    // 看起来像文件本身是空的）。
    return unexpected(ErrorCode::Invalid, "这个文件里没有可解码的视频流");
  }
  return info;
}

auto VideoDecoder::frame_at(std::int64_t at_ms, std::optional<int> scale_to_width) const
    -> Result<raster::Canvas> {
  if (path_.empty()) return unexpected(ErrorCode::Invalid, "未指定视频文件");
  const std::string tool = available() ? ffmpeg_path() : std::string{};
  if (tool.empty()) {
    return unexpected(ErrorCode::Unsupported, "本机没有可用的 ffmpeg（视频解码不可用）");
  }
  // 落到临时 PNG 再读回来（而不是让它写 rawvideo 到 stdout 再解析）：
  // rawvideo 要自己知道像素格式与行对齐（`-pix_fmt rgba` 下每行可能带填充），
  // 而 PNG 由框架自己的解码器读——**那一层已经有测试覆盖**，不必再赌一次。
  const std::string temp = fs::join(fs::temp_dir(),
                                    std::format("st_video_{}_{}.png", process::current_id(),
                                                at_ms));
  if (auto status = extract_png(at_ms, temp, scale_to_width); !status) {
    return unexpected(status.error().code, status.error().message);
  }

  auto image = codec::png_read_file(temp);
  std::error_code ignored;
  std::filesystem::remove(temp, ignored);   // 临时文件删不掉不影响结果
  if (!image.has_value()) {
    return unexpected(ErrorCode::Io, "抽出的帧读不回来：" + image.error().message);
  }
  // `PngImage` → `Canvas`（像素逐行拷；两者都是非预乘 RGBA8）。
  const int width = static_cast<int>(image->width);
  const int height = static_cast<int>(image->height);
  // `for_logical_size` 收 int 尺寸 + float scale（见其签名）。
  raster::Canvas canvas = raster::Canvas::for_logical_size(width, height, 1.0f);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t at =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          4U;
      if (at + 4U > image->rgba.size()) break;
      canvas.set_pixel(x, y,
                       math::Color{image->rgba[at], image->rgba[at + 1], image->rgba[at + 2],
                                   image->rgba[at + 3]});
    }
  }
  return canvas;
}

auto VideoDecoder::extract_png(std::int64_t at_ms, const std::string& output_path,
                               std::optional<int> scale_to_width) const -> Status {
  if (path_.empty()) return unexpected(ErrorCode::Invalid, "未指定视频文件");
  const std::string tool = available() ? ffmpeg_path() : std::string{};
  if (tool.empty()) {
    return unexpected(ErrorCode::Unsupported, "本机没有可用的 ffmpeg（视频解码不可用）");
  }
  const double seconds = static_cast<double>(std::max<std::int64_t>(0, at_ms)) / 1000.0;
  std::vector<std::string> args{"-y", "-hide_banner", "-loglevel", "error",
                                // `-ss` 放在 `-i` **之前**：快速定位（容器索引直接跳），
                                // 放后面是精确但慢的逐帧解码——素材查看要的是前者。
                                "-ss", std::format("{:.3f}", seconds),
                                "-i", path_,
                                "-frames:v", "1"};
  if (scale_to_width.has_value() && *scale_to_width > 0) {
    // `-1` 让高度按比例自动定（且**保持偶数**：某些编码器要求偶数尺寸）。
    args.insert(args.end(), {"-vf", std::format("scale={}:-2", *scale_to_width)});
  }
  args.push_back(output_path);

  process::Options options;
  options.capture_output = true;
  const auto result = process::run(tool, args, options);
  if (!result.has_value()) {
    last_error_ = result.error().message;
    return unexpected(ErrorCode::Io, "无法运行 ffmpeg：" + last_error_);
  }
  if (result->exit_code != 0) {
    last_error_ = result->stderr_text.empty() ? result->stdout_text : result->stderr_text;
    if (last_error_.size() > 1000) last_error_.resize(1000);
    return unexpected(ErrorCode::Io,
                      std::format("抽帧失败（{}ms）：{}", at_ms, st::trim(last_error_)));
  }
  if (!fs::is_regular_file(output_path)) {
    return unexpected(ErrorCode::Io, "ffmpeg 退出成功但没写出文件（抽帧位置超出片长？）");
  }
  return ok();
}

}  // namespace st::ext
