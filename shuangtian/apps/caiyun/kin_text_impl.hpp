/// 裁云 · **逐字排版**（kinetic type）渲染 —— 实现。
///
/// ## 它为什么单独立一条路（而不是给 `Text` 加开关）
///
/// 现有 `Text` 图层是**整段**交给字体引擎排版（`draw_text_lines`）。
/// 那个路径上没有"第 3 个字现在偏移多少"这个概念，而这类片子
/// （对标 onetake 的 one-dot / motion-web）的主语言恰恰是**每个字独立动**。
///
/// 硬塞进 `Text` 得重写已有的行布局/换行/对齐（那段有大量实测过的对齐修正），
/// 风险高、收益低。所以另开一条：**自己负责排版（字宽用 TextPort 量），
/// 每个字自己算位置与透明度，逐个绘制**。
///
/// ## 排版怎么算
///
/// 一行字只需三件事：每个字的宽度、字距、整行如何对齐。
/// · 宽度：`TextPort::measure_width(单字)` —— **逐个量**（而非均分），
///   因为中英混排时字宽差异极大；
/// · 字距：`layer.tracking`；
/// · 对齐：整行总宽算出来后按 `align` 决定起点。
///
/// 不做换行：这类片子的标题通常是单行（换行会破坏"一气呵成"的节奏）。
/// 需要多行写多个 `kintext` 图层——顺带让每行能被独立驱动。

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace caiyun {
namespace detail_kin {

/// 把一个 UTF-8 字符串切成**码点**（而不是字节）。
///
/// 为何不能按字节切：中文一个字是 3 字节，按字节切会把一个字拆成三段乱码。
/// 这类片子里中英混排很常见，必须按码点。
[[nodiscard]] inline auto split_codepoints(std::string_view utf8) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(utf8.size());
  std::size_t index = 0;
  while (index < utf8.size()) {
    const auto lead = static_cast<std::uint8_t>(utf8[index]);
    std::size_t length = 1;
    if ((lead & 0x80U) == 0x00U) {
      length = 1;
    } else if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
    }
    // 越界保护：被截断的尾部按单字节处理（不至于崩，只影响一个字符）
    length = std::min(length, utf8.size() - index);
    out.emplace_back(utf8.substr(index, length));
    index += length;
  }
  return out;
}

/// 逐字入场曲线：ease-out cubic。
///
/// 为何不用线性：字出现的瞬间应该是"弹到位"而不是"匀速长大"——
/// 前者像印刷机落到纸上，后者像进度条。
[[nodiscard]] inline auto ease_out_cubic(float t) -> float {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const float inverse = 1.0f - clamped;
  return 1.0f - inverse * inverse * inverse;
}

}  // namespace detail_kin

