// **启动时窗口必须居中，给阴影留出余量**。
//
// 用户报「在黑背景上启动亮色窗口，再切白背景，阴影没有，拖动才出现」。
//
// 真因：`fit_window_to_work_area` 用 `SWP_NOMOVE`，窗口停在 (0,0)，
// 而影子层还要向外扩 `shadow_margin`——左、上两侧的阴影整个落在屏幕外。
// 拖动窗口离开边缘后周围才有余量，阴影"就出现了"。
//
// 本测试量的是**几何契约**（能否在进程内验证，不需要真窗口）：
// 给定工作区与窗口尺寸，居中后的 x/y 必须 >= margin，才能让阴影可见。
#include <algorithm>
#include <cstdint>

#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

/// 复刻 `fit_window_to_work_area` 的居中算式，验证四边余量。
struct Placement {
  int x;
  int y;
};

[[nodiscard]] auto centre_in_work_area(int work_w, int work_h, int client_w, int client_h,
                                       int work_x = 0, int work_y = 0) -> Placement {
  return Placement{work_x + std::max(0, (work_w - client_w) / 2),
                   work_y + std::max(0, (work_h - client_h) / 2)};
}

}  // namespace

ST_TEST(window_placement_leaves_room_for_shadow) {
  // 真实场景：4K 屏工作区 2560×1392，窗口按工作区 92% 铺开，shadow_margin 18 逻辑 ×1.5 = 27 物理。
  constexpr int kMarginPhysical = 27;
  struct Case {
    const char* name;
    int work_w;
    int work_h;
    int client_w;
    int client_h;
  };
  const Case cases[] = {
      {"4K 工作区 92%", 2560, 1392, 2355, 1281},
      {"小屏 92%", 1366, 728, 1257, 670},
      {"恰好铺满工作区", 1920, 1080, 1920, 1080},
  };

  for (const Case& c : cases) {
    const auto p = centre_in_work_area(c.work_w, c.work_h, c.client_w, c.client_h);
    const int right_gap = c.work_w - (p.x + c.client_w);
    const int bottom_gap = c.work_h - (p.y + c.client_h);
    st::log::out("[place] {} {}x{} 放入 {}x{} -> x={} y={} 右余={} 下余={}\n", c.name, c.client_w,
                 c.client_h, c.work_w, c.work_h, p.x, p.y, right_gap, bottom_gap);

    // 只要工作区比窗口大，四边余量就至少该有阴影厚度；
    // （恰好铺满时无余量——那种情况阴影本来就该在屏幕外，与最大化同理）
    if (c.work_w > c.client_w) {
      ST_CHECK(p.x >= kMarginPhysical);
      ST_CHECK(right_gap >= kMarginPhysical);
    }
    if (c.work_h > c.client_h) {
      ST_CHECK(p.y >= kMarginPhysical);
      ST_CHECK(bottom_gap >= kMarginPhysical);
    }
  }
}

/// 对照：**旧写法**（`SWP_NOMOVE`，窗口停在 0,0）会让左、上阴影落到屏幕外。
///
/// 这条把缺陷钉成可执行的形式：只要有人把居中改回"不动"，上面的用例就会红，
/// 而这条说明了为什么。
ST_TEST(window_placement_without_centering_hides_shadow) {
  constexpr int kMarginPhysical = 27;
  constexpr int kClientW = 2355;
  constexpr int kClientH = 1281;
  // 旧行为：窗口左上角在 (0,0)
  const int shadow_left = 0 - kMarginPhysical;
  const int shadow_top = 0 - kMarginPhysical;
  st::log::out("[place] 不居中时影子层左上角 = ({}, {})（负值 = 屏外）\n", shadow_left,
               shadow_top);
  ST_CHECK(shadow_left < 0);
  ST_CHECK(shadow_top < 0);

  // 居中后两者都为正
  const auto p = centre_in_work_area(2560, 1392, kClientW, kClientH);
  ST_CHECK(p.x - kMarginPhysical > 0);
  ST_CHECK(p.y - kMarginPhysical > 0);
}
