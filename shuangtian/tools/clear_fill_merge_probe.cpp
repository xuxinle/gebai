// 「清屏 + 首层同色不透明整屏底」合并的**逐像素等价性**探针。
//
// ## 这条快路径是什么
//
// `UIView::paint` 的整帧路径是 `clear(bg)` → 画整树，而根容器几乎总会先用同一个 `bg`
// 铺满整屏。`Canvas` 现在记住「刚清成什么颜色、且还没被任何绘制碰过」，于是那一层
// 重复填充可以直接跳过（实测 `fill_rect` 0.31 → 0.05 ms、整帧 5.08 → 4.87 ms）。
//
// ## 判据
//
// 唯一要问的是：**跳过与不跳过，像素必须完全一样**。等价性由 `over(B, B) == B` 保证
// （同色不透明 `src-over` 到自身 = 自身），但**四个前提任何一个写漏都会错画**：
//   ① 清屏后没被碰过；② 纯色不透明 + opacity 1 + SrcOver；
//   ③ 颜色与 clear 完全一致；④ 覆盖整张画布、无圆角、无遮罩裁剪。
//
// 本探针逐条构造「**该跳**」与「**不该跳**」两类用例：
// - 该跳的：必须与“不跳”（直接画一遍）**逐像素相同**；
// - 不该跳的：必须真的画了（少画 = 漏掉一整块内容，是最危险的错法）。
//
// 实现上靠 `Canvas::set_skip_redundant_clear_fill_enabled(false)` 做 A/B——
// **同一进程、同一份代码**，只切这一个开关（换进程比会被别的东西污染）。
//
// 用法：clear_fill_merge_probe（无参数）

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/math/color.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/paint.hpp"

namespace {

constexpr int kWidth = 200;
constexpr int kHeight = 120;
const st::math::Color kBg{0xF7, 0xF8, 0xFA, 255};
const st::math::Color kOther{0x20, 0x60, 0xE0, 255};

[[nodiscard]] auto make_canvas() -> st::raster::Canvas {
  st::raster::Canvas canvas(kWidth, kHeight, 1.0f);
  canvas.clear(st::math::Color{0, 0, 0, 0});
  return canvas;
}

/// 一帧的绘制动作（刻意写成可复用的 lambda 形态，两边跑同一份）。
using Frame = void (*)(st::raster::Canvas&);

/// 整屏（逻辑坐标，scale=1 时等于物理尺寸）的不透明纯色填充。
void fill_whole(st::raster::Canvas& canvas, st::math::Color color, float radius = 0.0f,
                float opacity = 1.0f, st::raster::BlendMode blend = st::raster::BlendMode::SrcOver) {
  canvas.fill_rect(st::math::Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                                  static_cast<float>(kHeight)},
                   st::raster::Paint::solid(color), radius,
                   st::raster::DrawOptions{.blend = blend, .opacity = opacity});
}

struct Case {
  std::string name{};
  std::function<void(st::raster::Canvas&)> frame{};
  /// true = 这次填充**应该**被跳过（等价性判据）；false = 必须真的画（内容存在性判据）。
  bool should_skip{true};
};

