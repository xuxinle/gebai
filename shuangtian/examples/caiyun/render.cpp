/// 成片渲染管线的实现（见 `render.hpp`）。
///
/// 三个阶段各自独立可成：帧（PNG）、声（WAV）、封装（ffmpeg）。前两个**始终可用**，
/// 第三个探测不到就如实记进 `status.error` 并保留前两个的产物。

#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "frame_stats.hpp"
#include "motion.hpp"
#include "st/app/async.hpp"
#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/process.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"

namespace caiyun {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// 与 UI 渲染**同一口径**的帧时间：`index / fps`。两者不一致会让"导出的第 N 帧"
/// 与"监视器 seek 到 N/fps 那一帧"对不上——逐帧回归最容易踩的一处。
[[nodiscard]] auto frame_time(int index, double fps) -> double {
  return static_cast<double>(index) / fps;
}

/// 帧 → PNG（直通 RGBA8）。`png_encode` 自己处理压缩与 CRC。
[[nodiscard]] auto write_frame_png(const st::raster::Canvas& canvas, const std::string& path)
    -> bool {
  st::codec::PngImage image;
  image.width = static_cast<std::uint32_t>(canvas.physical_width());
  image.height = static_cast<std::uint32_t>(canvas.physical_height());
  image.rgba = canvas.to_rgba8();
  if (image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U) return false;
  // 压缩级别取 1（不是默认 6）：PNG 是**中间产物**（随后交给 ffmpeg 编 h264），
  // 它只决定写盘大小与耗时，与成片画质**完全无关**（zlib 无损）。
  // 实测 1080p 单帧：级别 6 约 96ms、级别 1 约 40ms，而这两倍速度直接等于成片
  // 导出时间的下降（渲染本身已降到 14ms/帧）。
  return st::codec::png_write_file(path, image, 1).has_value();
}

/// 16 位小端写 WAV。
void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}
void put_u32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
  out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

/// 一个音符的时间包络（快起、指数衰减）——避免咔哒声。
[[nodiscard]] auto envelope(double local, double duration) -> double {
  if (local < 0.0 || local > duration) return 0.0;
  constexpr double attack = 0.012;
  constexpr double release = 0.28;
  double gain = 1.0;
  if (local < attack) gain = local / attack;
  const double tail = duration - local;
  if (tail < release) gain *= tail / release;
  return gain * std::exp(-2.4 * local / std::max(0.05, duration));
}

}  // namespace

// ══════════════════════════════ 运动模糊（快门超采样）══════════════════════════════
//
// ## 为何需要它（这是"看起来像成品"与"像幻灯片"的分界之一）
//
// 快动作（>80 px/帧）不模糊会"一格一格地闪"（频闪），人眼立刻会判"这是动画"。
// 真实摄影机有 180° 快门：在一帧的曝光时间里，运动物体在每个瞬时位置都留痕。
//
// ## 做法：时间超采样 + 线性光平均
//
// 不是"对成品帧做高斯模糊"（那是错的：会连静止部分一起糊），
// 而是在**快门开启区间**内多次渲染真实画面，再平均：
// 静止的东西多次采样结果一样 → 平均后仍然锐利；
// 运动的东西每次位置不同 → 自然拖出残影。

namespace {

/// sRGB 字节 → 线性 float（0..1）。
///
/// 为何不能直接对 sRGB 字节平均：两个都很亮的像素平均出来会变暗，
/// 拖影中间会发灰——真实的运动模糊没有这个"中间变暗"的伪影。
[[nodiscard]] auto srgb_to_linear(std::uint8_t value) -> float {
  const float srgb = static_cast<float>(value) / 255.0f;
  return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] auto linear_to_srgb(float value) -> std::uint8_t {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  const float srgb = clamped <= 0.0031308f ? clamped * 12.92f
                                           : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
  return static_cast<std::uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
}

/// 把一帧累加到线性光缓存（只取 RGB；运动模糊不需要 alpha）。
void accumulate_linear(const st::raster::Canvas& canvas, std::vector<float>& accum) {
  const int width = canvas.width();
  const int height = canvas.height();
  const auto pixels = canvas.to_rgba8();   // R,G,B,A 字节序
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t pixel =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);
      accum[pixel * 3U + 0U] += srgb_to_linear(pixels[pixel * 4U + 0U]);
      accum[pixel * 3U + 1U] += srgb_to_linear(pixels[pixel * 4U + 1U]);
      accum[pixel * 3U + 2U] += srgb_to_linear(pixels[pixel * 4U + 2U]);
    }
  }
}

/// 两帧是否"几乎一样"（用于跳过静止帧的多采样）。
///
/// 阈值 8/255：低于此人眼看不出来，但能让静止帧直接省掉 N 倍渲染成本。
/// 稀疏采样（每 7 个像素取一个）：够用且几乎不占时间。
[[nodiscard]] auto frames_are_nearly_identical(const st::raster::Canvas& a,
                                               const st::raster::Canvas& b) -> bool {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  const auto left = a.to_rgba8();
  const auto right = b.to_rgba8();
  constexpr std::size_t kStep = 7U * 4U;
  for (std::size_t index = 0; index + 3U < left.size(); index += kStep) {
    for (std::size_t channel = 0; channel < 3U; ++channel) {
      if (std::abs(static_cast<int>(left[index + channel]) -
                   static_cast<int>(right[index + channel])) > 8) {
        return false;
      }
    }
  }
  return true;
}

