/// 共享文本端口 fixture 的语义契约（`tests/support/text_port_fixtures.hpp`）。
///
/// ## 这组用例守的是什么
///
/// 那 10 份逐文件复制的桩被收敛成 4 个共享类型后，**最大的风险是后人图省事再把它们合并**——
/// 它们看起来"都是 TextPort 桩"，但**行为语义不同**，合并会悄悄改掉一批测试的断言基线：
///
/// | 类型 | 宽度模型 | draw 的副作用 |
/// |---|---|---|
/// | `FixedAdvanceTextPort` | 每码点 `kAdvance`（定宽） | 只记录文本，不落像素 |
/// | `ProportionalTextPort` | ASCII ≈ 0.55·size，非 ASCII = size | **真画像素矩形** |
/// | `RendererTextPort` | 委托真实字体引擎 | 真光栅化字形 |
/// | `RecordingTextPort` | 转发任意端口 | 记录入参（不改绘制） |
///
/// 所以这里**逐条钉住差异**：定宽与比例必须给出不同宽度、比例型必须真的落像素、
/// 记录型必须完整转发。将来谁把它们合成一个，这里当场红灯。

#include "st/test/test.hpp"

#include <string>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"
#include "tests/support/text_port_fixtures.hpp"

namespace {

[[nodiscard]] auto make_canvas(int width, int height) -> st::raster::Canvas {
  st::raster::Canvas canvas{width, height, 1.0f};
  canvas.clear(st::math::Color{0, 0, 0, 0});
  return canvas;
}

}  // namespace

/// 定宽与比例是**两种不同**的宽度模型（这正是不能合并的原因）。
ST_TEST(text_port_fixture_fixed_and_proportional_disagree_on_width) {
  const st::test::FixedAdvanceTextPort fixed{};
  const st::test::ProportionalTextPort proportional{};
  constexpr std::string_view kText{"abcdefgh"};
  constexpr float kSize = 10.0f;

  // 定宽：8 个码点 × 8 = 64
  ST_CHECK_EQ(fixed.measure_width(kText, kSize), 64.0f);
  // 比例：8 个 ASCII 码点 × (10 × 0.55) = 44
  ST_CHECK_EQ(proportional.measure_width(kText, kSize), 44.0f);
  // 两者必须不同——若相同，说明有人把它们合并成了一个模型。
  ST_CHECK(fixed.measure_width(kText, kSize) != proportional.measure_width(kText, kSize));
}

/// 行高一致（这是真正共享的部分）。
ST_TEST(text_port_fixture_line_heights_agree) {
  const st::test::FixedAdvanceTextPort fixed{};
  const st::test::ProportionalTextPort proportional{};
  for (const float size : {11.0f, 16.0f, 20.25f}) {
    ST_CHECK_EQ(fixed.line_height(size), size * 1.45f);
    ST_CHECK_EQ(proportional.line_height(size), proportional.line_height(size));
    ST_CHECK_EQ(fixed.line_height(size), proportional.line_height(size));
  }
}

/// 定宽型**不落像素**（只记录）；比例型**必须落像素**（亚像素对齐断言靠它）。
ST_TEST(text_port_fixture_fixed_records_while_proportional_paints) {
  const st::test::FixedAdvanceTextPort fixed{};
  st::raster::Canvas canvas_fixed = make_canvas(64, 24);
  fixed.draw(canvas_fixed, "ab", st::math::Point{2.0f, 2.0f}, 10.0f, st::math::Color{255, 255, 255});
  ST_CHECK_EQ(fixed.drawn().size(), 1U);
  ST_CHECK_EQ(fixed.drawn()[0], std::string("ab"));
  // 定宽型不画：画布应当仍是全透明。
  ST_CHECK_EQ(canvas_fixed.pixel_at(4, 4).a, 0U);

  const st::test::ProportionalTextPort proportional{};
  st::raster::Canvas canvas_prop = make_canvas(64, 24);
  proportional.draw(canvas_prop, "ab", st::math::Point{2.0f, 2.0f}, 10.0f,
                    st::math::Color{255, 255, 255});
  // 比例型真画：应当有非透明像素。
  bool painted = false;
  for (int y = 0; y < 24 && !painted; ++y) {
    for (int x = 0; x < 64; ++x) {
      if (canvas_prop.pixel_at(x, y).a != 0U) {
        painted = true;
        break;
      }
    }
  }
  ST_CHECK(painted);
}

/// 记录型必须**完整转发**（它只是观测层，不能改变被观测对象的行为）。
ST_TEST(text_port_fixture_recording_forwards_and_records) {
  const st::test::FixedAdvanceTextPort inner{};
  const st::test::RecordingTextPort recorder{inner};

  constexpr std::string_view kText{"hello"};
  constexpr float kSize = 12.0f;
  ST_CHECK_EQ(recorder.measure_width(kText, kSize), inner.measure_width(kText, kSize));
  ST_CHECK_EQ(recorder.line_height(kSize), inner.line_height(kSize));

  st::raster::Canvas canvas = make_canvas(64, 24);
  recorder.draw(canvas, kText, st::math::Point{0.0f, 0.0f}, kSize, st::math::Color{255, 255, 255},
                st::text::FontRole::Proportional, 1.5f, true);
  // 转发：底层记录到了这次绘制。
  ST_CHECK_EQ(inner.drawn().size(), 1U);
  // 记录：字重实参被如实留下（字重链路用例全靠它）。
  ST_CHECK(recorder.last_bold);
  ST_CHECK_EQ(recorder.last_embolden, 1.5f);
  ST_REQUIRE(recorder.calls.size() == 1U);
  ST_CHECK_EQ(recorder.calls[0].size, kSize);
}