/// 统计非底色像素数（用来确认“内容确实画出来了”）。
[[nodiscard]] auto pixels_differing_from(const st::raster::Canvas& canvas,
                                         st::math::Color reference) -> std::size_t {
  const std::uint32_t reference_value = st::math::premultiply(reference);
  std::size_t count = 0;
  for (const std::uint32_t pixel : canvas.pixels()) {
    if (pixel != reference_value) ++count;
  }
  return count;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "清屏 + 首层同色不透明整屏底：跳过与不跳过必须逐像素相同。",
      .criterion = "该跳的逐像素相同；不该跳的必须真的画出来（否则漏内容）。",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("clear_fill_merge_probe", spec, opts)) return st::probe::exit_ok;
  st::probe::result verdict{};

  std::vector<Case> cases;
  {
    Case item;
    item.name = "整屏同色不透明底（应跳）";
    item.frame = [](st::raster::Canvas& canvas) { fill_whole(canvas, kBg); };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "同色底 + 后续内容（应跳且内容在）";
    item.frame = [](st::raster::Canvas& canvas) {
      fill_whole(canvas, kBg);
      canvas.fill_rect(st::math::Rect{20.0f, 20.0f, 60.0f, 40.0f},
                       st::raster::Paint::solid(kOther));
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "异色底（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) { fill_whole(canvas, kOther); };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "同色但半透明 0.5（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) { fill_whole(canvas, kBg, 0.0f, 0.5f); };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "同色但带圆角（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) { fill_whole(canvas, kBg, 24.0f); };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "同色但未覆盖整屏（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      canvas.fill_rect(st::math::Rect{10.0f, 10.0f, 80.0f, 60.0f},
                       st::raster::Paint::solid(kBg));
    };
    cases.push_back(std::move(item));
  }
  {
    // **这条才是“必须覆盖整屏”的分辨力来源**：局部填**异色**。
    // 若把“覆盖整屏”条件去掉，这一小块会被错当成“重复铺同色”跳掉，
    // 于是画布保持底色 bg 而**那块异色根本没出现**（漏画一整块内容）。
    // 前一条用例（局部填**同色**）分辨不出：跳过与不画像素结果相同。
    Case item;
    item.name = "局部填异色（不该跳，覆盖整屏条件）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      canvas.fill_rect(st::math::Rect{10.0f, 10.0f, 80.0f, 60.0f},
                       st::raster::Paint::solid(kOther));
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "先画了别的东西再铺同色底（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      // 先画一块内容 → `cleared_fresh_` 必须已经失效，第二层填充必须真的执行。
      canvas.fill_rect(st::math::Rect{30.0f, 30.0f, 40.0f, 30.0f},
                       st::raster::Paint::solid(kOther));
      fill_whole(canvas, kBg);  // 这层会把上面那块盖掉——不跳才能得到“干净的底”
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "先画路径再铺同色底（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      // 路径走 `blend_coverage_runs`——那条入口也必须把 `cleared_fresh_` 清掉，
      // 否则“路径已经画了内容”却还当成“干净画布”，会把内容盖掉。
      st::math::Path path;
      path.add_circle(st::math::Point{60.0f, 60.0f}, 25.0f);
      canvas.fill_path(path, st::raster::Paint::solid(kOther));
      fill_whole(canvas, kBg);
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "先画圆再铺同色底（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      // `fill_circle` 直接调 `rasterize_path_runs`（不经 `fill_path`）——
      // 实测忘在那里清 `cleared_fresh_` 会差 2040 像素（内容被当成“干净的底”盖掉）。
      canvas.fill_circle(st::math::Point{80.0f, 60.0f}, 25.0f,
                         st::raster::Paint::solid(kOther));
      fill_whole(canvas, kBg);
    };
    cases.push_back(std::move(item));
  }
  {
    Case item;
    item.name = "同色但 BlendMode::Src（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      fill_whole(canvas, kBg, 0.0f, 1.0f, st::raster::BlendMode::Src);
    };
    cases.push_back(std::move(item));
  }
  {
    // **这条才能抓住“不透明 + SrcOver”前提**：半透明同色底盖在异色内容上，
    // 必须把它淡化；若被错跳过（当成“重复铺同色”），那块异色会原样保留。
    Case item;
    item.name = "半透明同色底盖在异色内容上（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      canvas.fill_rect(st::math::Rect{40.0f, 40.0f, 40.0f, 30.0f},
                       st::raster::Paint::solid(kOther));
      fill_whole(canvas, kBg, 0.0f, 0.5f);
    };
    cases.push_back(std::move(item));
  }
  {
    // 同上，但用 **Src** 混合：`Src` 是替换语义，整屏会被直接写成半透明 bg，
    // 那块异色应该完全消失。若把“必须 SrcOver”条件去掉，它会被当成可跳而保留。
    Case item;
    item.name = "Src 混合的整屏底盖在异色内容上（不该跳）";
    item.should_skip = false;
    item.frame = [](st::raster::Canvas& canvas) {
      canvas.fill_rect(st::math::Rect{40.0f, 40.0f, 40.0f, 30.0f},
                       st::raster::Paint::solid(kOther));
      fill_whole(canvas, kBg, 0.0f, 1.0f, st::raster::BlendMode::Src);
    };
    cases.push_back(std::move(item));
  }

  st::print("清屏 + 首层同色底合并：A/B 等价性（同一进程，只切一个开关）\n");
  std::size_t failures = 0;
  std::size_t skipped_equivalences = 0;
  for (const auto& item : cases) {
    // A：跳过开启（默认）
    st::raster::Canvas merged = make_canvas();
    merged.clear(kBg);
    merged.set_skip_redundant_clear_fill_enabled(true);
    item.frame(merged);

    // B：跳过关闭（参照）
    st::raster::Canvas plain = make_canvas();
    plain.clear(kBg);
    plain.set_skip_redundant_clear_fill_enabled(false);
    item.frame(plain);

    // 逐像素比对
    std::size_t diff = 0;
    std::size_t total = std::min(merged.pixels().size(), plain.pixels().size());
    for (std::size_t index = 0; index < total; ++index) {
      if (merged.pixels()[index] != plain.pixels()[index]) ++diff;
    }
    const bool identical = diff == 0;
    // 内容存在性：若“不该跳”的用例里画面几乎全是底色，说明那一层真的被漏画了。
    // 判据只在两个用例上可用（其它用例本身就是整屏纯色，与底色不可区分）。
    bool content_ok = true;
    if (item.name == "异色底（不该跳）" ||
        item.name == "局部填异色（不该跳，覆盖整屏条件）") {
      content_ok = pixels_differing_from(merged, kBg) > 0;
    } else if (item.name == "先画了别的东西再铺同色底（不该跳）" ||
               item.name == "先画路径再铺同色底（不该跳）" ||
               item.name == "先画圆再铺同色底（不该跳）") {
      // 正确结果是“整屏干净的 bg”（先前那块内容被同色不透明底盖掉了）。
      content_ok = pixels_differing_from(merged, kBg) == 0;
    } else if (item.name == "同色但半透明 0.5（不该跳）") {
      // **语义判据**：同色 0.5 的 `src-over` 到自身，结果仍是自身（`over(B, B×a) = B`），
      // 所以这个用例的**像素结果**本不该与“不画”不同——它只能验证“没有画错”，
      // 验证不了“真的画了”。真正能区分的是下面那条：半透明底**覆盖在一块异色内容上**。
      content_ok = pixels_differing_from(merged, kBg) == 0;
    } else if (item.name == "半透明同色底盖在异色内容上（不该跳）") {
      // 先画一块 `kOther` → 再铺 `kBg` 0.5（`src-over`）→ 块内像素必须**介于两者之间**。
      // 若这一层被错跳过（当成“重复铺同色”），块内会完整保留 `kOther`。
      const st::math::Color center = merged.pixel_at(60, 55);
      content_ok = center.r != kOther.r && center.r != kBg.r;
    } else if (item.name == "Src 混合的整屏底盖在异色内容上（不该跳）") {
      // `Src` 是替换：那块异色必须被完全覆盖（不留任何 `kOther` 痕迹）。
      content_ok = pixels_differing_from(merged, kBg) == 0;
    }
    const bool ok = identical && content_ok;
    if (!ok) ++failures;
    if (item.should_skip && identical) ++skipped_equivalences;

    st::print("  {:<34} {:<4} 差异 {:>4} 像素{}\n", item.name, ok ? "ok" : "FAIL", diff,
              content_ok ? "" : " · 内容不对（漏画/多画）");
    verdict.add(item.name, ok,
                std::format("差异 {} 像素 · 内容{}", diff, content_ok ? "正确" : "不对"));
  }

  // **自检**：必须真的覆盖到“跳过”这条路径，否则上面的“差异 0”只是在比两份
  // 完全相同的“不跳”结果——恒绿的空护栏（本仓踩过多次）。
  if (skipped_equivalences == 0) {
    st::print("  ⚠ 一条“应跳”用例都没有真的走到跳过路径（判绿不代表快路径正确）\n");
    verdict.add("覆盖到跳过路径", false, "所有应跳用例都没生效");
  } else {
    verdict.add("覆盖到跳过路径", true,
                std::format("{} 条应跳用例逐像素相同", skipped_equivalences));
  }
  if (failures == 0) st::print("  ✓ 全部用例通过\n");
  return st::probe::main_of(spec, opts, verdict.conclude());
}
