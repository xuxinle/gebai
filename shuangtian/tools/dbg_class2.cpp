#include <cstdio>
#include "st/text/text.hpp"
using namespace st::text;
auto main() -> int {
  auto stack = FontStack::system_default();
  if (!stack) return 1;
  TextRenderer r{*stack, 1.5f};
  r.set_subpixel(true);
  r.set_grid_fit(GridFitMode::Light);
  r.set_class_gamma(TextRenderer::GlyphClass::Digit, 0.90f);
  const auto b = r.glyph_bitmap_of(U'3', 15.0f);
  std::printf("set_class_gamma(0.90) -> 位图 sum = %.1f  (默认 1.1 下应为 88.9)\n",
              [&]{ double s=0; for (float v : b->coverage) s += v; return s; }());
  r.set_coverage_gamma(1.1f);   // ← 与 ab::apply 的顺序一致
  const auto b2 = r.glyph_bitmap_of(U'3', 15.0f);
  std::printf("之后再 set_coverage_gamma(1.1) -> sum = %.1f\n",
              [&]{ double s=0; for (float v : b2->coverage) s += v; return s; }());
  return 0;
}