/// **带运动模糊地渲染一帧**（快门超采样）。
///
/// `scratch` 是每 worker 的临时画布，用于采样子帧（不能与 `target` 同一块）。
void render_frame_with_motion_blur(Project& project, st::raster::Canvas& target,
                                   const RenderJob& job, int index,
                                   const st::ui::TextPort* text_port,
                                   st::raster::Canvas& scratch,
                                   std::vector<ElementTrack>* trajectory = nullptr) {
  const double center = frame_time(index, job.fps);
  // 轨迹与运动模糊**不兼容**：模糊会在快门区间内多次渲染，
  // 每渲一次都会往 sink 里压一份，结果是同一帧好几份重复记录。
  // 所以采集轨迹时强制单采样（轨迹是诊断输出，不需要模糊）。
  set_trajectory_sink(trajectory);
  const int samples = trajectory != nullptr ? 1 : std::clamp(job.motion_samples, 1, 16);
  if (samples <= 1 || job.shutter <= 0.0) {
    project.render_frame(target, center, text_port);
    return;
  }

  // 180° 快门 = 半个帧间隔。`shutter` 是"快门开启时间 / 帧间隔"（0.5 = 180°）。
  const double exposure = (1.0 / std::max(1.0, static_cast<double>(job.fps))) * job.shutter;

  // ① 先用两个端点估"这一帧到底动了多少"。
  //    静止帧直接退回单采样——大多数帧几乎不动，这一步省掉大量无谓成本。
  project.render_frame(target, center - exposure * 0.5, text_port);
  project.render_frame(scratch, center + exposure * 0.5, text_port);
  if (frames_are_nearly_identical(target, scratch)) {
    project.render_frame(target, center, text_port);
    return;
  }

  // ② 真正采样：从快门开到快门关，等间隔采 `samples` 次，在线性光里平均。
  std::vector<float> accum(
      static_cast<std::size_t>(target.width()) * static_cast<std::size_t>(target.height()) * 3U,
      0.0f);
  for (int sample = 0; sample < samples; ++sample) {
    // 采样点取每个子区间的**中点**：端点会让最先/最后的瞬间权重偏高，拖影两端发虚。
    const double t = center - exposure * 0.5 +
                     exposure * ((static_cast<double>(sample) + 0.5) /
                                 static_cast<double>(samples));
    project.render_frame(scratch, t, text_port);
    accumulate_linear(scratch, accum);
  }
  // resolve：线性光平均 → sRGB
  const int width = target.width();
  const int height = target.height();
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  const float inverse = 1.0f / static_cast<float>(samples);
  for (std::size_t pixel = 0;
       pixel < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++pixel) {
    pixels[pixel * 4U + 0U] = linear_to_srgb(accum[pixel * 3U + 0U] * inverse);
    pixels[pixel * 4U + 1U] = linear_to_srgb(accum[pixel * 3U + 1U] * inverse);
    pixels[pixel * 4U + 2U] = linear_to_srgb(accum[pixel * 3U + 2U] * inverse);
  }
  target.replace_rgba8(pixels, width, height);
}

}  // namespace

auto Renderer::find_ffmpeg(const std::string& override_path) -> std::string {
  if (!override_path.empty()) {
    return st::fs::exists(override_path) ? override_path : std::string{};
  }
  const auto found = st::process::which("ffmpeg");
  return found.has_value() ? *found : std::string{};
}

