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
#include "layer.hpp"
#include "layout.hpp"
#include "render_preset.hpp"
#include "schema.hpp"
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
  /// **自定义布局**：画面完全由 `Scene::layers` 描述（不写 C++ 也能做任意版式）。
  ///
  /// 这是"工具"与"示例"的分界线：五个内置模板是**调味料**，Custom 才是主菜——
  /// 用户/AI 用图层脚本画任何画面，模板只用于"想快速出个像样的片头"。
  Custom,
};

inline constexpr SceneKind kSceneKinds[] = {SceneKind::Title, SceneKind::Terminal,
                                            SceneKind::Triptych, SceneKind::Metrics,
                                            SceneKind::Outro, SceneKind::Custom};
inline constexpr std::size_t kSceneKindCount = std::size(kSceneKinds);

[[nodiscard]] auto scene_kind_name(SceneKind kind) -> std::string_view;
[[nodiscard]] auto scene_kind_from(std::string_view name) -> std::optional<SceneKind>;

/// 一个**跨场承接图元**（carrier）：跨越两个分镜边界、在两边是**同一个东西**。
///
/// ## 为何需要它（这是“像 PPT”的真正根源）
///
/// 裁云原本的架构是：每个分镜**独立渲染**，然后在边界处交叉淡入。两个画面之间
/// 没有任何东西“活下来”——这正是成熟动效片判定“幻灯片”的准确形态：
/// 后一个画面把前一个**替掉**了。
///
/// 承接图元的做法：声明一个矩形在 A 幕末帧与 B 幕首帧是**同一个物体**，
/// 它在这段时间里从 A 的形状/位置/尺寸变形成 B 的。因为它是同一个物体、
/// 连续地变，观众看到的是“这个长成了下一个”，而不是“换了一张”。
///
/// ## 为何挂在场景边界上（而不是图层）
///
/// 图层是**幕内**的概念（生命周期等于那一幕）。承接必须穿透边界，
/// 所以它的坐标定义在**全片时间轴**上（`at`/`dur` 是成片时间）。
/// 它仍然**由场景驱动**：
///
/// ```jsonc
/// // scenes[0] 上声明：本幕末尾把一个卡片交给下一幕，变成整页
/// "carry_out": {
///   "into": 1,                    // 交给 scenes[1]（默认下一幕）
///   "at": 2.2,                    // 开始变形的成片时间（秒）
///   "dur": 0.6,                   // 变形时长
///   "from": {"x": "20%", "y": "30%", "w": "25%", "h": "18%", "radius": 12},
///   "to":   {"x": "0%",  "y": "0%",  "w": "100%", "h": "100%", "radius": 0},
///   "fill": "#0f1728",           // 填充（无填则不画底色）
///   "stroke": "#4d6bfe",         // 描边
///   "text": "产品界面",            // 可选：画在图元中心随矩形缩放的文字
///   "curve": "expo_out"           // 缓动
/// }
/// ```
///
/// ## 它为何能让 carry score 上去
///
/// 验收器看的是“边界两侧有没有**同一身份**的东西，并且它位移 ≥ 画面对角线 2%
/// 或缩放 ≥ 10%”。承接图元天然满足：名字稳定，且矩形在变。
///
/// ## 三条约束（不做的事）
///
/// 1. **不改场景架构**：分镜仍然是独立渲染单位，承接层只是合成之上的**一层**；
///    不声明 `carry_out` 时行为与旧版**逐像素一致**。
/// 2. **只承接、不造内容**：它画矩形 + 文字，不接管幕内图层。
/// 3. **一次一个**：一个边界最多一个承接图元（多个承接会让主次不清，
///    而观众的注意力只有一个）。
struct CarryRect {
  /// 画面比例（“50%”）或设计像素（640）
  std::string x{"0"};
  std::string y{"0"};
  std::string w{"0"};
  std::string h{"0"};
  /// 圆角（设计像素）
  float radius{0.0f};
};

