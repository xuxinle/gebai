// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与同名 HTML **同源**：改行内容请改生成器再重跑。
constexpr int kWidth = 900;
constexpr int kHeight = 1269;

struct Line {
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  st::math::Color color;
  st::text::FontRole role;
  bool bold;
};

constexpr std::array<Line, 18> kLines{{
    {"10px 霜天组件画廊 Overview 2026 ABC abc", 4.0f, 10.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"11px 霜天组件画廊 Overview 2026 ABC abc", 73.6f, 11.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"12px 霜天组件画廊 Overview 2026 ABC abc", 143.2f, 12.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"13px 霜天组件画廊 Overview 2026 ABC abc", 212.8f, 13.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"13.5px 霜天组件画廊 Overview 2026 ABC", 282.4f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"14px 霜天组件画廊 Overview 2026 ABC abc", 352.0f, 14.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"15px 霜天组件画廊 Overview 2026 ABC abc", 421.6f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"16px 霜天组件画廊 Overview 2026 ABC abc", 491.2f, 16.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"17px 霜天组件画廊 Overview 2026 ABC abc", 560.8f, 17.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"20px 霜天组件画廊 Overview 2026 ABC abc", 630.4f, 20.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"24px 霜天组件画廊 Overview 2026 ABC", 700.0f, 24.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"32px 霜天组件画廊 Overview", 769.6f, 32.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"48px 霜天组件", 839.2f, 48.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"", 908.8f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"13.5px const auto p = x->y;", 978.4f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"15px const auto p = x->y;", 1048.0f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"17px const auto p = x->y;", 1117.6f, 17.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"20px const auto p = x->y;", 1187.2f, 20.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
}};
