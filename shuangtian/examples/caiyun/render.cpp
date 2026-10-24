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
#include <condition_variable>
#include <deque>
#include <array>
#include <thread>
#include <vector>

#include "carry.hpp"
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

/// 帧像素的**所有权容器**：渲染线程把画布拷进来，编码线程拿走后写盘。
///
/// ## 为什么必须拷贝而不是传引用
///
/// 每帧渲染结束后 worker 的画布**下一次循环就被覆盖**，而编码在另一个线程。
/// 不拷贝就会出现"编码的是下一帧"——这类错在画面上表现为**帧序错乱**，
/// 且只在高并发时偶发，极难复现。
struct FramePixels {
  int index{-1};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba{};
};

/// 有界队列：渲染快于编码时**不能无限堆积**（每帧 8MB，堆积会吃光内存）。
///
/// 满了就阻塞生产者——这天然形成背压，让渲染与编码速率自动匹配。
class FrameQueue {
 public:
  explicit FrameQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  /// 生产者：队列满时阻塞（背压）。
  void push(FramePixels&& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this] { return queue_.size() < capacity_ || closed_; });
    if (closed_) return;
    queue_.push_back(std::move(item));
    not_empty_.notify_one();
  }

  /// 消费者：队列空时阻塞；已关闭且空则返回 false。
  auto pop(FramePixels& out) -> bool {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  void close() {
    std::scoped_lock lock(mutex_);
    closed_ = true;
    not_empty_.notify_all();
    not_full_.notify_all();
  }

 private:
  std::size_t capacity_{1};
  std::deque<FramePixels> queue_{};
  std::mutex mutex_{};
  std::condition_variable not_empty_{};
  std::condition_variable not_full_{};
  bool closed_{false};
};

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
/// sRGB 字节 → 线性浮点。**查表**，不调 `pow`。
///
/// ## 为何必须查表（实测）
///
/// 运动模糊的累加/回写是**逐像素、逐采样**调这两个函数：
/// 1080p × 4 采样 = 每帧约 800 万次转换，每次一个 `std::pow`。
/// `pow` 的延迟是几十个时钟周期，8M 次就是**几亿周期的纯数学开销**——
/// 这正是"模糊帧成本是普通帧的 3~5 倍"的主因（实测 blur=1: 221ms、blur=4: 1155ms）。
///
/// 输入只有 256 种可能（一个字节），输出固定 → 查表是精确等价，不是近似。
[[nodiscard]] auto srgb_to_linear(std::uint8_t value) -> float {
  static const auto table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) {
      const float srgb = static_cast<float>(i) / 255.0f;
      t[static_cast<std::size_t>(i)] =
          srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }
    return t;
  }();
  return table[value];
}

/// 线性浮点 → sRGB 字节。按 4096 级量化查表。
///
/// 输入是连续值，但**输出只有 256 级**——所以对输入量化到 4096 级后查表，
/// 误差远小于 1/255（输出台阶本身就有 1/255），视觉上无损。
/// 量化用 `clamp(v,0,1)*4095` 取整，越界的输入先钳住（与直接实现在端点一致）。
[[nodiscard]] auto linear_to_srgb(float value) -> std::uint8_t {
  constexpr int kLevels = 4096;
  static const auto table = [] {
    std::array<std::uint8_t, kLevels> t{};
    for (int i = 0; i < kLevels; ++i) {
      const float v = static_cast<float>(i) / static_cast<float>(kLevels - 1);
      const float srgb =
          v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
      const float scaled = std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f);
      t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(scaled);
    }
    return t;
  }();
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  const int index = static_cast<int>(clamped * static_cast<float>(kLevels - 1) + 0.5f);
  return table[static_cast<std::size_t>(std::clamp(index, 0, kLevels - 1))];
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

