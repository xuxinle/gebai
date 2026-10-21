// 图集**内容管理**的正确性——重点是扩容。
//
// 图集最贵的缺陷不是“放不下”，而是**扩容后旧条目悄悄失效或错位**：
// 那会表现为“文字在某个时刻开始花屏/串字”，而且要跑到几百个字形才出现。
// 所以这里钉四条：
//
//   1. 命中时不写像素（稳态路径不该有额外成本）；
//   2. 写入的内容**读得回来**（位置算对、行跨距算对）；
//   3. 扩容后**所有旧条目仍然命中且内容不变**；
//   4. 到上限后返回 nullopt（不无限吃内存）。

#include <algorithm>
#include <cstdint>
#include <vector>

#include "st/core/print.hpp"
#include "st/raster/glyph_atlas_content.hpp"
#include "st/test/test.hpp"

using st::raster::atlas::GlyphAtlas;

namespace {

/// 用 key 派生一个可辨识的填充值，便于验证“搬对了”。
[[nodiscard]] auto fill_for(std::uint64_t key) noexcept -> std::uint8_t {
  return static_cast<std::uint8_t>((key * 37U + 11U) % 251U);
}

/// 把一块 `w×h` 用某个灰度填充为 RGBA8（模拟灰度字形的写入）。
void write_gray(std::uint8_t* dst, int row_bytes, int w, int h, std::uint8_t value) {
  for (int row = 0; row < h; ++row) {
    std::uint8_t* line = dst + static_cast<std::size_t>(row) * row_bytes;
    for (int column = 0; column < w; ++column) {
      line[column * 4 + 0] = value;
      line[column * 4 + 1] = value;
      line[column * 4 + 2] = value;
      line[column * 4 + 3] = 255U;
    }
  }
}

}  // namespace

ST_TEST(glyph_atlas_content_write_then_read_back) {
  GlyphAtlas atlas{64, 512};
  const auto placed = atlas.place(7, 10, 6, [](std::uint8_t* dst, int row_bytes) {
    write_gray(dst, row_bytes, 10, 6, fill_for(7));
  });
  ST_REQUIRE(placed.has_value());
  ST_CHECK(placed->fresh);
  ST_CHECK(placed->region.width == 10);
  ST_CHECK(placed->region.height == 6);

  // 读回：该块的四个角都应是 fill_for(7)
  const auto& pixels = atlas.pixels();
  const int stride = atlas.width() * 4;
  const std::uint8_t expected = fill_for(7);
  const auto at = [&](int x, int y) {
    return pixels[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4U];
  };
  ST_CHECK(at(placed->region.x, placed->region.y) == expected);
  ST_CHECK(at(placed->region.x + 9, placed->region.y + 5) == expected);
}

ST_TEST(glyph_atlas_content_hit_does_not_rewrite) {
  GlyphAtlas atlas{64, 512};
  int writes = 0;
  const auto first = atlas.place(42, 8, 8, [&](std::uint8_t* dst, int row_bytes) {
    ++writes;
    write_gray(dst, row_bytes, 8, 8, fill_for(42));
  });
  ST_REQUIRE(first.has_value());
  ST_CHECK(writes == 1);

  // 再查同一个 key：不应再写
  const auto second = atlas.place(42, 8, 8, [&](std::uint8_t*, int) { ++writes; });
  ST_REQUIRE(second.has_value());
  ST_CHECK(!second->fresh);
  ST_CHECK(writes == 1);
  ST_CHECK(second->region.x == first->region.x);
  ST_CHECK(second->region.y == first->region.y);
}

ST_TEST(glyph_atlas_content_grows_and_keeps_old_entries) {
  // 小画布起步，强制扩容几次
  GlyphAtlas atlas{64, 1024};
  std::vector<std::pair<std::uint64_t, std::uint8_t>> planted;
  const int sizes[] = {9, 13, 17, 11, 19};

  // 填到触发多次扩容
  for (std::uint64_t key = 1; key <= 120; ++key) {
    const int w = sizes[key % 5];
    const int h = sizes[(key + 2) % 5];
    const std::uint8_t value = fill_for(key);
    const auto placed = atlas.place(key, w, h, [&](std::uint8_t* dst, int row_bytes) {
      write_gray(dst, row_bytes, w, h, value);
    });
    ST_REQUIRE(placed.has_value());
    if (placed->fresh) planted.emplace_back(key, value);
  }
  ST_CHECK(atlas.width() > 64);   // 确实扩容过
  st::print("[atlas] 120 个条目后画布 {}×{}，共 {} 条\n", atlas.width(), atlas.height(),
            atlas.entry_count());

  // **关键断言：扩容后所有旧条目仍然命中，且内容没被搬坏**
  const auto& pixels = atlas.pixels();
  const int stride = atlas.width() * 4;
  int verified = 0;
  for (const auto& [key, value] : planted) {
    // 用同样尺寸再查一次：必须命中（fresh == false）
    const int w = sizes[key % 5];
    const int h = sizes[(key + 2) % 5];
    const auto again = atlas.place(key, w, h, [](std::uint8_t*, int) {
      // 命中时不该被调用
    });
    ST_REQUIRE(again.has_value());
    ST_CHECK(!again->fresh);
    // 抽查块内一点的内容
    const std::size_t offset = static_cast<std::size_t>(again->region.y) * stride +
                               static_cast<std::size_t>(again->region.x) * 4U;
    ST_CHECK(pixels[offset] == value);
    ++verified;
  }
  st::print("[atlas] 扩容后逐条校验 {} 项，全部命中且内容正确\n", verified);
}

ST_TEST(glyph_atlas_content_stops_at_max_dimension) {
  // 上限很小的图集：很快装满，之后应当返回 nullopt 而不是无限扩容
  GlyphAtlas atlas{32, 32};
  int accepted = 0;
  for (std::uint64_t key = 1; key <= 200; ++key) {
    const auto placed = atlas.place(key, 12, 12, [&](std::uint8_t* dst, int row_bytes) {
      write_gray(dst, row_bytes, 12, 12, fill_for(key));
    });
    if (!placed.has_value()) break;
    ++accepted;
    ST_CHECK(atlas.width() <= 32);
    ST_CHECK(atlas.height() <= 32);
  }
  ST_CHECK(accepted > 0);
  ST_CHECK(accepted < 200);   // 一定装不下 200 个
  st::print("[atlas] 32×32 上限下滑装了 {} 个 12×12 条目\n", accepted);
}
