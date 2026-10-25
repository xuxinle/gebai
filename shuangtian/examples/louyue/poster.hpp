#pragma once

/// 镂月（Lunaris）· **海报作品模型**：一份可被 AI 写、被人工微调的海报脚本。
///
/// ## 为什么需要它
///
/// 镂月原本只有「像素 + 图层」——AI 与人都能改，但**没有"作品"这个概念**：
/// AI 改完之后没法描述自己做了什么，人也无法在"改文字"这个层面操作。
/// 裁云的教训是：真值源必须是**结构化作品**（那里是导演脚本），像素只是它的投影。
/// 海报同理——真值是一份 `PosterScript`，`render_poster` 把它投影成图层与像素。
///
/// ## 图层顺序
///
/// `PosterScript::blocks` 自底向上（与 `Document::layers` 同向），每块一个图层。
/// 渲染时按同样顺序叠加，所以"最后一块在最上面"是稳定的。
///
/// ## 为什么块只有五种
///
/// 五种覆盖了产品海报的全部骨架：底色、标题、副标题、强调线、徽标。
/// 加更多种类之前先问：**人真的会在这一层微调吗**——不会的就该是模板参数，
/// 而不是一个可编辑块（否则面板上一堆没人动的图层）。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "st/math/color.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/text_port.hpp"

namespace louyue {

class AssetLibrary;   // 前向声明

using st::math::Color;

/// 海报块类型。
enum class BlockKind : std::uint8_t {
  Background,   ///< 整幅底色（可竖向渐变）
  Title,        ///< 主标题（大字距）
  Subtitle,     ///< 副标题
  Rule,         ///< 强调细线
  Badge,        ///< 圆环徽标（纯几何）
  Body,         ///< 正文/说明文字（多行）
  Image,        ///< **素材图**（cover/contain，可选圆角/叠色）

  // —— 结构块（2026-10-17）：让版面"被设计过"而不是文字浮在照片上 ——
  //
  // 为什么这几样是"产品化"的关键：单张海报里只有居中堆叠的文字时，
  // 无论字号多大都读起来像"临时做的"。印刷/包装设计的**结构语言**
  // （版心框线、栅格、角标、信息区）才是"这是正式物料"的暗示来源。
  Frame,        ///< 版心框线（四周细线，可设内缩与线宽）
  GridLines,    ///< 栅格参考线（竖向等分细线，用于"技术感"版式）
  CornerMarks,  ///< 四角对齐标记（印刷裁切标的视觉语言）
  Divider,      ///< 横向分隔线（版式分区，比 Rule 更"重"）

  // —— 通用图元（2026-10-18）：让"任意版式"不再需要改 C++ ——
  //
  // 在此之前想做一条斜线、一个自定义形状、或一段左对齐的多行正文，
  // 都得去改渲染代码。这几样补齐后，海报版式由**数据**决定而不是代码。
  //
  // 与裁云的 `LayerKind` 刻意同名同义（line/path/rect），
  // 用户学一次就够，AI 也不必记两套画布语义。
  Line,         ///< 任意直线（任意角度；用 x1/y1→x2/y2）
  Path,         ///< 折线/多边形（点列表，可闭合填充）
  Rect,         ///< 任意位置的矩形（可圆角/描边）
  Paragraph,    ///< **多行左/中/右对齐段落**（Body 只能单行居中）
};

[[nodiscard]] auto block_kind_name(BlockKind kind) -> const char*;
/// 从名字解析；不认识返回 `nullopt`（`parse_poster` 据此报错而不是静默降级）。
[[nodiscard]] auto parse_block_kind(std::string_view name) -> std::optional<BlockKind>;

/// 一个图层块。**按需字段**：不同 kind 只用其中一部分（见各字段注释）。
struct Block {
  BlockKind kind{BlockKind::Title};
  std::string name{};        ///< 图层名（面板显示；空则用 kind 名）
  std::string text{};        ///< Title/Subtitle/Body 的文字

