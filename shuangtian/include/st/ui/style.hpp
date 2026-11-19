#pragma once

/// 样式与设计 token 绑定：布局属性（Flex 子集）+ 外观属性（背景/边框/圆角/阴影）+ 排版属性。
/// 组件通过 `Style` 表达外观，具体色值来自 `Theme`（见 theme.hpp），实现"换主题不改组件代码"。

#include <cstdint>
#include <optional>
#include <utility>
#include <string>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/paint.hpp"

namespace st::ui {

enum class FlexDirection : std::uint8_t { Row, Column };
enum class Align : std::uint8_t { Start, Center, End, Stretch };
enum class Justify : std::uint8_t { Start, Center, End, SpaceBetween, SpaceAround };
enum class FontWeight : std::uint8_t { Regular, Medium, SemiBold, Bold };
enum class TextAlign : std::uint8_t { Start, Center, End };

/// 自动尺寸哨兵。
inline constexpr float kAuto = -1.0f;
inline constexpr float kUnbounded = 1.0e9f;

struct Shadow {
  math::Color color{0, 0, 0, 0};
  float blur{0.0f};
  float offset_x{0.0f};
  float offset_y{0.0f};

  /// **内阴影**（inset）：阴影改画在框体**内侧**，用来表达“凹进去”。
  ///
  /// ## 为什么需要它
  ///
  /// 换底色（`Surface::Sunken`）只能表达“这个区域比周围深”，表达不了“它被压进去了”——
  /// 后者靠的是**光影方向**：上缘一道暗线、下缘一道弱亮线。输入框、凹槽、按下态
  /// 都需要这一层，否则放大看就是一个“较深的圆角矩形”，没有体积感。
  ///
  /// ## 怎么实现（不改光栅层）
  ///
  /// 内阴影 ≡「把外投影的偏移取反、并裁剪到框内」。于是 `paint_box` 只需
  /// `push_clip_rounded_rect(框)` 包一次已有的 `draw_shadow`，
  /// 光栅层与缓存机制全部复用（阴影遮罩本来就按几何参数缓存）。
  ///
  /// 语义上**不要**把 `inset` 与 `color2`（第二层环境光）叠成“两层内阴影”：
  /// 内阴影的作用是把物体“压下去”，叠两层只会让凹槽边缘发脏。
  bool inset{false};

  /// **第二层（环境光）**：与主层叠加成“关键光 + 环境光”。
  ///
  /// 单层投影在浅底上会被读成一条“灰边”，而不是“浮起来的物体”——
  /// 真实光照是近处紧、远处散的叠加，单个高斯无法同时表达两者。
  /// 两层各自与背景混合（不是先加后混），且阴影遮罩按几何参数缓存，
  /// 多一层只多一次查表合成（实测单张卡片投影 0.37 → 0.55 ms，可忽略）。
  math::Color color2{0, 0, 0, 0};
  float blur2{0.0f};
  float offset2_x{0.0f};
  float offset2_y{0.0f};

  [[nodiscard]] auto visible() const noexcept -> bool { return color.a != 0U && blur > 0.0f; }
  [[nodiscard]] auto second_visible() const noexcept -> bool {
    // `inset` 不参与第二层：内阴影只有一层才有“压下去”的读法（见 `inset` 的说明）。
    return !inset && color2.a != 0U && blur2 > 0.0f;
  }
};

struct Style {
  // —— 布局（Flex 子集）——
  FlexDirection direction{FlexDirection::Column};
  float gap{0.0f};
  math::Insets padding{};
  math::Insets margin{};
  bool grow{false};
  bool shrink{true};
  Align align_items{Align::Stretch};
  Align align_self{Align::Stretch};
  Justify justify{Justify::Start};
  bool wrap{false};
  float width{kAuto};
  float height{kAuto};
  float min_width{0.0f};
  float max_width{kUnbounded};
  float min_height{0.0f};
  float max_height{kUnbounded};

