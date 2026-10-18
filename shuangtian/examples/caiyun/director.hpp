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

  // —— 素材（2026-10-17）：让外部图片进入画面 ——
  //
  // 在此之前画面只有几何与文字，做不出真正的宣传片（产品图/品牌图/生成的背景进不来）。
  // `image` 支持三种写法：绝对路径 / 相对脚本目录 / 素材库内名称（见 `assets_lib.hpp`）。
  // 缺失时渲染侧画"素材缺失"占位框——比静默跳过好：静默会让"图没进来"看起来像设计。
  std::string image{};        ///< 背景图（铺满全画面，cover）
  std::string image_card{};   ///< 卡片/图文场景里的插图（嵌在卡片内，contain）
  /// 背景图上的**压暗强度**（0~1）：有文字的场合需要压暗才能保证对比度。
  /// 默认 0.45——不压暗时亮背景上的白字几乎不可读（实测）。
  float image_dim{0.45f};
  /// 背景图的**轻微推拉**幅度（0~0.25）：静止背景在视频里显得廉价，
  /// 缓慢 zoom 让它"活"起来。0 = 关闭。
  float image_ken_burns{0.06f};

  // —— 影像特效（2026-10-17）：发布会/预告片质感的来源 ——
  //
  // 全部默认 0（关闭且零开销）：老脚本行为完全不变，新脚本按需开。
  // 数值语义见 `fx.hpp` 各参数注释。

  /// 景深强度（0~1）：背景"失焦"程度。0 = 关闭。
  ///
  /// 实现是降采样+双线性放大（廉价低通），不是真高斯——本框架没有整画布模糊原语。
  /// 观感上足够：人眼判断"虚"靠的是细节消失，不是逐像素模糊核。
  float dof{0.0f};
  /// 粒子数（0 = 关闭）。发布会质感里"浮尘/光点"是氛围的主要来源之一。
  int particles{0};
  float particle_alpha{0.5f};
  /// 径向光晕强度（0~1）。0 = 关闭。
  float glow{0.0f};
  float glow_cx{0.5f};
  float glow_cy{0.38f};
  float glow_radius{0.45f};
  /// 体积光强度（0~1）：一束斜向光柱。0 = 关闭。
  float god_ray{0.0f};
  float god_ray_angle{-1.25f};   ///< 弧度（-1.25 ≈ 向右下 72°）
  /// 光扫强度（0~1）：每段内扫过一次（入场用）。0 = 关闭。
  float light_sweep{0.0f};

  // —— 运镜（摄影机运动，见 `fx.hpp` 的 CameraMove）——
  //
  // 默认"完全不运动"（from=to=1、其余 0）：老脚本行为不变。
  // 开启后整段按进度**线性**推拉/旋转/平移（镜头是匀速的，不是动画）。
  float cam_zoom_from{1.0f};
  float cam_zoom_to{1.0f};
  float cam_rotate{0.0f};     ///< 整段旋转总量（度，建议 <= 1.0）
  float cam_pan_x{0.0f};
  float cam_pan_y{0.0f};
  float cam_shake{0.0f};      ///< 冲击震动幅度（相对短边比例）

  /// 页脚左下角的小字（包装层用）。空则用片名。
  ///
  /// 它不是"随便一句"：页脚要交代**这是什么片子**（片名/期号/机构），
  /// 空着会让包装层少一块结构，画面读起来仍然"散"。
  std::string footer_note{};
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
