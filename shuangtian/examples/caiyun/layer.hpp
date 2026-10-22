#pragma once

/// 裁云（Nimbus）· **通用图层系统**。
///
/// ## 为什么要有这一层（这是"工具"与"示例"的分界线）
///
/// 在此之前裁云的画面由 5 个**硬编码场景模板**（title/terminal/triptych/metrics/outro）
/// 决定。那意味着：想要一个「左边图片、右边三行要点」的画面，唯一的办法是
/// **去改 C++ 再重新编译**。
///
/// 工具不该是这个形状。用户（或 AI）应该能用**脚本**描述任意画面，
/// 而渲染器只负责把它画出来。所以这一层把画面拆成**图层**：
///
/// ```
/// scene.layers = [
///   {kind=rect,  x=80, y=200, w=700, h=420, fill="#1a2740", radius=18},
///   {kind=image, src="media/a.png", x=80, y=200, w=700, h=420},
///   {kind=text,  text="左边是图，右边是字", x=830, y=240, size=56, bold=true},
///   {kind=line,  x1=830, y1=330, x2=1500, y2=330, width=2, color="#7ab4ff"},
/// ]
/// ```
///
/// ## 与旧场景模板的关系
///
/// **并存，不是替换**。`kind`（场景类型）仍然有效，五个内置模板继续工作
/// （它们已经调好了动画与排版，不该为了"通用"而丢掉）。`layers` 是**追加**的：
/// 场景既可以是 `kind=title`，也可以是 `kind=custom` + 任意图层。
///
/// 这样老脚本一行不改，新脚本不用碰 C++。
///
/// ## 坐标语义（三套，别混）
///
/// | 单位 | 写法 | 用途 |
/// |---|---|---|
/// | 设计像素 | `x: 640` | 绝对定位，随画幅缩放（推荐） |
/// | 画面比例 | `x: "50%"` | 相对定位，换画幅时自适应 |
/// | 相对锚点 | `x: 0.5` + `anchor: center` | 居中/右对齐 |
///
/// 首版只实现**设计像素**与**画面比例**（字符串带 `%`）。相对锚点等有真实需求再加——
/// 三套语义同时存在已经够容易混淆了。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"

namespace caiyun {

using st::math::Color;

/// 图层类型。
///
/// 取值刻意与 `louyue` 的块类型保持**同名同义**（text/image/rect/line/ellipse）——
/// 两个工具的画布语义统一，用户学一次就够，AI 也不必记两套。
enum class LayerKind : std::uint8_t {
  Rect,      ///< 矩形（可圆角、可渐变填充）
  Ellipse,   ///< 椭圆/圆
  Line,      ///< 直线（可设线宽与端点样式）
  Text,      ///< 文字（可设字号/字重/字距/对齐/行高）
  Image,     ///< 素材图（cover/contain、圆角、压暗）
  Path,      ///< 折线/多边形（点列表，可闭合填充）
  Glow,      ///< 径向光晕（复用 fx.hpp 的实现）
  Particles, ///< 粒子场（复用 fx.hpp 的实现）
  /// **三维模型**：把真实的三维几何画进图层矩形。
  ///
  /// 这一项与上面所有项的区别是**降维不同**：其余都是二维图元（最终都是
  /// 往画布写色块），而它是“三维场景→一张图→再当位图叠加”。于是它要额外
  /// 回答三件二维不存在的事：相机在哪、光从哪来、模型怎么转。
  ///
  /// 实现走框架的 `raster::Scene3D`（平台中立），因此**有显卡走显卡、
  /// 无显卡自动降级**，调用方不需要判断（见 `model3d.hpp`）。
  Model3d,

