#pragma once

/// 裁云（Nimbus）· **成片渲染管线**：帧序列 → 配乐波形 → 成片容器。
///
/// ## 为什么是三个阶段，而不是"直接写一个 mp4"
///
/// 霜天没有视频编码器（`DESIGN`/BACKLOG 里如实记着缺口）。硬塞一个编码器进框架
/// 不是本次任务要解决的事，而"成片"是用户的真实诉求。因此：
///
/// 1. **帧**：`Project::render_frame` 逐帧渲染 → PNG（纯本地光栅，无外部依赖，**始终可用**）；
/// 2. **声**：内置合成器按 BPM 生成配乐 → WAV（纯本地，无素材库、无解码器）；
/// 3. **封装**：调外部 `ffmpeg` 把两者合成为 mp4（`st::process::run`）。
///
/// 前两步的成功不依赖第三步：找不到 ffmpeg 时**如实回报**并保留 PNG 序列 + WAV，
/// 用户可自行封装——不静默失败、不冒充"已成片"。
///
/// ## 线程与进度
///
/// 渲染是**工作线程**（`Application::tasks().run_async`）里跑的：1080p 逐帧渲染是
/// 秒级到分钟级的长任务，塞进主线程会把界面冻成一张静止画（连"中止"都点不到）。
/// 进度/日志经 `post_ui` 回主线程写 UI——工作线程**不碰 UI 树**（CONVENTIONS §11）。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "director.hpp"
#include "frame_stats.hpp"
#include "project.hpp"
#include "st/app/async.hpp"
#include "st/ui/text_port.hpp"

namespace caiyun {

/// 配音轨的处理结果：**读不到也要能看见**。
///
/// 配音文件是外部产物（TTS / 录音），路径错、格式不对、文件缺席都是常态。早先这些
/// 情况一律静默 `continue`，成片“看起来正常、就是没人声”，而日志只有一行“配乐：…”。
/// 失败原因必须能被调用方看见（日志、状态栏、e2e 断言）。
struct VoiceReport {
  int requested{0};                     ///< 脚本里声明的配音轨数
  int mixed{0};                         ///< 真的混进成片的轨数
  std::vector<std::string> problems{};  ///< 每条失败一句可读说明
};

/// 一次渲染作业的规格。
struct RenderJob {
  std::string output_dir{};
  /// 输出文件路径（`.../promo.mp4`）；空 = 只出帧序列与配乐。
  std::string output_file{};
  /// 成片分辨率（与工程脚本里的 `size` 独立：同一份工程可出 720p 预览与 1080p 成片）。
  int width{1920};
  int height{1080};
  double fps{30.0};
  /// 抽帧预览：`stride > 1` 时每隔 `stride` 帧存一张（只用于快速轮检）。
  int stride{1};
  /// 只渲染这段帧区间 `[first, last)`；`last <= 0` = 到结尾（抽帧预览用）。
  int first_frame{0};
  int last_frame{0};
  /// 是否合成配乐（关闭只出无声 mp4）。
  bool audio{true};
  /// ffmpeg 可执行文件路径（空 = 用 PATH 里的）。
  std::string ffmpeg{};
};

/// 作业状态（UI 与 e2e 都读它）。
struct RenderStatus {
  bool running{false};
  bool finished{false};
  bool failed{false};
  bool cancelled{false};
  int frames_total{0};
  int frames_done{0};
  /// 阶段：`frames` / `audio` / `encode` / `done`。
  std::string stage{"idle"};
  /// 成片路径（成功时有值）。
  std::string output{};
  /// 最后一帧 PNG 目录（成功/部分成功都有值）。
  std::string frames_dir{};
  /// 配乐路径（`audio == false` 或失败时为空）。
  std::string audio_file{};
  /// 真正混进成片的配音轨数；与 `voice_requested` 不等就是有轨没读到（见 `voice_notes`）。
  int voice_mixed{0};
  int voice_requested{0};
  /// 配音轨的失败原因（路径不存在/格式不支持/音频头异常）；每条一句可读说明。
  std::vector<std::string> voice_notes{};
  /// 失败原因或编码器缺失的说明（**如实**：可读的失败比"静默出半成品"强）。
  std::string error{};
  /// 最近若干条日志（UI 面板里显示，e2e 用来断言阶段推进）。
  std::vector<std::string> log{};
  /// **画面质量度量**（渲完帧阶段后填）。逐镜静止占比 / 过亮帧 / 平均亮度。
  ///
  /// 为什么放进状态而不是只写日志：日志是给人扫的，`get` 是给**智能体与 e2e**读的。
  /// 判据要能进断言、能前后对比，就不能只存在于一行文本里。
  QualityReport quality{};
  double elapsed_s{0.0};

  [[nodiscard]] auto progress() const -> float {
    if (frames_total <= 0) return 0.0f;
    const float ratio = static_cast<float>(frames_done) / static_cast<float>(frames_total);
    return ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
  }
};

/// 渲染器：持工程快照 + 状态；一次只跑一个作业。
///
/// 为什么持**快照**而不是 `Project*`：渲染跑在工作线程上，而主线程随时可能拖动滑杆
/// 改片段（那是数据竞争）。快照（`Project` 的拷贝）几 KB，换掉一整类难查的崩溃。
class Renderer {
 public:
  Renderer() = default;
  Renderer(const Renderer&) = delete;
  auto operator=(const Renderer&) -> Renderer& = delete;

  /// 启动一次渲染（已在跑则拒绝并返回 false）。`text_port` 必须存活到作业结束
  /// （调用方传 `app.root().text_port()`——它随 `Application` 存活）。
  ///
  /// `tasks` 是应用级异步任务系统（`Application::tasks()`）：渲染是独立线程的长任务，
  /// 而主线程只负责渲染界面（CONVENTIONS §11）。
  auto start(st::app::Tasks& tasks, const Project& project, RenderJob job,
             const st::ui::TextPort* text_port, std::function<void()> on_update) -> bool;

  /// 请求中止（协作式：当前帧画完就停）。
  void cancel();
  [[nodiscard]] auto status() const -> RenderStatus;
  [[nodiscard]] auto running() const -> bool { return running_.load(); }

  /// 探测 ffmpeg（找不到返回空）。
  [[nodiscard]] static auto find_ffmpeg(const std::string& override_path) -> std::string;

  /// 生成配乐 WAV + 混入配音轨（独立于渲染线程；返回写入与否）。
  ///
  /// `report` 非空时带回「配音轨读到几条 / 哪几条为什么没读到」—— 静默不出声是
  /// 宣传片最隐蔽的失败，必须能说出来。
  [[nodiscard]] static auto synth_audio(const AudioSpec& spec, double duration_s,
                                        const std::string& path, VoiceReport* report = nullptr)
      -> bool;

 private:
  void run(Project project, RenderJob job, const st::ui::TextPort* text_port,
           std::function<void()> on_update);
  void log_line(std::string line, const std::function<void()>& on_update);

  mutable std::mutex mutex_{};
  RenderStatus status_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
};

}  // namespace caiyun