auto Renderer::synth_audio(const AudioSpec& spec, double duration_s, const std::string& path,
                           VoiceReport* report) -> bool {
  if (!spec.enabled || duration_s <= 0.05) return false;
  constexpr int kRate = 44100;
  // 输出电平预算（混音全程共用）：峰值空间配乐 0.28 / 人声 0.55 / 余量到 0.83。
  // 配乐峰值必须低于人声峰值——否则无论怎么 duck 都盖不住旁白。
  constexpr double kMusicCeiling = 0.28;
  constexpr double kTotalCeiling = 0.86;
  const double bpm = static_cast<double>(std::min(std::max(spec.bpm, 40), 240));
  const double beat = 60.0 / bpm;
  const std::int64_t total = static_cast<std::int64_t>(duration_s * kRate) + kRate / 2;
  std::vector<double> mix(static_cast<std::size_t>(total), 0.0);

  // 和弦：C - G - Am - F（明亮上行），每两拍换一次。
  const std::vector<std::vector<double>> chords = {
      {261.63, 329.63, 392.00},   // C
      {392.00, 493.88, 587.33},   // G
      {440.00, 523.25, 659.26},   // Am
      {349.23, 440.00, 523.25},   // F
  };
  const bool uplift = spec.style != "calm";
  const double pad_gain = uplift ? 0.16 : 0.22;
  const double lead_gain = uplift ? 0.13 : 0.05;

  const auto add_note = [&mix, total](double start, double freq, double length, double gain,
                                      double detune) {
    const std::int64_t begin = static_cast<std::int64_t>(start * kRate);
    const std::int64_t count = static_cast<std::int64_t>(length * kRate);
    for (std::int64_t i = 0; i < count; ++i) {
      const std::int64_t at = begin + i;
      if (at < 0 || at >= total) continue;
      const double local = static_cast<double>(i) / kRate;
      const double phase = 2.0 * kPi * freq * (1.0 + detune) * local;
      // 基础正弦 + 一点二次谐波（让音色不至于"电子管风琴"那么薄）。
      const double wave = std::sin(phase) * 0.8 + std::sin(phase * 2.0) * 0.16;
      mix[static_cast<std::size_t>(at)] += wave * gain * envelope(local, length);
    }
  };

  // 铺底：每两拍一个和弦，音符长度跨两拍（连续感）。
  for (double start = 0.0; start < duration_s + beat; start += beat * 2.0) {
    const auto chord_index =
        static_cast<std::size_t>(std::llround(start / (beat * 2.0))) % chords.size();
    for (const double freq : chords[chord_index]) {
      add_note(start, freq, beat * 2.4, pad_gain, 0.0007);
      add_note(start, freq, beat * 2.4, pad_gain * 0.7, -0.0007);
    }
  }
  // 律动：每拍一个短促高音（calm 档更稀疏：两拍一次、更长）。
  const double step = uplift ? beat : beat * 2.0;
  int index = 0;
  for (double start = 0.0; start < duration_s; start += step) {
    const auto chord_index =
        static_cast<std::size_t>(std::llround(start / (beat * 2.0))) % chords.size();
    const double freq = chords[chord_index][static_cast<std::size_t>(index) % 3] * 2.0;
    add_note(start, freq, uplift ? 0.30 : 0.55, lead_gain, 0.0);
    ++index;
  }
  // 收尾：最后 1.1s 的上行音（"落地"感）。
  const double outro_start = std::max(0.0, duration_s - 1.1);
  const std::vector<double> rise = {523.25, 587.33, 659.26, 783.99};
  for (std::size_t i = 0; i < rise.size(); ++i) {
    add_note(outro_start + static_cast<double>(i) * 0.18, rise[i], 0.75, lead_gain * 1.5, 0.0);
  }
  // 首尾淡入淡出的包络在最后的混音循环里一并施（见下方 `tail_fade`）。

  // —— 配乐归一化（先定好配乐自己的音量，再叠人声）——
  //
  // 电平分配（关键——直接决定「配乐盖不盖旁白」）：
  // 峰值空间按 配乐 0.28 / 人声 0.55 划分（合计 0.83，留出 -1.6dBFS 余量）。
  // 旧实现把配乐峰值直接推到 0.80，人声只剩下 0.15 的余地——峰值上配乐比人声
  // 还高，无论怎么 ducking 都盖不住（实测人声仅比配乐高 1.7dB）。
  // 宣传片里旁白是主、配乐是底，峰值上就该让配乐更低。
  double music_peak = 0.0;
  for (const double sample : mix) music_peak = std::max(music_peak, std::abs(sample));
  const double music_target = kMusicCeiling * static_cast<double>(std::clamp(spec.gain, 0.0f, 1.0f));
  const double music_scale = music_peak > 1e-6 ? music_target / music_peak : 0.0;
  for (double& sample : mix) sample *= music_scale;

  // —— 旁白轨：外部 WAV（16 位 PCM）重采样到 44.1k，按 `start` 叠入 ——
  //
  // 分工写在 `director.hpp` 的 VoiceTrack 上：**TTS 由 AI 侧做**（框架不含语音合成），
  // 裁云负责把成品人声与配乐混在一起封进 mp4。
  std::vector<double> voice_mix(static_cast<std::size_t>(total), 0.0);
  // 人声能量包络（供 ducking 用）：**逐样本的 |x|>阈值 不能当 ducking 门控**——
  // 语音波形每个周期都穿零两次，逐样本判定会让压音增益在 1.0 与 0.35 之间以每秒上千次
  // 的频率跳变，形成明显的门限噪声（实测 1777 次/秒，听感就是持续沙沙声）。
  // 改为先求整流后的瞬时能量，再用一阶低通做快速起/慢速落的包络跟随，增益就变连续了。
  std::vector<double> voice_env(static_cast<std::size_t>(total), 0.0);
  // 每一条读不到的轨都要留一句话：成片会“看起来正常、只是没人声”，
  // 而这正是最需要**说出来**的那类失败（配音是外部产物，缺席是常态）。
  const auto note = [report](std::string text) {
    if (report != nullptr) report->problems.push_back(std::move(text));
  };
  if (report != nullptr) report->requested = static_cast<int>(spec.voice.size());
  for (const VoiceTrack& track : spec.voice) {
    const auto bytes = st::fs::read_bytes(track.path);
    if (!bytes) {
      note(std::format("配音轨读不到：{}", track.path));
      continue;
    }
    const std::span<const std::uint8_t> data(bytes.value());
    // 只认 16 位 PCM（TTS 输出与 ffmpeg 默认都是它）；格式不符就**跳过这一轨**
    // 而不是让整条渲染失败——画面与配乐仍然可用。
    if (data.size() < 44 || data[0] != 'R' || data[1] != 'I' || data[2] != 'F' || data[3] != 'F') {
      note(std::format("配音轨不是 RIFF/WAV：{}", track.path));
      continue;
    }
    const auto read_u16 = [&data](std::size_t at) -> std::uint16_t {
      return static_cast<std::uint16_t>(data[at] |
                                        (static_cast<std::uint16_t>(data[at + 1]) << 8U));
    };
    const auto read_u32 = [&data](std::size_t at) -> std::uint32_t {
      return static_cast<std::uint32_t>(data[at]) |
             (static_cast<std::uint32_t>(data[at + 1]) << 8U) |
             (static_cast<std::uint32_t>(data[at + 2]) << 16U) |
             (static_cast<std::uint32_t>(data[at + 3]) << 24U);
    };
    if (read_u16(20) != 1) {   // audio_format = 1（PCM）
      note(std::format("配音轨不是 PCM：{}", track.path));
      continue;
    }
    const std::uint16_t channels = read_u16(22);
    const std::uint32_t source_rate = read_u32(24);
    const std::uint16_t bits = read_u16(34);
    if (channels == 0 || source_rate == 0 || bits != 16) {
      note(std::format("配音轨需 16 位 PCM（当前 {} 位 / {} 通道）：{}", bits, channels,
                       track.path));
      continue;
    }
    // 按块链找 `data`（`fmt ` 之后不一定是 `data`）。
    std::size_t cursor = 12;
    std::size_t data_at = 0;
    std::size_t data_size = 0;
    while (cursor + 8 <= data.size()) {
      const std::uint32_t chunk = read_u32(cursor);
      const std::uint32_t size = read_u32(cursor + 4);
      if (chunk == 0x61746164U) {   // "data"
        data_at = cursor + 8;
        data_size = std::min<std::size_t>(size, data.size() - data_at);
        break;
      }
      cursor += 8 + size + (size % 2);
    }
    if (data_at == 0 || data_size == 0) {
      note(std::format("配音轨缺 data 块：{}", track.path));
      continue;
    }
    const std::size_t frames = data_size / (2U * channels);
    if (frames == 0) {
      note(std::format("配音轨没有采样：{}", track.path));
      continue;
    }
    const double ratio = static_cast<double>(source_rate) / static_cast<double>(kRate);
    const std::int64_t start_index = static_cast<std::int64_t>(track.start * kRate);
    const std::int64_t voice_length = static_cast<std::int64_t>(static_cast<double>(frames) / ratio);
    const auto sample_at = [&](std::size_t frame) -> double {
      std::int64_t sum = 0;
      for (std::uint16_t c = 0; c < channels; ++c) {
        const std::size_t offset = data_at + (frame * channels + c) * 2U;
        if (offset + 1 >= data.size()) return 0.0;
        const auto raw = static_cast<std::int16_t>(read_u16(offset));
        sum += raw;
      }
      return static_cast<double>(sum) / (static_cast<double>(channels) * 32768.0);
    };
    for (std::int64_t i = 0; i < voice_length; ++i) {
      const std::int64_t at = start_index + i;
      if (at < 0 || at >= total) continue;
      const double source_pos = static_cast<double>(i) * ratio;
      const auto frame_index = static_cast<std::size_t>(source_pos);
      if (frame_index >= frames) break;
      const double fraction = source_pos - static_cast<double>(frame_index);
      const double value = sample_at(frame_index) * (1.0 - fraction) +
                           sample_at(std::min(frame_index + 1, frames - 1)) * fraction;
      const std::size_t slot = static_cast<std::size_t>(at);
      voice_mix[slot] += value * static_cast<double>(track.gain);
      // 包络取整流后的幅度（不带 gain：门控只管"有没有人在说"）
      voice_env[slot] = std::max(voice_env[slot], std::abs(value));
    }
    if (report != nullptr) ++report->mixed;
  }

  // —— 混音 + ducking ——
  //
  // **不能用逐样本的 |x|>阈值 当门控**：语音波形每周期穿零两次，二值门控会让压音增益
  // 以每秒上千次的频率跳变——实测 1777 次/秒，听感是持续沙沙声（门限噪声）。
  // 改为整流包络 + 一阶低通跟随，增益随人声连续变化。
  constexpr double kDuckDepth = 0.65;   // 最多压下 65%（配乐 → 35%）
  const double env_attack = 1.0 - std::exp(-1.0 / (0.015 * kRate));
  const double env_release = 1.0 - std::exp(-1.0 / (0.350 * kRate));
  constexpr double kEnvThreshold = 0.02;   // 低于此幅度视为「没人在说」
  constexpr double kEnvFull = 0.12;        // 高于此幅度视为「满压」

  // 人声轨先单独限幅到剩余余量（体积小，单跑一遗）
  const double voice_room = std::max(0.0, kTotalCeiling - kMusicCeiling);
  double voice_peak = 0.0;
  for (const double sample : voice_mix) voice_peak = std::max(voice_peak, std::abs(sample));
  // 人声归一化到自己的峰值上限（不问「剩余多少」，而是给足空间）
  const double voice_scale = voice_peak > 1e-6 ? voice_room / voice_peak : 1.0;

  // 叠混：同时求出配乐侧与总输出的峰值
  double env = 0.0;
  double ducked_music_peak = 0.0;
  double total_peak = 0.0;
  for (std::size_t i = 0; i < mix.size(); ++i) {
    const double instant = voice_env[i];
    // 快起慢落：起用 15ms、落用 350ms——与广播音频的 duck 惯例一致
    const double coeff = instant > env ? env_attack : env_release;
    env += (instant - env) * coeff;
    const double control =
        std::clamp((env - kEnvThreshold) / (kEnvFull - kEnvThreshold), 0.0, 1.0);
    const double music = mix[i] * (1.0 - kDuckDepth * control);
    const double mixed = music + voice_mix[i] * voice_scale;
    mix[i] = mixed;
    ducked_music_peak = std::max(ducked_music_peak, std::abs(music));
    total_peak = std::max(total_peak, std::abs(mixed));
  }

  // 最后一道：只修「超顶」的突发峰值，不做整体抬升（抬升会把配乐一起推高，
  // 反而把人声盖住——原实现 limit=0.95 就有这个毛病）。
  const double scale_final = total_peak > kTotalCeiling ? kTotalCeiling / total_peak : 1.0;
  // 配乐侧的归一化已经在 music_target 一步做过，此处只补突变保护
  (void)ducked_music_peak;
  std::string wav;
  wav.reserve(static_cast<std::size_t>(total) * 2U + 44U);
  wav.append("RIFF");
  put_u32(wav, static_cast<std::uint32_t>(total * 2 + 36));
  wav.append("WAVEfmt ");
  put_u32(wav, 16);
  put_u16(wav, 1);                       // PCM
  put_u16(wav, 1);                       // 单声道
  put_u32(wav, static_cast<std::uint32_t>(kRate));
  put_u32(wav, static_cast<std::uint32_t>(kRate * 2));
  put_u16(wav, 2);
  put_u16(wav, 16);
  wav.append("data");
  put_u32(wav, static_cast<std::uint32_t>(total * 2));
  const std::int64_t tail_fade = static_cast<std::int64_t>(0.06 * kRate);
  for (std::int64_t i = 0; i < total; ++i) {
    double envelope_gain = 1.0;
    if (i < tail_fade) envelope_gain = static_cast<double>(i) / static_cast<double>(tail_fade);
    const std::int64_t from_end = total - 1 - i;
    if (from_end < tail_fade) {
      envelope_gain = std::min(envelope_gain, static_cast<double>(from_end) / static_cast<double>(tail_fade));
    }
    const double sample = mix[static_cast<std::size_t>(i)];
    const double value = std::clamp(sample * scale_final * envelope_gain, -1.0, 1.0);
    put_u16(wav,
            static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value * 32767.0))));
  }
  const std::vector<std::uint8_t> bytes(wav.begin(), wav.end());
  return st::fs::write_bytes(path, bytes).has_value();
}