  /// **逐字排版**（kinetic type）：每个字符是独立元素，可各自错峰入场/位移/缩放。
  ///
  /// ## 为何要单独立一个图层类型（而不是给 `Text` 加开关）
  ///
  /// `Text` 是**整段**绘制的：一次 `draw` 把整个字符串交给字体引擎排版。
  /// 那个路径上不存在“第 3 个字现在偏移了多少”这个概念。
  /// 要在那里加逐字控制，得把已有的行布局/换行/对齐全部重写
  /// （而那段代码有大量实测过的对齐修正，动它风险极高）。
  ///
  /// 于是另开一条路：**每个字符自己算位置与样式，逐个 `draw`**。
  /// 代价是自己管排版（字宽靠 `TextPort::measure_width` 量，而不是靠字体引擎）；
  /// 好处是每个字完全可独立驱动——这正是这类片子的主语言。
  KinText,
};

[[nodiscard]] auto layer_kind_name(LayerKind kind) -> std::string_view;
[[nodiscard]] auto layer_kind_from(std::string_view name) -> std::optional<LayerKind>;

/// 一个关键帧。
struct KeyFrame {
  float time{0.0f};
  float value{0.0f};
  /// 到达本帧的缓动：0=线性 1=ease-out 2=ease-in-out 3=过冲
  int ease{1};
};

/// **脚本提交的临时图元**（每帧重建，不进入图层集合）。
///
/// ## 为何需要它（“像 PPT”的真正技术根因）
///
/// JSON 图层是**静态集合**：写几个就是几个。于是“一个点分裂成四个”、
/// “一排珠子沿曲线跑”、“20 个图标错峰入场”都做不了——
/// 而这类**结构变化**正是高端动效片的主体。
///
/// 给 JS 开一个出口：`layout()` 额外返回 `shapes` 数组，即“这一帧额外要画的东西”。
/// 它们**不参与场景结构**（不增加图层数、不影响关键帧、不需在 JSON 声明）。
///
/// ## 边界（故意画死）
///
/// · 只能画基本形状与文字（不能加载素材、不能开子场景）；
/// · 每帧重建（不跨帧保存状态）——这保证确定性；
/// · 只能在所在场景的矩形内画。
struct ScriptShape {
  /// 0=圆 1=短形 2=线 3=文字
  int kind{0};
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
  /// 线专用：终点
  float x2{0.0f};
  float y2{0.0f};
  /// 文字专用
  std::string text{};
  Color color{255, 255, 255, 255};
  /// 短形圆角（像素）
  float radius{0.0f};
  /// 线宽（0 = 填充）
  float stroke{0.0f};
  /// 旋转角度（度，绕自身中心；0 = 不转）
  float rotate{0.0f};
};

/// 一条属性曲线（关键帧列表）。
///
/// 设计取舍：**一个轨道只驱动一个属性**（`x`/`y`/`scale`/`alpha`/`rotate`），
/// 而不是“一个时刻一张全属性表”。
/// 为何：真实动画里各属性变化节奏不同（位置先动、透明度后跟），
/// 按属性分轨能各自用不同缓动与不同关键帧数；
/// 按时刻合并则每个时刻都得写全属性，改一个值要改很多行。
struct KeyTrack {
  /// 属性名：`x` / `y` / `w` / `h` / `scale` / `alpha` / `rotate`。
  std::string property{};
  /// 关键帧：按 `time` 升序（解析时排序，保证乱序输入也能用）。
  std::vector<KeyFrame> keys{};
};

/// 一个图层。**按需字段**：不同 kind 只用其中一部分。
///
/// 用扁平字段而不是 `std::variant` 的理由：脚本是 JSON、序列化要对称、
/// 而且字段少（十来个）。variant 会让"读一个不存在的字段"变成类型错误，
/// 而这里更希望"给了无效字段就忽略"——脚本是外部输入，宽容比严格好用。
struct Layer {
  LayerKind kind{LayerKind::Rect};
  std::string name{};   ///< 图层名（面板显示与调试用）

