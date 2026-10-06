// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与同名 HTML **同源**：改行内容请改生成器再重跑。
constexpr int kWidth = 900;
constexpr int kHeight = 91;

struct Line {
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  st::math::Color color;
  st::text::FontRole role;
  bool bold;
};

constexpr std::array<Line, 4> kLines{{
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 4.0f, 10.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 22.85f, 11.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 41.7f, 12.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 60.55f, 13.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
}};