struct CarryOut {
  /// 交给哪一个分镜（下标）。-1 = 下一幕。
  int into{-1};
  /// 变形起点（成片时间，秒）与时长（秒）。
  double at{0.0};
  double dur{0.5};
  /// 起点与终点的矩形（各自带圆角）
  CarryRect from{};
  CarryRect to{};
  /// 名字（用于元素追踪 / 验收器认身份；必填，否则这个承接不算数）。
  std::string name{};
  /// 填充色（`a == 0` = 不画底色，只画描边/文字）
  st::math::Color fill{0, 0, 0, 0};
  st::math::Color stroke{0, 0, 0, 0};
  float stroke_width{0.0f};
  /// 可选文字（画在图元中心；矩形变化时字号跟着缩，保持“同一块东西长开”的观感）
  std::string text{};
  st::math::Color text_color{255, 255, 255, 255};
  float text_size{48.0f};
  /// 缓动名（`linear`/`smooth`/`expo_out`/`cubic_out`/`smoother`…），空 = `expo_out`。
  std::string curve{};
  /// 除了矩形变形之外，额外的轻微旋转（度，从 0 到该值）——用于带方向感的承接。
  float rotate{0.0f};
  /// **在变形的最后这段比例里淡出**（0~1；0 = 不淡出，一直画到 `at+dur`）。
  ///
  /// ## 为何需要它（实测缺陷）
  ///
  /// 承接是合成**之上**的一层。当它长到铺满整屏时，会**盖住下一幕的内容**：
  /// 实测卡片长满后，下一幕的页标题已经进场，却被那块不透明矩形完全遮住，
  /// 观众看到 0.4 秒空屏（第 160 帧全空）。
  ///
  /// 它本来的职责是“把这个东西交代给下一幕”；长到终点时职责就完了，
  /// **应该让位给真正的页面内容**。典型值：`0.35` = 最后 35% 的时间里淡出。
  float fade_out{0.0f};
};

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
  /// **背景颗粒**数（0 = 关闭）。
  ///
  /// ## 为何需要它（实测得出的必要层）
  ///
  /// 对标成熟动效片时发现：它的背景不是纯色，而是**持续闪烁的细颗粒**。
  /// 逐帧测量下，正是这层让它的「变化强度中位数」达到 0.196，
  /// 而纯色背景的片子只有 0.006——差 33 倍。
  ///
  /// 观感差别：纯色背景在「静止段」看起来像**卡住了**（画面完全冻结），
  /// 有颗粒时则是「安静但活着」。这是静止段能不能成立的关键。
  ///
  /// 与 `particles` 的区别：`particles` 是**大颗浮尘**（数十像素、缓慢飘），
  /// 颗粒是**微光点**（1~2px、高频闪、数量多）——后者提供质感而不是内容。
  int grain{0};
  /// 颗粒亮度（建议 0.05~0.2；太高会看起来像噪点视频）。
  float grain_alpha{0.10f};
  /// 包装层（识标/四角标/页脚/进度）开关。
  ///
  /// 默认 true（= 旧行为）。关掉它是为了**纯画面**镜头——例如“模拟桌面/界面”那种片，
  /// 四角标与 `01 / 03` 会立刻把观众拉回“这是一部宣传片”，反而破。
  bool wrap{true};
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
  /// **缩放销点**（归一化 0~1，负值 = 画面中心）。
  ///
  /// 它是“从这一格里钻出去”与“整幅等比放大”的全部区别：
  /// 给了销点，缩放时那个位置**在屏幕上不动**，变大的只有周围。
  float cam_anchor_x{-1.0f};
  float cam_anchor_y{-1.0f};
  /// 缩放曲线：`linear`（默认，镜头匀速）/ `expo_in`（吸进去）/ `expo_out`（弹出来）。
  std::string cam_zoom_curve{"linear"};
  /// 相机进度的**窗口**（秒）：只为 `[起, 止]` 这段推进，窗口外夹在 0/1。
  ///
  /// ## 为何需要它
  ///
  /// 没有窗口时进度是硬编码的整段线性（`time / duration`），于是
  /// “钻 8 秒、然后静静呆 4 秒让观众读”**表达不出来**——相机只能一路走到幕尾。
  /// 而“落定后留 ≥1s 静止”是本仓自己的纪律；当前实现让这条纪律
  /// 在**所有大幅运镜的镜头上都做不到**。
  ///
  /// 实测参考片 clearing：13.1s→14.8s 几乎静止（钻到位就稳住让观众读）。
  ///
  /// ## 为何不能靠拉长 duration 代替
  ///
  /// 拉长让整段变慢，但 `expo_in` 的**末段变化最快**，拉长反而让尾部更刷。
  /// 曲线与窗口是两件正交的事；用曲线去模拟窗口，方向恰好相反。
  ///
  /// 默认 `end <= start` = 不启用（进度仍按整段算，老脚本逐像素不变）。
  float cam_window_start{0.0f};
  float cam_window_end{0.0f};
  /// **矢量相机**：在绘制期施加缩放（图元含文字按新尺寸**重新光栅化**），
  /// 而不是画完再整幅重采样。
  ///
  /// 两者是**不同的能力，不是新旧替代**：
  /// · 重采样：小幅度运镜（呼吸 0.95~1.08）——一次重采样、所有图元一起动、
  ///   含粒子/三维/辉光，语义一致。
  /// · 矢量：**钻穿镜头**（1→300）——只有它能做到大幅缩放不糊，
  ///   因为矢量矩形的细节是无限的。
  ///
  /// 实测：重采样在 130× 时插值糊肉眼可见；参考片 clearing 的 300× 钻穿
  /// 抽帧完全不糊（它每帧重绘）。
  ///
  /// **限制**：矢量模式下旋转/震动不参与（只支持等比缩放 + 锚点）；
  /// 粒子/三维/辉光这些屏幕空间效果不会跟着缩放。
  bool cam_vector{false};
  float cam_rotate{0.0f};     ///< 整段旋转总量（度，建议 <= 1.0）
  float cam_pan_x{0.0f};
  float cam_pan_y{0.0f};
  float cam_shake{0.0f};      ///< 冲击震动幅度（相对短边比例）

  /// **通用图层**（`kind=custom` 时生效；其他 kind 也可叠加，画在模板之上）。
  ///
  /// 有了它，加一种新版式**不需要改 C++**——写几个图层即可。
  /// 字段语义见 `layer.hpp`。
  std::vector<Layer> layers{};

  /// 页脚左下角的小字（包装层用）。空则用 Script 级的 `footer_note`。
  ///
  /// 它不是"随便一句"：页脚要交代**这是什么片子**（片名/期号/机构），
  /// 空着会让包装层少一块结构，画面读起来仍然"散"。
  std::string footer_note{};

  /// 品牌识标（由 `apply_script` 从 Script 复制，供渲染期使用）。
  /// 为何复制而不是渲染时回查 Script：渲染是**纯函数**（只依赖 Scene 与 time），
  /// 回查 Script 会引入隐藏输入，破坏"同一时刻两次渲染逐像素相同"的判据。
  std::string brand_wordmark{};
  std::string brand_latin{};

  /// 脚本编排层（可选）：空 = 纯 JSON 行为。
  ///
  /// 只存**源文本**，不存已编译的引擎——因为每个渲染线程要各自的实例。
  LayoutScript layout{};

  /// **本幕交出时的承接图元**（见 `CarryOut` 的注释）。
  ///
  /// 空格名字 = 不承接（旧行为，逐像素不变）。
  CarryOut carry{};

  /// **JS 返回的临时图元**（每帧重建，定义见 `layer.hpp` 的 `ScriptShape`）。
  std::vector<ScriptShape> shapes{};
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
  /// **品牌标识**（包装层的识标与页脚文字）。
  ///
  /// 默认值刻意保持**中性空串**：这是通用工具，不该预设某个产品的名字。
  /// 空串时包装层不画识标——比印一个假品牌好（假品牌会被误以为是模板残留）。
  std::string brand_wordmark{};
  std::string brand_latin{};
  std::string footer_note{};
  /// 识标是否显示（品牌名都为空时自动隐藏，这个开关用于"有名字但不想要识标"）
  bool show_brand{true};
  std::vector<Scene> scenes{};

  /// 解析期诊断（未知字段 / 越界 / 类型不符）。
  ///
  /// ## 为什么诊断要跟着 Script 走，而不是只打日志
  ///
  /// 写脚本的主体是**智能体**：它拿到的是控制通道的返回值，而不是终端日志。
  /// 早先 `warnings` 参数一路传 `nullptr`，于是"未知字段"这类问题在数据层就消失了——
  /// 实测：往三维脚本里写 `model_rot` / `camera_azimuth` / `light_rimm`，
  /// 程序照常出片、零提示，而"球没自转"与"写对了但转速是 0"在画面上完全一样。
  ///
  /// 所以诊断必须成为**脚本数据的一部分**：随 `apply_script` 一起进工程状态，
  /// 经控制通道 `schema_diagnostics` 可读——这样"我写的字段没生效"是一个
  /// 一秒钟能回答的问题，而不是靠盯画面猜。
  std::vector<Diagnostic> diagnostics{};

  [[nodiscard]] auto total_duration() const noexcept -> double;

  /// 有无错误级诊断（`warning` 不算：宽容解析下未知字段不阻断加载）。
  [[nodiscard]] auto has_errors() const -> bool;
};