  /// **三维场景组**（仅 `model3d` 有意义）：同一个组名的图层**共用一个三维场景**。
  ///
  /// ## 为何需要它（踩过，必看）
  ///
  /// 最初每个 `model3d` 图层都是独立场景、独立相机。结果是：桌面块与图标块
  /// **不在同一个世界空间**里——图标无论怎么设 `x/y`，都只是“另一个小窗口里的小盒子”，
  /// 它不可能真的**站在**桌面块上、更不可能穿过桌面块弹跳。
  /// 而“把桌面做成一块真正的立体板，图标在上面弹”恰恰要求它们**共享透视**。
  ///
  /// 所以：**组名相同的图层共用一次渲染**（同一相机、同一光照、同一纹理），
  /// 按图层顺序依次 `draw_mesh`，最后合成一次。
  ///
  /// 组内坐标怎么定：用**第一层**（组内最先出现的）的矩形作为“画面框”，
  /// 后续层的 `x/y/w/h` 被解释为**在该框内的相对位置**（像素，设计坐标），
  /// 而 `model_scale` 变成**世界单位里的尺寸**（1.0 = 基准尺寸）。
  /// 这样才能保证“桌面宽 2.0、图标宽 0.3、图标放在桌面右半边”——那是真实的物理关系。
  std::string model_group{};

  // —— 位置与尺寸（设计像素；字符串带 % 表示画面比例）——
  // 用 `std::string` 存原始值，解析时再判定是像素还是百分比：
  // JSON 里 `"x": 640` 与 `"x": "50%"` 都合法，存成 double 会丢掉后者。
  std::string x{"0"};
  std::string y{"0"};
  std::string w{"0"};
  std::string h{"0"};

  // —— 外观 ——
  Color fill{255, 255, 255, 255};
  Color fill_alt{0, 0, 0, 0};    ///< 渐变第二色（a==0 = 纯色填充）
  /// 渐变方向：0=竖向（默认，本框架有快速路径）/ 1=横向
  int gradient_dir{0};
  Color stroke{0, 0, 0, 0};      ///< 描边色（a==0 = 无描边）
  float stroke_width{0.0f};
  float radius{0.0f};            ///< 圆角（Rect）
  float opacity{1.0f};           ///< 整体透明度 0~1

  // —— 文字 ——
  std::string text{};
  float size{48.0f};
  float tracking{0.0f};          ///< 字距
  float line_height{1.45f};      ///< 行高倍数（多行文字）
  bool bold{false};
  /// 对齐：0=居中 1=左 2=右 3=两端（3 首版按左对齐处理）
  int align{0};
  /// 文字颜色（独立于 `fill`：文字常需要与形状不同色）
  Color color{255, 255, 255, 255};

  // —— 素材与路径 ——
  std::string src{};             ///< Image 的素材引用（绝对路径/相对脚本/素材库名）
  bool contain{false};           ///< Image: true=完整可见留边；false=cover 填满裁切
  float dim{0.0f};               ///< Image 的压暗 0~1
  /// Path 的点列表：`[x0,y0, x1,y1, ...]`（设计像素，按序列解释）
  std::vector<float> points{};
  bool closed{false};            ///< Path 是否闭合（闭合则填充，否则只描边）

  // —— 特效（Glow/Particles 用）——
  float intensity{0.5f};
  int count{60};                 ///< 粒子数
  float spread{0.4f};            ///< 光晕半径 / 粒子扩散（相对画面短边）

  // —— 进出场动画 ——
  //
  // 通用动画只保留三种（够覆盖绝大多数需求，且不需要为每种写代码）：
  // fade（淡入）/ rise（上浮淡入）/ none。更复杂的时间线交给场景级特效。
  /// 0=none 1=fade 2=rise 3=scale（从 0.92 缓入到 1）
  int enter{2};
  /// 入场起始时间（相对场景起点，秒）
  float enter_at{0.0f};
  /// 入场时长（秒）
  float enter_dur{0.6f};

