/// 海报作品模型的实现（见 `poster.hpp`）。
///
/// 解析一律走 `st/ext/json.hpp` 的安全读取面：缺字段取默认、类型不符取默认——
/// 海报脚本是外部输入（AI 生成的），写错一个字段不该让整个作品打不开。
/// 只有「顶层不是对象」「不认识 block kind」如实报错（否则会得到一张空海报，
/// 而空海报看起来像"渲染坏了"，离真因很远）。

#include "poster.hpp"

#include "assets_lib.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "st/core/error.hpp"
#include "st/ext/json.hpp"
#include "st/math/color.hpp"
#include "st/raster/paint.hpp"
#include "st/ui/text_port.hpp"

namespace louyue {

namespace {

using st::raster::Canvas;
using st::raster::Gradient;
using st::raster::GradientStop;
using st::raster::Paint;
using st::math::Rect;
using st::math::Point;

[[nodiscard]] auto to_byte(double value) -> std::uint8_t {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0) + 0.5);
}

/// 颜色：`#rrggbb` / `#rrggbbaa` / `0xRRGGBBAA`。
[[nodiscard]] auto color_field(const st::Json& object, std::string_view key, Color fallback)
    -> Color {
  const st::Json* node = st::json_find(object, key);
  if (node == nullptr) return fallback;
  if (node->is_string()) {
    const std::string text = node->get<std::string>();
    std::string hex = text;
    if (hex.starts_with("#")) hex.erase(0, 1);
    else if (hex.starts_with("0x") || hex.starts_with("0X")) hex.erase(0, 2);
    if (hex.size() != 6 && hex.size() != 8) return fallback;
    std::uint32_t value = 0;
    for (const char c : hex) {
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
      else return fallback;
      value = value * 16U + digit;
    }
    if (hex.size() == 6) value = value * 256U + 0xFFU;
    return Color{to_byte(static_cast<double>((value >> 24U) & 0xFFU)),
                 to_byte(static_cast<double>((value >> 16U) & 0xFFU)),
                 to_byte(static_cast<double>((value >> 8U) & 0xFFU)),
                 to_byte(static_cast<double>(value & 0xFFU))};
  }
  if (node->is_number()) {
    const auto value = static_cast<std::uint32_t>(node->get<double>());
    return Color{to_byte(static_cast<double>((value >> 24U) & 0xFFU)),
                 to_byte(static_cast<double>((value >> 16U) & 0xFFU)),
                 to_byte(static_cast<double>((value >> 8U) & 0xFFU)),
                 to_byte(static_cast<double>(value & 0xFFU))};
  }
  return fallback;
}

[[nodiscard]] auto color_to_hex(Color color) -> std::string {
  return std::format("#{:02x}{:02x}{:02x}{:02x}", color.r, color.g, color.b, color.a);
}

[[nodiscard]] auto with_alpha(Color color, float alpha) -> Color {
  return Color{color.r, color.g, color.b,
      to_byte(static_cast<double>(color.a) * static_cast<double>(std::clamp(alpha, 0.0f, 1.0f)))};
}

/// 一块的渲染几何：设计坐标 → 输出坐标的缩放。
struct Frame {
  float scale{1.0f};
  int width{0};
  int height{0};
};

