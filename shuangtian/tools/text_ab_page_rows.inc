// 本文件由 `tools/gen_text_ab.py` 生成——**不要手改**（见该脚本说明）。
// 行表与 `tools/text_ab_page.html` **同源**：改行内容请改生成器再重跑。
// 行距 36 逻辑 px（> 最大字号 20 × 1.5，保证两侧的行带都能被空白分开）。
constexpr int kWidth = 700;
constexpr int kHeight = 1024;

struct Line {
  std::string_view text;
  float top;        ///< 逻辑 y（= HTML 的 `top`）
  float size;       ///< 逻辑 px
  Color color;
  FontRole role;
  bool bold;
};

constexpr std::array<Line, 28> kLines{{
    {"中文标题：组件画廊·资源管理器", 4.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"ABC abc 0123 Il1| O0 ꞁ", 40.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"标点，。、；：？！“”‘’（）《》—…", 76.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"符号 →←↑↓ ✓✗ ●○■□ ★☆ §¶†‡ °±×÷ ≠≤≥ ∞≈", 112.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"路径 src/raster/renderer.cpp:128", 148.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"全角ＡＢＣ１２３　全角空格", 184.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"font_xs 12 · 辅助信息 · 版本号 v0.1.0", 220.0f, 12.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"font_sm 13 · 次要说明文字", 256.0f, 13.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"font_base 15 · 正文默认字号", 292.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"font_lg 17 · 大号正文", 328.0f, 17.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"font_xl 20 · 小节标题", 364.0f, 20.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"text #0F172A 正文", 400.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"text_muted #56647C 次要", 436.0f, 15.0f, Color{0x56, 0x64, 0x7C, 0xFF},
     FontRole::Proportional, false},
    {"text_faint #66768C 辅助", 472.0f, 15.0f, Color{0x66, 0x76, 0x8C, 0xFF},
     FontRole::Proportional, false},
    {"primary #2563EB 主色", 508.0f, 15.0f, Color{0x25, 0x63, 0xEB, 0xFF},
     FontRole::Proportional, false},
    {"常规 霜天组件画廊 Regular 15px", 544.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"粗体 霜天组件画廊 Bold 15px", 580.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, true},
    {"半粗 霜天组件画廊 SemiBold 15px", 616.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"const auto polylines = p", 652.0f, 13.5f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Monospace, false},
    {"namespace st::raster { return a->b; }", 688.0f, 13.5f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Monospace, false},
    {"0123456789 => (){}[];: iIl1|0O", 724.0f, 13.5f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Monospace, false},
    {"中文注释必须能显示", 760.0f, 13.5f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Monospace, false},
    {"11px 最小字号 · 菜单项 · 状态栏", 796.0f, 11.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"12px 小字 · 混合 m4 S2 W3 · 日月水火", 832.0f, 12.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"13px 小字 · iliI1l 边界 · 明暗ab", 868.0f, 13.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"AVATAR To Wa fi fl ffi 连字测试", 904.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"行高测试第一行：重影是否出现", 940.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
    {"行高测试第二行：重影是否出现", 976.0f, 15.0f, Color{0x0F, 0x17, 0x2A, 0xFF},
     FontRole::Proportional, false},
}};
