// 字形图集分配器的正确性。
//
// 为什么这条值得写：图集是“把 N 张小图塞进一张大图”，**错一行就是文字花屏或串色**，
// 而那类缺陷在小样本上往往看不出来（要多到某一行刚好填满才暴露）。
// 所以这里钉的是**不变量**，而不是某个具体布局：
//
//   1. 任何分配结果都**完全落在画布内**；
//   2. 任意两个分配结果**互不重叠**（含 padding）；
//   3. 放不下时返回空且**不改变已占用状态**（扩容后重试能接着用）；
//   4. 超画布的条目直接拒绝（不要白试）；
//   5. 扩容策略单调增长、到上限后返回空。

#include <algorithm>
#include <random>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/raster/glyph_atlas.hpp"
#include "st/test/test.hpp"

using st::raster::atlas::next_capacity;
using st::raster::atlas::Region;
using st::raster::atlas::ShelfPacker;

namespace {

[[nodiscard]] auto overlaps(const Region& a, const Region& b) noexcept -> bool {
  return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

}  // namespace

ST_TEST(glyph_atlas_regions_stay_inside_canvas) {
  ShelfPacker packer{1};
  packer.set_canvas(128, 128);
  std::vector<Region> regions;
  // 用不同尺寸（字形实际就是这样：宽高随字变）
  const int widths[] = {7, 13, 21, 5, 31, 11};
  const int heights[] = {9, 15, 13, 17, 11, 19};
  for (int index = 0; index < 200; ++index) {
    const int w = widths[index % 6];
    const int h = heights[index % 6];
    const auto region = packer.allocate(w, h);
    if (!region.has_value()) break;   // 画布满了
    regions.push_back(*region);
  }

  ST_CHECK(!regions.empty());
  for (const Region& region : regions) {
    ST_CHECK(region.x >= 0);
    ST_CHECK(region.y >= 0);
    ST_CHECK(region.x + region.width <= 128);
    ST_CHECK(region.y + region.height <= 128);
  }
  st::log::out("[atlas] 128×128 画布装了 {} 个条目\n", regions.size());
}

ST_TEST(glyph_atlas_regions_never_overlap) {
  ShelfPacker packer{1};
  packer.set_canvas(256, 128);
  std::vector<Region> regions;
  std::mt19937 random{12345};
  std::uniform_int_distribution<int> width_dist{3, 40};
  std::uniform_int_distribution<int> height_dist{3, 40};
  for (int index = 0; index < 400; ++index) {
    const auto region = packer.allocate(width_dist(random), height_dist(random));
    if (!region.has_value()) continue;   // 满了就跳过（后面可能还有小条目能放下）
    regions.push_back(*region);
  }
  ST_CHECK(regions.size() > 10);
  // 两两比较（几百个条目，O(n²) 在这个规模下无所谓，换来的是判据最直白）
  for (std::size_t i = 0; i < regions.size(); ++i) {
    for (std::size_t j = i + 1; j < regions.size(); ++j) {
      ST_CHECK(!overlaps(regions[i], regions[j]));
    }
  }
  st::log::out("[atlas] 256×128 装了 {} 个随机尺寸条目，无重叠\n", regions.size());
}

ST_TEST(glyph_atlas_rejects_oversized_and_keeps_state) {
  ShelfPacker packer{0};
  packer.set_canvas(64, 64);
  const auto first = packer.allocate(10, 10);
  ST_REQUIRE(first.has_value());
  const std::size_t after_first = packer.entry_count();

  // 超过画布的条目：直接拒绝，且**不改变已占用状态**
  ST_CHECK(!packer.allocate(65, 10).has_value());
  ST_CHECK(!packer.allocate(10, 65).has_value());
  ST_CHECK(!packer.allocate(0, 10).has_value());
  ST_CHECK(!packer.allocate(10, -1).has_value());
  ST_CHECK(packer.entry_count() == after_first);

  // 把剩余空间填满后再分配也应当失败而不破坏状态
  while (packer.allocate(10, 10).has_value()) {
  }
  const std::size_t when_full = packer.entry_count();
  ST_CHECK(!packer.allocate(10, 10).has_value());
  ST_CHECK(packer.entry_count() == when_full);
  st::log::out("[atlas] 超限与满页都返回空且不改变状态（已占 {} 项）\n", when_full);
}

ST_TEST(glyph_atlas_reset_clears_state) {
  ShelfPacker packer{1};
  packer.set_canvas(32, 32);
  ST_REQUIRE(packer.allocate(30, 30).has_value());
  ST_CHECK(packer.entry_count() == 1);
  packer.reset();
  ST_CHECK(packer.entry_count() == 0);
  ST_CHECK(packer.used_height() == 0);
  // reset 之后能重新分配同一块
  ST_CHECK(packer.allocate(30, 30).has_value());
}

ST_TEST(glyph_atlas_next_capacity_grows_then_stops) {
  // 宽度优先翻倍
  const auto a = next_capacity(64, 64, 1024);
  ST_REQUIRE(a.has_value());
  ST_CHECK(a->first == 128);
  ST_CHECK(a->second == 64);

  // 宽度到上限后翻高度
  const auto b = next_capacity(1024, 64, 1024);
  ST_REQUIRE(b.has_value());
  ST_CHECK(b->first == 1024);
  ST_CHECK(b->second == 128);

  // 两维都到上限：再也长不大了
  ST_CHECK(!next_capacity(1024, 1024, 1024).has_value());

  // 非法输入
  ST_CHECK(!next_capacity(0, 64, 1024).has_value());
  ST_CHECK(!next_capacity(64, 64, 0).has_value());
}