auto Renderer::status() const -> RenderStatus {
  const std::scoped_lock lock(mutex_);
  return status_;
}

void Renderer::log_line(std::string line, const std::function<void()>& on_update) {
  {
    const std::scoped_lock lock(mutex_);
    status_.log.push_back(std::move(line));
    if (status_.log.size() > 40) status_.log.erase(status_.log.begin());
  }
  if (on_update) on_update();
}

auto Renderer::start(st::app::Tasks& tasks, const Project& project, RenderJob job,
                     const st::ui::TextPort* text_port, std::function<void()> on_update) -> bool {
  if (running_.exchange(true)) return false;
  cancel_.store(false);
  {
    const std::scoped_lock lock(mutex_);
    status_ = RenderStatus{};
    status_.running = true;
    status_.stage = "frames";
    status_.frames_total =
        static_cast<int>(std::ceil(project.duration * job.fps));
    if (job.last_frame > 0) {
      status_.frames_total = std::max(0, job.last_frame - job.first_frame);
    }
    status_.frames_dir = st::fs::join(job.output_dir, "frames");
  }
  // 快照：工作线程读自己的副本（主线程随时可能拖滑杆改片段——那是数据竞争）。
  //
  // 注意 lambda **不能是 mutable**：`Tasks::run_async` 把回调存进
  // `std::packaged_task` 并以 **const** 调用它——mutable lambda 在这里编不过
  //（实测报“passing lambda* as this discards qualifiers”）。`run` 自己拿值参数，
  // 所以捕获值无需可变。
  Project snapshot = project;
  auto [future, handle] = tasks.run_async(
      [this, snapshot, job, text_port, on_update](std::stop_token token) {
        (void)token;
        run(snapshot, job, text_port, on_update);
      });
  (void)future;
  (void)handle;
  return true;
}