  // —— 阻尼弹跳（“跳几下然后定住”）——
  //
  // 为何要有它：`enter` 那三种都是**单调**的（只会往上、只会变大），
  // 而“图标在桌面上跳动最后落入任务栏”需要**往复**动作（上去→下来→再上去）。
  // 用单一缓动做不出来；用一堆关键帧写又太重。
  //
  // 公式：`amp * e^(-damping*t) * |sin(π*hz*t)|`（详见 `layer.cpp` 的 `bounce_offset`）。
  // 用 `|sin|` 而不是 `sin`：取绝对值才是“弹起——落地——再弹起”，
  // 用 `sin` 会变成“上下摆动”且能摆到地面以下（那看起来像沉入地板）。
  /// 弹跳幅度（像素，设计坐标尺度）：0 = 不弹
  float bounce{0.0f};
  /// 弹跳频率（Hz）：每秒跳几下。2~3 是“有活力”，>5 像抽搞。
  float bounce_hz{2.2f};
  /// 衰减系数（1/秒）：越大停得越快。3~5 是“跳三下就定”的手感。
  float bounce_damping{3.5f};
  /// 弹跳起点（相对场景起点，秒）。
  float bounce_at{0.0f};

  // —— 三维模型（`kind=model3d`）——
  //
  // 下面的字段只在 `kind == Model3d` 时生效。之所以用扁平字段而不是嵌套结构：
  // 图层已经是扁平的，嵌一层会让“哪些字段属于三维”变成看代码才知道的事；
  // 而注释分段 + 统一前缀（`model_` / `cam_`）已经够区分了。

  /// 形状：`box` / `sphere` / `torus` / `cylinder` / `plane` / `obj:<路径>`。
  ///
  /// `obj:` 前缀是**外部模型**的唯一入口（Wavefront OBJ，只取几何）。
  /// 为什么要前缀而不是“看扩展名”：路径可能没有扩展名（素材库按名字引用），
  /// 以名字猜测会变成“有时候能加载、有时候当成内置形状”的漂移行为。
  std::string model_shape{"box"};
  /// 模型颜色（第二色 `model_color_alt` 非透明时按竖直方向做双色渐变）。
  Color model_color{120, 176, 255, 255};
  Color model_color_alt{0, 0, 0, 0};
  /// 形状细分（球/环/柱用；越大越光滑，成本也越高）。
  ///
  /// 参考值：24 段在 720p 下已看不出棱（球 ~576 面），48 段用于特写（~2300 面）。
  /// 上限 96：再高只是在为“贴着屏幕看”付费，而裁云的成片是 1080p。
  int model_segments{24};
  /// 模型整体缩放（相对图层矩形的适配尺寸；1 = 按矩形铺满兼顾长宽比）。
  float model_scale{1.0f};

  // —— 相机（球坐标；比直接给 eye 位置好写得多）——
  //
  // 为什么是方位角/仰角/距离而不是 `eye[x,y,z]`：写脚本时人（和 AI）想的是
  // “从右前方看”、而不是“眼在 (2.1, 1.7, 2.6)”。球坐标三个数直接对应该意图，
  // 而且改视角时不会把相机弄进模型内部。
  /// 水平方位角（度）：0 = 正前方，正值向右绕，360 一圈。
  float cam_azimuth{35.0f};
  /// 仰角（度，-85~85）：正值从上方俯视。
  float cam_elevation{18.0f};
  /// 相机到模型中心的距离（模型单位为 1，建议 2.2~4.0）。
  float cam_distance{3.0f};
  /// 视野角（度）。小 = 长焦压缩（更“产品摄影”），大 = 广角变形。
  float cam_fov{38.0f};
  /// 整段**推拉**：从 `cam_distance_from` 到 `cam_distance_to`（0 = 不推）。
  /// 用乘法叠在 `cam_distance` 上，所以写 1.0→1.08 就是“推 8%”。
  float cam_dolly_from{1.0f};
  float cam_dolly_to{1.0f};