/// 按锚点画一段文字。`y` 在设计坐标里是**文字行盒顶部**（与裁云的 TextPort 口径一致）。
void draw_text(const Frame& frame, Canvas& target, const st::ui::TextPort* port,
               std::string_view text, float x_ratio, float y_design, float size_design,
               float tracking_design, Color color, bool bold, int align = 0) {
  if (port == nullptr || text.empty()) return;
  const float size = size_design * frame.scale;
  const float advance = port->measure_width(text, size);
  const float tracking = tracking_design * frame.scale;
  const float total = advance + tracking * static_cast<float>(text.size() - 1);
  // align: 0=居中（x 是文字中心，旧语义）、1=左对齐（x 是左边界）、2=右对齐（x 是右边界）。
  const float anchor_x = x_ratio * static_cast<float>(frame.width);
  const float left = align == 1 ? anchor_x
                     : align == 2 ? anchor_x - total
                                  : anchor_x - total * 0.5f;
  const float top = y_design * frame.scale;
  // `bold=true` = 该字重已由真粗体字面承担，合成加粗不该再叠（叠了会把字再撑开）；
  // embolden 半径按字号 1/20 给——与裁云 `Painter::text` 同口径。
  const float embolden = bold ? 0.0f : size / 20.0f;
  const auto put = [&](std::string_view glyph, float x) {
    port->draw(target, glyph, Point{x, top}, size, color, st::text::FontRole::Proportional,
               embolden, bold);
  };
  if (tracking <= 0.01f) {
    put(text, left);
    return;
  }
  // 逐字加距（CJK 大标题的"高级感"来源，与裁云同一做法）。
  float cursor = left;
  std::size_t begin = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    const bool boundary = i == text.size() ||
                          (i > begin && (static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U);
    if (!boundary) continue;
    const std::string_view glyph = text.substr(begin, i - begin);
    if (!glyph.empty()) {
      put(glyph, cursor);
      cursor += port->measure_width(glyph, size) + tracking;
    }
    begin = i;
  }
}

}  // namespace

auto block_kind_name(BlockKind kind) -> const char* {
  switch (kind) {
    case BlockKind::Background: return "background";
    case BlockKind::Title: return "title";
    case BlockKind::Subtitle: return "subtitle";
    case BlockKind::Rule: return "rule";
    case BlockKind::Badge: return "badge";
    case BlockKind::Body: return "body";
    case BlockKind::Image: return "image";
    case BlockKind::Frame: return "frame";
    case BlockKind::GridLines: return "grid";
    case BlockKind::CornerMarks: return "corners";
    case BlockKind::Divider: return "divider";
  }
  return "title";
}

auto parse_block_kind(std::string_view name) -> std::optional<BlockKind> {
  if (name == "background") return BlockKind::Background;
  if (name == "title") return BlockKind::Title;
  if (name == "subtitle") return BlockKind::Subtitle;
  if (name == "rule") return BlockKind::Rule;
  if (name == "badge") return BlockKind::Badge;
  if (name == "body") return BlockKind::Body;
  if (name == "image") return BlockKind::Image;
  if (name == "frame") return BlockKind::Frame;
  if (name == "grid") return BlockKind::GridLines;
  if (name == "corners") return BlockKind::CornerMarks;
  if (name == "divider") return BlockKind::Divider;
  return std::nullopt;
}