/// 绘制一个 `KinText` 图层。
///
/// `PainterT` 需提供：
/// · `text(content, size, x, y, color)` —— `(x, y)` 是**左上角**
/// · `fill(x, y, w, h, color, radius)`
///
/// 用模板而非具名类型：`Painter` 是 `motion.cpp` 内部**匿名命名空间**的类，
/// 头文件无法前向声明它（声明出来是另一个类型）。模板让它变成
/// "任何有这两个方法的东西"，顺带解除了本模块对调用方内部结构的依赖。
template <typename PainterT>
void paint_kin_text(PainterT& painter, const Layer& layer, const LayerState& state, float scale,
                    const st::ui::TextPort* port, float local_time) {
  if (layer.text.empty() || port == nullptr) return;

  const std::vector<std::string> glyphs = detail_kin::split_codepoints(layer.text);
  if (glyphs.empty()) return;

  const float font_size = std::max(1.0f, layer.size * scale);
  const float tracking = layer.tracking * scale;
  const float rise = layer.kin_rise * scale;
  const float float_amount = layer.kin_float * scale;

  // ── ① 量每个字的宽度，算整行总宽（对齐要用）──
  std::vector<float> widths;
  widths.reserve(glyphs.size());
  float total = 0.0f;
  for (const std::string& glyph : glyphs) {
    // 空格宽度有些引擎会返回 0，给保底值（否则空格塌陷、字间距突变）
    float width = port->measure_width(glyph, font_size);
    if (width <= 0.0f) {
      width = glyph == " " ? font_size * 0.30f : font_size * 0.5f;
    }
    widths.push_back(width);
    total += width;
  }
  if (glyphs.size() > 1) {
    total += tracking * static_cast<float>(glyphs.size() - 1);
  }

  // ── ② 起笔位置（按对齐）──
  // `align` 是 int：0=居中 1=左 2=右（与其余文字图层同一口径）。
  float cursor = state.x;
  if (layer.align == 0) {
    cursor = state.x + (state.w - total) * 0.5f;
  } else if (layer.align == 2) {
    cursor = state.x + state.w - total;
  }

  // 基线：让墨迹区在矩形里垂直居中。
  //
  // 用 `shaped_ascent` 而非 `ascent`：前者是 draw 真正用的基线位，
  // 中英混排时两者不等（实测差 1~5px，用后者会系统性偏下）。
  const float ascent = port->shaped_ascent(layer.text, font_size);
  const float descent = port->descent(font_size);
  const float ink_height = ascent + descent;
  const float baseline = state.y + (state.h - ink_height) * 0.5f + ascent;

  // ── ③ 打字机推进：`kin_type_cps` 决定"这一行先出现几个字" ──
  //
  // 它与"逐字入场"是两件事：打字机管**几个字存在**，入场管**每个字怎么出现**。
  // 两者可叠加（先逐字出现，每个字又各自上浮）。
  std::size_t visible_limit = glyphs.size();
  if (layer.kin_type_cps > 0.001f) {
    const float shown = std::floor(local_time * layer.kin_type_cps);
    visible_limit = shown <= 0.0f ? 0U : static_cast<std::size_t>(shown);
  }

  const auto base_alpha = static_cast<std::uint8_t>(
      std::lround(static_cast<float>(layer.color.a) * std::clamp(state.alpha, 0.0f, 1.0f)));

  // ── ④ 逐字绘制 ──
  for (std::size_t index = 0; index < glyphs.size(); ++index) {
    if (index >= visible_limit) break;

    // 第 index 个字比前一个晚 `kin_stagger` 秒开始
    const float start = static_cast<float>(index) * layer.kin_stagger;
    const float progress =
        layer.kin_dur <= 0.001f ? 1.0f
                                : std::clamp((local_time - start) / layer.kin_dur, 0.0f, 1.0f);
    if (progress <= 0.0f) {
      // 还没轮到它：仍要推进光标，否则后续字位置全错
      cursor += widths[index] + tracking;
      continue;
    }
    const float eased = detail_kin::ease_out_cubic(progress);

    // 入场形态
    float offset_y = (1.0f - eased) * rise;
    float glyph_alpha = eased;
    float glyph_scale = 1.0f;
    if (layer.kin_enter == 1) {
      // 微缩放：从 1.18 缩到 1.0（"字落下来"的手感）
      glyph_scale = 1.0f + 0.18f * (1.0f - eased);
      offset_y = (1.0f - eased) * rise * 0.4f;
    } else if (layer.kin_enter == 2) {
      // 模糊上浮的近似：更低透明度 + 更大位移（真模糊要多次渲染，成本高）
      offset_y = (1.0f - eased) * rise * 1.6f;
      glyph_alpha = eased * eased;
    }

    // 持续起伏：每个字有自己相位，否则整行像一块板子在动
    if (float_amount > 0.01f) {
      const float phase = static_cast<float>(index) * 0.55f;
      offset_y +=
          std::sin((local_time * layer.kin_float_hz + phase) * 6.283185307179586f) * float_amount;
    }

    const float glyph_size = font_size * glyph_scale;
    float draw_x = cursor;
    if (glyph_scale != 1.0f) {
      // 字变宽了，左移一半增量让**中心**不动（否则字会绕着左边跳）
      draw_x -= widths[index] * (glyph_scale - 1.0f) * 0.5f;
    }
    const auto alpha = static_cast<std::uint8_t>(std::lround(
        static_cast<float>(base_alpha) * std::clamp(glyph_alpha, 0.0f, 1.0f)));
    if (alpha > 1U) {
      // `Painter::text` 的 `(x, y)` 是**左上角**（不是基线点），
      // 所以把基线反推成顶部：top = baseline - ascent。
      const float top = baseline + offset_y - ascent * glyph_scale;
      painter.text(glyphs[index], glyph_size, draw_x, top,
                   st::math::Color{layer.color.r, layer.color.g, layer.color.b, alpha});
    }
    cursor += widths[index] + tracking;
  }

  // ── ⑤ 光标（打字机竖线）──
  //
  // 它是"因果可见"的一部分：让观众看到字是**被打出来的**，而不是凭空出现。
  if (layer.kin_caret != 0) {
    const float caret_x = cursor;
    const bool blink_on = std::sin(local_time * 6.9115038f) > -0.2f;
    if (blink_on) {
      const float caret_width = std::max(1.5f, font_size * 0.06f);
      painter.fill(caret_x + tracking * 0.4f, baseline - ascent * 0.92f, caret_width,
                   font_size * layer.kin_caret_scale,
                   st::math::Color{layer.color.r, layer.color.g, layer.color.b, base_alpha});
    }
  }
}

}  // namespace caiyun
