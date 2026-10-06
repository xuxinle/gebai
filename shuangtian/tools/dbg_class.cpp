#include <cmath>
#include "st/core/print.hpp"
#include "st/text/text.hpp"
using namespace st::text;
// 同一渲染器、改类 γ 前后取同一字形的墨量和（缓存必须按类区分）
double ink(TextRenderer& r, char32_t cp){
  auto b = r.glyph_bitmap_of(cp, 15.0f);
  if(!b) return -1;
  double s=0; for(float v:b->coverage) s+=v; return s;
}
int main(){
  auto stack = FontStack::system_default();
  if(!stack) return 1;
  TextRenderer r{*stack, 1.5f};
  r.set_subpixel(true); r.set_grid_fit(GridFitMode::Light); r.set_coverage_gamma(1.1f);
  st::print("默认档        : 3={:.1f}  A={:.1f}\n", ink(r,U'3'), ink(r,U'A'));
  r.set_class_gamma(TextRenderer::GlyphClass::Digit, 0.90f);
  st::print("数字类 γ=0.90 : 3={:.1f}  A={:.1f}   <- 3 应变大、A 不变\n", ink(r,U'3'), ink(r,U'A'));
  r.set_class_gamma(TextRenderer::GlyphClass::Digit, 0.85f);
  st::print("数字类 γ=0.85 : 3={:.1f}  A={:.1f}\n", ink(r,U'3'), ink(r,U'A'));
  return 0;
}
