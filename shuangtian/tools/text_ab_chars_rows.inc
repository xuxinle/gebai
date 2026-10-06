// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与同名 HTML **同源**：改行内容请改生成器再重跑。
constexpr int kWidth = 900;
constexpr int kHeight = 625;

struct Line {
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  st::math::Color color;
  st::text::FontRole role;
  bool bold;
};

constexpr std::array<Line, 28> kLines{{
    {"中文标题：组件画廊·资源管理器", 4.0f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"一二十丁厂七卜人入八九几儿了力乃刀又", 25.75f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"三上下个大天大地小口山日月水火木金土", 47.5f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"中文测试字体渲染清晰组件画廊", 69.25f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"霜天字体渲染粗细均匀锐利清晰", 91.0f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"魏蜀黍龘龗爨蠹灩鬱麤齉齾", 112.75f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"藏彝僰瓯鼍蠡饕餮麒麟麒麟", 134.5f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZ", 156.25f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"abcdefghijklmnopqrstuvwxyz", 178.0f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"0123456789 !\"#$%&'()*+,-./:;<=>?@", 199.75f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"[]^_`{|}~ \\ / | 0123456789", 221.5f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"Il1| O0 oO 8B S5 2Z rn m cl dq", 243.25f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"AVATAR To Wa fi fl ffi ffl", 265.0f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"标点，。、；：？！“”‘’（）《》—…", 286.75f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"全角ＡＢＣａｂｃ１２３　全角空格", 308.5f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"符号 →←↑↓ ✓✗ ●○■□ ★☆ §¶†‡ °±×÷ ≠≤≥ ∞≈", 330.25f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"路径 src/raster/renderer.cpp:128", 352.0f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"中英混排 Mixed 中英 2026 版 v0.1.0", 373.75f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"text #0F172A 正文", 395.5f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"text_muted #56647C 次要", 417.25f, 15.0f, st::math::Color{0x56, 0x64, 0x7C, 0xFF},
     st::text::FontRole::Proportional, false},
    {"text_faint #66768C 辅助", 439.0f, 15.0f, st::math::Color{0x66, 0x76, 0x8C, 0xFF},
     st::text::FontRole::Proportional, false},
    {"primary #2563EB 主色", 460.75f, 15.0f, st::math::Color{0x25, 0x63, 0xEB, 0xFF},
     st::text::FontRole::Proportional, false},
    {"常规 霜天组件画廊 Regular", 482.5f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, false},
    {"粗体 霜天组件画廊 Bold", 504.25f, 15.0f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Proportional, true},
    {"const auto polylines = points[i];", 526.0f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"namespace st::raster { return a->b; }", 547.75f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"0123456789 => (){}[];: iIl1|0O", 569.5f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
    {"中文注释必须能显示 汉字宽度", 591.25f, 13.5f, st::math::Color{0x0F, 0x17, 0x2A, 0xFF},
     st::text::FontRole::Monospace, false},
}};
