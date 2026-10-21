// 由 tools/gen_dw_ab.py 生成（theme=light）——不要手改。
namespace dwab_light {
struct DwColor { unsigned char r, g, b; };
inline constexpr DwColor kDwBg = {0xFF, 0xFF, 0xFF};
inline constexpr DwColor kDwFg = {0x0F, 0x17, 0x2A};
inline constexpr float kDwWidth = 640.0f;
inline constexpr float kDwHeight = 504.6f;
struct DwLine { const char* label; const char* text; float size; float top; bool mono; bool bold; };
inline constexpr DwLine kDwLines[] = {
  {"han-body", "永东国画不清风飞翔", 15.0f, 8.0f, false, false},
  {"han-dense", "疆馨密霸繁悬整靓霾", 15.0f, 34.0f, false, false},
  {"han-simple", "一二十上下三人", 15.0f, 60.0f, false, false},
  {"han-thin", "纤纱细线红绿经终", 15.0f, 86.0f, false, false},
  {"latin-body", "Hamburgefonstiv III", 15.0f, 112.0f, false, false},
  {"latin-thin", "illumination minimal", 15.0f, 138.0f, false, false},
  {"digits", "0123456789 3.14 42", 15.0f, 164.0f, false, false},
  {"mixed", "霜天渲染器 vs DirectWrite 123", 15.0f, 190.0f, false, false},
  {"mono", "->=>(){}[]; if (x) {", 15.0f, 216.0f, true, false},
  {"size-09", "永文字Aa9", 9.0f, 242.0f, false, false},
  {"size-10", "永文字Aa10", 10.0f, 268.0f, false, false},
  {"size-11", "永文字Aa11", 11.0f, 294.0f, false, false},
  {"size-12", "永文字Aa12", 12.0f, 320.0f, false, false},
  {"size-13", "永文字Aa13", 13.0f, 346.0f, false, false},
  {"size-14", "永文字Aa14", 14.0f, 372.0f, false, false},
  {"size-16", "永文字Aa16", 16.0f, 398.0f, false, false},
  {"size-18", "永文字Aa18", 18.0f, 425.2f, false, false},
  {"size-24", "永文字Aa24", 24.0f, 455.8f, false, false},
};
}  // namespace dwab_light
