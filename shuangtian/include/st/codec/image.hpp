#pragma once

/// @file image.hpp
/// @brief **图片解码统一入口**：按**魔数**识别格式，不信任扩展名。
///
/// ## 为什么按魔数而不是扩展名
///
/// 素材包里"扩展名与实际内容不符"是真实存在的（下载来的 `.png` 实际是 JPEG、
/// 从 URL 保存时名字被截断、某些工具导出时写错后缀）。按扩展名分发会得到
/// "明明有图却解不出来"，而错误信息里只说"不是 PNG"，查起来极慢。
///
/// ## 支持的格式
///
/// | 格式 | 魔数 | 说明 |
/// |---|---|---|
/// | PNG | `89 50 4E 47` | 完整支持（8 位，颜色类型 0/2/3/4/6） |
/// | JPEG | `FF D8 FF` | 基线 + 渐进式（生态素材的绝大多数 albedo 是它） |
///
/// ## 输出格式统一
///
/// 一律输出 **RGBA8、非预乘、行优先**（与 `Scene3D::set_texture` 同一契约）。
/// 这样下游不必知道来源格式——而"格式差异泄漏到渲染层"是这类模块最常见的错误来源
/// （灰度图少一个通道、带 alpha 的图预乘了、16 位图被当 8 位）。
///
/// ## 失败语义
///
/// 返回 `Result`，错误消息里带**识别到的格式**与**失败原因**：
/// - 识别不出格式 → `Unsupported`，消息里给出前 8 字节（便于判断"这是不是图"）；
/// - 识别出但解码失败 → `Invalid`，消息里带具体阶段（"霍夫曼表损坏"/"缺少 SOF"）。

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "st/core/error.hpp"

namespace st::codec {

/// 解码结果：RGBA8、非预乘、行优先。
struct DecodedImage {
  std::vector<std::uint8_t> rgba{};
  int width{0};
  int height{0};
  /// 识别到的格式（`"png"` / `"jpeg"`），用于日志与报错。
  std::string format{};

  [[nodiscard]] auto is_valid() const noexcept -> bool {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  }
};

/// 按魔数识别格式名（不解码）。识别不出返回空串。
[[nodiscard]] auto sniff_image_format(std::span<const std::uint8_t> bytes) -> std::string;

/// 解码任意受支持的图片格式。
[[nodiscard]] auto decode_image(std::span<const std::uint8_t> bytes) -> Result<DecodedImage>;

/// 读文件并解码（按内容识别格式，不看扩展名）。
[[nodiscard]] auto decode_image_file(std::string_view path) -> Result<DecodedImage>;

}  // namespace st::codec
