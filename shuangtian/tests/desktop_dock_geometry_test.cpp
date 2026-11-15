/// Dock 与系统任务栏「等高 / 零缝隙 / 零遮蔽」的几何契约。
///
/// ## 缺陷（2026-11-15，用户实测报"避免最大化窗口遮蔽和间隙"）
///
/// 当时的 Dock 是**浮在中间的一条圆角条**：高 72、底部还留了 12px 边距。三条几何
/// 关系全不成立，而每一条都对应一个用户能看见的问题：
///
/// | 当时 | 后果 |
/// |---|---|
/// | 高 72 ≠ 任务栏厚度 48 | 顶缘比 `rcWork.bottom` 高 24px ⇒ 上方 24px 永不可用 |
/// | 底边留 12px | 屏幕底与 Dock 之间露一条壁纸缝 |
/// | 四角圆角 | 下方两角贴屏幕底露出两个缺角 |
///
/// ## 真正的约束
///
/// 最大化窗口的底边 = `rcWork.bottom`（实测：2560×1440 屏幕上为 1392）。所以：
///
///     Dock 顶缘 == rcWork.bottom       ← 与窗口底边重合，既不遮它也不留缝
///     Dock 底缘 == rcMonitor.bottom    ← 贴屏幕底
///     Dock 高度 == 任务栏厚度（48）    ← 由上面两条推出来
///
/// 本测试钉住 `resolve_dock_geometry` 的这个推导（含退化输入）。
#include <cmath>
#include <utility>

#include "st/math/geometry.hpp"
#include "st/test/test.hpp"

namespace {

using st::math::Size;

// 与 `apps/st_desktop/main.cpp` 同一份常量与推导（应用是 exe，不能链接进测试）。
constexpr float kFallback = 48.0f;

struct DockGeometry {
  float height{kFallback};
  float top{0.0f};
};

[[nodiscard]] auto resolve(Size viewport, Size work) -> DockGeometry {
  DockGeometry dock;
  const float strip = viewport.height - work.height;
  const bool plausible = strip > 0.5f && strip < viewport.height * 0.34f;
  dock.height = plausible ? strip : kFallback;
  dock.top = viewport.height - dock.height;
  return dock;
}

}  // namespace

ST_TEST(dock_geometry_matches_taskbar_strip) {
  // 实测环境：显示器 2560×1440，工作区 2560×1392（任务栏 48px）。
  const auto dock = resolve(Size{2560.0f, 1440.0f}, Size{2560.0f, 1392.0f});
  ST_CHECK(std::abs(dock.height - 48.0f) < 0.5f);
  // 顶缘必须 == 工作区底缘（1440 − 48 = 1392）。
  ST_CHECK(std::abs(dock.top - 1392.0f) < 0.5f);
}

ST_TEST(dock_geometry_follows_taskbar_thickness_at_other_dpi) {
  // 125% 缩放下任务栏是 60 逻辑 px，工作区相应变矮。
  // 这条钉的是"不许写死 48"：写死会在高 DPI 机器上出现缝或遮蔽。
  const auto dock = resolve(Size{2048.0f, 1152.0f}, Size{2048.0f, 1092.0f});
  ST_CHECK(std::abs(dock.height - 60.0f) < 0.5f);
  ST_CHECK(std::abs(dock.top - 1092.0f) < 0.5f);
}

ST_TEST(dock_geometry_falls_back_when_no_taskbar) {
  // 工作区 == 视口（没有任务栏，或无头后端不知道）：退回兜底，而不是把 Dock 压成 0。
  const auto dock = resolve(Size{1280.0f, 800.0f}, Size{1280.0f, 800.0f});
  ST_CHECK(std::abs(dock.height - kFallback) < 0.5f);
  ST_CHECK(std::abs(dock.top - (800.0f - kFallback)) < 0.5f);
}

ST_TEST(dock_geometry_rejects_absurd_work_area) {
  // 探测值可疑（工作区比视口矮了 80%）时不信任它：否则 Dock 会吃掉大半屏。
  const auto dock = resolve(Size{1280.0f, 1000.0f}, Size{1280.0f, 200.0f});
  ST_CHECK(std::abs(dock.height - kFallback) < 0.5f);
}

ST_TEST(dock_geometry_never_overlaps_maximized_window) {
  // 核心不变量：Dock 顶缘**不低于**工作区底缘。
  //
  // 低 → 最大化窗口的底边进到 Dock 里面 ≡ 遮蔽；
  // 高 → 中间留一条缝。
  // 两种都错，所以断言必须写成"相等"（容差 1px 吸收取整）。
  for (const auto& [monitor_h, work_h] : {std::pair{1440.0f, 1392.0f},
                                          std::pair{1152.0f, 1092.0f},
                                          std::pair{1080.0f, 1032.0f}}) {
    const auto dock = resolve(Size{1920.0f, monitor_h}, Size{1920.0f, work_h});
    const float work_bottom = work_h;
    ST_CHECK(std::abs(dock.top - work_bottom) <= 1.0f);
    // 且 Dock 底缘贴屏幕底。
    ST_CHECK(std::abs((dock.top + dock.height) - monitor_h) <= 1.0f);
  }
}
