#pragma once

/// 裁云（Nimbus）· **AI 导演脚本**——成片的唯一输入面。
///
/// ## 为什么成片由脚本描述，而不是由时间轴拖出来
///
/// 裁云的定位升级为「**AI 控制生成高质量视频 + 人工微调**」：智能体天然擅长写结构化
/// 数据（分镜 / 文案 / 节奏 / 配色），而拖拽时间轴恰恰是它的弱项。于是真值源是这份
/// JSON 脚本：
///
/// ```
/// { "name": …, "fps": 30, "size": [1920,1080], "transition": 0.35,
///   "audio": { "style": "uplift", "bpm": 110, "gain": 0.55 },
///   "scenes": [ { "kind": "title", "duration": 2.2, "title": "歌白", … } ] }
/// ```
///
/// 脚本经 `Project::load_script` 投影成时间轴片段：**每个场景 = 一个片段**。于是
/// 「AI 写脚本」与「人工在时间轴上修剪/移动/改不透明度/改速度」操作的是同一份数据，
/// 不需要两套模型（两套必然漂移）。脚本→片段是**单向投影**：应用脚本会重建片段，
/// 但人工编辑只改片段、不回写脚本（要让编辑回到脚本，用「保存脚本」）。
///
/// ## 为什么 scene 是"模板 + 参数"，而不是自由图元
///
/// 视频的"高质量"来自**运动设计与排版纪律**（节奏、层次、留白、缓动、对齐），不是
/// 来自能任意摆方块。模板把观感决策收敛在 `motion.cpp` 一处（可整体调），参数只留
/// 文案/配色/时长——智能体能控制的是内容与节奏，而不是逐像素几何。
///
/// ## 诚实边界
///
/// 渲染器是**程序化合成**（本地光栅原语 + 系统字体），不是生成式视频模型；本机没有
/// 可离线使用的视频生成模型，也不假装有。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/math/color.hpp"

namespace caiyun {

// 框架的错误/结果类型（`Result<T>` 在 `st` 命名空间下）。
using st::Result;
using st::Status;

/// 场景模板（`motion.cpp` 里各有一套渲染实现）。
enum class SceneKind : std::uint8_t {
  Title,     ///< 片头：大标题 + 副标题 + 光晕背景
  Terminal,  ///< 终端/代码：逐字符敲出的代码卡
  Triptych,  ///< 三联卡：三条卖点并排升入
  Metrics,   ///< 数字：大数字滚动 + 说明
  Outro,     ///< 片尾：标志 + slogan + 淡出
};

inline constexpr SceneKind kSceneKinds[] = {SceneKind::Title, SceneKind::Terminal,
                                            SceneKind::Triptych, SceneKind::Metrics,
                                            SceneKind::Outro};
inline constexpr std::size_t kSceneKindCount = std::size(kSceneKinds);

[[nodiscard]] auto scene_kind_name(SceneKind kind) -> std::string_view;
[[nodiscard]] auto scene_kind_from(std::string_view name) -> std::optional<SceneKind>;

/// 一个分镜。
struct Scene {
  SceneKind kind{SceneKind::Title};
  /// 该场景在成片里的时长（秒）。片段被人工修剪后以**片段**为准。
  double duration{2.0};
  std::string title{};      ///< 主文案（大标题 / 卡片组标题 / …）
  std::string subtitle{};   ///< 副文案
  /// 逐行文案（终端场景 = 代码行；三联卡 = 三张卡的标签；数字场景 = 说明）。
  std::vector<std::string> lines{};
  /// 强调色（标题下的细线、终端标题、卡片描边、数字色）。
  st::math::Color accent{110, 168, 255, 255};
  st::math::Color bg_top{8, 12, 24, 255};
  st::math::Color bg_bottom{18, 26, 48, 255};
  /// 可选：三段数字（`Metrics` 用）。空则从 `lines` 里按 `标签|数值` 解析。
  std::vector<double> values{};
};

/// 旁白/人声轨：一段外部 WAV 在成片里的位置。
///
/// 为何不自己做 TTS：霜天是原生 UI 框架，**不含语音合成引擎**（那是平台能力，
/// 与窗口后端一样属 `platform_*` 层之外的宿主职责）。所以分工是：**AI 用它的
/// TTS 能力生成人声 WAV，再写进脚本**，裁云负责把它与配乐混在一起封进成片。
struct VoiceTrack {
  std::string path{};   ///< 外部 WAV（PCM 16 位；16k/44.1k 均可，会自动重采样）
  double start{0.0};    ///< 在成片里的起点（秒）
  float gain{1.0f};     ///< 线性增益
};

/// 配乐规格（内置合成器；不依赖任何音频素材与解码器）。
struct AudioSpec {
  bool enabled{true};
  /// 曲风档：`uplift`（明亮上行）/ `calm`（稀疏铺底）。两档都是合成器现场生成，
  /// 差别在音色与节奏密度——不是从素材库里挑的。
  std::string style{"uplift"};
  int bpm{110};
  /// 线性增益（0~1）；成片里配乐不该压过画面。
  float gain{0.55f};
  /// 旁白轨（可多段）。**配乐会自动让路**：有人声的时段把配乐压到 35%，
  /// 否则“BGM 盖住旁白”是宣传片最常见的听感缺陷。
  std::vector<VoiceTrack> voice{};
};

/// 一份导演脚本（成片真值源）。
struct Script {
  std::string name{"未命名工程"};
  double fps{30.0};
  int width{1920};
  int height{1080};
  /// 场景之间的交叉淡入时长（秒）：由片段边界推断"上一场景"，不引入额外状态。
  double transition{0.35};
  /// 全片淡入/淡出（秒）：黑场包边，避免第一帧与最后一帧"硬切"。
  double fade_in{0.30};
  double fade_out{0.50};
  AudioSpec audio{};
  std::vector<Scene> scenes{};

  [[nodiscard]] auto total_duration() const noexcept -> double;
};

/// 内置脚本：**歌白 10 秒宣传片**（`--script` 不传时的默认工程，也是 AI 的起点范本）。
[[nodiscard]] auto builtin_gebai_promo() -> Script;

/// 解析脚本文本（错误：`Parse`，消息带**行号无关**的字段路径，便于脚本作者定位）。
[[nodiscard]] auto parse_script(std::string_view json) -> Result<Script>;
/// 序列化（美化 2 空格；键序固定，便于人工 diff）。
[[nodiscard]] auto script_to_json(const Script& script) -> std::string;

/// `#rrggbb` / `#rrggbbaa` → Color（解析失败返回 `fallback`，不崩）。
[[nodiscard]] auto parse_color(std::string_view text, st::math::Color fallback) -> st::math::Color;
/// Color → `#rrggbb`。
[[nodiscard]] auto color_to_hex(st::math::Color color) -> std::string;

}  // namespace caiyun
