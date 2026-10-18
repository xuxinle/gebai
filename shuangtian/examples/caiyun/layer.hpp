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
};

[[nodiscard]] auto layer_kind_name(LayerKind kind) -> std::string_view;
[[nodiscard]] auto layer_kind_from(std::string_view name) -> std::optional<LayerKind>;

/// 一个图层。**按需字段**：不同 kind 只用其中一部分。
///
/// 用扁平字段而不是 `std::variant` 的理由：脚本是 JSON、序列化要对称、
/// 而且字段少（十来个）。variant 会让"读一个不存在的字段"变成类型错误，
/// 而这里更希望"给了无效字段就忽略"——脚本是外部输入，宽容比严格好用。
struct Layer {
  LayerKind kind{LayerKind::Rect};
  std::string name{};   ///< 图层名（面板显示与调试用）

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
};

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
};

/// 求某图层在 `local_time` 时刻的动画状态（纯函数，可单测）。
///
/// `canvas_w/canvas_h` 用于解析百分比坐标；`design_w/design_h` 是设计基准
/// （百分比按画面算、像素按设计坐标缩放）。
[[nodiscard]] auto evaluate_layer(const Layer& layer, float local_time, float canvas_w,
                                  float canvas_h, float design_w, float design_h) -> LayerState;

}  // namespace caiyun