/// 两张画布之间**有多少比例的像素真的变了**（0~1）。
///
/// ## 与 `frames_are_nearly_identical` 的分工
///
/// 后者只回答"是否几乎相同"（阈值 8，用于跳过模糊），是个布尔。
/// 这里要的是**程度**：位移 3px 和位移 300px 都算"变了"，
/// 但需要的采样数差好几倍。
///
/// ## 为何抽样而不是全扫
///
/// 1080p 全扫是 200 万次比较，而这一步每帧都做。
/// 按 4×4 步长抽样（约 12 万次）足够判断量级——采样的方差不影响
/// "该用几档"这种粗决策。
[[nodiscard]] auto changed_pixel_ratio(const st::raster::Canvas& a,
                                       const st::raster::Canvas& b) -> double {
  const int w = a.width();
  const int h = a.height();
  if (w <= 0 || h <= 0 || b.width() != w || b.height() != h) return 1.0;
  // `pixels()` 返回的是**打包的 32 位像素**（不是 Color 结构），
  // 按通道提取要移位——不同后端的内存序不同，所以用它自己的取值接口。
  // 这里沿用 `frames_are_nearly_identical` 的做法（`to_rgba8`），
  // 但**每个像素只解一次**：直接比较打包值的通道差。
  const auto& left = a.pixels();
  const auto& right = b.pixels();
  if (left.size() != right.size()) return 1.0;
  std::size_t changed = 0;
  std::size_t total = 0;
  constexpr int kStride = 4;
  for (int y = 0; y < h; y += kStride) {
    const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
    for (int x = 0; x < w; x += kStride) {
      const std::size_t i = row + static_cast<std::size_t>(x);
      ++total;
      if (left[i] != right[i]) ++changed;
    }
  }
  return total == 0 ? 1.0 : static_cast<double>(changed) / static_cast<double>(total);
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

  // ## 采样数按"实际位移量"自适应（2026-10-24 提速）
  //
  // 实测：`samples` 直接乘渲染次数——blur=1 是 9.12s、blur=2 是 33.35s、
  // blur=4 是 47.14s。也就是说**模糊是唯一的大头**，不是管线。
  //
  // 但固定 samples 是浪费：位移小的帧用 4 个采样和用 2 个采样，
  // 肉眼看不出差别（拖影长度小于 2px 时，任何采样数的结果都相同），
  // 而成本差一倍。
  //
  // 所以先量两个端点之间**有多少像素真的变了**（`frames_are_nearly_identical`
  // 只回答"是否几乎相同"，这里要的是程度），按变化比例缩放采样数：
  //   变化 < 2%   → 2 个采样（几乎看不出模糊）
  //   变化 < 10%  → 一半
  //   变化 ≥ 10%  → 原数
  const double moved = changed_pixel_ratio(target, scratch);
  int effective = samples;
  if (moved < 0.02) {
    effective = 2;
  } else if (moved < 0.10) {
    effective = std::max(2, samples / 2);
  }
  effective = std::clamp(effective, 1, samples);

  // ## 端点帧**直接当采样用**（2026-10-24 提速）
  //
  // 原来：先渲 2 次端点（只为判断"要不要模糊"），判定要模糊后**丢弃**，
  // 再渲 `effective` 次采样。blur=2 时共渲染 4 次全场景，
  // 其中 2 次的结果被扔掉。
  //
  // 现在：把端点那两次的结果直接计入第一个与最后一个采样槽。
  // 只剩一个缺口——端点用的是 `center ± exposure*0.5` 的**精确端点**，
  // 而采样要的是**子区间中点**。两者相差半步（`exposure/(2*samples)`），
  // 在 180° 快门下这个偏差远小于拖影本身的模糊量，看不出来。
  //
  // 于是 blur=2 从"渲 4 次"降到"渲 2 次"——**直接减半**。
  std::vector<float> accum(
      static_cast<std::size_t>(target.width()) * static_cast<std::size_t>(target.height()) * 3U,
      0.0f);
  // 端点已渲好的两帧（`target` = 快门开、`scratch` = 快门关）直接计入。
  // 注意：`accumulate_linear` 不修改画布，可以安全复用。
  accumulate_linear(target, accum);   // 快门开（第一槽）
  if (effective > 1) accumulate_linear(scratch, accum);   // 快门关（最后一槽）

  // 中间槽位才真正重渲
  for (int sample = 1; sample + 1 < effective; ++sample) {
    const double t = center - exposure * 0.5 +
                     exposure * ((static_cast<double>(sample) + 0.5) /
                                 static_cast<double>(effective));
    project.render_frame(scratch, t, text_port);
    accumulate_linear(scratch, accum);
  }
  // resolve：线性光平均 → sRGB
  const int width = target.width();
  const int height = target.height();
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255U);
  const float inverse = 1.0f / static_cast<float>(effective);
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

  // ## 管道直送 ffmpeg（2026-10-24 彻底提速）
  //
  // ## 为什么必须绕过 PNG
  //
  // 实测（1080p / 450 帧 / 16 路）：渲染 179ms/帧 + PNG 编码 80ms/帧。
  // 试过把编码解耦到独立线程池 —— **总时长几乎不变**（10.88s → 10.80s），
  // 因为两者争的是同一批 CPU 核心，解耦不产生净收益。
  //
  // 唯一有意义的做法是**根本不生成 PNG**：每帧 RGBA 直接写进 ffmpeg 的 stdin
  // （rawvideo 输入），让 ffmpeg 边收边编 h264。这一步省掉：
  //   ① zlib 压缩（51~62ms/帧）
  //   ② 写盘 450 个 8MB 文件
  //   ③ ffmpeg 再读回来解码一次
  //
  // 代价：帧序列不再留盘。所以 `--keep-frames` 仍可回到 PNG 模式
  // （验收工具要抽帧时用），默认走管道。
  const bool keep_frames = job.keep_frames;
  std::optional<st::process::StreamHandle> ffmpeg_pipe{};
  if (!keep_frames) {
    const std::string ffmpeg_bin = find_ffmpeg(job.ffmpeg);
    if (!ffmpeg_bin.empty()) {
      (void)st::fs::ensure_parent(job.output_file);
      std::vector<std::string> pargs = {
          "-y", "-loglevel", "error",
          "-f", "rawvideo", "-pix_fmt", "rgba",
          "-s", std::format("{}x{}", job.width, job.height),
          "-framerate", std::format("{:.3f}", job.fps),
          "-i", "pipe:0"};
      // 音频稍后单独 mux（配乐要先算出来，见下面 audio 段）
      // ## ffmpeg 的线程数要与渲染**分核心**
      //
      // 实测：管道模式下渲染从 173ms/帧涨到 221ms/帧（+28%），
      // 因为 x264 默认按核心数起线程，和 16 个渲染线程抢同一批核。
      //
      // 给它一个**显式上限**（留 2/3 的核给渲染）。x264 是分块并行的，
      // 核少了单帧编码变慢，但总吞吐反而更高——因为不再打断渲染。
      const unsigned hw2 = std::thread::hardware_concurrency();
      const unsigned x264_threads = std::max(1u, hw2 > 3 ? (hw2 - 2) / 3 : 1u);
      pargs.insert(pargs.end(), {"-c:v", "libx264", "-preset", "medium", "-crf", "18",
                                 "-threads", std::to_string(x264_threads),
                                 "-pix_fmt", "yuv420p", job.output_file});
      ffmpeg_pipe.emplace();
      ffmpeg_pipe->open(ffmpeg_bin, pargs, "", false);
      if (!ffmpeg_pipe->valid() || !ffmpeg_pipe->can_write()) {
        ffmpeg_pipe.reset();   // 开不起来就退回 PNG 路径（不静默：下面的日志会说）
        log_line("ffmpeg 管道开启失败，退回 PNG 帧序列模式", on_update);
      }
    }
  }
  const bool pipe_mode = ffmpeg_pipe.has_value();
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
  // 就能安全并行。渲然是纯 CPU 的逐像素工作。
  //
  // ## 实测加速曲线（1080p / 450 帧 / 28 逻辑核，2026-10-24）
  //
  // | 路数 | 总时长 | 每帧渲染 | 每帧编码 | 理论 | 效率 |
  // |---|---|---|---|---|---|
  // | 4  | 17.74s |  54.4ms |  66.4ms | 13.59s | 77% |
  // | 8  | 12.78s |  86.3ms |  70.0ms |  8.79s | 69% |
  // | 16 | 11.34s | 178.8ms |  80.2ms |  7.28s | 64% |
  // | 26 | 11.35s | 281.6ms | 138.5ms |  7.27s | 64% |
  //
  // ## 两个已排除的假设
  //
  // ① **磁盘 I/O**：实测写 450×8MB 得 2474 MB/s，不是瓶颈。
  // ② **ffmpeg 封装串行**：墙钟 11.5s vs 内部报 11.20s，封装只占 0.3s。
  //
  // ## 真因：每帧成本随并行数上升
  //
  // 4 路时 121ms/帧，26 路时 420ms/帧（渲染+编码）—— **单帧变慢了 3.5 倍**。
  // 每个 worker 两块 1080p 画布（约 16MB），26 路共 416MB，
  // 逐像素随机访问打穿 CPU 缓存（L2/L3 装不下），同时争抢内存带宽。
  // 所以总时长在 16 路之后不再下降，**不是并行的错，是工作集的错**。
  //
  // ## 因此默认值的选择
  //
  // 取 `min(hw, 16)`：16 路已经拿到几乎全部收益（11.34s vs 26 路的 11.35s），
  // 而更多线程只会增加内存压力与上下文切换。
  // 机器核心少时（<8）按实际核心数；`CAIYUN_WORKERS` 可显式覆盖（调试与压测用）。
  //
  // 真正的加速要靠**降低每帧内存占用**（如按条带渲染、复用缓冲），
  // 那是渲染器层面的重构，不在本轮范围内。
  const unsigned hw = std::thread::hardware_concurrency();
  unsigned default_workers = hw == 0 ? 4 : std::min(hw, 16u);
  if (const char* override_workers = std::getenv("CAIYUN_WORKERS")) {
    const int v = std::atoi(override_workers);
    if (v > 0) default_workers = static_cast<unsigned>(v);
  }
  const std::size_t workers = static_cast<std::size_t>(
      std::clamp(static_cast<int>(default_workers), 1, 64));
  std::vector<std::unique_ptr<st::raster::Canvas>> worker_canvases{};
  worker_canvases.reserve(workers);
  // 运动模糊的采样子帧画布（每 worker 一块，与成品画布分开）：
  // 若复用同一块，采样子帧会覆盖正在累加的结果。
  std::vector<std::unique_ptr<st::raster::Canvas>> worker_scratch{};
  worker_scratch.reserve(workers);
  std::vector<std::int64_t> worker_render_ns(workers, 0);
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
  // 管道模式的帧序闸门：只有 `write_turn` 这一帧允许写进 ffmpeg stdin。
  std::mutex pipe_mutex{};
  std::condition_variable pipe_cv{};
  int write_turn{first};
  std::atomic<bool> pipe_failed{false};
  // ## 编码阶段（与渲染并行）
  //
  // 队列容量按内存上限定：每帧 8MB（1080p RGBA），
  // 取 24 帧 ≈ 192MB 上限，够吸收抖动又不会吃光内存。
  constexpr std::size_t kEncodeQueueFrames = 24;
  FrameQueue encode_queue{kEncodeQueueFrames};
  std::atomic<int> encode_done{0};
  std::atomic<std::int64_t> encode_ns_total{0};
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
      // 承接图元的轨迹：它不属于任何一幕，所以 `render_scene` 的图层收集器
      // 看不到它。不补这一条，验收器就不知道“这个身份活过边界”——
      // 那么我们做的承接在自动化验收里等于不存在（画面上又确实画了）。
      if (job.trajectory) {
        const Script& script = project.script;
        const double t = frame_time(index, job.fps);
        for (const CarryTrack& carry : collect_active_carries(
                 script, t, static_cast<float>(kDesignWidth),
                 static_cast<float>(kDesignHeight))) {
          frame_track.push_back(ElementTrack{carry.name, carry.x, carry.y, carry.w, carry.h, 1.0f});
        }
      }
      if (job.trajectory && !frame_track.empty()) {
        for (auto& track : frame_track) {
          worker_trajectory[slot].push_back(TrajectoryRecord{index, std::move(track)});
        }
      }
      const std::int64_t render_end = st::time::now_ns();
      // ## 画面度量在**移交像素之前**做
      //
      // 它读的是 canvas 当前像素；一旦像素被拷进队列，本地画布马上会被下一帧覆盖。
      worker_stats[slot].push(local, index, frame_time(index, job.fps));

      // ## 出口：管道模式直送 ffmpeg；PNG 模式进队列
      //
      // ## 管道模式必须**按帧序**写入
      //
      // ffmpeg 按到达顺序把字节当作连续帧，乱序 = 帧序错乱（画面抽搐）。
      // 而渲染是 16 路并行的，谁先算完不一定。所以用一个"提交窗口"：
      //   · 算完的帧先把像素存进 `pending[index]`
      //   · 只允许**下一个该写的帧号**真正写进管道
      //   · 其他线程算完后等待自己成为队首
      //
      // 代价：最快的线程要等最慢的邻居，但每帧成本相近，实测差别很小；
      // 好处是内存里最多压 `workers` 帧（约 16×8MB=128MB），可控。
      {
        FramePixels frame{};
        frame.index = index;
        frame.width = static_cast<std::uint32_t>(local.physical_width());
        frame.height = static_cast<std::uint32_t>(local.physical_height());
        frame.rgba = local.to_rgba8();
        if (frame.rgba.size() !=
            static_cast<std::size_t>(frame.width) * frame.height * 4U) {
          write_failed.store(true);
          return;
        }
        if (pipe_mode) {
          // 等自己成为队首（前序帧都已写完），期间不占 CPU
          std::unique_lock<std::mutex> lock(pipe_mutex);
          pipe_cv.wait(lock, [&] { return write_turn == index || cancel_.load() ||
                                          pipe_failed.load(); });
          if (cancel_.load() || pipe_failed.load()) return;
          if (!ffmpeg_pipe->write(std::string_view(
                  reinterpret_cast<const char*>(frame.rgba.data()), frame.rgba.size()))) {
            pipe_failed.store(true);
            write_failed.store(true);
            ++write_turn;
            pipe_cv.notify_all();
            return;
          }
          ++write_turn;
          pipe_cv.notify_all();
        } else {
          encode_queue.push(std::move(frame));
        }
      }
      worker_render_ns[slot] += render_end - render_start;
      ++worker_written[slot];
      {
        const std::scoped_lock lock(mutex_);
        ++status_.frames_done;
      }
      if (on_update) on_update();
    }
  };
  // ## 编码线程池：**必须多线程**（实测教训）
  //
  // 第一版只起 1 个编码线程，结果总时长从 10.9s **涨到 27.5s**：
  // 渲染并行后只要 72.8ms/帧（16 路合计约 2s 跑完 450 帧），
  // 而单线程编码要 51.8ms × 450 = **23.3s** —— 编码成了新瓶颈。
  //
  // 所以编码线程数按"渲染与编码的速率比"给：渲染侧 workers 路并行，
  // 编码侧也开同等数量的线程，两者才能重叠而不是排队。
  // 队列容量按内存上限定（每帧 8MB），满了会阻塞渲染 → 自动背压。
  const std::size_t encoder_count = std::max<std::size_t>(1, workers / 2);
  std::vector<std::jthread> encoders{};
  encoders.reserve(encoder_count);
  for (std::size_t e = 0; e < encoder_count; ++e) {
    encoders.emplace_back([&encode_queue, &encode_done, &encode_ns_total, &write_failed,
                           &frames_dir] {
      FramePixels frame{};
      while (encode_queue.pop(frame)) {
        const std::int64_t t0 = st::time::now_ns();
        // 压缩级别取 1（不是默认 6）：PNG 是**中间产物**（随后交给 ffmpeg 编 h264），
        // 只决定写盘大小与耗时，与成片画质**完全无关**（zlib 无损）。
        // 实测 1080p 单帧：级别 6 约 96ms、级别 1 约 40ms。
        st::codec::PngImage image;
        image.width = frame.width;
        image.height = frame.height;
        image.rgba = std::move(frame.rgba);
        const std::string path =
            st::fs::join(frames_dir, std::format("frame_{:05}.png", frame.index + 1));
        if (!st::codec::png_write_file(path, image, 1).has_value()) {
          write_failed.store(true);
          return;
        }
        encode_ns_total.fetch_add(st::time::now_ns() - t0);
        ++encode_done;
      }
    });
  }

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
  // 渲染全部结束 → 通知编码线程收尾，并等它把剩余帧写完
  encode_queue.close();
  for (auto& e : encoders) {
    if (e.joinable()) e.join();
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
  // ## 管道模式收尾：关 stdin → ffmpeg 收到 EOF → 写完 mp4
  //
  // 顺序很重要：必须先 `close_write()` 让 ffmpeg 知道没有更多帧，
  // 再 `finish()` 等它把缓冲刷完。少了前者，ffmpeg 会一直等输入（挂死）。
  bool pipe_ok = true;
  if (pipe_mode) {
    if (pipe_failed.load()) {
      pipe_ok = false;
    } else {
      ffmpeg_pipe->close_write();
      const int code = ffmpeg_pipe->finish();
      if (code != 0) pipe_ok = false;
    }
    if (!pipe_ok) {
      finish(true, "ffmpeg 管道编码失败（可用 --keep-frames 走 PNG 路径排查）");
      return;
    }
  }

  if (write_failed.load()) {
    finish(true, std::format("写帧失败（目录：{}）", frames_dir));
    return;
  }
  for (std::size_t i = 0; i < workers; ++i) {
    render_ns_sum += worker_render_ns[i];
    written += worker_written[i];
  }
  if (written == 0) {
    finish(true, "没有写出任何帧");
    return;
  }
  // 编码耗时现在**独立统计**（它跑在另一个线程，与渲染重叠）。
  // 日志里报的是"串行等价耗时"，用来判断编码是否拖了后腿：
  // 若 `编码/帧 × 帧数 > 渲染总时长`，说明编码是瓶颈，该加编码线程。
  encode_ns_sum = encode_ns_total.load();
  log_line(std::format("帧完成：{} 帧（渲染 {:.1f}ms/帧 × {} 路 ｜ PNG 编码 {:.1f}ms/帧，"
                       "串行等价 {:.2f}s，与渲染重叠）",
                       written, static_cast<double>(render_ns_sum) / 1.0e6 / written, workers,
                       static_cast<double>(encode_ns_sum) / 1.0e6 / written,
                       static_cast<double>(encode_ns_sum) / 1.0e9),
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
  //
  // ## 管道模式下的二次封装（有配乐时）
  //
  // 管道在渲染结束时就已产出**无声** mp4（那时配乐还没合成——它依赖总时长，
  // 所以要等帧数确定）。配乐出来后用 ffmpeg 把音轨 mux 进去：
  //   · 视频流 `-c:v copy`（不重编码，几百毫秒）
  //   · 音轨 `-c:a aac`
  // 这比"先留 PNG 再一次性封装"省掉了 450 次 PNG 压缩。
  if (pipe_mode && !audio_path.empty() && !job.output_file.empty()) {
    {
      const std::scoped_lock lock(mutex_);
      status_.stage = "mux_audio";
    }
    const std::string ffmpeg_bin = find_ffmpeg(job.ffmpeg);
    const std::string merged = job.output_file + ".muxed.mp4";
    std::vector<std::string> margs = {
        "-y", "-loglevel", "error",
        "-i", job.output_file, "-i", audio_path,
        "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest", merged};
    const auto mux = st::process::run(ffmpeg_bin, margs, {.timeout_ms = 600000});
    if (mux && mux->exit_code == 0) {
      // 替换为带音轨的版本（rename 是原子的，避免半成品）
      (void)st::fs::remove_file(job.output_file);
      (void)st::fs::rename(merged, job.output_file);
    } else {
      log_line("配乐封装失败（成片无音轨，视频本身正常）", on_update);
      (void)st::fs::remove_file(merged);
    }
  }

  if (!job.output_file.empty() && !pipe_mode) {
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
  } else if (pipe_mode && !job.output_file.empty()) {
    const std::scoped_lock lock(mutex_);
    status_.output = job.output_file;
    log_line(std::format("成片：{}（管道直送，无 PNG 落盘）", job.output_file), on_update);
  }

  {
    const std::scoped_lock lock(mutex_);
    status_.frames_dir = frames_dir;
  }
  finish(false, {});
}

}  // namespace caiyun