auto parse_poster(std::string_view json) -> st::Result<PosterScript> {
  const auto parsed = st::json_parse(json);
  if (!parsed) return st::forward_error(parsed.error());
  const st::Json& root = parsed.value();
  if (!root.is_object()) {
    return st::unexpected(st::ErrorCode::Parse, "海报脚本顶层必须是对象");
  }
  PosterScript poster{};
  poster.name = st::json_get_string(root, "name", poster.name);
  poster.width = static_cast<int>(std::clamp(st::json_get_i64(root, "width", poster.width), 64LL, 7680LL));
  poster.height =
      static_cast<int>(std::clamp(st::json_get_i64(root, "height", poster.height), 64LL, 7680LL));
  poster.out_width = static_cast<int>(st::json_get_i64(root, "out_width", 0));
  poster.out_height = static_cast<int>(st::json_get_i64(root, "out_height", 0));
  poster.background = color_field(root, "background", poster.background);

  const st::Json* blocks = st::json_find(root, "blocks");
  if (blocks == nullptr || !blocks->is_array() || blocks->empty()) {
    return st::unexpected(st::ErrorCode::Parse, "海报脚本缺少 blocks（至少一块）");
  }
  std::size_t index = 0;
  for (const st::Json& node : *blocks) {
    if (!node.is_object()) {
      return st::unexpected(st::ErrorCode::Parse, std::format("blocks[{}] 不是对象", index));
    }
    const std::string kind_name = st::json_get_string(node, "kind");
    const auto kind = parse_block_kind(kind_name);
    if (!kind) {
      return st::unexpected(st::ErrorCode::Parse,
                            std::format("blocks[{}].kind 不认识：「{}」", index, kind_name));
    }
    Block block{};
    block.kind = *kind;
    block.name = st::json_get_string(node, "name");
    block.text = st::json_get_string(node, "text");
    block.x = static_cast<float>(std::clamp(st::json_get_double(node, "x", block.x), 0.0, 1.0));
    block.y = static_cast<float>(st::json_get_double(node, "y", block.y));
    block.size = static_cast<float>(std::clamp(st::json_get_double(node, "size", block.size), 4.0, 800.0));
    block.tracking = static_cast<float>(std::clamp(st::json_get_double(node, "tracking", 0.0), 0.0, 200.0));
    block.bold = st::json_get_bool(node, "bold", false);
    block.align = static_cast<int>(
        std::clamp(st::json_get_i64(node, "align", block.align), 0LL, 2LL));
    // 窄幅重排（见 poster.hpp 的 flow 注释）
    block.flow = static_cast<int>(
        std::clamp(st::json_get_i64(node, "flow", block.flow), 0LL, 9999LL));
    block.flow_rank = static_cast<int>(
        std::clamp(st::json_get_i64(node, "flow_rank", block.flow_rank), 0LL, 9999LL));
    block.alpha = static_cast<float>(std::clamp(st::json_get_double(node, "alpha", 1.0), 0.0, 1.0));
    block.width = static_cast<float>(std::clamp(st::json_get_double(node, "width", 0.0), 0.0, 8000.0));
    block.radius = static_cast<float>(std::clamp(st::json_get_double(node, "radius", 0.0), 0.0, 2000.0));
    block.color = color_field(node, "color", block.color);
    block.color_alt = color_field(node, "color_alt", block.color_alt);
    // Image 块
    block.image = st::json_get_string(node, "image");
    block.contain = st::json_get_bool(node, "contain", block.contain);
    block.height = static_cast<float>(
        std::clamp(st::json_get_double(node, "height", block.height), 0.0, 8000.0));
    block.tint_alpha = static_cast<float>(
        std::clamp(st::json_get_double(node, "tint_alpha", block.tint_alpha), 0.0, 1.0));
    // 结构块（frame/grid/corners/divider）
    block.inset = static_cast<float>(
        std::clamp(st::json_get_double(node, "inset", block.inset), -1.0, 2000.0));
    block.thickness = static_cast<float>(
        std::clamp(st::json_get_double(node, "thickness", block.thickness), 0.1, 80.0));
    poster.blocks.push_back(std::move(block));
    ++index;
  }
  return poster;
}

auto poster_to_json(const PosterScript& poster) -> std::string {
  st::Json root = st::Json::object();
  root["name"] = poster.name;
  root["width"] = poster.width;
  root["height"] = poster.height;
  root["out_width"] = poster.out_width;
  root["out_height"] = poster.out_height;
  root["background"] = color_to_hex(poster.background);
  st::Json blocks = st::Json::array();
  for (const Block& block : poster.blocks) {
    st::Json node = st::Json::object();
    node["kind"] = block_kind_name(block.kind);
    if (!block.name.empty()) node["name"] = block.name;
    if (!block.text.empty()) node["text"] = block.text;
    node["x"] = static_cast<double>(block.x);
    node["y"] = static_cast<double>(block.y);
    node["size"] = static_cast<double>(block.size);
    if (block.tracking > 0.0f) node["tracking"] = static_cast<double>(block.tracking);
    if (block.bold) node["bold"] = true;
    if (block.align != 0) node["align"] = static_cast<std::int64_t>(block.align);
    if (block.flow > 0) {
      node["flow"] = static_cast<std::int64_t>(block.flow);
      node["flow_rank"] = static_cast<std::int64_t>(block.flow_rank);
    }
    if (block.alpha < 1.0f) node["alpha"] = static_cast<double>(block.alpha);
    if (block.width > 0.0f) node["width"] = static_cast<double>(block.width);
    if (block.radius > 0.0f) node["radius"] = static_cast<double>(block.radius);
    node["color"] = color_to_hex(block.color);
    if (block.color_alt.a > 0) node["color_alt"] = color_to_hex(block.color_alt);
    if (block.inset >= 0.0f) node["inset"] = static_cast<double>(block.inset);
    if (block.thickness != 1.5f) node["thickness"] = static_cast<double>(block.thickness);
    if (!block.image.empty()) {
      node["image"] = block.image;
      if (block.contain) node["contain"] = true;
      if (block.height > 0.0f) node["height"] = static_cast<double>(block.height);
      if (block.tint_alpha > 0.0f) node["tint_alpha"] = static_cast<double>(block.tint_alpha);
    }
    blocks.push_back(std::move(node));
  }
  root["blocks"] = std::move(blocks);
  return st::json_dump(root, true, 2);
}