  // —— 叠放（绝对定位）——
  /// 本元素是否**脱离 flex 流**，改按父元素内容盒的左上角 + 下面的偏移定位。
  ///
  /// 为何需要它（2026-11-05，被两个需求同时撞到）：
  /// - 列表行的「文字盖在可点区上」：视觉两行、点击落在整行；
  /// - 差异视图的「右缘缩略条」：它是覆盖层，不该把编辑器挤窄。
  ///
  /// 以前只能靠“子元素先后 + 碰巧重叠”凑，而 flex 是**排流**一—它不会把
  /// 两个子元素放同一个位置。没有叠放就只能把内容塞进 `Button` 的单行 label
  /// （排版直接崩：两行挤到同一基线上）。
  ///
  /// 语义与 CSS `position:absolute` 一致：
  /// - 不参与父元素的 flex 分配（不占位、不影向兄弟元素的位置与父的 `measure`）；
  /// - 尺寸用自身的 `width`/`height`（`kAuto` = 沿用其测量值）；
  /// - 定位基准是**父元素 content box**（已扣 padding）的左上角；
  /// - 绘制顺序仍按声明顺序（后声明的画在上面）。
  bool absolute{false};
  /// 绝对定位时的偏移（相对父内容盒左上角；`absolute` 为 false 时无意义）。
  ///
  /// 默认 `-1` = **未锚定**（与 `right`/`bottom` 同口径）。为何必须区分“0”与“未设”：
  /// `left`+`right` 同时锚定才触发横向拉伸（CSS 语义），而默认 0 会让“只写 right”的
  /// 元素被误判成“两端都锚定”而拉满——属性看着对、画面却铺满整块。
  /// 只锚定一侧时另一侧保持 auto（按内容测量）。
  float left{-1.0f};
  float top{-1.0f};
  /// 绝对定位时用“右缘/底缘”锚定：非负则按 `父宽 - right - 自身宽` 定位。
  /// 用于「贴右缘」这类需求，免得调用方自己算父宽（它往往拿不到）。
  ///
  /// **`left` 与 `right` 同时给、且 `width` 为 `kAuto` = 横向拉伸**（CSS 同语义）：
  /// 宽度 = 父宽 − left − right。全宽覆盖层（桌面底栏、页面级遮罩）靠它跟随容器，
  /// 因而不能靠“显式写 width”来实现：那要把窗口尺寸抄一份到样式里，窗口一改就错。
  /// 纵向同理（`top` + `bottom` 且 `height` 为 `kAuto`）。
  /// **只给一侧 = 贴边**（不拉伸，尺寸按内容）。
  float right{-1.0f};
  float bottom{-1.0f};

  // —— 外观 ——
  math::Color background{0, 0, 0, 0};
  /// **渐变背景**（可选）：非空时 `paint_box` 优先用它填底，
  /// `background` 退为它不可用时的回退色。
  ///
  /// 几何坐标是 **0~1 归一化**（相对本元素框）：`(0,0)` 到 `(1,1)` 就是
  /// 「左上 → 右下」的 135° 斜向。`paint_box` 会把它映射到当前 `box`。
  ///
  /// 为何用相对坐标而不是绝对坐标：`apply_theme` 在**布局之前**跑
  /// （`bounds_` 还是空的），那时拿不到尺寸；而渐变几何一旦存成绝对值，
  /// 就会随窗口缩放/内容变化而错位。
  std::optional<raster::Gradient> background_gradient{};
  /// 渐变的不透明度（`1.0` = 按渐变自身的颜色合成）。
  float gradient_opacity{1.0f};

  /// **用主题的页面底渐变**（而不是自己给一组色标）。
  ///
  /// 声明这个标志后，每次 `refresh_theme` 会把当前主题的
  /// `backdrop_gradient()` 写进 `background_gradient`——亮/暗主题各有一套
  /// 同构档位，换肤自动跟随，应用不需要知道具体颜色，也不用自己监听主题变更
  ///（自己写在 `build` 里会拿不到新主题：`build` 只在结构变化时跑）。
  ///
  /// 主题没定渐变时退回 `Theme::colors().bg` 纯色（不报错）。
  bool use_theme_backdrop{false};