  /// **窄幅重排组**：同一组的块在「画幅变窄」时改为一列堆叠。
  ///
  /// ## 为什么需要它（这是重排版问题，不是缩放问题）
  ///
  /// `x` 是**导出宽度**的比例，而字号只按`导出高/设计高`缩放。所以方版/竖版时
  /// 文字相对宽度暴增：横版里并排三个词刚好，竖版里它们会**撞在一起**
  /// （实测 1080×1920 上「任意语言」「任意进程」「收编为工具」叠成了
  /// 「任意语言任意进程收编为工具」，完全不可读）。
  ///
  /// 缩放救不了这个问题——只有**改布局**。同一 `flow` 组 ≥2 个块时：
  /// 画幅宽高比小于阈值就按 `flow_rank` 顺序纵向堆叠，否则保持各自 `x` 横排。
  int flow{0};          ///< 0 = 不参与重排；>0 = 组号
  int flow_rank{0};     ///< 组内顺序（0 起），堆叠时自上而下

  /// 位置：`x` 是**锚点横向百分比**（0=左、1=右、0.5=居中），`y` 是像素纵向基线/中心。
  /// 用百分比而不是像素，是因为同一份脚本要能出横版与方版两种尺寸。
  float x{0.5f};
  float y{0.5f};

  /// 文字横向对齐：0=居中（默认，与 `x` 作为锚点的旧语义一致）、1=左对齐、2=右对齐。
  ///
  /// 为何需要它：`x` 是**文字中心**的比例位置，所以贴边写 `x=0` 会把一半字推到画布外
  /// （实测：左上角品牌词只剩半个字）。左/右对齐时 `x` 改为"对齐边的位置"，
  /// 于是"距版心 96px"这种意图可以直接表达。
  int align{0};

  float size{48.0f};         ///< 字号（像素，按 1080 高设计）
  float tracking{0.0f};      ///< 字距（CJK 大标题用）
  bool bold{false};
  float alpha{1.0f};

  /// 颜色（Background 的 `color` 是上端色，`color_alt` 是下端色；其余块 `color` 即主色）。
  Color color{255, 255, 255, 255};
  Color color_alt{0, 0, 0, 0};

  float width{0.0f};         ///< Rule/Badge/Image：几何宽度（0 = 按尺寸自动）
  float radius{0.0f};        ///< Badge 半径 / Image 圆角
  /// Frame/CornerMarks 的**内缩**（距画幅边缘的像素）；GridLines 的线数用它除以 0 取默认。
  float inset{-1.0f};
  float thickness{1.5f};     ///< 线宽（Frame/GridLines/Divider/CornerMarks）

  // —— Image 块（素材）——
  //
  // `image` 支持三种写法：绝对路径 / 相对脚本目录 / 素材库内名称（见 `assets_lib.hpp`）。
  // 缺失时画“缺图”占位框——不静默跳过（静默会让“图没进来”看起来像设计就是没图）。
  std::string image{};       ///< 素材引用
  /// `cover`（填满+裁切）还是 `contain`（完整可见+留边）。
  /// 海报里两者都常用：满幅背景图要 cover，产品图/截屏要 contain。
  bool contain{false};
  float height{0.0f};        ///< Image 块高度（设计像素）
  /// 叠色（品牌色统一 / 压暗背景让文字可读）；`tint_alpha` 为 0 时不用。
  float tint_alpha{0.0f};