  // —— 自转与浮动（“活起来”的最小代价）——
  /// 自转角速度（度/秒，绕 Y 轴）。0 = 不转。
  ///
  /// 为何用**速度**而不是“总角度”：片长改了之后总角度不变会拖慢旋转，
  /// 而速度是镜头语言里真正想表达的那个量（“缓慢自转”就是 8°/s）。
  float model_spin{0.0f};
  /// 初始角度（度）：让同一模型在不同镜头里不是同一个朝向。
  float model_yaw{0.0f};
  /// 绕 X 轴的固定倾斜（度）：产品展示常用 10~20° 俯角显得“立体”。
  float model_tilt{12.0f};
  /// 上下浮动幅度（相对模型尺寸）与频率（Hz）。0 = 不动。
  float model_bob{0.0f};
  float model_bob_hz{0.35f};

  // —— 光照 ——
  /// 主光方位角（度，水平）与仰角（度）：打光方向是构图的一部分。
  float light_azimuth{40.0f};
  float light_elevation{45.0f};
  /// 环境光（0~1）：0 = 背光面全黑，0.35 = 默认的柔和底光。
  float light_ambient{0.35f};
  /// 边缘光强度（0~1）：让模型从背景里“浮”出来。
  float light_rim{0.25f};

  /// 背景：`true` 时用场景底色铺满图层矩形（`false` 时带透明通道，可叠在视频上）。
  ///
  /// 透明背景需要场景清成 `alpha=0`；GPU 腿与软件腿都支持（软件腿画布本就是预乘）。
  bool model_backdrop{false};

  // —— 形变（Squash & Stretch，Disney 12 原则之首）——
  /// 形变强度 0~1（0 = 刚体）。建议 0.10~0.20。
  ///
  /// ## 为何必须有它
  ///
  /// 实测代价：第一版图标上下平移、形状**完全不变**，被判“像贴纸在移动”。
  /// 真实物体在加减速时会形变，而形变是“有重量”唯一的视觉证据。
  /// 原文要点（Thomas & Johnston）：**体积不能变**——压扁多少，另两轴就要鼓多少。
  /// 本实现用 `XZ = 1/sqrt(Y)` 保证 X·X·Y ≈ 1。
  ///
  /// 只在**组内**且有弹跳时生效（相位来自弹跳高度）。
  float model_bounce_squash{0.0f};
  /// 阴影强度 0~1（0 = 不画）。建议 0.45~0.65。
  ///
  /// 为何需要它：三维形状靠**明暗差**被感知。物体与地面接触处若两侧同亮，
  /// 大脑判“贴纸”；实测（2026-10-22）无阴影时“物体正下方”亮度 74.8、
  /// “旁边空地” 48.3——完全无反差，“粗糙”的主要来源就是这个。
  ///
  /// 只在**组内**生效（需要知道地面在哪；单图层没有共享世界，无法定义“地面”）。
  float model_shadow{0.0f};
  /// 阴影颜色（默认近黑；带一点环境色比纯黑自然）。
  Color model_shadow_color{0, 0, 0, 255};

  // —— 贴图（需要网格带 UV；内置里只有 `textured_box` / `plane` 字头带 UV）——
  /// 纹理源：SVG 或 PNG 的路径（相对脚本目录/素材库名/绝对路径）。空 = 不贴图。
  ///
  /// 为何在这里才加贴图：`Mesh` 只存位置/法线/顶点色时，模型只能是纯色实体；
  /// 而“把歌白图标做成三维物体”这类需求必须有图案。见 `mesh.hpp` 里
  /// UV 边表的设计说明（为何不改 stride）。
  std::string model_texture{};
  /// 纹理平铺次数（1 = 铺满整个面；>1 重复）。当前用于“墙面/地面”类底图。
  float model_texture_tile{1.0f};