auto builtin_gebai_poster() -> PosterScript {
  PosterScript poster{};
  poster.name = "歌白 · 宣传海报";
  poster.width = 1920;
  poster.height = 1080;
  poster.background = Color{6, 10, 22, 255};
  // 版式骨架（**结构在文字之下**）：块数组自底向上，越靠后越在上。
  //
  // 这张海报与"几个字"的差别主要在这几行——版心框 + 栅格 + 角标 + 分区线
  // 给出的是"正式物料"的视觉语言；文字本身没变多少。
  poster.blocks = {
      Block{.kind = BlockKind::Background,
            .name = "底色",
            .color = Color{8, 14, 30, 255},
            .color_alt = Color{20, 34, 68, 255}},
      // 竖向栅格：技术感骨架。极淡（alpha 很低）——结构元素一显眼就变廉价装饰。
      Block{.kind = BlockKind::GridLines,
            .name = "栅格",
            .color = Color{122, 180, 255, 15},
            .width = 6.0f,
            .inset = 96.0f,
            .thickness = 1.0f},
      // 版心框线
      Block{.kind = BlockKind::Frame,
            .name = "版心",
            .color = Color{122, 180, 255, 34},
            .inset = 64.0f,
            .thickness = 1.5f},
      // 四角裁切标（印刷语言）
      Block{.kind = BlockKind::CornerMarks,
            .name = "角标",
            .size = 42.0f,
            .color = Color{122, 180, 255, 110},
            .inset = 40.0f,
            .thickness = 2.0f},

      // —— 信息区：左上品牌、右上版本 ——
      // 左上：品牌（**左对齐**——x 是左边界，不是文字中心；
      // 早期写 x=0 + 居中，结果完全在画布外/被裁，实测踩到）。
      Block{.kind = BlockKind::Body,
            .name = "品牌词",
            .text = "歌白",
            .x = 0.052f,
            .y = 112.0f,
            .align = 1,
            .size = 34.0f,
            .bold = true,
            .color = Color{246, 249, 255, 255}},
      Block{.kind = BlockKind::Body,
            .name = "品牌拉丁",
            .text = "GEBAI AGENT",
            .x = 0.052f,
            .y = 160.0f,
            .align = 1,
            .size = 19.0f,
            .tracking = 6.0f,
            .color = Color{132, 170, 225, 235}},
      // 右上：右对齐（x 是右边界）
      Block{.kind = BlockKind::Body,
            .name = "版本",
            .text = "PRODUCT SHEET · 2026",
            .x = 0.948f,
            .y = 112.0f,
            .align = 2,
            .size = 19.0f,
            .tracking = 5.0f,
            .color = Color{120, 158, 214, 220}},

      // —— 中心主视觉 ——
      Block{.kind = BlockKind::Badge,
            .name = "徽标",
            .x = 0.5f,
            .y = 296.0f,
            .color = Color{122, 180, 255, 255},
            .radius = 54.0f},
      Block{.kind = BlockKind::Title,
            .name = "主标题",
            .text = "歌白",
            .x = 0.5f,
            .y = 392.0f,
            .size = 196.0f,
            .tracking = 30.0f,
            .bold = true,
            .color = Color{245, 248, 255, 255}},
      Block{.kind = BlockKind::Rule,
            .name = "强调线",
            .x = 0.5f,
            .y = 640.0f,
            .color = Color{122, 180, 255, 220},
            .width = 300.0f},
      Block{.kind = BlockKind::Subtitle,
            .name = "副标题",
            .text = "GEBAI AGENT",
            .x = 0.5f,
            .y = 668.0f,
            .size = 44.0f,
            .tracking = 16.0f,
            .color = Color{180, 206, 244, 255}},

      // —— 分区线 + 三段式理念（横排，而不是竖着堆）——
      //
      // 竖着堆两行长句是"临时排版"的典型特征：没有分区、没有对齐参照。
      // 横排三段 + 分隔线，读起来才是"产品信息架构"。
      Block{.kind = BlockKind::Divider,
            .name = "分区线",
            .x = 0.5f,
            .y = 780.0f,
            .color = Color{122, 180, 255, 34},
            .thickness = 1.2f},
      Block{.kind = BlockKind::Body,
            .name = "理念1",
            .text = "任意语言",
            .flow = 1,
            .flow_rank = 0,
            .x = 0.22f,
            .y = 806.0f,
            .size = 38.0f,
            .tracking = 4.0f,
            .color = Color{214, 228, 248, 255}},
      Block{.kind = BlockKind::Body,
            .name = "理念2",
            .text = "任意进程",
            .flow = 1,
            .flow_rank = 1,
            .x = 0.5f,
            .y = 806.0f,
            .size = 38.0f,
            .tracking = 4.0f,
            .color = Color{214, 228, 248, 255}},
      Block{.kind = BlockKind::Body,
            .name = "理念3",
            .text = "收编为工具",
            .flow = 1,
            .flow_rank = 2,
            .x = 0.78f,
            .y = 806.0f,
            .size = 38.0f,
            .tracking = 4.0f,
            .color = Color{214, 228, 248, 255}},

      // —— 页脚：一句定位语 ——
      Block{.kind = BlockKind::Body,
            .name = "页脚",
            .text = "融合旧世界 IT 的所有技术 · 打造新世界智能的躯体",
            .x = 0.5f,
            .y = 974.0f,
            .size = 24.0f,
            .color = Color{138, 168, 212, 230}},
  };
  return poster;
}

