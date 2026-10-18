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
};

[[nodiscard]] auto block_kind_name(BlockKind kind) -> const char*;
/// 从名字解析；不认识返回 `nullopt`（`parse_poster` 据此报错而不是静默降级）。
[[nodiscard]] auto parse_block_kind(std::string_view name) -> std::optional<BlockKind>;

/// 一个图层块。**按需字段**：不同 kind 只用其中一部分（见各字段注释）。
struct Block {
  BlockKind kind{BlockKind::Title};
  std::string name{};        ///< 图层名（面板显示；空则用 kind 名）
  std::string text{};        ///< Title/Subtitle/Body 的文字

  /// 位置：`x` 是**锚点横向百分比**（0=左、1=右、0.5=居中），`y` 是像素纵向基线/中心。
  /// 用百分比而不是像素，是因为同一份脚本要能出横版与方版两种尺寸。
  float x{0.5f};
  float y{0.5f};

  float size{48.0f};         ///< 字号（像素，按 1080 高设计）
  float tracking{0.0f};      ///< 字距（CJK 大标题用）
  bool bold{false};
  float alpha{1.0f};

  /// 颜色（Background 的 `color` 是上端色，`color_alt` 是下端色；其余块 `color` 即主色）。
  Color color{255, 255, 255, 255};
  Color color_alt{0, 0, 0, 0};

  float width{0.0f};         ///< Rule/Badge/Image：几何宽度（0 = 按尺寸自动）
  float radius{0.0f};        ///< Badge 半径 / Image 圆角

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

/// 内置的**歌白宣传海报**（不依赖任何外部素材）。
[[nodiscard]] auto builtin_gebai_poster() -> PosterScript;

/// 把海报渲染成一块画布（尺寸 = `export_width/height`）。
///
/// `text_port` 为空时文字退化为几何占位（与裁云同策略：导出路径不依赖 UI 线程字体）。
void render_poster(st::raster::Canvas& target, const PosterScript& poster,
                   const st::ui::TextPort* text_port,
                   const AssetLibrary* assets = nullptr,
                   std::string_view asset_basis = {});

}  // namespace louyue