  // —— 路径动画（"从 A 飞到 B，走一条弧线"）——
  //
  // ## 为何需要它（能做与不能做的分界）
  //
  // `bounce` 只能做**原地竖直**弹跳。而“图标跳到任务栏并定住”、“卡片从左侧
  // 飞入落在中位”这类动作都需要**水平位移 + 弧线**。
  // Disney 12 原则的 **Arcs**：自然运动走弧线，直线只适合机械体。
  //
  // ## 与 `bounce` 的关系
  //
  // 两者**可叠加**：`path_*` 管“从哪飞到哪”，`bounce_*` 管“落地后弹几下”。
  // 典型写法：`path_dur=0.9`（飞过去）+ `bounce_at=0.9`（到了开始弹）。
  /// 起点（设计坐标；空 = 用图层自身的 `x/y`）。
  std::string path_from_x{};
  std::string path_from_y{};
  /// 终点（设计坐标；空 = 用图层自身的 `x/y`）。
  std::string path_to_x{};
  std::string path_to_y{};
  /// 弧高（像素，在直线上方拱起的高度）。0 = 直线。
  ///
  /// 取值建议：位移距离的 15~30%。太小看不出弧、太大变成“抛上天空”。
  float path_arc{0.0f};
  /// 飞行时长与起始时刻（秒）。
  float path_dur{0.8f};
  float path_at{0.0f};
  /// 缓动：0=线性 1=ease-out（默认，落地减速）2=ease-in-out 3=落地过冲
  ///
  /// `3`（过冲）实现 Anticipation 的逆向：物体**冲过头再回来**，
  /// 这是“有质量”的关键——匀速到达看起来就像幻灯片切换。
  int path_ease{1};

  // —— 关键帧时间线（`tracks`）——
  //
  // ## 它与 `path_*` / `bounce_*` / `layout.js` 的分工
  //
  // · `path_*`：**一次**位移（A → B，走弧线）。一个图层只能有一段。
  // · `bounce_*`：落地后弹几下。也是单段。
  // · `tracks`：**多段**任意属性变化（就是传统动画的“关键帧”）。
  //   想“先飞过来、停一拍、再缩一下、再淡出”，用 `tracks`。
  // · `layout.js`：任意代码（互相牵引、循环生成、自定义曲线）。
  //
  // 优先级：三者**可叠加**（先 `tracks` 再 `path` 再 `layout`），
  // 因为它们是不同量：`tracks` 给绝对值，`path` 给位移增量，
  // `layout` 给最终增量。叠加顺序固定，结果确定。
  /// 关键帧轨道：每个轨道是一条属性曲线。
  std::vector<KeyTrack> tracks{};