/// 给**大字号**文字加一层柔和投影。
///
/// 为何只给大字：小字叠阴影会糊成一团（反而不可读）；而大标题在深色底上
/// 加一层 120 alpha 的暗色偏移，能把它从背景里“拉”出来。
void draw_text_with_shadow(const Frame& frame, Canvas& target, const st::ui::TextPort* port,
                           const Block& block, Color color) {
  if (port == nullptr || block.text.empty()) return;
  const bool big = block.size >= 40.0f;
  if (!big) {
    draw_text(frame, target, port, block.text, block.x, block.y, block.size, block.tracking,
              color, block.bold, block.align);
    return;
  }
  // 阴影：同字再画一次，往下偏 2 物理像素、透明度按 block.alpha 缩。
  const float off_design = 2.0f / std::max(0.01f, frame.scale);
  const auto shadow_alpha = static_cast<std::uint8_t>(120.0f * block.alpha);
  draw_text(frame, target, port, block.text, block.x, block.y + off_design, block.size,
            block.tracking, Color{0, 0, 0, shadow_alpha}, block.bold, block.align);
  draw_text(frame, target, port, block.text, block.x, block.y, block.size, block.tracking, color,
            block.bold, block.align);
}

void render_poster(Canvas& target, const PosterScript& poster,
                   const st::ui::TextPort* text_port, const AssetLibrary* assets,
                   std::string_view asset_basis) {
  const Frame frame{.scale = static_cast<float>(poster.export_height()) /
                             static_cast<float>(std::max(1, poster.height)),
                    .width = poster.export_width(),
                    .height = poster.export_height()};
  target.clear(poster.background);

  // —— 窄幅重排（flow）——
  //
  // 判定依据是**导出画幅的宽高比**，不是尺寸：横版 16:9 时四个块可以并排，
  // 方版 1:1 勉强、竖版 9:16 必然撞（实测叠成一串不可读的字）。
  //
  // 这里先把"哪些块要改成堆叠、堆在哪"算成一张覆盖表，
  // 渲染时按块的内存地址查表——不改 `poster`（它是真值源，不能被渲染污染）。
  const float aspect = static_cast<float>(frame.width) / static_cast<float>(std::max(1, frame.height));
  constexpr float kFlowBreakAspect = 1.25f;   // 宽高比低于此值就认为是"窄幅"
  struct FlowSlot {
    const Block* block{nullptr};
    float x{0.5f};
    float y{0.0f};
  };
  std::vector<FlowSlot> flow_slots;
  if (aspect < kFlowBreakAspect) {
    // 按组收集
    std::vector<int> groups;
    for (const Block& block : poster.blocks) {
      if (block.flow <= 0) continue;
      if (std::find(groups.begin(), groups.end(), block.flow) == groups.end()) {
        groups.push_back(block.flow);
      }
    }
    for (const int group : groups) {
      std::vector<const Block*> members;
      for (const Block& block : poster.blocks) {
        if (block.flow == group) members.push_back(&block);
      }
      if (members.size() < 2) continue;
      // 按 flow_rank 排序（同 rank 保持出现顺序，稳定排序）
      std::stable_sort(members.begin(), members.end(),
                       [](const Block* a, const Block* b) { return a->flow_rank < b->flow_rank; });
      // 堆叠要**以原组中心为基准上下展开**，不是从最小 y 往下推。
      //
      // 为什么：从 top 往下排会让整组向**下**生长，竖版上直接撞到页脚那句定位语
      //（实测：堆叠修好三词碰撞后，第三行「收编为工具」压在页脚上，
      // 两行字叠成一团）。以中心展开则整组的视觉重心不变。
      float top = members.front()->y;
      float bottom = members.front()->y;
      for (const Block* member : members) {
        top = std::min(top, member->y);
        bottom = std::max(bottom, member->y);
      }
      // 行距还要受**可用高度**约束：方版比竖版矮得多，固定 1.9 倍字号会让
      // 整组向下溢出到页脚（实测方版第三行压在页脚定位语上）。
      // 可用空间 = 从组顶到画幅底部留出 12% 边距；不够就按比例压行距（下限 1.25）。
      const float group_top = std::min(top, bottom);
      const float limit_y = static_cast<float>(frame.height) * 0.88f;
      const float available = std::max(0.0f, limit_y - group_top * frame.scale);
      const float wanted_step = members.front()->size * 1.9f * frame.scale;
      const int gaps = static_cast<int>(members.size()) - 1;
      const float max_step = gaps > 0 ? available / static_cast<float>(gaps + 1) : wanted_step;
      const float min_step = members.front()->size * 1.25f * frame.scale;
      // 回到设计坐标系（÷ frame.scale），因为 slot.y 是设计坐标
      const float step = std::max(min_step, std::min(wanted_step, max_step)) / frame.scale;
      const float span = step * static_cast<float>(members.size() - 1);
      const float center = (top + bottom) * 0.5f;
      for (std::size_t i = 0; i < members.size(); ++i) {
        FlowSlot slot{};
        slot.block = members[i];
        slot.x = 0.5f;   // 堆叠时一律居中
        slot.y = center - span * 0.5f + static_cast<float>(i) * step;
        flow_slots.push_back(slot);
      }
    }
  }

  for (const Block& source_block : poster.blocks) {
    // 查重排表：命中就**拷一份改位置**再用。
    //
    // 为什么不逐处替换 `block.x`：块有 12 处绘制点各自读 block.x/y，
    // 逐处改既容易漏（漏一处就有一个元素留在旧位置），也把"重排"这件事
    // 散进了十几个分支里。拷一份改两个字段，覆盖全部绘制点。
    Block block = source_block;
    for (const FlowSlot& slot : flow_slots) {
      if (slot.block == &source_block) {
        block.x = slot.x;
        block.y = slot.y;
        block.align = 0;   // 堆叠时一律居中（原来可能是左/右对齐）
        break;
      }
    }
    const Color color = with_alpha(block.color, block.alpha);
    switch (block.kind) {
      case BlockKind::Background: {
        // 竖向渐变（本框架对竖向线性渐变有逐行 SIMD 快路径，见 gradient_fill_probe）。
        if (block.color_alt.a == 0) {
          target.fill_rect(Rect{0.0f, 0.0f, static_cast<float>(frame.width),
                                static_cast<float>(frame.height)},
                           Paint::solid(color));
          break;
        }
        Gradient gradient = Gradient::linear(
            Point{0.0f, 0.0f}, Point{0.0f, static_cast<float>(frame.height)},
            {GradientStop{0.0f, block.color}, GradientStop{1.0f, block.color_alt}});
        target.fill_rect(Rect{0.0f, 0.0f, static_cast<float>(frame.width),
                              static_cast<float>(frame.height)},
                         Paint::with_gradient(std::move(gradient)));
        break;
      }
      case BlockKind::Rule: {
        const float width = (block.width > 0.0f ? block.width : 320.0f) * frame.scale;
        const float x = block.x * static_cast<float>(frame.width) - width * 0.5f;
        const float y = block.y * frame.scale;
        target.fill_rect(Rect{x, y, width, std::max(1.0f, 2.0f * frame.scale)}, Paint::solid(color));
        break;
      }
      case BlockKind::Badge: {
        const float radius = (block.radius > 0.0f ? block.radius : 56.0f) * frame.scale;
        const float cx = block.x * static_cast<float>(frame.width);
        const float cy = block.y * frame.scale;
        target.fill_circle(Point{cx, cy}, radius, Paint::solid(with_alpha(color, 0.16f)));
        target.stroke_path(st::raster::make_circle(Point{cx, cy}, radius),
                           Paint::solid(color), std::max(1.5f, 3.0f * frame.scale));
        target.fill_circle(Point{cx, cy}, radius * 0.34f, Paint::solid(color));
        break;
      }
      case BlockKind::Title:
      case BlockKind::Subtitle:
      case BlockKind::Body: {
        // 大标题带投影（深色底上把它从背景"拉"出来）；小字保持干净。
        draw_text_with_shadow(frame, target, text_port, block, color);
        break;
      }
      case BlockKind::Image: {
        // 素材块：`width`/`height` 是两个尺寸（设计像素，随 frame.scale 缩放）。
        // 未给高度时按 16:9 估（海报里图片块最常见的比例）。
        const float w = (block.width > 0.0f ? block.width : 800.0f) * frame.scale;
        const float h = (block.height > 0.0f ? block.height : w * 9.0f / 16.0f / frame.scale) *
                        frame.scale;
        const float x = block.x * static_cast<float>(frame.width) - w * 0.5f;
        const float y = block.y * frame.scale;
        const Rect area{x, y, w, h};
        if (assets == nullptr || block.image.empty()) {
          // 无素材库 / 未指定图片：画占位框（不静默——让人一眼看出"这里该有图"）。
          target.fill_rect(area, Paint::solid(Color{30, 40, 60, 255}));
          {
            st::raster::Path border;
            border.add_rect(area);
            target.stroke_path(border, Paint::solid(with_alpha(color, 0.35f)),
                               std::max(1.0f, 1.5f * frame.scale));
          }
          break;
        }
        const auto path = assets->resolve(block.image, asset_basis);
        const auto image = path ? assets->image(*path) : std::shared_ptr<st::raster::Canvas>{};
        if (!image) {
          // **缺失要看得见**：静默跳过会让"图没进来"看起来像设计就是没图。
          target.fill_rect(area, Paint::solid(Color{48, 24, 28, 255}));
          {
            st::raster::Path border;
            border.add_rect(area);
            target.stroke_path(border, Paint::solid(Color{220, 120, 120, 255}),
                               std::max(1.0f, 2.0f * frame.scale));
          }
          break;
        }
        const float corner = block.radius * frame.scale;
        // 图片投影：让素材块从底色里"浮"起来（海报层次感的主要来源）。
        if (corner > 0.5f) {
          target.draw_shadow(area, corner, 18.0f * frame.scale,
                             Color{0, 0, 0, 110}, Point{0.0f, 6.0f * frame.scale});
        }
        if (block.contain) {
          blit_contain(target, *image, area, with_alpha(Color{255, 255, 255, 255}, block.alpha).a /
                                                              255.0f,
                       corner);
        } else {
          blit_cover(target, *image, area,
                     static_cast<float>(with_alpha(Color{255, 255, 255, 255}, block.alpha).a) /
                         255.0f,
                     corner, block.color, block.tint_alpha);
        }
        break;
      }
      // —— 结构块（2026-10-17）——
      //
      // 这几样是"正式物料"的视觉语言来源：版心框线、栅格、裁切角标、分区线。
      // 它们本身信息量很低，但没有它们的版面读起来像"临时做的"
      // （对比：同样的文字 + 结构线，观感差别远大于把字号加大 30%）。
      case BlockKind::Frame: {
        const float inset = (block.inset >= 0.0f ? block.inset : 48.0f) * frame.scale;
        const float t = std::max(1.0f, block.thickness * frame.scale);
        const float w = static_cast<float>(frame.width);
        const float h = static_cast<float>(frame.height);
        // 四条边各画一次（不用 stroke_path：矩形描边的接角在自绘画布上会有缺口）
        target.fill_rect(Rect{inset, inset, w - inset * 2.0f, t}, Paint::solid(color));
        target.fill_rect(Rect{inset, h - inset - t, w - inset * 2.0f, t}, Paint::solid(color));
        target.fill_rect(Rect{inset, inset, t, h - inset * 2.0f}, Paint::solid(color));
        target.fill_rect(Rect{w - inset - t, inset, t, h - inset * 2.0f}, Paint::solid(color));
        break;
      }
      case BlockKind::GridLines: {
        // 竖向等分细线：`width` 复用为线数（0 = 6 列）。技术感版式的骨架。
        const int count = block.width > 0.5f ? static_cast<int>(block.width) : 6;
        const float t = std::max(0.6f, block.thickness * frame.scale);
        const float inset = (block.inset >= 0.0f ? block.inset : 80.0f) * frame.scale;
        const float w = static_cast<float>(frame.width);
        const float h = static_cast<float>(frame.height);
        for (int i = 1; i < count; ++i) {
          const float x = inset + (w - inset * 2.0f) * static_cast<float>(i) /
                                      static_cast<float>(count);
          target.fill_rect(Rect{x, inset, t, h - inset * 2.0f}, Paint::solid(color));
        }
        break;
      }
      case BlockKind::CornerMarks: {
        const float inset = (block.inset >= 0.0f ? block.inset : 40.0f) * frame.scale;
        const float length = std::max(12.0f, block.size * frame.scale);
        const float t = std::max(1.0f, block.thickness * frame.scale);
        const float w = static_cast<float>(frame.width);
        const float h = static_cast<float>(frame.height);
        const auto mark = [&](float x, float y, float sx, float sy) {
          target.fill_rect(Rect{x, y, length * sx, t}, Paint::solid(color));
          target.fill_rect(Rect{x, y, t, length * sy}, Paint::solid(color));
        };
        mark(inset, inset, 1.0f, 1.0f);
        mark(w - inset - length, inset, 1.0f, 1.0f);
        mark(inset, h - inset - t, 1.0f, -1.0f);
        mark(w - inset - length, h - inset - t, 1.0f, -1.0f);
        break;
      }
      case BlockKind::Divider: {
        // 横向分区线：`width` 是线长（0 = 版心宽），`y` 是位置。
        const float inset = (block.inset >= 0.0f ? block.inset : 120.0f) * frame.scale;
        const float w = block.width > 0.0f ? block.width * frame.scale
                                           : static_cast<float>(frame.width) - inset * 2.0f;
        const float t = std::max(0.8f, block.thickness * frame.scale);
        const float cx = block.x * static_cast<float>(frame.width);
        target.fill_rect(Rect{cx - w * 0.5f, block.y * frame.scale, w, t}, Paint::solid(color));
        break;
      }
    }
  }
}

}  // namespace louyue