  /// **玻璃参数跟随主题缺省档**（`Metrics::glass_fill` / `glass_tint`）。
  ///
  /// 声明它后，`refresh_theme` 会把当前主题的玻璃底色与 `backdrop_tint` 写进来。
  /// 为何需要：亮/暗主题的玻璃**不能共用同一组亮度参数**——实测暗色下
  /// 沿用亮色的纯白+tint 0.22，气泡内亮度 73.7 vs 背景 23.0（亮 50），
  /// 看上去是一块“发辉的灰纸”。所以底色与 tint 都要随主题换档。
  bool theme_glass{false};

  /// **纯色底的唯一写入口**：写 `background` 的同一步清掉渐变。
  ///
  /// 为何必须是唯一入口（2026-11-10，相期「全部忽略」蓝底）：
  /// `paint_box` 的绘制优先级是「**渐变优先于纯色**」（那是正确的绘制语义——
  /// 设了渐变就该画渐变）。但它隐含一条配套义务：**任何想让纯色可见的写入，
  /// 都必须把渐变清掉**。这条义务原先靠 156 处写入点各自记得（实测全仓
  /// `background=` 写入 156 处 / 72 文件，而真正清渐变的只有 Button 的 Primary
  /// 分支 1 处），于是任何「先铺渐变、后写纯色」的路径都留下残影。
  ///
  /// 现象极难查：`style.background` 读回是完全正确的（透明就是透明），
  /// 但画面上是旧渐变——因为绘制时根本不看 `background`。实测相期 XML 写
  /// `variant="ghost"`、`get(variant)` 报 ghost、`background.a==0` 全对，
  /// 屏幕上却是一块主色渐变。
  ///
  /// 与 `surface` 那次静默失效**同构**：都是「一个字段两条通道」，
  /// 都是靠约定保护的不变式，都由历史证据证明守不住。所以把义务提进入口。
  void set_solid_background(math::Color fill) noexcept {
    background = fill;
    background_gradient.reset();
    gradient_opacity = 1.0f;
  }

