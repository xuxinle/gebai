#pragma once

/// **视频解码**（`st::ext::VideoDecoder`）：经外部 `ffmpeg` 子进程抽帧与读元信息。
///
/// ## 为什么走子进程 CLI，而不是链接 libavcodec
///
/// 两条路都真实可行，选前者的理由是**代价与收益的对比**（本机实测过的条件）：
///
/// | | 链接 libavcodec | 子进程 CLI |
/// |---|---|---|
/// | 依赖 | 需要头文件 + 一堆 `.a`（本机 mingw 里**没有**），且要挑 configure 组合 | 只要一个可执行文件 |
/// | 平台 | 每个目标平台各编一份（交叉编译时最痛） | 同一份代码，探测路径即可 |
/// | 版本 | 与 ffmpeg 的 ABI 绑定（大版本升级会断） | 命令行参数是稳定契约 |
/// | 启动成本 | 无 | 每帧一次进程（**这才是真代价**，见下） |
///
/// **它换来的代价是真实的**：抽一帧起一个进程（~30ms 量级）。所以本接口的定位是
/// **素材查看与预处理**（导入时读元信息、时间轴上显示缩略图、导出前抽一帧做对齐），
/// **不是**播放。逐帧实时播放要的是解码器常驻——那才是必须链接库的场景，
/// 而它需要先解决上面的依赖与交叉编译问题。**这一条如实写在这里，不含糊过去。**
///
/// ## 探测不到就如实不支持
///
/// 与 GPU 后端同一模式：`ffmpeg` 找不到时 `available()` 为假，而**不是**造一个
/// 静默失败的空实现（那会让"没有解码器"看起来像"这个文件坏了"）。
///
/// ## 与裁云（caiyun）的关系
///
/// 裁云原先的"素材"全部是程序化生成的片段，导出是 PNG 帧序列 + manifest。
/// 本接口补的是**真实视频进来**这一段（导入 mp4 → 抽帧/读时长 → 时间轴显示），
/// 而"真视频编出去"仍走帧序列（那是无损且可复现的，且不引入容器封装的复杂度）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"

namespace st::ext {

/// 视频流的基本信息（`ffprobe` 或 `ffmpeg -i` 的解析结果）。
struct VideoInfo {
  std::int64_t duration_ms{0};
  int width{0};
  int height{0};
  double fps{0.0};
  std::string codec{};       ///< 如 `h264`/`hevc`/`vp9`
  std::string container{};   ///< 如 `mov,mp4,m4a,3gp,3g2,mj2`
  /// 是否含音轨（导出时要不要保留声音——不问的话会静默丢掉）。
  bool has_audio{false};
};

/// 解码器：一次构造绑定一个视频文件，之后按时间抽帧。
///
/// **不持有 ffmpeg 进程**（每次抽帧起一个）：这看起来低效，但换来的是
/// "任何一帧失败都不影响下一帧"——而常驻进程在错误恢复上要复杂得多
///（进程死了、管道堵了、卡在某帧上）。素材查看场景下这个折中是对的。
class VideoDecoder {
 public:
  /// 探测本机是否有可用的 ffmpeg（结果缓存：探测一次就够）。
  ///
  /// 为何不是"构造时才发现"：宿主需要在**决定要不要显示相关 UI** 之前就知道
  ///（没有解码器时"导入视频"这个按钮就不该亮）。
  [[nodiscard]] static auto available() -> bool;
  /// 可执行文件路径（未探测到时为空）。
  [[nodiscard]] static auto ffmpeg_path() -> std::string;
  /// 覆盖探测结果（测试注入假路径/指定另一份 ffmpeg）。
  static void set_ffmpeg_path(std::string path);
  /// 清掉缓存（测试用；改过环境变量后重新探测）。
  static void reset_probe();

  VideoDecoder() = default;
  explicit VideoDecoder(std::string path) : path_(std::move(path)) {}

  [[nodiscard]] auto path() const -> const std::string& { return path_; }
  [[nodiscard]] auto valid() const -> bool { return !path_.empty(); }

  /// 读元信息（时长/分辨率/帧率/编码）。
  ///
  /// 失败时返回错误（**不是**一个全零的 `VideoInfo`）：全零会被上层显示成
  /// "0×0、时长 0"，看起来像"这个文件是空的"。
  [[nodiscard]] auto probe() const -> Result<VideoInfo>;

  /// 抽一帧到画布（`at_ms` 定位；`0` 是首帧）。
  ///
  /// `scale_to` 非零时按该宽度等比缩放（缩略图用——解码全尺寸再缩是浪费）。
  [[nodiscard]] auto frame_at(std::int64_t at_ms, std::optional<int> scale_to_width = std::nullopt)
      const -> Result<raster::Canvas>;

  /// 抽一帧为 PNG 文件（时间轴缩略图要落盘缓存时用）。
  [[nodiscard]] auto extract_png(std::int64_t at_ms, const std::string& output_path,
                                 std::optional<int> scale_to_width = std::nullopt) const -> Status;

  /// 错误详情（最近一次失败的 stderr 尾部；诊断用）。
  [[nodiscard]] auto last_error() const -> const std::string& { return last_error_; }

 private:
  std::string path_{};
  mutable std::string last_error_{};
};

}  // namespace st::ext