  // —— 通用图元字段（Line/Path/Rect/Paragraph）——
  //
  // 坐标口径与裁云一致：**设计像素**（按 1080 高设计，随画幅缩放）。
  // 不用 `x`（那是 0~1 的比例锚点）——两者混用会算错，所以图元用独立字段。
  float x1{0.0f};              ///< Line 起点 X（设计像素）
  float y1{0.0f};              ///< Line 起点 Y
  float x2{0.0f};              ///< Line 终点 X
  float y2{0.0f};              ///< Line 终点 Y
  float box_w{0.0f};           ///< Rect/Paragraph 宽度（设计像素；0 = 按内容/画面自动）
  float box_h{0.0f};           ///< Rect/Paragraph 高度（0 = 按行数自动）
  float corner{0.0f};          ///< Rect 圆角
  bool filled{true};           ///< Path 是否填充（false = 只描边）
  /// Path 的点列表，成对解释（x0,y0,x1,y1,...），设计像素。
  std::vector<float> points{};
  /// Paragraph 的行高倍数（1.0~2.5；1.45 是"舒展但不散"）
  float line_height{1.45f};

  [[nodiscard]] auto display_name() const -> std::string {
    return name.empty() ? std::string(block_kind_name(kind)) : name;
  }
};

/// 一份海报作品。
struct PosterScript {
  std::string name{"未命名海报"};
  /// 设计基准尺寸（`Block` 的像素字段都按这个高度等比缩放）。
  int width{1920};
  int height{1080};
  /// 导出尺寸（0 = 与设计尺寸相同）。横版/方版共用同一份脚本靠的就是这两个字段。
  int out_width{0};
  int out_height{0};
  Color background{6, 10, 22, 255};   ///< 兜底底色（Background 块缺失时用）
  std::vector<Block> blocks{};

  // —— 影像质感（全部默认 0 = 关闭且零开销；实现来自框架 `st/raster/texture.hpp`）——
  //
  // 海报与视频的差别往往就在这几层：光从哪来、底上有没有尘、四角要不要收。
  // 它们都是**背景层**（画在块之下），不会盖住文字。

  /// 径向光晕强度 0~1（0 = 不画）。
  float glow{0.0f};
  float glow_cx{0.5f};      ///< 光源横向位置（0~1 画面比例）
  float glow_cy{0.32f};     ///< 光源纵向位置
  float glow_radius{0.46f}; ///< 光晕半径（相对画面**短边**）
  /// 光晕色；alpha 为 0 时按背景亮度自动取（深底取冷白，浅底取暖白）。
  Color glow_color{0, 0, 0, 0};

  /// 背景颗粒数（0 = 不画）。建议 200~900；它让大面积平涂不发死。
  int grain{0};
  float grain_alpha{0.08f};

  /// 暗角强度 0~1（0 = 不画）。把视线收到中心。
  ///
  /// **浅色底会自动跳过**（四角刷黑是灾难）——判据在框架的 `backdrop_is_light`。
  float vignette{0.0f};

  [[nodiscard]] auto export_width() const noexcept -> int {
    return out_width > 0 ? out_width : width;
  }
  [[nodiscard]] auto export_height() const noexcept -> int {
    return out_height > 0 ? out_height : height;
  }
};

/// 解析海报脚本 JSON。失败时给**可读的**原因（哪一块、哪个字段）。
[[nodiscard]] auto parse_poster(std::string_view json)
    -> st::Result<PosterScript>;

/// 序列化成 JSON（另存/往返用；键序与 `parse_poster` 对称）。
[[nodiscard]] auto poster_to_json(const PosterScript& poster) -> std::string;

/// 内置的**示例海报**（不依赖任何外部素材）。
///
/// 文案是中性占位：这是通用工具，不预设任何产品名。
[[nodiscard]] auto builtin_sample_poster() -> PosterScript;

/// 把海报渲染成一块画布（尺寸 = `export_width/height`）。
///
/// `text_port` 为空时文字退化为几何占位（与裁云同策略：导出路径不依赖 UI 线程字体）。
void render_poster(st::raster::Canvas& target, const PosterScript& poster,
                   const st::ui::TextPort* text_port,
                   const AssetLibrary* assets = nullptr,
                   std::string_view asset_basis = {});

}  // namespace louyue