/// 内置脚本：**通用示例片**（`--script` 不传时的默认工程，也是 AI 的起点范本）。
///
/// 内容是**中性的占位文案**，不是任何具体产品——它是"能跑起来的起点"，
/// 用户/AI 把它改成自己的内容。
[[nodiscard]] auto builtin_sample_promo() -> Script;

/// 解析脚本文本（错误：`Parse`，消息带**行号无关**的字段路径，便于脚本作者定位）。
/// 解析脚本 JSON。
///
/// `asset_basis`：脚本所在目录。它用于解析**相对路径**的外部件
/// （`layout` 的 JS 文件、素材图等）。不传则相对路径按当前工作目录解析。
[[nodiscard]] auto parse_script(std::string_view json,
                                std::string_view asset_basis = {}) -> Result<Script>;
/// 序列化（美化 2 空格；键序固定，便于人工 diff）。
[[nodiscard]] auto script_to_json(const Script& script) -> std::string;

/// `#rrggbb` / `#rrggbbaa` → Color（解析失败返回 `fallback`，不崩）。
[[nodiscard]] auto parse_color(std::string_view text, st::math::Color fallback) -> st::math::Color;
/// Color → `#rrggbb`。
[[nodiscard]] auto color_to_hex(st::math::Color color) -> std::string;