  // —— 逐字排版（`KinText`）——
  //
  // ## 为何这些参数在图层上而不是交给 JS
  //
  // 它们描述的是“这一行字长什么样”（字距、逐字节秦延迟、逐字起伏幅度），
  // 属于**静态风格**；而“这一刻轮到第几个字了”是动态的，由时间算。
  // 把风格写在 JSON 里（可读、可 diff），把时间演化交给渲染器算。
  /// 逐字入场延迟（秒/字）。0 = 同时出现。建议 0.02~0.06（太快像闪现）。
  float kin_stagger{0.035f};
  /// 逐字入场时长（每个字自己的动画时长）。
  float kin_dur{0.42f};
  /// 入场方式：0=上浮淡入 1=微缩放 2=模糊上浮（近似）
  int kin_enter{0};
  /// 逐字上浮距离（像素）。
  float kin_rise{22.0f};
  /// 逐字起伏幅度（像素）：入场后持续呼吸，0 = 不动。
  ///
  /// 为何需要：全静止的字看起来像贴图；极小的持续起伏（2~6px）
  /// 让画面“活着”而不抢戏。
  float kin_float{0.0f};
  /// 逐字起伏频率（Hz）。
  float kin_float_hz{0.6f};
  /// 光标：0=无 1=在末尾闪烁的竖线（打字机感）
  int kin_caret{0};
  /// 光标宽度与高度比例（高 = 字号 × 本值）
  float kin_caret_scale{0.92f};
  /// 逐字显示上限：只显示前 N 个字符（打字机效果）；<0 = 全部显示。
  /// 由渲染器根据时间自动推进（`kin_type_cps`），也可由 JS 覆盖。
  float kin_type_cps{0.0f};
};


/// 求一条轨道在 `time` 时刻的值。
///
/// 边界行为（三条都有用例守）：
/// · `time` 早于首帧 → 取首帧值（**不外插**：外插会让物体从奇怪的地方进场）；
/// · `time` 晚于末帧 → 取末帧值（停在最后一个姿势）；
/// · 只有一帧 → 恒为该值。
[[nodiscard]] auto evaluate_track(const KeyTrack& track, float time) -> float;

/// 关键帧求值结果（**绝对值**，不是增量）。
struct TrackValues {
  /// 位掩码：哪些属性真的被轨道驱动了（未驱动的保持 JSON 基准值）。
  bool has_x{false};
  bool has_y{false};
  bool has_w{false};
  bool has_h{false};
  bool has_scale{false};
  bool has_alpha{false};
  bool has_rotate{false};
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
  float scale{1.0f};
  float alpha{1.0f};
  float rotate{0.0f};
};

/// 求一个图层全部轨道的值（纯函数，可单测）。
///
/// 轨道里的 `x`/`y`/`w`/`h` 支持与图层字段同样的写法（**设计像素**或**画面比例**），
/// 所以 `"50%"` 也能当关键帧值用。
[[nodiscard]] auto evaluate_tracks(const Layer& layer, float time, float canvas_w,
                                   float canvas_h, float design_w, float design_h)
    -> TrackValues;

/// 路径动画的求值结果（相对图层基础位置的增量）。
struct PathOffset {
  float dx{0.0f};
  float dy{0.0f};   ///< 正值 = 向下（与画布 y 轴同向）
};

/// 求 `layer` 在 `local_time` 时刻的路径位移（纯函数，可单测）。
///
/// 返回的是**相对图层 `x/y` 的增量**，所以没写 `path_*` 字段时返回 `{0,0}`，
/// 老脚本行为完全不变。
[[nodiscard]] auto evaluate_path(const Layer& layer, float local_time, float canvas_w,
                                 float canvas_h, float design_w, float design_h) -> PathOffset;

/// 图层列表在某一时刻的绘制结果（由渲染器内部使用）。
///
/// 独立成结构是为了让"动画求值"与"绘制"分开：
/// 前者可测（纯函数），后者依赖画布。
struct LayerState {
  float x{0.0f};
  float y{0.0f};
  float w{0.0f};
  float h{0.0f};
  float alpha{1.0f};
  float scale{1.0f};
  float offset_y{0.0f};
  /// 额外旋转（度）。目前只有 `model3d` 图层会用；其余图层忽略。
  float rotate{0.0f};
  /// 颜色覆盖（`a=0` = 不覆盖）。目前只有部分图层支持。
  Color tint{0, 0, 0, 0};
};

/// 求某图层在 `local_time` 时刻的动画状态（纯函数，可单测）。
///
/// `canvas_w/canvas_h` 用于解析百分比坐标；`design_w/design_h` 是设计基准
/// （百分比按画面算、像素按设计坐标缩放）。
[[nodiscard]] auto evaluate_layer(const Layer& layer, float local_time, float canvas_w,
                                  float canvas_h, float design_w, float design_h) -> LayerState;

/// 阻尼弹跳的竖直位移（像素；**负值 = 向上**，与画布 y 轴向相反）。
///
/// 公开它的理由：三维组渲染要把“跳起来”映射成**世界的 Y 抬高**（而不是画面上的位移），
/// 所以它需要直接取这个值，而不是从 `evaluate_layer` 的 `offset_y` 里反推。
[[nodiscard]] auto bounce_offset(const Layer& layer, float local_time) -> float;

}  // namespace caiyun