void Renderer::cancel() { cancel_.store(true); }

void Renderer::run(Project project, RenderJob job, const st::ui::TextPort* text_port,
                   std::function<void()> on_update) {
  const std::int64_t started_ms = st::time::now_ms();
  const auto finish = [&](bool failed, std::string error) {
    const std::scoped_lock lock(mutex_);
    status_.running = false;
    status_.finished = !failed;
    status_.failed = failed;
    status_.stage = failed ? "failed" : "done";
    status_.error = std::move(error);
    status_.elapsed_s = static_cast<double>(st::time::now_ms() - started_ms) / 1000.0;
    running_.store(false);
    if (on_update) on_update();
  };

  if (st::fs::create_directories(job.output_dir).has_value() == false) {
    finish(true, std::format("无法创建输出目录：{}", job.output_dir));
    return;
  }
  const std::string frames_dir = st::fs::join(job.output_dir, "frames");
  if (!st::fs::create_directories(frames_dir).has_value()) {
    finish(true, std::format("无法创建帧目录：{}", frames_dir));
    return;
  }
  log_line(std::format("开始渲染 {} 帧 → {}", status().frames_total, frames_dir), on_update);

  // —— 阶段 1：帧 ——
  const int total_frames = static_cast<int>(std::ceil(project.duration * job.fps));
  const int first = std::max(0, job.first_frame);
  const int last = job.last_frame > 0 ? std::min(job.last_frame, total_frames) : total_frames;
  st::raster::Canvas canvas{job.width, job.height, 1.0f};  // —— 画面质量度量：逐镜区间先备好（按片段区间聚合，见 frame_stats.hpp）——
  // 为什么在循环前算：span 只依赖工程快照，循环里每帧都要用，算一次省 N 次。
  std::vector<StatsAccumulator::ShotSpan> shot_spans{};
  shot_spans.reserve(project.clips.size());
  for (const Clip& clip : project.clips) {
    StatsAccumulator::ShotSpan span{};
    span.clip_id = clip.id;
    span.scene_index = clip.scene_index;
    span.t_in = clip.t_in;
    span.t_out = clip.t_out;
    if (const Scene* scene = project.scene_of(clip); scene != nullptr && !scene->title.empty()) {
      span.label = scene->title;
    } else {
      span.label = std::format("片段 #{}", clip.id);
    }
    shot_spans.push_back(std::move(span));
  }
  int written = 0;
  std::int64_t render_ns_sum = 0;
  std::int64_t encode_ns_sum = 0;
  // 并行预算：每帧独立（`project` 是快照、画布是局部的），所以**每线程一块画布**
  // 就能安全并行。渲然是纯 CPU 的逐像素工作，多核近线性加速（实测 1080p
  // 从 ~110ms/帧降到 ~15ms/帧）。
  const std::size_t workers = static_cast<std::size_t>(std::clamp(
      static_cast<int>(std::thread::hardware_concurrency()), 1, 16));
  std::vector<std::unique_ptr<st::raster::Canvas>> worker_canvases{};
  worker_canvases.reserve(workers);
  // 运动模糊的采样子帧画布（每 worker 一块，与成品画布分开）：
  // 若复用同一块，采样子帧会覆盖正在累加的结果。
  std::vector<std::unique_ptr<st::raster::Canvas>> worker_scratch{};
  worker_scratch.reserve(workers);
  std::vector<std::int64_t> worker_render_ns(workers, 0);
  std::vector<std::int64_t> worker_encode_ns(workers, 0);
  std::vector<int> worker_written(workers, 0);
  // 每 worker 一份度量累加器：每帧都要写，共享加锁会成热点；几十字节的状态，
  // 每线程一份再归并的代价可忽略（与上面 render/encode 计时同一套做法）。
  std::vector<StatsAccumulator> worker_stats(workers);
  // 轨迹记录（只在 job.trajectory 时填；每 worker 一份再归并）
  struct TrajectoryRecord {
    int frame{0};
    ElementTrack track{};
  };
  std::vector<TrajectoryRecord> trajectory_records{};
  std::vector<std::vector<TrajectoryRecord>> worker_trajectory(workers);
  std::atomic<int> next_index{first};
  std::atomic<bool> write_failed{false};
  for (std::size_t i = 0; i < workers; ++i) {
    worker_canvases.push_back(std::make_unique<st::raster::Canvas>(job.width, job.height, 1.0f));
    worker_scratch.push_back(std::make_unique<st::raster::Canvas>(job.width, job.height, 1.0f));
  }
  const auto worker_body = [&](std::size_t slot) {
    st::raster::Canvas& local = *worker_canvases[slot];
    while (true) {
      if (cancel_.load() || write_failed.load()) return;
      const int index = next_index.fetch_add(1);
      if (index >= last) return;
      const std::int64_t render_start = st::time::now_ns();
      // 轨迹采集：每 worker 一份本地缓冲（不共享，无锁）
      std::vector<ElementTrack> frame_track{};
      render_frame_with_motion_blur(project, local, job, index, text_port,
                                    *worker_scratch[slot],
                                    job.trajectory ? &frame_track : nullptr);
      if (job.trajectory && !frame_track.empty()) {
        for (auto& track : frame_track) {
          worker_trajectory[slot].push_back(TrajectoryRecord{index, std::move(track)});
        }
      }
      const std::int64_t render_end = st::time::now_ns();
      const std::string path = st::fs::join(frames_dir, std::format("frame_{:05}.png", index + 1));
      if (!write_frame_png(local, path)) {
        write_failed.store(true);
        return;
      }
      const std::int64_t encode_end = st::time::now_ns();
      // 画面度量：在 PNG 编码**之后**做（不占渲染计时、不影响写盘），
      // 此时 canvas 里的像素仍是这一帧（下一个循环才覆盖）。
      worker_stats[slot].push(local, index, frame_time(index, job.fps));
      worker_render_ns[slot] += render_end - render_start;
      worker_encode_ns[slot] += encode_end - render_end;
      ++worker_written[slot];
      {
        const std::scoped_lock lock(mutex_);
        ++status_.frames_done;
      }
      if (on_update) on_update();
    }
  };
  if (workers > 1) {
    std::vector<std::jthread> pool{};
    pool.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
      pool.emplace_back([&worker_body, i] { worker_body(i); });
    }
    // `jthread` 析构会自动 join（本仓纪律：不 detach）。
  } else {
    worker_body(0);
  }
  // 归并各 worker 的轨迹（按帧号排序，保证输出稳定可 diff）
  if (job.trajectory) {
    for (auto& per_worker : worker_trajectory) {
      for (auto& record : per_worker) {
        trajectory_records.push_back(std::move(record));
      }
      per_worker.clear();
    }
    std::sort(trajectory_records.begin(), trajectory_records.end(),
              [](const TrajectoryRecord& a, const TrajectoryRecord& b) {
                return a.frame != b.frame ? a.frame < b.frame : a.track.name < b.track.name;
              });
  }
  if (cancel_.load()) {
    {
      const std::scoped_lock lock(mutex_);
      status_.cancelled = true;
      status_.running = false;
      status_.stage = "cancelled";
      status_.elapsed_s = static_cast<double>(st::time::now_ms() - started_ms) / 1000.0;
    }
    running_.store(false);
    if (on_update) on_update();
    return;
  }
  if (write_failed.load()) {
    finish(true, std::format("写帧失败（目录：{}）", frames_dir));
    return;
  }
  for (std::size_t i = 0; i < workers; ++i) {
    render_ns_sum += worker_render_ns[i];
    encode_ns_sum += worker_encode_ns[i];
    written += worker_written[i];
  }
  if (written == 0) {
    finish(true, "没有写出任何帧");
    return;
  }
  log_line(std::format("帧完成：{} 帧（每帧渲染 {:.1f}ms + PNG 编码 {:.1f}ms，{} 路并行）",
                       written, static_cast<double>(render_ns_sum) / 1.0e6 / written,
                       static_cast<double>(encode_ns_sum) / 1.0e6 / written, workers),
           on_update);

  // —— 画面质量报告（逐镜静止占比 / 过亮帧 / 平均亮度）——
  // 这一步替代"渲完再抽几帧用眼睛看"：结论是数，可复核、可对比、能指出哪一镜最静。
  {
    StatsAccumulator merged{};
    for (auto& acc : worker_stats) merged.merge(acc);
    merged.attach_spans(shot_spans);
    const double covered = static_cast<double>(written) / std::max(1.0, job.fps);
    const bool is_partial = job.first_frame > 0 || (job.last_frame > 0 && job.last_frame < total_frames);
    const QualityReport quality = merged.report(covered, is_partial);
    {
      const std::scoped_lock lock(mutex_);
      status_.quality = quality;
    }
    log_line(std::format("画面质量：{}", quality.summary), on_update);
    // 逐镜明细进日志（只记最静的三镜——全列会把日志淹掉，明细在 status_ 里）
    std::vector<ShotStats> ranked = quality.shots;
    std::sort(ranked.begin(), ranked.end(), [](const ShotStats& a, const ShotStats& b) {
      return a.still_ratio() > b.still_ratio();
    });
    int shown = 0;
    for (const ShotStats& shot : ranked) {
      if (shown >= 3 || shot.frames <= 1) break;
      log_line(std::format("  最静镜头 {:.1f}% 静止｜{}（{:.2f}–{:.2f}s，{} 帧，亮度 {:.0f}）",
                           shot.still_ratio(), shot.label, shot.t_in, shot.t_out, shot.frames,
                           shot.mean_luma),
               on_update);
      ++shown;
    }
    if (quality.still_ratio > 40.0) {
      log_line("  提示：全片静止占比超过 40% —— 「慢」通常是镜内大量时间静止，"
               "先缩短时长或落定后加持续微动，不要先减镜头数",
               on_update);
    }
    if (quality.bright_ratio > 20.0) {
      log_line("  提示：过亮帧超过 20% —— 暗场片里嵌了白底素材？需要降亮/压色/暗角融合",
               on_update);
    }
  }

  // —— 阶段 1 收尾：导出元素轨迹（供连贯性验收做“按元素追踪”）——
  //
  // 为何要写文件而不是当场判定：验收器是独立工具（`motion_qa`），
  // 它需要与帧图分开的两份证据：画面（看元素还在不在）+ 轨迹（看元素是谁、去哪了）。
  // 两者结合才能区分“同一个东西滑过去”与“同一个位置换了一个新东西”。
  if (job.trajectory && !trajectory_records.empty()) {
    const std::string path = st::fs::join(job.output_dir, "trajectory.tsv");
    std::string text = "frame\tname\tx\ty\tw\th\talpha\n";
    for (const auto& record : trajectory_records) {
      text += std::format("{}\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.3f}\n", record.frame,
                          record.track.name, static_cast<double>(record.track.x),
                          static_cast<double>(record.track.y), static_cast<double>(record.track.w),
                          static_cast<double>(record.track.h),
                          static_cast<double>(record.track.alpha));
    }
    if (const auto saved = st::fs::write_text(path, text); saved.has_value()) {
      log_line(std::format("元素轨迹：{} 条 → {}", trajectory_records.size(), path), on_update);
    }
  }

  // —— 阶段 2：配乐 ——
  std::string audio_path{};
  if (job.audio && project.script.audio.enabled) {
    {
      const std::scoped_lock lock(mutex_);
      status_.stage = "audio";
    }
    audio_path = st::fs::join(job.output_dir, "score.wav");
    const double audio_duration = static_cast<double>(last - first) / job.fps;
    VoiceReport voice{};
    if (synth_audio(project.script.audio, audio_duration, audio_path, &voice)) {
      {
        const std::scoped_lock lock(mutex_);
        status_.audio_file = audio_path;
        status_.voice_requested = voice.requested;
        status_.voice_mixed = voice.mixed;
        status_.voice_notes = voice.problems;
      }
      log_line(std::format("配乐：{}", audio_path), on_update);
      // 配音轨的状态**每条都说**：声明了却一条都没混进去，是「成片正常但没人声」
      // 这类最隐蔽失败的唯一线索。
      if (voice.requested > 0) {
        log_line(std::format("配音：{}/{} 轨已混入", voice.mixed, voice.requested), on_update);
      }
      for (const std::string& problem : voice.problems) {
        log_line(problem, on_update);
      }
    } else {
      log_line("配乐合成失败（继续出无声成片）", on_update);
    }
  }

  // —— 阶段 3：封装 ——
  if (!job.output_file.empty()) {
    {
      const std::scoped_lock lock(mutex_);
      status_.stage = "encode";
    }
    const std::string ffmpeg = find_ffmpeg(job.ffmpeg);
    if (ffmpeg.empty()) {
      const std::scoped_lock lock(mutex_);
      status_.output.clear();
      finish(true, "未找到 ffmpeg（已保留 PNG 帧序列与配乐，可自行封装）");
      return;
    }
    (void)st::fs::ensure_parent(job.output_file);
    std::vector<std::string> args = {"-y", "-loglevel", "error",
                                     "-framerate", std::format("{:.3f}", job.fps),
                                     "-start_number", std::to_string(first + 1),
                                     "-i", st::fs::join(frames_dir, "frame_%05d.png")};
    if (!audio_path.empty()) {
      args.push_back("-i");
      args.push_back(audio_path);
    }
    args.insert(args.end(), {"-c:v", "libx264", "-preset", "medium", "-crf", "18",
                             "-pix_fmt", "yuv420p"});
    if (!audio_path.empty()) {
      args.insert(args.end(), {"-c:a", "aac", "-b:a", "192k", "-shortest"});
    }
    args.push_back(job.output_file);
    const auto result = st::process::run(ffmpeg, args, {.timeout_ms = 600000});
    if (!result || result->exit_code != 0) {
      const std::string detail =
          result ? (result->stderr_text.empty() ? std::string("退出码非零") : result->stderr_text)
                 : result.error().message;
      finish(true, std::format("ffmpeg 编码失败：{}", detail));
      return;
    }
    const std::scoped_lock lock(mutex_);
    status_.output = job.output_file;
    log_line(std::format("成片：{}", job.output_file), on_update);
  }

  {
    const std::scoped_lock lock(mutex_);
    status_.frames_dir = frames_dir;
  }
  finish(false, {});
}

}  // namespace caiyun
