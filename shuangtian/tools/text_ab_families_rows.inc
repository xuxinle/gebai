// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与同名 HTML **同源**：改行内容请改生成器再重跑。
constexpr int kWidth = 900;
constexpr int kHeight = 712;

struct Line {
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  st::math::Color color;
  st::text::FontRole role;
  bool bold;
};

constexpr std::array<Line, 15> kLines{{
    {"Handgloves 0123 ABC abc", 4.0f, 11.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"霜天组件画廊渲染测试", 50.4f, 11.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"const auto p = x;", 96.8f, 11.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"Handgloves 0123 ABC abc", 143.2f, 13.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"霜天组件画廊渲染测试", 189.6f, 13.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"const auto p = x;", 236.0f, 13.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"Handgloves 0123 ABC abc", 282.4f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"霜天组件画廊渲染测试", 328.8f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"const auto p = x;", 375.2f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"Handgloves 0123 ABC abc", 421.6f, 20.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"霜天组件画廊渲染测试", 468.0f, 20.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"const auto p = x;", 514.4f, 20.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"Handgloves 0123 ABC abc", 560.8f, 32.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"霜天组件画廊渲染测试", 607.2f, 32.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"const auto p = x;", 653.6f, 32.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
}};