  /// 设渐变底。`fallback` 是**渐变画不出来时的兜底色**（`gradient_opacity=0`、
/// 色标为空等），语义上它是“这个变体的实底”，而不是“随便一个色”。
///
/// 为何兜底色要进参数（2026-11-10 实测踩到）：渐变变体（Primary）的回退色
/// 就是它的主色。若不写，`background` 会停在元素**上一次**的值上（新元素就是透明）——
/// 于是 `gradient_opacity=0` 或某个分支跳过渐变时，那个按钮变成**透明底**，
/// 而所有只查 `background` 的地方（包括单测）都会看到错的东西。
void set_gradient_background(raster::Gradient fill, math::Color fallback) noexcept {
  background = fallback;
  background_gradient = std::move(fill);
  gradient_opacity = 1.0f;
}
  /// **背板毛玻璃（元素级）**：> 0 时，本元素底色绘制**之前**先把
  /// `bounds_` 内已绘的背板内容模糊掉（`Surface::blur_region`，与 CSS
  /// `backdrop-filter: blur()` 同口径）。与 `Metrics::overlay_blur_radius`
  /// （只作用于弹层）互不影响：这是元素自带的能力，供工具栏/Dock/玻璃卡片用。
  ///
  /// 降级契约：后端不支持（`supports_blur()` 为假，如软件腿在性能预算下不开）
  /// 或半径为 0 时**跳过模糊**，下方的 `background`/`top_highlight` 照常画——
  /// 观感退为“实色底”而不是报错或半透明裸底。
  ///
  /// 为何不接 `hover_t`/动画：模糊是重操作，逐帧变半径会把成本变成动画时长 × 频率；
  /// 需要“玻璃淡入”时用 `opacity` 动画叠在已模糊的底上，成本不变。
  float backdrop_blur{0.0f};
  /// **毛玻璃上叠的底色不透明度**（0 = 只模糊不叠色，1 = 完全不透明 = 模糊白做）。
  ///
  /// 毛玻璃 = 模糊 + 半透明着色，两者必须同时存在（`Metrics::overlay_blur_tint`
  /// 同一条口径）：只模糊不叠色 → 背后内容抢眼，文字读不出来；
  /// 只叠色不模糊 → 就是实色底。着色色就用 `background`，本字段控制它的 alpha。
  float backdrop_tint{1.0f};
  /// **液态玻璃折射强度**（> 0 且后端支持时，在毛玻璃之上叠加「透镜畸变 +
  /// 色散」——`Surface::refract_region`，GPU 腿专属）。0 = 关。
  ///
  /// 语义：与 `backdrop_blur` 组合使用（折射自带模糊半径）；后端不支持折射时
  /// 自动退回 `backdrop_blur` 的普通毛玻璃（`supports_refraction()` 判定），
  /// 界面仍正确。量纲是逻辑像素（边缘位移量）。
  float backdrop_refraction{0.0f};
  /// **色散强度**（无量纲，0 = 关）：作用在折射位移上——R/B 通道采样坐标
  /// 沿法线各偏一点，玻璃边缘出现细微彩虹纹。仅在 `backdrop_refraction > 0`
  /// 且后端支持折射时生效。
  float backdrop_dispersion{0.0f};
  float radius{0.0f};
  /// 圆角**上限**（`<= 0` = 不设上限，只受绘制层的“短边一半”夹取）。
  ///
  /// 用途：让“胶囊”随高度**变高后自然转为圆角矩形**。
  /// 胶囊的常见写法是 `radius = 999`（或 `RadiusMode::Pill`），绘制层会把它夹到短边
  /// 一半——于是多行输入框长到 120 高时，两端会变成半径 60 的大半圆（看着像椭圆形）。
  /// 设 `radius_max = 22` 后：矮时仍是胶囊（半高 < 22），长高后停在 22 = 圆角矩形。
  ///
  /// 为何是独立字段而不是让调用方自己 `min(半高, 22)`：半高**要等布局完才知道**，
  /// 调用方在声明界面时拿不到（胶囊高度本来就由内容决定）——自己算就会写死数值，
  /// 高度一变就错位。
  float radius_max{0.0f};
  math::Color border_color{0, 0, 0, 0};
  float border_width{0.0f};
  Shadow shadow{};
  /// **顶部内高光**（亚克力的「玻璃边缘」）：不为透明时，在圆角内沿顶边画一条
  /// `highlight_width` 高的横向亮线（两端淡出）。
  ///
  /// 为什么是「顶部」而不是四周描边：真实玻璃的边缘受光只在**上缘**，四周一圈会把
  /// 色块封成"双层轮廓"反而显脏（与 `Card::apply_theme` 里"有阴影的卡片再配一道
  /// 同样明显的描边会变脏"同因）。
  /// 消费点是 `Element::paint_box`——组件只需填本字段，不必各自绘制。
  math::Color top_highlight{0, 0, 0, 0};
  float highlight_width{1.0f};
  float opacity{1.0f};
  /// 是否把子元素裁到自己的矩形内。
  ///
  /// 它同时决定**绘制**与**命中**：子元素超出本矩形的那一部分既不显示，也不可点。
  ///
  /// 命中侧的口径很重要（实测）：滚动容器里的长内容（如终端滚回文本）会被排成
  /// 一个**远高于视口**的矩形（y 为负、高 851px 盖住整个窗口）。若只有绘制裁剪而
  /// 命中不裁，那一次“点标题栏/菜单栏”就会被看不见的滚回文本抢走（命中报告会
  /// 直接写出 `#terminal-output`）——用户看到的是“顶部那条点不动了”。
  bool clip_children{false};

  // —— 排版 ——
  math::Color color{};
  float font_size{14.0f};
  FontWeight font_weight{FontWeight::Regular};
  TextAlign text_align{TextAlign::Start};
  bool monospace{false};

  [[nodiscard]] auto has_explicit_width() const noexcept -> bool { return width != kAuto; }
  [[nodiscard]] auto has_explicit_height() const noexcept -> bool { return height != kAuto; }
};

/// 常用属性构造（链式设置保持可读）。
[[nodiscard]] inline auto padding_all(float value) -> math::Insets { return math::Insets::all(value); }
[[nodiscard]] inline auto padding_xy(float horizontal, float vertical) -> math::Insets {
  return math::Insets::symmetric(horizontal, vertical);
}

}  // namespace st::ui
