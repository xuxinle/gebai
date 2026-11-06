#pragma once

/// 格斗示例 · **程序化贴图**（皮肤 / 布料 / 帽子 / 眼镜）。
///
/// ## 为什么要程序化生成贴图
///
/// 当前用的 `CesiumMan.glb` 自带一张 1024² 贴图，内容是**蓝绿品牌图案**
/// （那是 Cesium 的标志色，不是皮肤）。直接用它，角色看起来像
/// "穿了迷彩服的人"——这是本项目里"质量不够"最主要的一个来源。
///
/// 换贴图有两条路：找素材（有授权问题、且要匹配 UV）、或**按 UV 生成**。
/// 这里取后者：模型自带的 UV 布局是现成的，我们只需要往里"填颜色 + 细节"，
/// 于是几十行代码就能把"迷彩人"变成"穿衣服的人"。
///
/// ## 生成的图案是什么
///
/// | 贴图 | 内容 | 关键手法 |
/// |---|---|---|
/// | 皮肤 | 米色底 + 细微色斑 + 毛孔噪点 | 多频噪声叠乘（**不在**单频正弦上妥协） |
/// | 布料 | 主色底 + 织物斜纹 + 接缝暗线 | 高频斜纹 × 低频渐变 |
/// | 帽子 | 深蓝底 + 红/白横条 + 星点 | 按 V 轴分段取色 |
/// | 镜片 | 深紫底 + 高光渐变 | 径向渐变（模拟反光） |
///
/// ## 为什么不用随机数
///
/// 用**确定性的哈希噪声**（`hash2(x, y)`）而不是 `std::rand`：
/// 同一张图每次生成必须一模一样，否则"改了别处、贴图跟着变"会让
/// 视觉回归失去意义（截图对比会一直是红的）。

#include <cstdint>
#include <vector>

#include "st/math/color.hpp"

namespace gedou {

namespace math = st::math;

/// 一张 RGBA8 贴图（行优先、非预乘——与 `Scene3D::set_texture` 同格式）。
struct TextureBitmap {
  std::vector<std::uint8_t> rgba{};
  int width{0};
  int height{0};

  [[nodiscard]] auto valid() const noexcept -> bool {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  }
};

/// 外观配色（与 `FighterLook` 对应，但这里只需要"填进贴图的颜色"）。
struct SkinPalette {
  math::Color skin{0xE8, 0xB4, 0x92, 0xFF};
  math::Color cloth{0xD9, 0x2B, 0x3A, 0xFF};
  math::Color cloth_accent{0xFA, 0xFA, 0xFA, 0xFF};
};

/// 生成一张**皮肤**贴图（米色 + 色斑 + 毛孔）。
///
/// `size` 取 512 足够：皮肤是低频内容，再大也看不出差别，只是多占内存。
[[nodiscard]] auto make_skin_texture(const SkinPalette& palette, int size = 512) -> TextureBitmap;

/// 生成一张**布料**贴图（主色 + 织物斜纹 + 接缝）。
///
/// 斜纹频率按 `size` 归一，所以 256 与 512 出来的"织物密度"看起来一致。
[[nodiscard]] auto make_cloth_texture(math::Color base, math::Color accent, int size = 512)
    -> TextureBitmap;

/// 生成**美国国旗棒球帽**贴图（深蓝底 + 红白横条 + 星点）。
[[nodiscard]] auto make_cap_texture(math::Color cap_base, math::Color cap_stripe,
                                    math::Color star_color, int size = 256) -> TextureBitmap;

/// 生成**亮片镜片**贴图（深紫底 + 对角高光）。
[[nodiscard]] auto make_lens_texture(int size = 128) -> TextureBitmap;

}  // namespace gedou