/// 预设表：相机与光照各一份。
///
/// 为何是 thread_local 而不是 `parse_script` 的参数：预设是**工程级的静止配置**，
/// 而 `parse_script` 的签名已被多处调用（测试、控制通道、main）。
/// 每处都加两个参数会把“只是想解析一段 JSON”变成必须懂预设。
/// thread_local 与导出时 16 路并行相容（每线程一份，读同一份内容）。
struct RenderPresetTable {
  std::vector<RenderPreset> cameras{};
  std::vector<RenderPreset> lightings{};
  /// 是否已初始化（未初始化的空表 = 不允许用预设）。
  bool initialized{false};
};

[[nodiscard]] auto render_presets() -> RenderPresetTable&;

/// 设置预设表（载入工程包时调一次）。传入空目录也能用——只剩内置预设。
///
/// `package_root` 为空：只用内置；否则额外读 `<root>/cameras/*.json`
/// 与 `<root>/lighting/*.json`（同名覆盖内置）。
void set_render_presets_from_package(std::string_view package_root);

/// 把一份 `Script` 里的相机/光照预设**展开成实值**（就地）。
///
/// 为何对外暴露：`--flatten` 导出的脚本必须是**自包含**的——
/// 导出后不依赖预设文件也能跑。所以拍平前要先展开。
[[nodiscard]] auto expand_script_presets(Script& script) -> Result<void>;

}  // namespace caiyun
